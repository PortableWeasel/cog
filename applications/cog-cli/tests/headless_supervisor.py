#!/usr/bin/env python3
"""Own and reap one harness tree, including escaped-session descendants.

Fresh Linux subreaper process only: every child enumerated here is this
supervisor's actual immediate child. Never signal a remembered process group or
reap an unspecified PID. Harness exit is observed without consuming its status
until cleanup owns the entire remaining descendant tree.
"""
import ctypes
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import time


def children():
    values = (Path("/proc/self/task") / str(os.getpid()) / "children").read_text().split()
    pids = [int(value) for value in values]
    assert all(pid > 0 for pid in pids)
    return pids


def peek(pid):
    assert pid > 0
    return os.waitid(os.P_PID, pid, os.WEXITED | os.WNOHANG | os.WNOWAIT)


def exit_status(info):
    return info.si_status if info.si_code == os.CLD_EXITED else -info.si_status


def drain(process, seconds=5):
    """Kill/reap only proved positive immediate children, including adoptees."""
    until = time.monotonic() + seconds
    while True:
        owned = children()
        if not owned:
            return True
        for pid in owned:
            info = peek(pid)  # ECHILD is failure, never permission to signal.
            if info is None:
                try:
                    os.kill(pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass  # Exited but remains our unreaped child.
            info = peek(pid)
            if info is not None:
                reaped, status = os.waitpid(pid, os.WNOHANG)
                assert reaped in (0, pid)
                if reaped == pid and process is not None and pid == process.pid:
                    process.returncode = os.waitstatus_to_exitcode(status)
        if time.monotonic() >= until:
            return not children()
        time.sleep(0.005)


def main():
    timeout = float(sys.argv[1])
    result_path = Path(sys.argv[2])
    command = sys.argv[3:]
    assert 0 < timeout <= 600 and command
    result = {"status": None, "timed_out": False, "cleanup": False, "error": None}
    process = None
    stopped = False

    def stop(signum, frame):
        nonlocal stopped
        stopped = True

    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)
    try:
        libc = ctypes.CDLL(None, use_errno=True)
        libc.prctl.argtypes = [ctypes.c_int, ctypes.c_ulong, ctypes.c_ulong,
            ctypes.c_ulong, ctypes.c_ulong]
        libc.prctl.restype = ctypes.c_int
        if libc.prctl(36, 1, 0, 0, 0) != 0:  # PR_SET_CHILD_SUBREAPER
            raise OSError(ctypes.get_errno(), "PR_SET_CHILD_SUBREAPER")
        assert not children()
        process = subprocess.Popen(command, start_new_session=True)
        until = time.monotonic() + timeout
        while not stopped:
            info = peek(process.pid)
            if info is not None:
                result["status"] = exit_status(info)
                break
            if time.monotonic() >= until:
                result["timed_out"] = True
                break
            time.sleep(0.005)
    except BaseException as error:
        result["error"] = repr(error)
    finally:
        try:
            result["cleanup"] = drain(process)
            if result["status"] is None and process is not None:
                result["status"] = process.returncode
        except BaseException as error:
            result["error"] = result["error"] or repr(error)
        temporary = result_path.with_suffix(".new")
        temporary.write_text(json.dumps(result))
        temporary.replace(result_path)
    return 0 if result["cleanup"] and result["error"] is None else 1


if __name__ == "__main__":
    sys.exit(main())

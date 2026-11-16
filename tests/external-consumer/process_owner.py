"""Fresh Linux owner for one external proof process and any adopted children.

Only exact positive immediate children are signalled/reaped after WNOWAIT proves
native ownership. No remembered PGID, broad waitpid(-1), or parent-global reap
policy is used. This is test infrastructure, never a Cog runtime fallback.
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
    return [int(pid) for pid in Path(f"/proc/self/task/{os.getpid()}/children").read_text().split()]


def supervise(command, seconds, receipt):
    library = ctypes.CDLL(None, use_errno=True)
    if library.prctl(36, 1, 0, 0, 0) != 0:
        raise OSError(ctypes.get_errno(), "PR_SET_CHILD_SUBREAPER")
    assert not children()
    stopped = False

    def stop(_signum, _frame):
        nonlocal stopped
        stopped = True

    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)
    process = subprocess.Popen(command, start_new_session=True)
    result = dict(status=None, timed_out=False, forced_children=0, cleanup=False)
    end = time.monotonic() + seconds
    while not stopped:
        status = os.waitid(os.P_PID, process.pid, os.WEXITED | os.WNOHANG | os.WNOWAIT)
        if status is not None:
            result["status"] = status.si_status if status.si_code == os.CLD_EXITED else -status.si_status
            break
        if time.monotonic() >= end:
            result["timed_out"] = True
            break
        time.sleep(0.005)
    end = time.monotonic() + 5
    while children() and time.monotonic() < end:
        for pid in children():
            assert pid > 0
            status = os.waitid(os.P_PID, pid, os.WEXITED | os.WNOHANG | os.WNOWAIT)
            if status is None:
                try:
                    os.kill(pid, signal.SIGKILL)
                    result["forced_children"] += 1
                except ProcessLookupError:
                    pass  # Still owned and unreaped; never reused here.
            status = os.waitid(os.P_PID, pid, os.WEXITED | os.WNOHANG | os.WNOWAIT)
            if status is not None:
                reaped, value = os.waitpid(pid, os.WNOHANG)
                if reaped == process.pid:
                    process.returncode = os.waitstatus_to_exitcode(value)
        time.sleep(0.005)
    result["cleanup"] = not children()
    Path(receipt).write_text(json.dumps(result) + "\n")
    return 0 if result["cleanup"] else 1


def run_owned(command, env, cwd, receipt, seconds=35):
    result = subprocess.run([sys.executable, str(Path(__file__).resolve()),
        str(seconds), str(receipt), *map(str, command)], env=env, cwd=cwd,
        text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=seconds + 8)
    facts = json.loads(Path(receipt).read_text())
    if result.returncode or facts["status"] != 0 or facts["timed_out"] or not facts["cleanup"] or facts["forced_children"]:
        raise RuntimeError(f"external owned proof failed: {facts}\n{result.stdout}")
    return result.stdout, facts


if __name__ == "__main__":
    sys.exit(supervise(sys.argv[3:], float(sys.argv[1]), sys.argv[2]))

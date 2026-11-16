#!/usr/bin/env python3
"""Diagnostic: establish a full-pipe write before signalling the installed host.

No timing assumption establishes readiness: /proc must show write(1/2), the
pipe must still be full, and both host termination signals must be masked.
Drain after the bounded observation so even a defective host exits normally.
"""
import array
import fcntl
import os
import pathlib
import select
import signal
import subprocess
import sys
import tempfile
import termios
import time


def until(predicate, timeout):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        value = predicate()
        if value:
            return value
        select.select([], [], [], 0.005)
    raise AssertionError("bounded readiness handshake expired")


def case(cli, destination, sig, root):
    rd, wr = os.pipe()
    capacity = fcntl.fcntl(wr, fcntl.F_SETPIPE_SZ, 4096)
    assert os.write(wr, b"p" * capacity) == capacity
    env = {k: v for k, v in os.environ.items() if not k.startswith("COG_")}
    env.update(HOME=root, XDG_STATE_HOME=root)
    outputs = {destination: wr, "stderr" if destination == "stdout" else "stdout": subprocess.DEVNULL}
    proc = subprocess.Popen([cli, "--mock", "--prompt", "pipe probe", "--chunks", "answer"],
                            stdin=subprocess.DEVNULL, env=env, **outputs)
    os.close(wr)
    try:
        fd = 1 if destination == "stdout" else 2
        def blocked():
            assert proc.poll() is None, "host exited before handshake"
            try:
                syscall = pathlib.Path(f"/proc/{proc.pid}/syscall").read_text().split()
                status = pathlib.Path(f"/proc/{proc.pid}/status").read_text()
            except FileNotFoundError:
                return False
            mask = int(next(line.split()[1] for line in status.splitlines() if line.startswith("SigBlk:")), 16)
            available = array.array("i", [0])
            fcntl.ioctl(rd, termios.FIONREAD, available, True)
            # Linux x86-64 SYS_write; inspect fd too, rather than any sleeping task.
            return (len(syscall) >= 2 and syscall[0] == "1" and int(syscall[1], 16) == fd
                    and available[0] == capacity and mask & 0x4002 == 0x4002)
        until(blocked, 5)
        os.kill(proc.pid, sig)
        try:
            proc.wait(timeout=0.3)
            responsive = True
        except subprocess.TimeoutExpired:
            responsive = False
        status = pathlib.Path(f"/proc/{proc.pid}/status").read_text() if proc.poll() is None else ""
        pending = next((line for line in status.splitlines() if line.startswith("ShdPnd:")), "exited")
        print(f"{destination} {signal.Signals(sig).name}: responsive={responsive}; {pending}", flush=True)
        # Release backpressure and prove this was not an unrelated startup hang.
        os.set_blocking(rd, False)
        end = time.monotonic() + 5
        while proc.poll() is None and time.monotonic() < end:
            ready, _, _ = select.select([rd], [], [], 0.05)
            if ready:
                try:
                    os.read(rd, 65536)
                except BlockingIOError:
                    pass
        assert proc.wait(timeout=1) == 0
        return responsive
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait()
        os.close(rd)


if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="cog-full-pipe-") as root:
        results = [case(sys.argv[1], destination, sig, root)
                   for destination in ("stdout", "stderr")
                   for sig in (signal.SIGINT, signal.SIGTERM)]
    sys.exit(0 if all(results) else 1)

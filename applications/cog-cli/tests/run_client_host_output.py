#!/usr/bin/env python3
"""Installed host full-pipe failure/signal joins and inherited FD semantics."""
import fcntl
import os
import pathlib
import select
import signal
import subprocess
import sys
import tempfile
import time


def until(predicate, seconds=5):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        value = predicate()
        if value:
            return value
        select.select([], [], [], 0.005)
    raise AssertionError("bounded readiness handshake expired")


def full_pipe(cli, root, env, destination, sig, write_first):
    rd, wr = os.pipe()
    capacity = fcntl.fcntl(wr, fcntl.F_SETPIPE_SZ, 4096)
    original_flags = fcntl.fcntl(wr, fcntl.F_GETFL)
    directory = root / f"{destination}-{sig}-{write_first}"
    seed = subprocess.run([cli, "--mock", "--session", "pipe", "--store", str(directory),
                           "--prompt", "seed checkpoint", "--chunks", "seed"],
                          env=env, capture_output=True, timeout=5)
    assert seed.returncode == 0 and seed.stdout == b"seed\n", seed.stderr
    args = [cli, "--mock", "--interactive", "--resume", "pipe", "--store", str(directory),
            "--chunks", "answer", "--usage"]
    streams = {destination: wr, "stderr" if destination == "stdout" else "stdout": subprocess.PIPE}
    proc = subprocess.Popen(args, stdin=subprocess.PIPE, env=env, **streams)
    log_fd = rd if destination == "stderr" else proc.stderr.fileno()
    observed = bytearray()
    try:
        def startup():
            assert proc.poll() is None
            readable, _, _ = select.select([log_fd], [], [], 0)
            if readable:
                observed.extend(os.read(log_fd, 65536))
            return b"core.run_start" in observed
        until(startup)
        status = pathlib.Path(f"/proc/{proc.pid}/status").read_text()
        mask = int(next(line.split()[1] for line in status.splitlines() if line.startswith("SigBlk:")), 16)
        assert mask & 0x4002 == 0x4002  # actual host signalfd ownership
        assert fcntl.fcntl(wr, fcntl.F_GETFL) == original_flags
        assert os.write(wr, b"p" * capacity) == capacity
        if write_first:
            # MOCK has no reported usage, so --usage alone intentionally emits
            # nothing. Reload's checked publication notice is a deterministic
            # stderr write; the ordinary model text exercises stdout instead.
            proc.stdin.write(b"hello\n" if destination == "stdout" else b"/reload model pipe-test\n")
            proc.stdin.flush()
            def completed_or_blocked():
                if proc.poll() is not None:
                    return "completed"
                try:
                    syscall = pathlib.Path(f"/proc/{proc.pid}/syscall").read_text().split()
                except (PermissionError, ProcessLookupError, FileNotFoundError):
                    # LSan's exit-time tracer can briefly own ptrace access.
                    # Wait for actual waitpid completion within the same bound;
                    # inaccessible proc data is never counted as readiness.
                    return False
                fd = 1 if destination == "stdout" else 2
                if len(syscall) >= 2 and syscall[0] == "1" and int(syscall[1], 16) == fd:
                    return "blocked"
                return False
            try:
                readiness = until(completed_or_blocked)
            except AssertionError as error:
                raise AssertionError((destination, sig, write_first,
                                      pathlib.Path(f"/proc/{proc.pid}/wchan").read_text())) from error
            if readiness == "blocked":
                os.kill(proc.pid, sig)
                # On the old host, the signal is pending forever in this write.
                # A correct host can instead have failed IO before this branch.
                assert proc.wait(timeout=2) == 1
            else:
                assert proc.returncode == 1
        else:
            # Separately exercise an actual masked signal with the pipe already
            # saturated. No prompt is needed to make signal delivery race-free.
            os.kill(proc.pid, sig)
            assert proc.wait(timeout=2) in (0, 1)
        assert fcntl.fcntl(wr, fcntl.F_GETFL) == original_flags
        os.close(wr)
        wr = -1
        # Teardown released the guard even when stderr could not report it.
        resumed = subprocess.run([cli, "--mock", "--resume", "pipe", "--store", str(directory),
                                  "--prompt", "after pipe", "--chunks", "resumed"],
                                 env=env, capture_output=True, timeout=5)
        assert resumed.returncode == 0 and resumed.stdout == b"resumed\n", (destination, sig, write_first, resumed.stderr)
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait()
        proc.stdin.close()
        if proc.stdout is not None:
            proc.stdout.close()
        if proc.stderr is not None:
            proc.stderr.close()
        if wr >= 0:
            os.close(wr)
        os.close(rd)


def regular_offsets(cli, root, env):
    for destination in ("stdout", "stderr"):
        path = root / f"regular-{destination}"
        with path.open("w+b", buffering=0) as file:
            file.write(b"prefix:")
            flags = fcntl.fcntl(file.fileno(), fcntl.F_GETFL)
            streams = {destination: file, "stderr" if destination == "stdout" else "stdout": subprocess.DEVNULL}
            result = subprocess.run([cli, "--mock", "--prompt", "regular offset", "--chunks", "answer"],
                                    env=env, timeout=5, **streams)
            assert result.returncode == 0
            assert fcntl.fcntl(file.fileno(), fcntl.F_GETFL) == flags
            final_offset = file.tell()
            data = path.read_bytes()
            assert final_offset == len(data) and data.startswith(b"prefix:")
            if destination == "stdout":
                assert data == b"prefix:answer\n"
            else:
                assert b"core.created" in data and b"core.destroyed" in data


if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="cog-client-host-") as directory:
        root = pathlib.Path(directory)
        prefix = root / "installed"
        subprocess.run(["cmake", "--install", sys.argv[1], "--prefix", str(prefix),
                        "--component", "cog-cli-runtime"], check=True, stdout=subprocess.DEVNULL)
        cli = str(prefix / "bin" / "cog-cli")
        env = {k: v for k, v in os.environ.items() if not k.startswith("COG_")}
        env.update(HOME=directory, XDG_STATE_HOME=str(root / "xdg"))
        for destination in ("stdout", "stderr"):
            for sig in (signal.SIGINT, signal.SIGTERM):
                for write_first in (True, False):
                    full_pipe(cli, root, env, destination, sig, write_first)
        regular_offsets(cli, root, env)

#!/usr/bin/env python3
"""Actual idle EOF/signals and owner-only restart through the installed host."""
import os
import pathlib
import select
import signal
import subprocess
import sys
import tempfile
import time

build, fixture = sys.argv[1:3]
with tempfile.TemporaryDirectory(prefix="cog-empty-installed-") as directory:
    root = pathlib.Path(directory)
    prefix, store = root / "installed", root / "store"
    store.mkdir()
    subprocess.run(["cmake", "--install", build, "--prefix", str(prefix), "--component", "cog-cli-runtime"],
                   check=True, stdout=subprocess.DEVNULL)
    cli = str(prefix / "bin" / "cog-cli")
    env = {key: value for key, value in os.environ.items() if not key.startswith("COG_")}
    env.update(HOME=directory, XDG_CONFIG_HOME=str(root / "config"), XDG_STATE_HOME=str(root / "state"))

    def run(*arguments, code=0, text=b""):
        result = subprocess.run([cli, *arguments, "--store", str(store)], env=env,
                                input=text, capture_output=True, timeout=5)
        assert result.returncode == code, (arguments, result.returncode, result.stdout, result.stderr)
        assert result.stdout == b"", result.stdout
        return result

    def state(identity):
        result = subprocess.run([fixture, "inspect", str(store), identity], env=env,
                                check=True, capture_output=True, timeout=5)
        return tuple(int(part) for part in result.stdout.split())

    run("--mock", "--interactive", "--session", "eof")
    assert state("eof") == (1, 1)
    run("--mock", "--interactive", "--resume", "eof", text=b"/history\n/quit\n")
    assert state("eof") == (2, 1)
    run("--mock", "--interactive", "--session", "eof", code=1)
    assert state("eof") == (2, 1)

    for sig in (signal.SIGINT, signal.SIGTERM):
        identity = signal.Signals(sig).name.lower()
        proc = subprocess.Popen([cli, "--mock", "--interactive", "--session", identity, "--store", str(store)],
                                env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        observed = bytearray()
        try:
            deadline = time.monotonic() + 5
            while b"core.run_start" not in observed:
                assert proc.poll() is None
                assert time.monotonic() < deadline, "idle readiness handshake timed out"
                if select.select([proc.stderr], [], [], 0.01)[0]:
                    observed.extend(os.read(proc.stderr.fileno(), 65536))
            status = pathlib.Path(f"/proc/{proc.pid}/status").read_text()
            mask = int(next(line.split()[1] for line in status.splitlines() if line.startswith("SigBlk:")), 16)
            assert mask & 0x4002 == 0x4002
            assert state(identity) == (1, 1)
            proc.send_signal(sig)
            assert proc.wait(timeout=5) == 0  # Keep stdin open: the signal is the stop cause.
            assert proc.stdout.read() == b""
            observed.extend(proc.stderr.read())
            assert b"core.destroyed" in observed
        finally:
            if proc.poll() is None:
                proc.kill()
                proc.wait()
            proc.stdin.close()
            proc.stdout.close()
            proc.stderr.close()
        run("--mock", "--interactive", "--resume", identity)
        assert state(identity) == (2, 1)

    subprocess.run([fixture, "reserve", str(store), "interrupted"], env=env, check=True, timeout=5)
    run("--mock", "--interactive", "--resume", "interrupted")
    assert state("interrupted") == (2, 1)
    run("--mock", "--interactive", "--resume", "absent", code=1)
    assert not (store / "absent" / "manifest.bin").exists()

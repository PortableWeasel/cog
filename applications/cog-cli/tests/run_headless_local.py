#!/usr/bin/env python3
"""Actual installed-role local root, watch and controller join; no substitutes."""
from pathlib import Path
import json
import os
import signal
import subprocess
import sys
import tempfile
import time


def wait_for(predicate, processes, seconds=6):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        value = predicate()
        if value:
            return value
        for process in processes:
            if process.poll() is not None:
                raise AssertionError(f"premature process exit: {process.args[1]} {process.returncode}")
        time.sleep(0.01)
    raise AssertionError("bounded local join timed out")


def replacements(path):
    records = []
    for line in path.read_text().splitlines(keepends=True):
        if not line.endswith("\n"):
            continue
        value = json.loads(line)
        if value.get("type") == "replacement":
            records.append(value)
    return records


def completed(path, prompt):
    for view in reversed(replacements(path)):
        items = view["items"]
        if len(items) >= 2 and items[-2][3] == prompt and items[-1][1:3] == [2, 0]:
            return view
    return None


def main():
    executable = Path(sys.argv[1]).resolve()
    build = Path(sys.argv[2]).resolve()
    scratch = build / "tmp"
    scratch.mkdir(exist_ok=True)
    env = dict(os.environ, TMPDIR=str(scratch))
    processes, streams = [], []
    with tempfile.TemporaryDirectory(prefix="local-", dir=scratch) as directory:
        root = Path(directory)
        socket = root / "ui.sock"
        def start(name, arguments):
            output, diagnostic = root / (name + ".out"), root / (name + ".err")
            out, err = output.open("wb"), diagnostic.open("wb")
            streams.extend([out, err])
            process = subprocess.Popen([str(executable), *arguments], stdin=subprocess.PIPE,
                                       stdout=out, stderr=err, env=env)
            processes.append(process)
            return process, output
        try:
            host, host_output = start("host", ["--headless", "--mock", "--store", str(root / "store"),
                "--socket", str(socket), "--children", "1", "--prompt", "initial"])
            wait_for(socket.exists, [host])
            watch, watch_output = start("watch", ["--watch", str(socket)])
            initial = wait_for(lambda: completed(watch_output, "initial"), [host, watch])
            controller, controller_output = start("controller", ["--attach", str(socket)])
            wait_for(lambda: completed(controller_output, "initial"), [host, controller])
            controller.stdin.write(b"explicit second turn\n")
            controller.stdin.flush()
            second = wait_for(lambda: completed(watch_output, "explicit second turn"), [host, watch, controller])
            wait_for(lambda: completed(controller_output, "explicit second turn"), [host, controller])
            assert initial["session"] == second["session"] and initial["generation"] == second["generation"]
            assert int(second["sequence"]) > int(initial["sequence"])
            assert len(second["items"]) == 4
            controller.stdin.write(b"/quit\n")
            controller.stdin.flush()
            assert controller.wait(timeout=6) == 0
            watch.send_signal(signal.SIGTERM)
            assert watch.wait(timeout=6) == 0
            host.send_signal(signal.SIGTERM)
            assert host.wait(timeout=6) == 0
            assert host_output.read_bytes() == b"", "headless host wrote conversation directly"
            print("headless local root/watch/controller: PASS")
        except BaseException:
            for path in sorted([*root.glob("*.err"), *root.glob("*.out")]):
                print(path.name + ": " + path.read_text(), file=sys.stderr)
            raise
        finally:
            for process in processes:
                if process.poll() is None:
                    process.kill()
                process.wait()
                if process.stdin:
                    process.stdin.close()
            for stream in streams:
                stream.close()


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Negative infrastructure controls: failed harness trees must not survive.

The tiny executable is only a runner-failure control; it supplies no runtime,
provider, registry or scenario evidence. Its finite escaped-session descendants
make leaks observable without signaling or reaping a foreign PID from this test.
"""
from pathlib import Path
import json
import subprocess
import sys
import tempfile
from headless_fixture import TLSFixture


def main():
    build = Path(sys.argv[1]).resolve()
    scratch = build / "tmp"
    scratch.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="hc-", dir=scratch) as directory:
        harness = Path(directory) / "negative.py"
        harness.write_text('''#!/usr/bin/env python3
from pathlib import Path
import json, os, resource, signal, subprocess, sys, time
workspace, mode = Path(sys.argv[4]), sys.argv[7]
marker = workspace / "owned-pids.json"
code = """import json, os, sys, time
from pathlib import Path
child = os.fork()
if child == 0:
    time.sleep(3)
    os._exit(0)
Path(sys.argv[1]).write_text(json.dumps([os.getpid(), child]))
time.sleep(3)
"""
subprocess.Popen([sys.executable, "-c", code, str(marker)], start_new_session=True)
until = time.monotonic() + 1
while not marker.exists():
    assert time.monotonic() < until
    time.sleep(0.005)
if mode == "abort":
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    os.abort()
if mode == "timeout":
    time.sleep(3)
sys.exit(17 if mode == "failure" else 0)
''')
        harness.chmod(0o755)
        for mode in ("success", "failure", "abort", "timeout"):
            failed = False
            with TLSFixture(build, lambda request, fixture: None) as fixture:
                try:
                    fixture.run(harness, sys.executable, extra=(mode,),
                        timeout=0.3 if mode == "timeout" else 3)
                except (AssertionError, subprocess.TimeoutExpired):
                    failed = True
                pids = json.loads((fixture.workspace / "owned-pids.json").read_text())
                result = json.loads(fixture.supervisor_result.read_text())
                assert result["cleanup"] and result["error"] is None, result
                assert result["status"] == {"success": 0, "failure": 17,
                    "abort": -6, "timeout": -9}[mode], result
                assert result["timed_out"] == (mode == "timeout"), result
            assert failed == (mode != "success"), mode
            assert len(pids) == 2 and len(set(pids)) == 2 and all(pid > 0 for pid in pids)
            assert all(not Path(f"/proc/{pid}").exists() for pid in pids), (mode, pids)
            print("owned harness tree cleanup:", mode, "PASS")


if __name__ == "__main__":
    main()

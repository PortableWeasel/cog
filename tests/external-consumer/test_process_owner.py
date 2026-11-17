"""Real escaped-child cleanup controls for the installed-proof supervisor."""
from pathlib import Path
import json
import os
import subprocess
import sys
import tempfile
import time


def main():
    scratch = Path(sys.argv[1]).resolve() / "installed-owner-controls"
    scratch.mkdir(parents=True, exist_ok=True)
    supervisor = Path(__file__).with_name("process_owner.py")
    with tempfile.TemporaryDirectory(dir=scratch, prefix="control-") as directory:
        root = Path(directory)
        for mode in ("success", "exit17", "abort", "timeout"):
            marker, receipt = root / (mode + ".pid"), root / (mode + ".json")
            command = """import os,signal,sys,time
pid=os.fork()
if pid==0:
    os.setsid()
    child=os.fork()
    if child==0:
        time.sleep(30)
        os._exit(0)
    open(sys.argv[1],"w").write(str(os.getpid())+" "+str(child))
    time.sleep(30)
    os._exit(0)
while not os.path.exists(sys.argv[1]):time.sleep(.002)
if sys.argv[2]=="timeout":time.sleep(30)
if sys.argv[2]=="abort":os.kill(os.getpid(),signal.SIGABRT)
sys.exit(17 if sys.argv[2]=="exit17" else 0)
"""
            # Core dumps are disabled inside this disposable subprocess only.
            command = "import resource;resource.setrlimit(resource.RLIMIT_CORE,(0,0))\n" + command
            result = subprocess.run([sys.executable, str(supervisor), "0.3" if mode == "timeout" else "3",
                str(receipt), sys.executable, "-c", command, str(marker), mode],
                cwd=root, capture_output=True, text=True, timeout=10)
            facts = json.loads(receipt.read_text())
            assert result.returncode == 0 and facts["cleanup"] and facts["forced_children"] >= 1, (mode, facts, result.stderr)
            if mode == "timeout":
                assert facts["timed_out"]
            else:
                assert not facts["timed_out"] and facts["status"] == {"success": 0, "exit17": 17, "abort": -6}[mode]
            for pid in marker.read_text().split():
                assert not Path("/proc", pid).exists(), (mode, "unreaped owned process", pid)
    print("installed supervisor success/exit17/SIGABRT/timeout escaped child+grandchild cleanup: PASS")


if __name__ == "__main__":
    main()

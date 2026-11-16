"""Run real partial native startup with bounded checkout-local storage."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile

build = Path(sys.argv[2]).resolve()
scratch = build / "tmp"
scratch.mkdir(exist_ok=True)
with tempfile.TemporaryDirectory(prefix="b-", dir=scratch) as directory:
    root = Path(directory)
    workspace = root / "workspace"
    workspace.mkdir()
    subprocess.run([str(Path(sys.argv[1]).resolve()), str(workspace),
                    str(root / "store"), str(root)], check=True, timeout=15,
                   env=dict(os.environ, TMPDIR=str(scratch),
                            COG_NATIVE_KEY="synthetic-native"))

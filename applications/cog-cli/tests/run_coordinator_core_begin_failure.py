"""Run pre-spawn coordinator failure with actual owned runtime cleanup."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile

binary, executable, build = (Path(value).resolve() for value in sys.argv[1:4])
scratch = build / "tmp"
scratch.mkdir(exist_ok=True)
with tempfile.TemporaryDirectory(prefix="core-begin-", dir=scratch) as directory:
    root = Path(directory)
    workspace = root / "workspace"
    workspace.mkdir()
    subprocess.run([str(binary), str(executable), str(workspace), str(root / "store"), str(root)],
                   check=True, timeout=15,
                   env=dict(os.environ, TMPDIR=str(scratch), COG_NATIVE_KEY="synthetic-native"))

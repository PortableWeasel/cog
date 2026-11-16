#!/usr/bin/env python3
"""Provide fresh real durable/workspace directories for the executor tool test."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile

build = Path(sys.argv[2]).resolve()
scratch = build / "tmp"
scratch.mkdir(exist_ok=True)
with tempfile.TemporaryDirectory(prefix="tools-", dir=scratch) as directory:
    root = Path(directory)
    workspace = root / "workspace"
    workspace.mkdir()
    subprocess.run([str(Path(sys.argv[1]).resolve()), str(root / "store"), str(workspace)],
                   check=True, timeout=15, env=dict(os.environ, TMPDIR=str(scratch)))

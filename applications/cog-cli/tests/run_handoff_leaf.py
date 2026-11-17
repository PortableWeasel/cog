#!/usr/bin/env python3
"""Run the real handoff leaf with bounded build-local store/socket fixtures."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile


def main():
    executable = Path(sys.argv[1]).resolve()
    build = Path(sys.argv[2]).resolve()
    scratch = build / "tmp"
    scratch.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="handoff-", dir=scratch) as directory:
        root = Path(directory)
        subprocess.run([str(executable), str(root / "store"), str(root / "ui.sock")],
                       check=True, timeout=20, env=dict(os.environ, TMPDIR=str(scratch)))


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Apply Cog packaging-only patches to a build-tree copy, leaving git clean."""
from pathlib import Path
import shutil
import subprocess
import sys

source, destination, patch = (Path(arg).resolve() for arg in sys.argv[1:4])
# The revision is part of the ExternalProject download command/stamp, so a
# changed submodule commit invalidates the prepared source on reconfiguration.
revision = sys.argv[4]
if destination.exists():
    shutil.rmtree(destination)
shutil.copytree(source, destination, ignore=shutil.ignore_patterns(".git"))
subprocess.run(["patch", "--batch", "--fuzz=0", "-p1", "-i", str(patch)],
               cwd=destination, check=True)
(destination / ".cog-upstream-revision").write_text(revision + "\n")

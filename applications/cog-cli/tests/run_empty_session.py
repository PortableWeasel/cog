#!/usr/bin/env python3
"""Run empty-session persistence checks without ambient configuration/credentials."""
import os
import subprocess
import sys
import tempfile

with tempfile.TemporaryDirectory(prefix="cog-empty-config-") as root:
    environment = {key: value for key, value in os.environ.items() if not key.startswith("COG_")}
    environment.update(HOME=root, XDG_CONFIG_HOME=root + "/config", XDG_STATE_HOME=root + "/state")
    subprocess.run([sys.argv[1]], env=environment, check=True, timeout=20)

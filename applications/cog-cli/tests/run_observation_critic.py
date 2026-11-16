#!/usr/bin/env python3
"""Run real unserviced-net ownership probes with a synthetic configuration."""
import os
import pathlib
import subprocess
import sys
import tempfile

if len(sys.argv) != 2:
    raise SystemExit("usage: run_observation_critic.py TEST_BINARY")
with tempfile.TemporaryDirectory(prefix="cog-observation-critic-") as directory:
    root = pathlib.Path(directory)
    config = root / "empty-config"
    config.write_text("")
    env = {key: value for key, value in os.environ.items()
           if not key.startswith("COG_") and key != "OPENROUTER_API_KEY"}
    env.update(HOME=str(root), XDG_CONFIG_HOME=str(root / "config"),
               XDG_STATE_HOME=str(root / "state"))
    subprocess.run([str(pathlib.Path(sys.argv[1]).resolve()), str(config)],
                   env=env, cwd=root, check=True, timeout=15)

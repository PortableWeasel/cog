#!/usr/bin/env python3
"""Direct real-code fixture: no runtime stubs or whole-project dependencies."""
from pathlib import Path
import os
import subprocess

root = Path(__file__).resolve().parents[2]
build = root / "build-wire"
(build / "tmp").mkdir(parents=True, exist_ok=True)
env = dict(os.environ, TMPDIR=str(build / "tmp"))
flags = "-std=c11 -g -O1 -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Wmissing-prototypes -Wstrict-prototypes -Wundef -Wpointer-arith -Wcast-align -Wwrite-strings -Wformat=2 -Werror".split()
includes = [f"-I{p}" for p in (root / "libraries").glob("*/include")]
sources = [str(root / "libraries/cog-orchestrator/src/orchestrator_wire.c"), str(root / "tests/orchestrator-wire/test_orchestrator_wire.c")]
wrap = ["-Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free"]
for name, compiler, extra in [
    ("gcc", "gcc", ["-Wlogical-op"]),
    ("clang", "clang", []),
    ("asan", "gcc", ["-Wlogical-op", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]),
]:
    binary = build / f"test-wire-{name}"
    command = [compiler, *flags, *extra, *includes, *sources, *wrap, "-o", str(binary)]
    with (build / f"{name}-build.log").open("w") as log:
        subprocess.run(command, check=True, stdout=log, stderr=subprocess.STDOUT, env=env)
    with (build / f"{name}-test.log").open("w") as log:
        subprocess.run([str(binary)], check=True, stdout=log, stderr=subprocess.STDOUT,
                       env=dict(env, ASAN_OPTIONS="detect_leaks=1:halt_on_error=1",
                                UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1"))
    print(f"{name}: PASS", flush=True)

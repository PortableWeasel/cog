#!/usr/bin/env python3
"""Compile-check the C examples embedded in docs/modules/*.md.

Every fenced ```c block that includes a cog header is treated as a complete
translation unit and compiled with the strict warning set used by
tests/critic-smoke. Blocks without a `#include <cog` line are skipped, so
short illustrative fragments are allowed.

Usage:
    python3 scripts/check-doc-examples.py [project-root]

Exit status is non-zero if any example fails to compile.
"""

import pathlib
import re
import subprocess
import sys

INCLUDE_DIRS = [
    "libraries/cog-core/include",
    "libraries/cog-provider/include",
    "libraries/cog-toolkit/include",
    "libraries/cog-ipc/include",
    "libraries/cog-obs/include",
    "libraries/cog-config/include",
    "libraries/cog-net/include",
    "libraries/cog-orchestrator/include",
    "applications/cog-cli/include",
]

FLAGS = [
    "-std=c11",
    "-fsyntax-only",
    "-Wall",
    "-Wextra",
    "-Wpedantic",
    "-Wshadow",
    "-Wstrict-prototypes",
    "-Wmissing-prototypes",
    "-Werror",
]

BLOCK_RE = re.compile(r"```c\n(.*?)```", re.DOTALL)


def main() -> int:
    root = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else ".").resolve()
    includes = []
    for rel in INCLUDE_DIRS:
        includes += ["-I", str(root / rel)]

    failures = 0
    checked = 0
    for page in sorted((root / "docs" / "modules").glob("*.md")):
        text = page.read_text(encoding="utf-8")
        for index, match in enumerate(BLOCK_RE.finditer(text), start=1):
            source = match.group(1)
            if "#include <cog" not in source and "#include <cog_cli" not in source:
                continue
            checked += 1
            tmp = root / "build-doc-examples" / f"{page.stem}-{index}.c"
            tmp.parent.mkdir(parents=True, exist_ok=True)
            tmp.write_text(source, encoding="utf-8")
            proc = subprocess.run(
                ["cc", *FLAGS, *includes, str(tmp)],
                capture_output=True,
                text=True,
            )
            if proc.returncode != 0:
                failures += 1
                print(f"FAIL {page.name} block {index}")
                print(proc.stderr.rstrip())
            else:
                print(f"ok   {page.name} block {index}")

    print(f"\n{checked - failures}/{checked} examples compiled")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""Reproduce private external-symbol namespaces without modifying upstream."""
import pathlib
import re
import sys

root = pathlib.Path(__file__).resolve().parent
header = root / "upstream" / "src" / "yyjson.h"
names = sorted(set(re.findall(
    r"^yyjson_api\s+[^\n]*?\b((?:unsafe_)?yyjson_\w+)\s*\(",
    header.read_text(), re.M)))
if not names:
    raise SystemExit("No yyjson API declarations found")
for component in ("provider", "toolkit"):
    guard = "COG_" + component.upper() + "_YYJSON_PREFIX_H"
    text = ("/* Generated from yyjson 0.12.0 external yyjson_api declarations. */\n"
            "#ifndef " + guard + "\n#define " + guard + "\n"
            + "".join("#define " + name + " cog_" + component + "_" + name + "\n"
                      for name in names)
            + "#endif\n")
    output = root / (component + "_prefix.h")
    if "--check" in sys.argv:
        if output.read_text() != text:
            raise SystemExit(str(output) + " differs; run generate_prefix.py")
    else:
        output.write_text(text)

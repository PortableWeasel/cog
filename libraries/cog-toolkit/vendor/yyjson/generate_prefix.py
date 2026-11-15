#!/usr/bin/env python3
"""Reproduce/check the private namespace without modifying upstream sources."""
import pathlib
import re
import sys
root = pathlib.Path(__file__).resolve().parent
names = sorted(set(re.findall(r'^yyjson_api\s+[^\n]*?\b((?:unsafe_)?yyjson_\w+)\s*\(', (root / 'yyjson.h').read_text(), re.M)))
text = ('/* Generated from yyjson 0.12.0 external yyjson_api declarations. */\n'
        '#ifndef COG_TOOLKIT_YYJSON_PREFIX_H\n#define COG_TOOLKIT_YYJSON_PREFIX_H\n'
        + ''.join('#define ' + name + ' cog_toolkit_' + name + '\n' for name in names)
        + '#endif\n')
if '--check' in sys.argv:
    if (root / 'prefix.h').read_text() != text:
        raise SystemExit('prefix.h differs; run generate_prefix.py')
else:
    (root / 'prefix.h').write_text(text)

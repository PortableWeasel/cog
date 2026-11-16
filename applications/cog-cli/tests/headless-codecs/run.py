#!/usr/bin/env python3
"""Build actual pure codecs + retained CGD1 implementation, never runtime stubs.
Each profile runs positive fixtures and deliberate failing assertion controls.
Unreferenced app entry points are discarded at link; CGD1 uses the real message
validator from cog-core. No curl/TLS/provider/config/runtime initialization.
"""
import argparse
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[4]
HERE = Path(__file__).resolve().parent

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--cc', default='gcc')
    parser.add_argument('--build-dir', type=Path, required=True)
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    args.build_dir.mkdir(parents=True, exist_ok=True)
    warnings = ['-Wall', '-Wextra', '-Wpedantic', '-Wconversion', '-Wsign-conversion',
                '-Wshadow', '-Wmissing-prototypes', '-Wstrict-prototypes', '-Wundef',
                '-Wpointer-arith', '-Wcast-align', '-Wwrite-strings', '-Wformat=2', '-Werror']
    if 'clang' not in Path(args.cc).name:
        warnings += ['-Wlogical-op']
    flags = ['-std=c11', '-O3', '-DNDEBUG', '-ffunction-sections', '-fdata-sections'] + warnings
    if args.sanitize:
        flags += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
    includes = ['-I' + str(ROOT / 'applications/cog-cli/src'), '-I' + str(ROOT / 'applications/cog-cli/include')]
    includes += ['-I' + str(p) for p in sorted((ROOT / 'libraries').glob('*/include'))]
    codecs = ['control', 'control_receipt', 'projection_codec', 'child_context']
    sources = [ROOT / ('applications/cog-cli/src/' + p + '.c') for p in codecs]
    sources += [ROOT / 'applications/cog-cli/src/app_display.c', ROOT / 'applications/cog-cli/src/cli.c', ROOT / 'libraries/cog-core/src/message.c']
    objects = []
    for source in sources:
        obj = args.build_dir / (source.stem + '.o')
        subprocess.run([args.cc, *flags, *includes, '-c', str(source), '-o', str(obj)], check=True)
        objects.append(str(obj))
    # These implementation objects have no constructor/runtime allocation imports.
    for name in codecs:
        symbols = subprocess.check_output(['nm', '-u', str(args.build_dir / (name + '.o'))], text=True)
        for symbol in ('malloc', 'calloc', 'realloc', 'free'):
            if any(line.split()[-1] == symbol for line in symbols.splitlines()):
                raise AssertionError('allocation import: ' + name + ':' + symbol)
    env = dict(os.environ, ASAN_OPTIONS='detect_leaks=1:halt_on_error=1', UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
    for name in ['receipt', 'child_context', 'control', 'projection']:
        exe = args.build_dir / ('test_' + name)
        subprocess.run([args.cc, *flags, *includes, str(HERE / ('test_' + name + '.c')), *objects,
                        '-Wl,--gc-sections', '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True, env=env, timeout=20)
        negative = subprocess.run([str(exe), '--negative-control'], capture_output=True, env=env, timeout=20)
        if negative.returncode == 0 or b'CHECK(false)' not in negative.stderr and b': false' not in negative.stderr:
            raise AssertionError('negative assertion control did not fail: ' + name)
        print(name + ': PASS (plus negative assertion control)', flush=True)

if __name__ == '__main__':
    main()

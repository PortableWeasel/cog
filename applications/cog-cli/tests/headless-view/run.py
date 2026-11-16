#!/usr/bin/env python3
"""Build real O8 owners, codecs and Unix IPC with bounded offline fixtures.
No runtime stubs, provider initialization, shared CMake edits or live calls.
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
    args.build_dir = args.build_dir.resolve()
    if not args.build_dir.is_relative_to(ROOT) or not args.build_dir.relative_to(ROOT).parts[0].startswith('build'):
        parser.error('build directory must be inside this checkout under build*')
    args.build_dir.mkdir(parents=True, exist_ok=True)
    scratch = args.build_dir / 'tmp'
    scratch.mkdir(exist_ok=True)
    os.environ['TMPDIR'] = str(scratch)
    warnings = ['-Wall', '-Wextra', '-Wpedantic', '-Wconversion', '-Wsign-conversion',
                '-Wshadow', '-Wmissing-prototypes', '-Wstrict-prototypes', '-Wundef',
                '-Wpointer-arith', '-Wcast-align', '-Wwrite-strings', '-Wformat=2', '-Werror']
    if 'clang' not in Path(args.cc).name:
        warnings += ['-Wlogical-op']
    flags = ['-std=c11', '-O3', '-DNDEBUG', '-ffunction-sections', '-fdata-sections'] + warnings
    if args.sanitize:
        flags += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
    includes = ['-I' + str(ROOT / 'applications/cog-cli/src'), '-I' + str(ROOT / 'applications/cog-cli/include'), '-I' + str(ROOT / 'libraries/cog-ipc/private')]
    includes += ['-I' + str(p) for p in sorted((ROOT / 'libraries').glob('*/include'))]
    codecs = ['projection_codec', 'projection', 'host_client', 'control']
    sources = [ROOT / ('applications/cog-cli/src/' + p + '.c') for p in codecs]
    sources += [ROOT / 'applications/cog-cli/src/app_display.c', ROOT / 'applications/cog-cli/src/cli.c', ROOT / 'libraries/cog-core/src/message.c']
    sources += [ROOT / 'libraries/cog-ipc/src/ipc_replay.c', ROOT / 'libraries/cog-ipc/src/ipc.c']
    objects = []
    for source in sources:
        obj = args.build_dir / (source.stem + '.o')
        subprocess.run([args.cc, *flags, *includes, '-c', str(source), '-o', str(obj)], check=True)
        objects.append(str(obj))
    env = dict(os.environ, ASAN_OPTIONS='detect_leaks=1:halt_on_error=1', UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
    (args.build_dir / 'peer.sock').unlink(missing_ok=True)
    for name in ['projection_owner', 'client']:
        exe = args.build_dir / ('test_' + name)
        subprocess.run([args.cc, *flags, *includes, str(HERE / ('test_' + name + '.c')), *objects,
                        '-Wl,--gc-sections', '-Wl,--wrap=cog_ipc_replay_append', '-o', str(exe)], check=True)
        subprocess.run([str(exe), *([str(args.build_dir.resolve() / 'peer.sock')] if name == 'client' else [])], check=True, env=env, timeout=20)
        negative = subprocess.run([str(exe), '--negative-control'], capture_output=True, env=env, timeout=20)
        if negative.returncode == 0 or b'CHECK(false)' not in negative.stderr and b': false' not in negative.stderr:
            raise AssertionError('negative assertion control did not fail: ' + name)
        print(name + ': PASS (plus negative assertion control)', flush=True)

if __name__ == '__main__':
    main()

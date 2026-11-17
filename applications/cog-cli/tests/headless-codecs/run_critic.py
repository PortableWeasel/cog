#!/usr/bin/env python3
"""Run the independent deterministic codec mutation/transactionality probe.

Usage matches run.py: --cc gcc|clang --build-dir PATH [--sanitize]. First run the
maintained four fixtures/negative controls and allocation-import checks, then
link this probe to those SAME actual codec/CGD1 objects. No runtime substitutes.
"""
import argparse
import os
from pathlib import Path
import subprocess
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[3]

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--cc', default='gcc')
    parser.add_argument('--build-dir', type=Path, required=True)
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    base = [sys.executable, str(HERE / 'run.py'), '--cc', args.cc, '--build-dir', str(args.build_dir)]
    if args.sanitize:
        base += ['--sanitize']
    subprocess.run(base, check=True)
    flags = ['-std=c11', '-O3', '-DNDEBUG', '-ffunction-sections', '-fdata-sections',
             '-Wall', '-Wextra', '-Wpedantic', '-Wconversion', '-Wsign-conversion',
             '-Wshadow', '-Wmissing-prototypes', '-Wstrict-prototypes', '-Wundef',
             '-Wpointer-arith', '-Wcast-align', '-Wwrite-strings', '-Wformat=2', '-Werror']
    if 'clang' not in Path(args.cc).name:
        flags += ['-Wlogical-op']
    if args.sanitize:
        flags += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
    includes = ['-I' + str(ROOT / 'applications/cog-cli/src'), '-I' + str(ROOT / 'applications/cog-cli/include')]
    includes += ['-I' + str(p) for p in sorted((ROOT / 'libraries').glob('*/include'))]
    objects = [str(args.build_dir / (name + '.o')) for name in
               ['control', 'control_receipt', 'projection_codec', 'child_context', 'app_display', 'cli', 'message']]
    exe = args.build_dir / 'test_critic_mutations'
    subprocess.run([args.cc, *flags, *includes, str(HERE / 'test_critic_mutations.c'),
                    *objects, '-Wl,--gc-sections', '-o', str(exe)], check=True)
    env = dict(os.environ, ASAN_OPTIONS='detect_leaks=1:halt_on_error=1',
               UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
    subprocess.run([str(exe)], check=True, env=env, timeout=30)
    negative = subprocess.run([str(exe), '--negative-control'], capture_output=True, env=env, timeout=20)
    if negative.returncode == 0 or b': false' not in negative.stderr:
        raise AssertionError('independent CHECK negative control failed')

if __name__ == '__main__':
    main()

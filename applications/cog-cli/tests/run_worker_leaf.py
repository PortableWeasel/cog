#!/usr/bin/env python3
"""Direct real owned-source fixtures; supplied joined archives are not rebuilt."""
import os
from pathlib import Path
import subprocess
import sys
root = Path(__file__).resolve().parents[3]
joined = Path(sys.argv[1]).resolve()
out = root / 'build-worker-leaf'
(out / 'tmp').mkdir(parents=True, exist_ok=True)
env = dict(os.environ, TMPDIR=str(out / 'tmp'))
includes = ['-I' + str(p) for p in (root / 'libraries').glob('*/include')]
includes += ['-I' + str(root / 'applications/cog-cli/include'), '-I' + str(root / 'libraries/cog-ipc/private')]
libraries = list(joined.glob('libraries/*/lib*.a')) + list((joined / 'applications/cog-cli').glob('lib*.a'))
cache = (joined / 'CMakeCache.txt').read_text().splitlines()
curl = next(Path(line.split('=', 1)[1]) for line in cache if line.startswith('CURL_LIBRARY:'))
sanitize = os.environ.get('COG_LEAF_SANITIZE') == '1'
for compiler in (['clang'] if sanitize else ['gcc', 'clang']):
    for name in ['worker_receipt_owned', 'delegation_callbacks', 'worker_handoff_critic']:
        suffix = compiler + ('-asan' if sanitize else '')
        binary = out / (name + '-' + suffix)
        cmd = [compiler, '-std=c11', '-Wall', '-Wextra', '-Wpedantic', '-Wconversion', '-Werror',
               '-ffunction-sections', '-fdata-sections', *includes,
               str(root / ('applications/cog-cli/tests/test_' + name + '.c')),
               '-Wl,--gc-sections', '-Wl,--start-group', *map(str, libraries), '-Wl,--end-group',
               str(curl), '-Wl,-rpath,' + str(curl.parent), '-lcrypto', '-ldl', '-lm', '-o', str(binary)]
        if sanitize:
            cmd += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
        with (out / (name + '-' + suffix + '.log')).open('w') as log:
            subprocess.run(cmd, check=True, env=env, cwd=root, stdout=log, stderr=log)
            args = [str(binary)]
            if name == 'worker_receipt_owned': args.append(str(out / ('receipt-' + compiler + '.sock')))
            subprocess.run(args, check=True, env=env, cwd=root, stdout=log, stderr=log)
        print(compiler, name, 'PASS')

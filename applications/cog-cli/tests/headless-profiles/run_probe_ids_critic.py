#!/usr/bin/env python3
"""Independent G/L grammar join and actual pre-fix negative source control."""
import argparse
import os
from pathlib import Path
import subprocess
root=Path(__file__).resolve().parents[4]
p=argparse.ArgumentParser();p.add_argument('--cc',default='gcc');p.add_argument('--build-dir',type=Path,required=True);p.add_argument('--sanitize',action='store_true');a=p.parse_args()
a.build_dir.mkdir(parents=True,exist_ok=True)
flags=['-std=c11','-O3','-DNDEBUG','-Wall','-Wextra','-Wpedantic','-Wconversion','-Wsign-conversion','-Wshadow','-Wmissing-prototypes','-Wstrict-prototypes','-Wundef','-Wpointer-arith','-Wcast-align','-Wwrite-strings','-Wformat=2','-Werror','-fno-builtin-malloc','-fno-builtin-calloc','-fno-builtin-realloc','-fno-builtin-free']
if 'clang' not in a.cc: flags+=['-Wlogical-op']
if a.sanitize:flags+=['-fsanitize=address,undefined','-fno-omit-frame-pointer']
incs=['-I'+str(root/'applications/cog-cli/src'),'-I'+str(root/'applications/cog-cli/include')]+['-I'+str(p) for p in sorted((root/'libraries').glob('*/include'))]
env=dict(os.environ,ASAN_OPTIONS='detect_leaks=1:halt_on_error=1',UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
source=root/'applications/cog-cli/src/configuration.c'
for label in ['current','before-fix']:
    selected=source
    if label=='before-fix':
        selected=a.build_dir/'configuration-before.c'
        selected.write_bytes(subprocess.check_output(['git','show','1416a5b:applications/cog-cli/src/configuration.c'],cwd=root))
    exe=a.build_dir/label
    subprocess.run([a.cc,*flags,*incs,str(root/'applications/cog-cli/tests/headless-profiles/test_probe_ids_critic.c'),str(selected),str(root/'applications/cog-cli/src/options.c'),'-Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free,--wrap=getenv,--wrap=fopen','-o',str(exe)],check=True)
    r=subprocess.run([str(exe)],env=env,capture_output=True,timeout=20)
    (a.build_dir/(label+'.stderr')).write_bytes(r.stderr)
    if label=='current' and r.returncode!=0:raise AssertionError(r.stderr.decode())
    if label=='before-fix' and (r.returncode!=1 or b'rc==' not in r.stderr):raise AssertionError('original source did not reproduce rejected invalid-ID assertion: '+r.stderr.decode())
print('independent probe grammar/transactionality PASS; actual original-source negative control rejected')

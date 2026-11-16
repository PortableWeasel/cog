#!/usr/bin/env python3
"""Independent strict native O1 probe; no build/production source mutation."""
import argparse
import os
from pathlib import Path
import subprocess

root=Path(__file__).resolve().parents[2]
p=argparse.ArgumentParser()
p.add_argument('--cc',default='gcc')
p.add_argument('--build-dir',type=Path,required=True)
p.add_argument('--sanitize',choices=['address','thread'])
a=p.parse_args()
a.build_dir.mkdir(parents=True,exist_ok=True)
flags=['-std=c11','-O3','-DNDEBUG','-Wall','-Wextra','-Wpedantic','-Wconversion','-Wsign-conversion',
       '-Wshadow','-Wmissing-prototypes','-Wstrict-prototypes','-Wundef','-Wpointer-arith',
       '-Wcast-align','-Wwrite-strings','-Wformat=2','-Werror',
       '-fno-builtin-malloc','-fno-builtin-calloc','-fno-builtin-realloc','-fno-builtin-free']
if 'clang' not in a.cc: flags+=['-Wlogical-op']
if a.sanitize: flags+=['-fsanitize='+('address,undefined' if a.sanitize=='address' else 'thread'),'-fno-omit-frame-pointer']
exe=a.build_dir/'test_ipc_o1_review'
subprocess.run([a.cc,*flags,'-I'+str(root/'libraries/cog-ipc/include'),'-I'+str(root/'libraries/cog-core/include'),
                str(root/'tests/unit/test_ipc_o1_review.c'),str(root/'libraries/cog-ipc/src/ipc.c'),
                '-Wl,--wrap=getsockopt,--wrap=recv,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free',
                '-o',str(exe)],check=True)
env=dict(os.environ,ASAN_OPTIONS='detect_leaks=1:halt_on_error=1',UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1',TSAN_OPTIONS='halt_on_error=1')
subprocess.run([str(exe)],check=True,env=env,timeout=20)
r=subprocess.run([str(exe),'--negative-control'],capture_output=True,env=env,timeout=20)
if r.returncode==0 or b'false' not in r.stderr: raise AssertionError('negative CHECK not executed')
print('independent O1 proof/retention/isolation/bounds PASS; negative CHECK rejected')

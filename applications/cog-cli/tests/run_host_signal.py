#!/usr/bin/env python3
"""Exercise the executable's signalfd ownership without global handlers."""
import os,signal,subprocess,sys
for sig in (signal.SIGINT,signal.SIGTERM):
    proc=subprocess.Popen([sys.argv[1],'--mock','--interactive'],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
    try:
        line=proc.stderr.readline()
        assert 'core.created' in line,line
        os.kill(proc.pid,sig)
        out,err=proc.communicate(timeout=5)
        assert proc.returncode==0,(sig,proc.returncode,out,err)
    finally:
        if proc.poll() is None:
            proc.kill();proc.wait()

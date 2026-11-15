#!/usr/bin/env python3
"""Real installed CLI restart/fork/recovery; synthetic offline data only."""
import os, pathlib, signal, subprocess, sys, tempfile
build, fixture = sys.argv[1:3]
with tempfile.TemporaryDirectory(prefix='cog-l-installed-') as root:
    root=pathlib.Path(root); prefix=root/'installed'; store=root/'state'
    subprocess.run(['cmake','--install',build,'--prefix',str(prefix),'--component','cog-cli-runtime'],check=True,stdout=subprocess.DEVNULL)
    cli=str(prefix/'bin'/'cog-cli')
    env={k:v for k,v in os.environ.items() if not k.startswith('COG_') and k!='OPENROUTER_API_KEY'}
    env.update(HOME=str(root),XDG_STATE_HOME=str(root/'xdg'))
    def run(*args, text=None, code=0):
        result=subprocess.run([cli,*args],env=env,input=text,text=True,capture_output=True,timeout=15)
        assert result.returncode==code,(args,result.returncode,result.stdout,result.stderr)
        return result
    common=['--mock','--store',str(store)]
    assert run(*common,'--session','story','--prompt','first','--chunks','one').stdout=='one\n'
    assert run('--sessions','--store',str(store)).stdout=='story\n'
    assert run(*common,'--resume','story','--prompt','second','--chunks','two').stdout=='two\n'
    assert run(*common,'--fork','story','--from-node','1','--prompt','branch','--chunks','three').stdout=='three\n'
    history=run(*common,'--resume','story','--interactive',text='/history\n/usage\n/quit\n').stdout
    assert '[node=1 user complete] first' in history and 'branch' in history and 'three' in history
    assert 'second' not in history and 'two' not in history and 'usage unavailable' in history
    assert run(*common,'--resume','story','--interactive','--chunks','eof',text='last line').stdout=='eof\n'
    missing=root/'does-not-exist'/'store'
    assert run('--sessions','--store',str(missing)).stdout=='' and not missing.exists()
    assert run(*common,'--resume','missing','--prompt','must not send',code=1).stdout==''
    run('--sessions','--no-store',code=2)
    subprocess.run([fixture,'seed',str(store)],env=env,check=True,timeout=10)
    assert run(*common,'--resume','uncertain','--prompt','must not send',code=1).stdout==''
    recover=['--store',str(store),'--recover','uncertain','--fenced','--interactive']
    first=run(*recover,'--tool-policy','allow',text='ordinary text must not execute\n/tools\n/reconcile 2 first acknowledged fixture fenced; effects unknown\n/quit\n',code=1)
    assert 'ordinal=1 call="first" state=indeterminate' in first.stdout and 'ordinal=2 call="second" state=indeterminate' in first.stdout
    assert 'does not stop old processes' in first.stderr
    assert run(*common,'--resume','uncertain','--prompt','still blocked',code=1).stdout==''
    run(*recover,text='/reconcile 2 second failed fixture verified no tool started\n/quit\n')
    assert run(*common,'--resume','uncertain','--prompt','after','--chunks','recovered').stdout=='recovered\n'
    audit=run(*common,'--resume','uncertain','--interactive',text='/history\n/quit\n').stdout
    assert 'effects remain unknown' in audit and 'operator-verified' in audit
    assert 'reasoning_details' not in audit
    usage=run(*common,'--resume','usage','--interactive',text='/usage\n/quit\n').stdout
    assert 'input=17 output=3' in usage and 'USD' in usage
    assert not (store/'effect').exists() and not (root/'effect').exists()
    # Broken stdout reaches owner-thread cancellation/cleanup, not SIGPIPE death.
    read_fd,write_fd=os.pipe();os.close(read_fd)
    proc=subprocess.Popen([cli,'--mock','--prompt','pipe fixture','--chunks','kept'],env=env,stdout=write_fd,stderr=subprocess.PIPE,text=True)
    os.close(write_fd);error=proc.stderr.read();status=proc.wait(timeout=10)
    assert status==1 and 'io-error' in error and 'core.destroyed' in error,(status,error)
    # Metadata RECOVER has no core: its signal still releases ownership cleanly.
    # Seed a distinct disposable store so unresolved history remains for SIGTERM.
    signal_store=root/'signal-state';subprocess.run([fixture,'seed',str(signal_store)],env=env,check=True,timeout=10)
    proc=subprocess.Popen([cli,'--store',str(signal_store),'--recover','uncertain','--fenced','--interactive'],env=env,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
    try:
        line=proc.stderr.readline();assert 'Recovery is offline' in line,line
        os.kill(proc.pid,signal.SIGTERM)
        out,error=proc.communicate(timeout=5);assert proc.returncode==1,(proc.returncode,out,error)
    finally:
        if proc.poll() is None:proc.kill();proc.wait()
    assert run('--store',str(signal_store),'--recover','uncertain','--fenced','--interactive',text='/quit\n',code=1).stdout==''

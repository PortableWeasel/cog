#!/usr/bin/env python3
"""Actual application TLS fixture. No paid requests or real credentials."""
import http.server,json,os,ssl,subprocess,sys,tempfile,threading
with tempfile.TemporaryDirectory(prefix='cog-app-tls-') as root:
    cert,key=root+'/cert.pem',root+'/key.pem'
    subprocess.run(['openssl','req','-x509','-newkey','rsa:2048','-nodes','-keyout',key,'-out',cert,'-days','1','-subj','/CN=localhost','-addext','subjectAltName=DNS:localhost'],check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
    open(root+'/example.txt','w').write('before')
    errors=[]
    class Handler(http.server.BaseHTTPRequestHandler):
        calls=0
        def do_POST(self):
            try:
                type(self).calls+=1
                r=json.loads(self.rfile.read(int(self.headers['Content-Length'])))
                assert self.path=='/v1/chat/completions'
                assert self.headers['Authorization']=='Bearer synthetic'
                assert r['max_tokens']==512 and r['stream'] and r['reasoning']=={'enabled':False}
                assert {t['function']['name'] for t in r['tools']}=={'read_file','search','edit_file','run_command'}
                opaque=[{'type':'reasoning.encrypted','data':'synthetic-opaque','id':'fixture'}]
                if self.calls==1:
                    assert len(r['messages'])==1
                    delta={'content':'editing','tool_calls':[{'index':0,'id':'call_edit','type':'function','function':{'name':'edit_file','arguments':'{"path":"example.txt","old":"before","new":"after"}'}}], 'reasoning_details':opaque}
                    finish='tool_calls'
                else:
                    assert self.calls==2 and len(r['messages'])==3
                    assert r['messages'][1]['reasoning_details']==opaque
                    assert r['messages'][1]['tool_calls'][0]['id']==r['messages'][2]['tool_call_id']=='call_edit'
                    assert 'status=ok' in r['messages'][2]['content']
                    assert open(root+'/example.txt').read()=='after'
                    delta={'content':'APP_WIRE_OK'};finish='stop'
                frame={'choices':[{'index':0,'delta':delta,'finish_reason':finish}], 'usage':{'prompt_tokens':2,'completion_tokens':3}}
                body=('data: '+json.dumps(frame)+'\r\n\r\ndata: [DONE]\r\n\r\n').encode()
                self.send_response(200);self.send_header('Content-Length',str(len(body)));self.send_header('Content-Type','text/event-stream');self.end_headers()
                for b in body:self.wfile.write(bytes([b]));self.wfile.flush()
            except BaseException as error:
                errors.append(repr(error));raise
        def log_message(self,*args):pass
    server=http.server.ThreadingHTTPServer(('127.0.0.1',0),Handler)
    tls=ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER);tls.load_cert_chain(cert,key);server.socket=tls.wrap_socket(server.socket,server_side=True)
    thread=threading.Thread(target=server.serve_forever,daemon=True);thread.start()
    try:
        env=os.environ.copy();env.update(OPENROUTER_API_KEY='synthetic',COG_NET_TEST_CA=cert,COG_APP_TEST_ROOT=root,COG_APP_TEST_STORE=root+'/state/parents',COG_APP_TEST_URL=f'https://localhost:{server.server_port}/v1')
        subprocess.run([sys.argv[1]],env=env,check=True,timeout=30)
        assert Handler.calls==2 and not errors,errors
    finally:server.shutdown();server.server_close();thread.join()

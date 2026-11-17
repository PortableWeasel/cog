#!/usr/bin/env python3
"""Real TLS provider + actual native child + CGIR durable publication oracle."""
from pathlib import Path
import http.server
import json
import os
import signal
import ssl
import subprocess
import sys
import tempfile
import threading
import traceback


def main():
    harness, worker, build = (Path(arg).resolve() for arg in sys.argv[1:4])
    scratch = build / 'tmp'
    scratch.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='native-instructions-', dir=scratch) as directory:
        root = Path(directory)
        workspace = root / 'workspace'
        (workspace / 'skills/check').mkdir(parents=True)
        (workspace / 'skills/check/data.txt').write_text('PRIVATE_RESOURCE\n')
        (workspace / 'skills/check/SKILL.md').write_text('MUTATED_MUST_NOT_APPEAR\n')
        (workspace / 'AGENTS.md').write_text('PRIVATE_SCOPE\n')
        (workspace / 'file.txt').write_text('READ_SUCCESS\n')
        cert, key = root / 'cert.pem', root / 'key.pem'
        subprocess.run(['openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes',
                        '-keyout', str(key), '-out', str(cert), '-days', '1',
                        '-subj', '/CN=localhost', '-addext', 'subjectAltName=DNS:localhost'],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        errors, requests, rounds = [], [], {}

        class Handler(http.server.BaseHTTPRequestHandler):
            def do_POST(self):
                try:
                    request = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
                    assert self.path == '/v1/chat/completions'
                    assert self.headers['Authorization'] == 'Bearer synthetic-native'
                    requests.append(request)
                    names = {tool['function']['name'] for tool in request['tools']}
                    messages = request['messages']
                    task = [m['content'] for m in messages if m['role'] == 'user'][-1]
                    system = '\n'.join(m['content'] for m in messages if m['role'] == 'system')
                    assert 'HOST_ROOT' in system
                    assert 'MUTATED_MUST_NOT_APPEAR' not in system
                    if 'delegate' in names:
                        assert all(text not in system for text in ['NAMED_CHILD_ROLE', 'PRIVATE_SKILL_BODY', 'PRIVATE_RESOURCE', 'PRIVATE_SCOPE'])
                        if messages[-1]['role'] == 'tool':
                            assert 'CHILD_DONE_local' in messages[-1]['content'], messages[-1]['content']
                            assert 'CHILD_DONE_native' in messages[-1]['content'], messages[-1]['content']
                            delta, finish = {'content': 'PARENT_DONE'}, 'stop'
                        else:
                            arguments = {'tasks': [{'task': 'child ' + kind, 'agent': 'reader'} for kind in ['local', 'native']]}
                            delta, finish = self.call('delegate', arguments, 'delegate'), 'tool_calls'
                    else:
                        assert names == {'read_file', 'activate_skill', 'read_skill_resource'}, names
                        assert 'NAMED_CHILD_ROLE' in system
                        kind = task.split()[-1]
                        number = rounds.get(kind, 0)
                        rounds[kind] = number + 1
                        if number == 0:
                            assert 'PRIVATE_SKILL_BODY' not in system
                            delta, finish = self.call('activate_skill', {'name': 'check'}, 'activate-' + kind), 'tool_calls'
                        elif number == 1:
                            assert 'PRIVATE_SKILL_BODY' in system, (kind, messages[-1])
                            assert 'status=ok' in messages[-1]['content']
                            delta, finish = self.call('read_skill_resource', {'skill': 'check', 'path': 'data.txt'}, 'resource-' + kind), 'tool_calls'
                        elif number == 2:
                            assert 'PRIVATE_RESOURCE' in system, (kind, messages[-1])
                            assert 'status=ok' in messages[-1]['content']
                            delta, finish = self.call('read_file', {'path': 'file.txt'}, 'scope-' + kind), 'tool_calls'
                        elif number == 3:
                            assert 'PRIVATE_SCOPE' in system
                            assert 'instruction context changed' in messages[-1]['content']
                            delta, finish = self.call('read_file', {'path': 'file.txt'}, 'read-' + kind), 'tool_calls'
                        elif number == 4:
                            assert 'PRIVATE_SCOPE' in system and 'PRIVATE_RESOURCE' in system
                            assert 'status=ok' in messages[-1]['content'] and 'READ_SUCCESS' in messages[-1]['content']
                            delta, finish = {'content': 'CHILD_DONE_' + kind}, 'stop'
                        else:
                            raise AssertionError(('unexpected retry', kind, number))
                    frame = {'choices': [{'index': 0, 'delta': delta, 'finish_reason': finish}]}
                    body = ('data: ' + json.dumps(frame) + '\n\ndata: [DONE]\n\n').encode()
                    self.send_response(200)
                    self.send_header('Content-Type', 'text/event-stream')
                    self.send_header('Content-Length', str(len(body)))
                    self.end_headers()
                    self.wfile.write(body)
                    self.wfile.flush()
                except BaseException as error:
                    errors.append(repr(error))
                    print('native instruction oracle:', traceback.format_exc(), flush=True)
                    self.close_connection = True

            @staticmethod
            def call(name, arguments, call_id):
                return {'tool_calls': [{'index': 0, 'id': call_id, 'type': 'function',
                                       'function': {'name': name, 'arguments': json.dumps(arguments)}}]}

            def log_message(self, *args):
                pass

        server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        tls = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        tls.load_cert_chain(cert, key)
        server.socket = tls.wrap_socket(server.socket, server_side=True)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        process = None
        try:
            env = {k: v for k, v in os.environ.items() if not k.startswith('COG_')}
            env['TMPDIR'] = str(scratch)
            process = subprocess.Popen([str(harness), str(worker), str(cert),
                f'https://localhost:{server.server_port}/v1', str(workspace), str(root / 'store'),
                str(root / 'worker.sock')], env=env, start_new_session=True)
            status = process.wait(timeout=75)
            assert status == 0, (status, errors, rounds)
            assert rounds == {'local': 5, 'native': 5} and len(requests) == 12 and not errors, (rounds, len(requests), errors)
        finally:
            if process is not None:
                if process.returncode is None:
                    assert process.pid > 0
                    try:
                        os.killpg(process.pid, signal.SIGKILL)
                    except ProcessLookupError:
                        pass
                process.wait()
            server.shutdown()
            server.server_close()
            thread.join()
        print('Native instruction requests:', len(requests), 'child rounds:', rounds)


if __name__ == '__main__':
    main()

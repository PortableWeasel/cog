#!/usr/bin/env python3
"""Actual TLS provider, native worker, workspace effects and bounded reuse oracle."""
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


def main():
    harness, executable, build = (Path(value).resolve() for value in sys.argv[1:4])
    mode = sys.argv[4] if len(sys.argv) > 4 else "success"
    scratch = build / "tmp"
    scratch.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="native-", dir=scratch) as directory:
        root = Path(directory)
        workspace = root / "workspace"
        workspace.mkdir()
        cert, key = root / "cert.pem", root / "key.pem"
        subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", str(key),
            "-out", str(cert), "-days", "1", "-subj", "/CN=localhost", "-addext", "subjectAltName=DNS:localhost"],
            check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        errors, requests = [], []
        class Handler(http.server.BaseHTTPRequestHandler):
            def do_POST(self):
                try:
                    request = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
                    assert self.path == "/v1/chat/completions"
                    assert self.headers["Authorization"] == "Bearer synthetic-native"
                    requests.append(request)
                    names = {tool["function"]["name"] for tool in request["tools"]}
                    messages = request["messages"]
                    task = [message["content"] for message in messages if message["role"] == "user"][-1]
                    if "delegate" in names:
                        round_number = int(task.split()[-1])
                        if messages[-1]["role"] == "tool":
                            print("parent TOOL result:", messages[-1]["content"], flush=True)
                            if mode == "limit":
                                assert "status=limit" in messages[-1]["content"]
                                assert "side_effects_possible=false" in messages[-1]["content"]
                                assert not list(workspace.glob("effect-*"))
                                delta, finish = {"content": "PARENT_REFUSED"}, "stop"
                            else:
                                delta, finish = {"content": f"PARENT_DONE_{round_number}"}, "stop"
                            for kind in ["local", "native"]:
                                token = f"{kind}-{round_number}"
                                if mode != "limit":
                                    assert (workspace / ("effect-" + token)).read_text() == "once"
                                    assert "CHILD_DONE_" + token in messages[-1]["content"]
                        else:
                            arguments = {"tasks": [{"task": f"child {kind} {round_number}"} for kind in ["local", "native"]]}
                            delta = {"tool_calls": [{"index": 0, "id": f"delegate-{round_number}", "type": "function",
                                "function": {"name": "delegate", "arguments": json.dumps(arguments)}}]}
                            finish = "tool_calls"
                    else:
                        assert "run_command" in names and "delegate" not in names
                        _, kind, round_number = task.split()
                        token = f"{kind}-{round_number}"
                        if messages[-1]["role"] == "tool":
                            assert "status=ok" in messages[-1]["content"]
                            assert (workspace / ("effect-" + token)).read_text() == "once"
                            delta, finish = {"content": "CHILD_DONE_" + token}, "stop"
                        else:
                            arguments = {"command": f"printf once >> effect-{token}; printf effect-complete"}
                            delta = {"tool_calls": [{"index": 0, "id": "effect-" + token, "type": "function",
                                "function": {"name": "run_command", "arguments": json.dumps(arguments)}}]}
                            finish = "tool_calls"
                    frame = {"choices": [{"index": 0, "delta": delta, "finish_reason": finish}]}
                    body = ("data: " + json.dumps(frame) + "\n\ndata: [DONE]\n\n").encode()
                    self.send_response(200)
                    self.send_header("Content-Length", str(len(body)))
                    self.send_header("Content-Type", "text/event-stream")
                    self.end_headers()
                    self.wfile.write(body)
                    self.wfile.flush()
                except BaseException as error:
                    errors.append(repr(error))
                    self.close_connection = True
            def log_message(self, *args):
                pass
        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        tls = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        tls.load_cert_chain(cert, key)
        server.socket = tls.wrap_socket(server.socket, server_side=True)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        process = None
        try:
            env = {key: value for key, value in os.environ.items() if not key.startswith("COG_")}
            env.update(COG_NATIVE_KEY="synthetic-native", TMPDIR=str(scratch))
            process = subprocess.Popen([str(harness), str(executable), str(cert), f"https://localhost:{server.server_port}/v1",
                str(workspace), str(root / "store"), str(root / "ipc"), mode], env=env, start_new_session=True)
            status = process.wait(timeout=140 if mode == "handoff" else 65)
            assert status == 0, (status, errors, len(requests))
            assert len(requests) == (2 if mode == "limit" else 6 if mode in ["single", "handoff"] else 12) and not errors, (len(requests), errors)
        finally:
            if process is not None:
                try:
                    os.killpg(process.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                process.wait()
            server.shutdown()
            server.server_close()
            thread.join()
            if errors:
                print("TLS oracle errors:", errors, file=sys.stderr)
            print("actual provider requests:", len(requests))


if __name__ == "__main__":
    main()

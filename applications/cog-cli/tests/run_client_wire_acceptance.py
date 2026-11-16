#!/usr/bin/env python3
"""Offline real app joins with packet, socket and terminal-delta handshakes."""
import http.server
import json
import os
import pathlib
import select
import socket
import ssl
import subprocess
import sys
import tempfile
import threading
import time


def run(binary, mode):
    with tempfile.TemporaryDirectory(prefix="cog-client-wire-") as temporary:
        root = pathlib.Path(temporary)
        ready = root / "ready"
        cert, key = root / "cert.pem", root / "key.pem"
        subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
                        "-keyout", str(key), "-out", str(cert), "-days", "1",
                        "-subj", "/CN=localhost", "-addext", "subjectAltName=DNS:localhost"],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        done = threading.Event()
        errors, requests = [], []
        opaque = [{"type": "reasoning.encrypted", "data": "ancestor-exact", "id": "opaque-identity"}]

        class Handler(http.server.BaseHTTPRequestHandler):
            def do_POST(self):
                try:
                    request = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
                    requests.append(request)
                    number = len(requests)
                    assert self.headers["Authorization"] == "Bearer synthetic"
                    assert self.path == "/v1/chat/completions"
                    assert request["stream"] and request["max_tokens"] == 512
                    if mode == "budget" and number == 1:
                        self.send_response(429)
                        self.send_header("Content-Length", "2")
                        self.send_header("Retry-After", "0")
                        self.end_headers()
                        self.wfile.write(b"{}")
                        return
                    if mode in ("cancel-stream", "cancel-finish", "cancel-retained"):
                        delta = {"content": "complete" if mode == "cancel-finish" else "retained" if mode == "cancel-retained" else "partial"}
                        finish = "stop" if mode == "cancel-finish" else None
                    else:
                        turn = number - (1 if mode == "budget" else 0)
                        if turn in (1, 3):
                            if turn == 3:
                                assert len(request["messages"]) == 4
                                assert request["messages"][1]["reasoning_details"] == opaque
                                assert request["messages"][1]["tool_calls"][0]["id"] == "same-call"
                                assert request["messages"][2]["tool_call_id"] == "same-call"
                                assert "status=ok" in request["messages"][2]["content"]
                                assert (root / "effect").read_text() == "once"
                                assert all(message.get("content") != "OLD_BRANCH" for message in request["messages"])
                            command = "printf once >> effect" if turn == 1 else "printf branch >> effect"
                            delta = {"tool_calls": [{"index": 0, "id": "same-call", "type": "function",
                                      "function": {"name": "run_command", "arguments": json.dumps({"command": command})}}],
                                     "reasoning_details": opaque}
                            finish = "tool_calls"
                        else:
                            assert mode == "fork" and turn in (2, 4)
                            assert request["messages"][1]["reasoning_details"] == opaque
                            assert request["messages"][-1]["tool_call_id"] == "same-call"
                            assert "status=ok" in request["messages"][-1]["content"]
                            assert (root / "effect").read_text() == ("once" if turn == 2 else "oncebranch")
                            delta = {"content": "OLD_BRANCH" if turn == 2 else "NEW_BRANCH"}
                            finish = "stop"
                    body = ("data: " + json.dumps({"choices": [{"index": 0, "delta": delta,
                             "finish_reason": finish}]}) + "\n\n").encode()
                    if finish:
                        body += b"data: [DONE]\n\n"
                    self.send_response(200)
                    self.send_header("Content-Type", "text/event-stream")
                    self.send_header("Content-Length", str(len(body) if finish else 65536))
                    self.end_headers()
                    self.wfile.write(body)
                    self.wfile.flush()
                    if not finish:
                        done.wait(10)
                except BaseException as error:
                    errors.append(repr(error))
            def log_message(self, *args):
                pass

        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        tls = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        tls.load_cert_chain(cert, key)
        server.socket = tls.wrap_socket(server.socket, server_side=True)
        server_thread = threading.Thread(target=server.serve_forever, daemon=True)
        server_thread.start()
        env = {k: v for k, v in os.environ.items()
               if not k.startswith("COG_") and k not in ("LD_PRELOAD", "LD_LIBRARY_PATH")}
        env.update(HOME=temporary, XDG_CONFIG_HOME=str(root / "config"),
                   XDG_STATE_HOME=str(root / "xdg"), OPENROUTER_API_KEY="synthetic",
                   COG_NET_TEST_CA=str(cert), COG_APP_TEST_ROOT=temporary,
                   COG_APP_TEST_STORE=str(root / "store"), COG_ACCEPT_MODE=mode,
                   COG_ACCEPT_READY=str(ready), COG_APP_TEST_URL=f"https://localhost:{server.server_port}/v1")
        fixture = filler = None
        helper = None
        if mode == "cancel-dns":
            fixture = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            fixture.bind(("127.0.0.1", 0))
            fixture.settimeout(5)
            env["COG_ACCEPT_DNS"] = f"127.0.0.1:{fixture.getsockname()[1]}"
            env["COG_APP_TEST_URL"] = "https://blackhole.cog-fixture.invalid/v1"
            def dns():
                try:
                    packet, _ = fixture.recvfrom(4096)
                    assert b"blackhole" in packet and len(packet) > 12
                    ready.touch()
                    done.wait(10)
                except BaseException as error:
                    errors.append(repr(error))
            helper = threading.Thread(target=dns)
            helper.start()
        elif mode in ("cancel-tls", "cancel-connect"):
            fixture = socket.socket()
            fixture.bind(("127.0.0.1", 0))
            fixture.listen(0 if mode == "cancel-connect" else 1)
            fixture.settimeout(5)
            env["COG_APP_TEST_URL"] = f"https://127.0.0.1:{fixture.getsockname()[1]}/v1"
            if mode == "cancel-connect":
                filler = socket.create_connection(fixture.getsockname(), timeout=2)
            else:
                def handshake():
                    try:
                        with fixture.accept()[0] as connection:
                            connection.settimeout(5)
                            hello = connection.recv(4096)
                            assert hello[0] == 22  # real TLS ClientHello record
                            ready.touch()
                            done.wait(10)
                    except BaseException as error:
                        errors.append(repr(error))
                helper = threading.Thread(target=handshake)
                helper.start()
        proc = None
        try:
            proc = subprocess.Popen([binary], env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            if mode == "cancel-connect":
                destination = f"0100007F:{fixture.getsockname()[1]:04X}"
                end = time.monotonic() + 5
                while time.monotonic() < end:
                    assert proc.poll() is None
                    records = pathlib.Path(f"/proc/{proc.pid}/net/tcp").read_text().splitlines()[1:]
                    if any(line.split()[2] == destination and line.split()[3] == "02" for line in records):
                        ready.touch()
                        break
                    select.select([], [], [], 0.005)
                assert ready.exists(), "no actual SYN_SENT connect observed"
            out, err = proc.communicate(timeout=15)
            assert proc.returncode == 0, (mode, proc.returncode, out.decode(), err.decode())
            expected = 4 if mode == "fork" else 2 if mode == "budget" else 1 if mode in ("cancel-stream", "cancel-finish", "cancel-retained") else 0
            assert len(requests) == expected and not errors, (mode, len(requests), expected, errors)
            if mode in ("cancel-dns", "cancel-tls", "cancel-connect"):
                assert ready.exists()
        finally:
            done.set()
            if proc is not None and proc.poll() is None:
                proc.kill()
                proc.wait()
            if helper is not None:
                helper.join(timeout=6)
                assert not helper.is_alive()
            if filler is not None:
                filler.close()
            if fixture is not None:
                fixture.close()
            server.shutdown()
            server.server_close()
            server_thread.join()


if __name__ == "__main__":
    run(*sys.argv[1:3])

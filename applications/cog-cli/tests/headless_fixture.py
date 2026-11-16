"""Bounded offline REAL TLS/process support for scenario-owned responders.

Only this module owns temporary paths, TLS listener and the deliberately isolated
harness process session. Responders script model replies; all host/worker/tool/
store/UI outcomes remain actual runtime observations. No external credentials.
"""
from pathlib import Path
import http.server
import json
import os
import signal
import ssl
import subprocess
import tempfile
import threading
import sys


def text(content):
    """A complete assistant SSE delta/finish pair."""
    return {"content": content}, "stop"


def tool(name, arguments, call_id):
    """One actual provider-proposed tool call, never a synthetic tool result."""
    return {"tool_calls": [{"index": 0, "id": call_id, "type": "function",
        "function": {"name": name, "arguments": json.dumps(arguments)}}]}, "tool_calls"


class TLSFixture:
    """Use ``with TLSFixture(build, responder) as fixture``.

    responder(request_dict, fixture) returns (delta, finish_reason), or None to
    close that provider connection (e.g. after a cancellation barrier). Own any
    choreography/barriers in a scenario's disjoint Python driver. ``stop`` is
    set during teardown so blocked responders can exit promptly.

    Paths: root, workspace, store, socket_prefix, cert, key, metadata. metadata
    is written by the C harness's read-only registry helper, never this module.
    requests/errors retain at most max_requests actual TLS requests/errors.
    ``run(harness, executable, extra=(mode,), timeout=65)`` passes the standard
    six fixture paths then extra args and checks actual owned process exit.
    """
    def __init__(self, build, responder, *, max_requests=256):
        if not 1 <= max_requests <= 4096:
            raise ValueError("finite max_requests required")
        self.build = Path(build).resolve()
        self.responder = responder
        self.max_requests = max_requests
        self.requests, self.errors = [], []
        self.stop = threading.Event()
        self.lock = threading.Lock()
        self.process = self.server = self.thread = self.directory = None
        self.supervisor_result = None

    def __enter__(self):
        scratch = self.build / "tmp"
        scratch.mkdir(parents=True, exist_ok=True)
        self.directory = tempfile.TemporaryDirectory(prefix="hf-", dir=scratch)
        self.root = Path(self.directory.name)
        self.workspace, self.store = self.root / "workspace", self.root / "store"
        self.workspace.mkdir()
        self.socket_prefix = self.root / "ipc"
        self.metadata = self.workspace / "registry.json"
        self.cert, self.key = self.root / "cert.pem", self.root / "key.pem"
        try:
            subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
                "-keyout", str(self.key), "-out", str(self.cert), "-days", "1",
                "-subj", "/CN=localhost", "-addext", "subjectAltName=DNS:localhost"],
                check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=15)
            fixture = self

            class Handler(http.server.BaseHTTPRequestHandler):
                def do_POST(self):
                    try:
                        length = int(self.headers["Content-Length"])
                        assert 0 < length <= 1024 * 1024, length
                        request = json.loads(self.rfile.read(length))
                        assert self.path == "/v1/chat/completions", self.path
                        assert self.headers["Authorization"] == "Bearer synthetic-native"
                        with fixture.lock:
                            assert len(fixture.requests) < fixture.max_requests, "request budget exceeded"
                            fixture.requests.append(request)
                        response = fixture.responder(request, fixture)
                        if response is None:
                            self.close_connection = True
                            return
                        delta, finish = response
                        frame = {"choices": [{"index": 0, "delta": delta, "finish_reason": finish}]}
                        body = ("data: " + json.dumps(frame) + "\n\ndata: [DONE]\n\n").encode()
                        self.send_response(200)
                        self.send_header("Content-Length", str(len(body)))
                        self.send_header("Content-Type", "text/event-stream")
                        self.end_headers()
                        self.wfile.write(body)
                        self.wfile.flush()
                    except BaseException as error:
                        with fixture.lock:
                            if len(fixture.errors) < fixture.max_requests:
                                fixture.errors.append(repr(error))
                        self.close_connection = True

                def log_message(self, *args):
                    pass

            self.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
            tls = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            tls.load_cert_chain(self.cert, self.key)
            self.server.socket = tls.wrap_socket(self.server.socket, server_side=True)
            self.url = f"https://localhost:{self.server.server_port}/v1"
            self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
            self.thread.start()
            return self
        except BaseException:
            self.__exit__(None, None, None)
            raise

    def run(self, harness, executable, *, extra=(), timeout=65):
        """Run exactly one isolated owned harness; check exit and TLS errors."""
        if self.process is not None:
            raise RuntimeError("one harness per fixture")
        env = {key: value for key, value in os.environ.items() if not key.startswith("COG_")}
        env.update(COG_NATIVE_KEY="synthetic-native", TMPDIR=str(self.build / "tmp"))
        arguments = [str(Path(harness).resolve()), str(Path(executable).resolve()), str(self.cert),
            self.url, str(self.workspace), str(self.store), str(self.socket_prefix), *map(str, extra)]
        self.supervisor_result = self.root / "supervisor.json"
        supervisor = Path(__file__).with_name("headless_supervisor.py")
        self.process = subprocess.Popen([sys.executable, str(supervisor), str(timeout),
            str(self.supervisor_result), *arguments], env=env, start_new_session=True)
        status = self.process.wait(timeout=timeout + 7)
        result = json.loads(self.supervisor_result.read_text())
        assert status == 0 and result["cleanup"] and result["error"] is None, result
        if result["timed_out"]:
            raise subprocess.TimeoutExpired(arguments, timeout)
        assert result["status"] == 0, (result, self.errors, len(self.requests))
        assert not self.errors, self.errors
        return result["status"]

    def registry(self):
        """Read one atomic C-published sample; never guess admitted identities."""
        return json.loads(self.metadata.read_text())

    def __exit__(self, exc_type, exc_value, traceback):
        self.stop.set()
        if self.process is not None:
            # Ask the still-owned supervisor to drain its actual child tree.
            # It alone adopts/signals/reaps native/tool descendants, including
            # tools that created separate sessions. Never use a remembered PGID.
            if self.process.returncode is None:
                assert self.process.pid > 0
                os.waitid(os.P_PID, self.process.pid, os.WEXITED | os.WNOHANG | os.WNOWAIT)
                self.process.send_signal(signal.SIGTERM)
                self.process.wait(timeout=7)
            if self.supervisor_result.exists():
                result = json.loads(self.supervisor_result.read_text())
                assert result["cleanup"], result
        if self.server is not None:
            self.server.shutdown()
            self.server.server_close()
        if self.thread is not None:
            self.thread.join(timeout=5)
            assert not self.thread.is_alive()
        if self.errors:
            print("TLS oracle errors:", self.errors)
        print("actual provider requests:", len(self.requests))
        if self.directory is not None:
            self.directory.cleanup()

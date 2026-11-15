#!/usr/bin/env python3
"""Offline loopback TLS fixture; all keys and outputs are temporary."""
import http.server
import os
import ssl
import subprocess
import sys
import tempfile
import threading

with tempfile.TemporaryDirectory(prefix="cog-net-test-") as d:
    cert = os.path.join(d, "cert.pem")
    key = os.path.join(d, "key.pem")
    subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
                    "-keyout", key, "-out", cert, "-days", "1", "-subj", "/CN=localhost",
                    "-addext", "subjectAltName=DNS:localhost"],
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    class Handler(http.server.BaseHTTPRequestHandler):
        requests = 0
        def do_GET(self):
            type(self).requests += 1
            self.send_response(429 if self.path == "/rate" else 200)
            if self.path == "/rate":
                self.send_header("Retry-After", "7")
            body = b"x" * 131072 if self.path == "/large" else b"hello"
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            try:
                self.wfile.write(body)
            except (BrokenPipeError, ConnectionResetError, ssl.SSLError):
                pass
        def log_message(self, *args):
            pass
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    tls = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    tls.load_cert_chain(cert, key)
    server.socket = tls.wrap_socket(server.socket, server_side=True)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        env = os.environ.copy()
        env["COG_NET_TEST_CA"] = cert
        env["COG_NET_TEST_URL"] = f"https://localhost:{server.server_port}/"
        subprocess.run([sys.argv[1]], env=env, check=True, timeout=20)
        assert Handler.requests == 5, f"unexpected replay count: {Handler.requests}"
    finally:
        server.shutdown()
        server.server_close()
        thread.join()

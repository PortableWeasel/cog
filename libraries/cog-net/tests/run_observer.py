"""Numeric observer exercise against an ephemeral offline TLS service."""
import http.server
import os
import ssl
import subprocess
import sys
import tempfile
import threading
import time

with tempfile.TemporaryDirectory(prefix="cog-net-observer-") as directory:
    cert, key = (os.path.join(directory, name) for name in ("cert.pem", "key.pem"))
    subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", key,
                    "-out", cert, "-days", "1", "-subj", "/CN=localhost",
                    "-addext", "subjectAltName=DNS:localhost"], check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    class Handler(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"
        posts = []

        def answer(self):
            body = b"q" * 65536 if self.path == "/large" else b"hello"
            self.send_response(429 if self.path == "/rate" else 200)
            self.send_header("Content-Length", str(len(body) + (5 if self.path == "/stall" else 0)))
            self.send_header("X-Secret", "synthetic-observer-secret")
            self.end_headers()
            try:
                self.wfile.write(body)
                self.wfile.flush()
                if self.path == "/stall":
                    time.sleep(0.3)
                    self.wfile.write(b"later")
            except (BrokenPipeError, ConnectionResetError, ssl.SSLError):
                pass

        def do_GET(self):
            self.answer()

        def do_POST(self):
            type(self).posts.append(self.rfile.read(int(self.headers["Content-Length"])))
            self.answer()

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
        env["COG_NET_TEST_URL"] = f"https://localhost:{server.server_port}/"
        env["COG_NET_TEST_CA"] = cert
        subprocess.run([sys.argv[1]], env=env, check=True, timeout=25)
        assert Handler.posts == [b"synthetic-observer-secret"] * 5, "POST outcome/replay changed"
    finally:
        server.shutdown()
        server.server_close()
        thread.join()

#!/usr/bin/env python3
"""Offline TLS regression oracles; all certificates and keys are temporary."""
import http.server
import os
import socket
import ssl
import subprocess
import sys
import tempfile
import threading

with tempfile.TemporaryDirectory(prefix="cog-net-regressions-") as directory:
    cert = os.path.join(directory, "cert.pem")
    key = os.path.join(directory, "key.pem")
    subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
                    "-keyout", key, "-out", cert, "-days", "1", "-subj", "/CN=localhost",
                    "-addext", "subjectAltName=DNS:localhost"], check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    class Handler(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"
        effects = []

        def do_GET(self):
            self.send_response(200)
            self.send_header("Content-Length", "5")
            self.end_headers()
            self.wfile.write(b"hello")

        def do_POST(self):
            type(self).effects.append(self.rfile.read(int(self.headers["Content-Length"])))
            if self.path in ("/auth", "/redirect", "/proxy"):
                status = {"/auth": 401, "/redirect": 307, "/proxy": 407}[self.path]
                self.send_response(status)
                if self.path == "/auth":
                    self.send_header("WWW-Authenticate", 'Digest realm="test", nonce="x"')
                elif self.path == "/proxy":
                    self.send_header("Proxy-Authenticate", 'Basic realm="test"')
                else:
                    self.send_header("Location", "/target")
                self.send_header("Content-Length", "0")
                self.end_headers()
                return
            if len(type(self).effects) == 1:
                self.close_connection = True
                self.connection.shutdown(socket.SHUT_RDWR)
                self.connection.close()
                return
            self.send_response(200)
            self.send_header("Content-Length", "0")
            self.end_headers()

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
        env["https_proxy"] = "http://127.0.0.1:1"
        env["HTTPS_PROXY"] = "http://127.0.0.1:1"
        env["NO_PROXY"] = ""
        env["no_proxy"] = ""
        env["COG_NET_TEST_CA"] = cert
        env["COG_NET_TEST_URL"] = f"https://localhost:{server.server_port}/"
        subprocess.run([sys.argv[1], sys.argv[2]], env=env, check=True, timeout=20)
        if sys.argv[2] in ("replay", "auth", "redirect", "proxy"):
            assert Handler.effects == [b"effect"], f"autonomous POST replay: {Handler.effects}"
    finally:
        server.shutdown()
        server.server_close()
        thread.join()

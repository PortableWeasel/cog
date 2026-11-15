"""Inject one driver failure after a real POST effect and accepted body bytes."""
import http.server
import os
import ssl
import subprocess
import sys
import tempfile
import threading

with tempfile.TemporaryDirectory(prefix="cog-observer-critic-") as directory:
    cert, key = (os.path.join(directory, name) for name in ("cert.pem", "key.pem"))
    subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", key,
                    "-out", cert, "-days", "1", "-subj", "/CN=localhost",
                    "-addext", "subjectAltName=DNS:localhost"], check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=10)
    finished = threading.Event()

    class Handler(http.server.BaseHTTPRequestHandler):
        posts = []
        protocol_version = "HTTP/1.1"

        def do_POST(self):
            type(self).posts.append(self.rfile.read(int(self.headers["Content-Length"])))
            self.send_response(200)
            self.send_header("Content-Length", "10")
            self.end_headers()
            self.wfile.write(b"hello")
            self.wfile.flush()
            finished.wait(10)
            self.close_connection = True

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
        subprocess.run([sys.argv[1]], env=env, check=True, timeout=15)
        assert Handler.posts == [b"critic-effect"], "driver error replayed the actual effect"
    finally:
        finished.set()
        server.shutdown()
        server.server_close()
        thread.join(5)
        assert not thread.is_alive()

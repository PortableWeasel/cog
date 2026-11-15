"""Numeric observer exercise against an ephemeral offline TLS service."""
import http.server
import os
import socket
import ssl
import subprocess
import sys
import tempfile
import threading

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
                # /stall deliberately leaves five declared bytes unsent.
                # HTTP/1.1 stays open until client cancellation, so a delayed
                # client cannot race with a later write and complete the body.
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
    # Hold an unlistened TCP port so connection refusal is entirely local.
    refused = socket.socket()
    refused.bind(("127.0.0.1", 0))
    dns = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    dns.bind(("127.0.0.1", 0))
    dns.settimeout(0.1)
    stop_dns = threading.Event()

    def serve_dns():
        while not stop_dns.is_set():
            try:
                query, peer = dns.recvfrom(4096)
            except socket.timeout:
                continue
            if b"observer-timeout" in query:
                continue
            end = 12
            while query[end]:
                end += query[end] + 1
            end += 5  # NUL label, QTYPE, QCLASS.
            reply = query[:2] + b"\x81\x83" + query[4:6] + b"\0" * 6 + query[12:end]
            dns.sendto(reply, peer)

    dns_thread = threading.Thread(target=serve_dns, daemon=True)
    dns_thread.start()
    try:
        env = os.environ.copy()
        env["COG_NET_TEST_URL"] = f"https://localhost:{server.server_port}/"
        env["COG_NET_TEST_CA"] = cert
        env["COG_NET_TEST_REFUSED"] = f"https://127.0.0.1:{refused.getsockname()[1]}/"
        env["COG_NET_TEST_IP"] = f"https://127.0.0.1:{server.server_port}/"
        env["COG_NET_TEST_DNS"] = f"127.0.0.1:{dns.getsockname()[1]}"
        subprocess.run([sys.argv[1]], env=env, check=True, timeout=25)
        assert Handler.posts == [b"synthetic-observer-secret"] * 7, "POST outcome/replay changed"
    finally:
        stop_dns.set()
        dns_thread.join()
        dns.close()
        refused.close()
        server.shutdown()
        server.server_close()
        thread.join()

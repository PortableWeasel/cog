#!/usr/bin/env python3
"""Loopback-only deterministic TLS fixtures; certificates/keys live in tempdir."""
import os
from pathlib import Path
import socket
import socketserver
import ssl
import subprocess
import sys
import tempfile
import threading
import time


def main():
    executable, openssl = sys.argv[1:]
    stop = threading.Event()
    with tempfile.TemporaryDirectory(prefix="cog-https-lifecycle-") as directory:
        root = Path(directory)
        cert, key = root / "cert.pem", root / "key.pem"
        subprocess.run([openssl, "req", "-x509", "-newkey", "rsa:2048", "-nodes",
                        "-keyout", str(key), "-out", str(cert), "-days", "2",
                        "-subj", "/CN=localhost", "-addext", "subjectAltName=DNS:localhost"],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        tls = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        tls.load_cert_chain(cert, key)

        class Server(socketserver.ThreadingTCPServer):
            daemon_threads = True
            allow_reuse_address = True

        class HTTPS(socketserver.BaseRequestHandler):
            def handle(self):
                try:
                    with tls.wrap_socket(self.request, server_side=True) as connection:
                        connection.settimeout(3)
                        request = b""
                        while b"\r\n\r\n" not in request and len(request) < 4096:
                            fragment = connection.recv(1024)
                            if not fragment:
                                return
                            request += fragment
                        path = request.split(b" ")[1]
                        body = b"data: first\n\ndata: second\n\n"
                        if path == b"/large":
                            body = b"X" * 1024
                        connection.sendall(b"HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
                                           b"Connection: close\r\nContent-Length: " +
                                           str(len(body) + (100 if path == b"/stall" else 0)).encode() + b"\r\n\r\n")
                        for fragment in (body[:3], body[3:9], body[9:]):
                            connection.sendall(fragment)
                            time.sleep(0.01)
                        if path == b"/stall":
                            stop.wait(10)
                except (OSError, ssl.SSLError, IndexError):
                    pass  # Expected verification/overflow/cancel client disconnects.

        class TLSStall(socketserver.BaseRequestHandler):
            def handle(self):
                # TCP succeeds; no TLS ServerHello is returned.
                stop.wait(10)

        https = Server(("127.0.0.1", 0), HTTPS)
        raw = Server(("127.0.0.1", 0), TLSStall)
        servers = [https, raw]
        threads = [threading.Thread(target=server.serve_forever, daemon=True) for server in servers]
        # Linux-specific true pre-TCP-connect stall: fill accept queue, never accept.
        backlog = socket.socket()
        backlog.bind(("127.0.0.1", 0))
        backlog.listen(1)
        fillers = []
        try:
            for _ in range(2):
                filler = socket.create_connection(backlog.getsockname(), timeout=0.5)
                fillers.append(filler)
            probe = socket.socket()
            probe.settimeout(0.1)
            try:
                probe.connect(backlog.getsockname())
                raise RuntimeError("cannot establish a deterministic saturated TCP fixture")
            except TimeoutError:
                pass
            finally:
                probe.close()
            for thread in threads:
                thread.start()
            environment = os.environ.copy()
            # No inherited trust overrides; untrusted case must use system roots.
            for name in ("CURL_CA_BUNDLE", "SSL_CERT_FILE", "SSL_CERT_DIR"):
                environment.pop(name, None)
            modes = ["success", "untrusted", "hostname", "cancel-connect", "cancel-tls",
                     "cancel-stall", "stall-timeout", "connect-timeout", "multi", "teardown", "overflow", "fairness"]
            for mode in modes:
                subprocess.run([executable, mode, str(cert), str(https.server_address[1]),
                                str(raw.server_address[1]), str(backlog.getsockname()[1]), "0"],
                               env=environment, check=True, timeout=5)
            # Fail each of the first 80 curl allocation attempts after host init.
            # These cover multi/easy construction, setopt, add and early perform.
            for allocation in range(1, 81):
                result = subprocess.run([executable, "oom", str(cert), str(https.server_address[1]),
                                         str(raw.server_address[1]), str(backlog.getsockname()[1]), str(allocation)],
                                        env=environment, text=True, capture_output=True, timeout=5)
                if result.returncode != 0 or "injected=1" not in result.stdout:
                    print(result.stdout, result.stderr, file=sys.stderr)
                    raise RuntimeError(f"OOM sweep failed at allocation {allocation}")
            print(f"PASS {len(modes)} transport cases + 80 allocation-failure cases", flush=True)
        finally:
            stop.set()
            for server, thread in zip(servers, threads):
                if thread.is_alive():
                    server.shutdown()
                    thread.join()
                server.server_close()
            for filler in fillers:
                filler.close()
            backlog.close()


if __name__ == "__main__":
    main()

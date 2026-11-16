"""Actual twice-relocated public host, native worker and installed CLI roles.

A finite local TLS server scripts model replies only. All process, tool, store,
provider and CLI outcomes are produced by the installed Cog artifacts. The
private application host API is deliberately outside this external consumer.
"""
from pathlib import Path
import http.server
import json
import os
import signal
import ssl
import subprocess
import threading
import time

from process_owner import run_owned


def wait_for(predicate, processes, seconds=8):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        value = predicate()
        if value:
            return value
        for process in processes:
            if process.poll() is not None:
                raise RuntimeError(f"installed role ended early: {process.returncode}")
        time.sleep(0.01)
    raise RuntimeError("installed role deadline")


def completed(path, prompt):
    if not path.exists():
        return None
    for line in reversed(path.read_text().splitlines(keepends=True)):
        if not line.endswith("\n"):
            continue
        record = json.loads(line)
        items = record.get("items", [])
        if record.get("type") == "replacement" and len(items) >= 2 and items[-2][3] == prompt and items[-1][1:3] == [2, 0]:
            return record
    return None


def cli_roles(executable, work, env):
    """Installed MOCK role protocol, independent of REAL public-app TLS proof."""
    roles = work / "roles"
    roles.mkdir()
    streams, processes = [], []
    # Relative socket names keep AF_UNIX paths bounded in deep disk worktrees.
    socket = "ui.sock"

    def start(name, arguments):
        output, diagnostic = roles / (name + ".out"), roles / (name + ".err")
        out, err = output.open("wb"), diagnostic.open("wb")
        streams.extend([out, err])
        process = subprocess.Popen([str(executable), *arguments], env=env, cwd=roles,
            stdin=subprocess.PIPE, stdout=out, stderr=err)
        processes.append(process)
        return process, output

    try:
        host, host_output = start("host", ["--headless", "--mock", "--config", str(work / "empty-config"),
            "--store", str(roles / "store"), "--socket", socket, "--children", "1", "--prompt", "initial"])
        wait_for(lambda: (roles / socket).exists(), [host])
        watch, watch_output = start("watch", ["--watch", socket])
        initial = wait_for(lambda: completed(watch_output, "initial"), [host, watch])
        controller, output = start("controller", ["--attach", socket])
        wait_for(lambda: completed(output, "initial"), [host, controller])
        controller.stdin.write(b"explicit installed turn\n"); controller.stdin.flush()
        second = wait_for(lambda: completed(output, "explicit installed turn"), [host, controller, watch])
        watched = wait_for(lambda: completed(watch_output, "explicit installed turn"), [host, controller, watch])
        assert second["session"] == watched["session"] == initial["session"]
        assert second["generation"] == watched["generation"] == initial["generation"]
        assert int(second["sequence"]) > int(initial["sequence"]) and len(second["items"]) == 4
        controller.stdin.write(b"/quit\n"); controller.stdin.flush()
        assert controller.wait(timeout=8) == 0
        for process in (watch, host):
            process.send_signal(signal.SIGTERM)
            assert process.wait(timeout=8) == 0
        assert host_output.read_bytes() == b""
        return dict(headless=True, attach=True, watch=True, explicit_turn=True, shutdown=True)
    finally:
        for process in processes:
            if process.poll() is None:
                process.kill()  # Exact still-owned unreaped Popen child only.
            process.wait(timeout=8)
            if process.stdin:
                process.stdin.close()
        for stream in streams:
            stream.close()


def public_proofs(prefix, bindir, work, env):
    certificate, key = work / "proof-ca.pem", work / "proof-key.pem"
    subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
        "-keyout", str(key), "-out", str(certificate), "-days", "1", "-subj", "/CN=localhost",
        "-addext", "subjectAltName=DNS:localhost"], check=True,
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=15)
    (work / "real-workspace").mkdir()
    requests, errors = [], []

    class Handler(http.server.BaseHTTPRequestHandler):
        def do_POST(self):
            try:
                length = int(self.headers["Content-Length"])
                assert 0 < length <= 1048576
                request = json.loads(self.rfile.read(length))
                assert self.path == "/v1/chat/completions"
                assert self.headers["Authorization"] == "Bearer synthetic-installed-proof"
                assert request["model"] == "fixture/model" and request["max_tokens"] == 128
                requests.append(request)
                assert len(requests) <= 2
                messages = request["messages"]
                if len(requests) == 1:
                    assert messages[-1]["role"] == "user" and messages[-1]["content"] == "installed effect"
                    delta = {"tool_calls": [{"index": 0, "id": "installed-effect", "type": "function",
                        "function": {"name": "run_command", "arguments": json.dumps({
                            "command": "printf once >> installed-effect; printf installed-command"})}}]}
                    finish = "tool_calls"
                else:
                    assert messages[-1]["role"] == "tool" and messages[-1]["tool_call_id"] == "installed-effect"
                    assert "status=ok" in messages[-1]["content"] and "installed-command" in messages[-1]["content"]
                    assert (work / "real-workspace/installed-effect").read_text() == "once"
                    delta, finish = {"content": "INSTALLED_REAL_DONE"}, "stop"
                body = ("data: " + json.dumps({"choices": [{"index": 0, "delta": delta,
                    "finish_reason": finish}]}) + "\n\ndata: [DONE]\n\n").encode()
                self.send_response(200); self.send_header("Content-Type", "text/event-stream")
                self.send_header("Content-Length", str(len(body))); self.end_headers()
                self.wfile.write(body); self.wfile.flush()
            except BaseException as error:
                errors.append(repr(error)); self.close_connection = True

        def log_message(self, *_args):
            pass

    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    server.daemon_threads = True
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(certificate, key)
    server.socket = context.wrap_socket(server.socket, server_side=True)
    thread = threading.Thread(target=server.serve_forever)
    thread.start()
    try:
        runtime_env = dict(env, COG_INSTALLED_PROOF_KEY="synthetic-installed-proof")
        output, ownership = run_owned([prefix / bindir / "installed_cli_only", prefix / bindir / "cog-cli",
            work, f"https://localhost:{server.server_port}/v1", certificate], runtime_env, work,
            work / "native-owner.json")
        assert not errors and len(requests) == 2, (errors, len(requests))
        assert (work / "real-workspace/installed-effect").read_text() == "once"
        for token in ("synthetic-installed-proof", "COG_INSTALLED_PROOF_KEY"):
            assert token not in output, "secret/reference reached public output"
        assert "native worker READY/reap" in output and "REAL TLS/tool/durable/core lifecycle: PASS" in output
        return dict(output=output, requests=2, once_only_effect=True, ownership=ownership,
            cli_roles=cli_roles(prefix / bindir / "cog-cli", work, env),
            remote_worker_tls="O9 in-tree composition; installed native lifecycle only")
    finally:
        server.shutdown(); server.server_close(); thread.join(timeout=5)
        assert not thread.is_alive()

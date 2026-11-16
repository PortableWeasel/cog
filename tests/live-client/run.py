#!/usr/bin/env python3
"""Explicit installed application smoke. Default is offline; --live is paid.

Only --live reads the authorized pi credential, into process memory. It is
delivered on one anonymous pipe consumed/closed by configuration construction.
No key is written to argv, environment, output, receipt or workspace.
"""
import argparse
import hashlib
import http.server
import json
import os
import pathlib
import socket
import ssl
import subprocess
import tempfile
import threading
import urllib.request

MODEL = "deepseek/deepseek-v4-flash-0731"
URL = "https://openrouter.ai/api/v1"
COMMAND = "./check-isolation.sh && cat main.c && sed -i 's/return 40;/return 42;/' main.c && make check && printf applied >> effect"
SOURCE = '#include <stdio.h>\nstatic int answer(void) { return 40; }\nint main(void) { printf("%d\\n", answer()); return 0; }\n'


def environment(root):
    # Deliberately omit inherited credentials, loader overrides and user config.
    return {"PATH": "/usr/bin:/bin", "HOME": str(root), "LC_ALL": "C.UTF-8", "TMPDIR": str(root),
            "XDG_CONFIG_HOME": str(root / "config"), "XDG_STATE_HOME": str(root / "xdg")}


def workspace(root):
    work = root / "workspace"
    work.mkdir()
    (work / "main.c").write_text(SOURCE)
    (work / "Makefile").write_text(
        "all: app\napp: main.c\n\tcc -std=c11 -Wall -Wextra -Werror main.c -o app\n"
        "check: app\n\t./app > result.txt\n\ttest \"$$(cat result.txt)\" = 42\n")
    return work


def child(binary, phase, work, store, url, key, ca, env):
    read_key, write_key = os.pipe()
    read_report, write_report = os.pipe()
    identity = os.fstat(read_key).st_ino
    if phase == "initial":
        (work / "check-isolation.sh").write_text(
            "#!/bin/sh\nset -eu\n"
            "test -z \"${OPENROUTER_API_KEY+x}\"\n"
            "test -z \"${COG_CREDENTIAL+x}\"\n"
            f"for fd in /proc/self/fd/*; do value=$(readlink \"$fd\" || true); test \"$value\" != 'pipe:[{identity}]'; done\n"
            "printf isolated > isolation.txt\n")
        (work / "check-isolation.sh").chmod(0o700)
    # Both secret and reports are bounded below pipe capacity; no writer thread
    # or temporary secret file is needed. This input is not inherited by tools.
    assert 0 < len(key) < 2048 and b"\0" not in key and b"\n" not in key
    assert os.write(write_key, key) == len(key)
    os.close(write_key)
    proc = None
    try:
        proc = subprocess.Popen([str(binary), phase, str(work), str(store), url,
                                 str(read_key), str(write_report), ca],
                                env=env, pass_fds=(read_key, write_report),
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        os.close(read_key)
        read_key = -1
        os.close(write_report)
        write_report = -1
        out, err = proc.communicate(timeout=105)
        # Never print raw application/model output. Even a failed confidentiality
        # oracle reports only a boolean; no credential-bearing excerpt escapes.
        assert key not in out and key not in err, "credential appeared in captured application output"
        report = os.read(read_report, 8192)
        assert key not in report, "credential appeared in report"
        result = json.loads(report)
        result["process_status"] = proc.returncode
        result["captured_stdout_bytes"] = len(out)
        result["captured_stderr_bytes"] = len(err)
        result["credential_in_output"] = False
        for path in work.parent.rglob("*"):
            if path.is_file() and not path.is_symlink():
                assert key not in path.read_bytes(), "credential persisted in disposable fixture"
        assert result["admitted"] <= (2 if phase == "initial" else 1)
        assert result["credential_closed"] and result["active_after"] == 0
        if phase != "guard-resume" and result["ok"]:
            assert (work / "main.c").read_text() == SOURCE.replace("return 40;", "return 42;")
            assert (work / "isolation.txt").read_text() == "isolated"
            assert (work / "effect").read_text() == "applied"
            assert (work / "result.txt").read_text() == "42\n"
            built = subprocess.run([str(work / "app")], env=env, capture_output=True, timeout=5)
            assert built.returncode == 0 and built.stdout == b"42\n"
        return result
    finally:
        if proc is not None and proc.poll() is None:
            proc.kill()
            proc.wait()
        for fd in (read_key, write_report, read_report):
            if fd >= 0:
                os.close(fd)


def installed_audit(prefix):
    expected = {"libcurl.so.4": "ae1a9e77488254d744c9064972b025d8eb7210e73ea900cd5f3632995bb29d18",
                "libcares.so.2": "b4d2377241c646dc2054e864e403f162d4b5969823bdd86ec5ff49ecf7e79f53"}
    for name, digest in expected.items():
        assert hashlib.sha256((prefix / "lib" / name).read_bytes()).hexdigest() == digest
    env = {"PATH": "/usr/bin:/bin", "LC_ALL": "C.UTF-8"}
    binary = prefix / "bin" / "installed_live_smoke"
    loaded = subprocess.check_output(["ldd", str(binary)], env=env, text=True)
    for name in expected:
        line = next(line for line in loaded.splitlines() if name in line)
        path = pathlib.Path(line.split("=>", 1)[1].split("(", 1)[0].strip()).resolve()
        assert prefix.resolve() in path.parents
    dynamic = subprocess.check_output(["readelf", "-d", str(binary)], env=env, text=True)
    assert "[$ORIGIN/../lib]" in dynamic
    subprocess.run([str(binary), "controls"], env=env, check=True, stdout=subprocess.DEVNULL)
    return binary


def offline(binary, root):
    cert, keyfile = root / "cert.pem", root / "key.pem"
    subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
                    "-keyout", str(keyfile), "-out", str(cert), "-days", "1",
                    "-subj", "/CN=localhost", "-addext", "subjectAltName=DNS:localhost"],
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    work = workspace(root)
    requests, errors = [], []
    class Handler(http.server.BaseHTTPRequestHandler):
        def do_POST(self):
            try:
                request = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
                requests.append(request)
                assert self.path == "/v1/chat/completions"
                assert self.headers["Authorization"] == "Bearer synthetic-offline"
                assert request["model"] == MODEL and request["max_tokens"] == 512 and request["stream"]
                assert request["reasoning"] == {"enabled": False}
                number = len(requests)
                if number == 1:
                    assert len(request["messages"]) == 1
                    delta = {"tool_calls": [{"index": 0, "id": "build-one", "type": "function",
                             "function": {"name": "run_command", "arguments": json.dumps({"command": COMMAND})}}]}
                    finish = "tool_calls"
                elif number in (2, 3):
                    assert len(request["messages"]) == (3 if number == 2 else 5)
                    assert request["messages"][1]["tool_calls"][0]["id"] == request["messages"][2]["tool_call_id"] == "build-one"
                    assert "status=ok" in request["messages"][2]["content"]
                    if number == 3:
                        assert "tools" not in request
                    delta, finish = {"content": "done"}, "stop"
                else:
                    assert number == 4 and "tools" not in request
                    delta = {"tool_calls": [{"index": 0, "id": "unsolicited", "type": "function",
                             "function": {"name": "run_command", "arguments": '{"command":"printf BAD >> effect"}'}}]}
                    finish = "tool_calls"
                body = ("data: " + json.dumps({"choices": [{"index": 0, "delta": delta, "finish_reason": finish}],
                                             "usage": {"prompt_tokens": 2, "completion_tokens": 3}})
                        + "\n\ndata: [DONE]\n\n").encode()
                self.send_response(200)
                self.send_header("Content-Type", "text/event-stream")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
            except BaseException as error:
                errors.append(type(error).__name__)
        def log_message(self, *args):
            pass
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    tls = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    tls.load_cert_chain(cert, keyfile)
    server.socket = tls.wrap_socket(server.socket, server_side=True)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        url = f"https://localhost:{server.server_port}/v1"
        phases = [child(binary, phase, work, root / "store", url, b"synthetic-offline", str(cert), environment(root))
                  for phase in ("initial", "resume", "guard-resume")]
        assert all(result["ok"] and result["process_status"] == 0 for result in phases), phases
        assert len(requests) == 4 and not errors, (len(requests), errors)
        assert phases[0]["admitted"] == 2 and phases[1]["admitted"] == 1
        assert phases[2]["admitted"] == 1 and phases[2]["refused"] == 1
        assert (work / "effect").read_text() == "applied"
        return {"mode": "offline", "ok": True, "http_requests": 4, "phases": phases}
    finally:
        server.shutdown()
        server.server_close()
        thread.join()


def live(binary, root):
    # Public catalog GET is not a generation request and uses no credential.
    with urllib.request.urlopen("https://openrouter.ai/api/v1/models", timeout=20) as response:
        catalog = json.load(response)
    selected = [entry for entry in catalog["data"] if entry["id"] == MODEL]
    assert len(selected) == 1 and "tools" in selected[0]["supported_parameters"]
    auth_path = pathlib.Path.home() / ".pi" / "agent" / "auth.json"
    with auth_path.open() as source:
        auth = json.load(source)
    key = auth["openrouter"]["key"].encode()
    del auth
    work = workspace(root)
    phases = []
    for phase in ("initial", "resume"):
        result = child(binary, phase, work, root / "store", URL, key, "system", environment(root))
        phases.append(result)
        assert sum(result["admitted"] for result in phases) <= 3
        if not result["ok"] or result["process_status"] != 0:
            break
    return {"mode": "live", "model": MODEL, "endpoint": URL,
            "request_limit": 3, "max_output_tokens_per_request": 512,
            "generation_requests_admitted": sum(result["admitted"] for result in phases),
            "ok": len(phases) == 2 and all(result["ok"] and result["process_status"] == 0 for result in phases),
            "phases": phases}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--prefix", type=pathlib.Path, required=True)
    parser.add_argument("--live", action="store_true", help="explicitly authorize this invocation to send up to three paid requests")
    parser.add_argument("--receipt", type=pathlib.Path)
    arguments = parser.parse_args()
    binary = installed_audit(arguments.prefix.resolve())
    scratch = pathlib.Path(__file__).resolve().parents[2] / "build-live-smoke"
    scratch.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="cog-live-smoke-", dir=scratch) as temporary:
        result = (live if arguments.live else offline)(binary, pathlib.Path(temporary))
    encoded = json.dumps(result, indent=2, sort_keys=True) + "\n"
    if arguments.receipt:
        arguments.receipt.write_text(encoded)
    print(encoded, end="")
    raise SystemExit(0 if result["ok"] else 1)

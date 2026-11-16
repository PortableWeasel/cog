#!/usr/bin/env python3
"""Finite synthetic provider scripts; C owns all actual agent/effect oracles."""
from pathlib import Path
import json
import os
import subprocess
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "applications/cog-cli/tests"))
from headless_fixture import TLSFixture, tool, text


def wait_for(f, predicate, seconds=10):
    end = time.monotonic() + seconds
    while not f.stop.is_set() and time.monotonic() < end:
        if predicate():
            return True
        f.stop.wait(0.005)
    assert f.stop.is_set(), "provider choreography deadline"
    return False


def main():
    harness, executable, build, mode = sys.argv[1:5]
    fanout = mode.startswith("fanout")
    crash = mode.startswith("crash")
    messaging = mode == "messaging"
    roles = ["local", "native-a", "native-b"] if fanout else ["local", "native"]
    final_results, chat_ids = {}, {}
    child_calls = {}

    def members_from(content):
        return json.loads(content.split("\n", 1)[1])["members"]

    def observed_ids(f, round_number):
        def ready():
            try:
                data = f.registry()["members"]
            except (FileNotFoundError, json.JSONDecodeError):
                return False
            if len(data) != 2 or any(m["status"] != 1 for m in data):
                return False
            current = {"local" if m["peer"] == 0 else "native": str(m["agent"]) for m in data}
            if round_number == 2 and set(current.values()) & set(chat_ids[1].values()):
                return False
            chat_ids[round_number] = current
            return True
        assert wait_for(f, ready)
        assert len(set(chat_ids[round_number].values())) == 2
        return chat_ids[round_number]

    def chat(request, f, role, round_number):
        ids = observed_ids(f, round_number) if round_number not in chat_ids else chat_ids[round_number]
        messages = request["messages"]
        results = [m for m in messages if m["role"] == "tool"]
        peer = "native" if role == "local" else "local"
        token = f"{role}-{round_number}"
        if role == "local":
            if not results:
                # Nine actual tool calls fill the eight-entry mailbox. The C
                # observer releases the receiver only after the same started
                # ninth handle has remained pending across multiple slices.
                calls = []
                for index in range(1, 10):
                    item = tool("send_message", {"agent_id": ids[peer],
                        "message": f'whole {round_number}/{index}: "quoted"\\tail\nline'}, f"send-{index}")[0]["tool_calls"][0]
                    item["index"] = index - 1
                    calls.append(item)
                return {"tool_calls": calls}, "tool_calls"
            if len(results) == 9:
                assert all("status=ok;" in m["content"] for m in results)
                assert all(json.loads(m["content"].split("\n", 1)[1]) == {"sent": True} for m in results)
                (f.workspace / f"sent-nine-{round_number}").write_text("actual sends acknowledged")
                assert wait_for(f, lambda: (f.workspace / f"ack-sent-{round_number}").exists())
                return tool("receive_messages", {}, "local-receive")
            if len(results) == 10:
                payload = json.loads(results[-1]["content"].split("\n", 1)[1])
                assert payload == {"messages": [{"agent_id": ids[peer], "message": f"reply {round_number}: native -> local"}], "more_pending": False}, payload
                return tool("receive_messages", {}, "local-empty")
            if len(results) == 11:
                assert json.loads(results[-1]["content"].split("\n", 1)[1]) == {"messages": [], "more_pending": False}
                if round_number == 2:
                    return tool("send_message", {"agent_id": chat_ids[1][peer], "message": "must not reach retired group"}, "stale-destination")
            elif len(results) == 12:
                assert round_number == 2 and "status=not-found;" in results[-1]["content"], results[-1]
            else:
                raise AssertionError((role, len(results)))
        else:
            if not results:
                assert wait_for(f, lambda: (f.workspace / f"pressure-{round_number}").exists())
                return tool("receive_messages", {}, "native-receive-eight")
            if len(results) in [1, 2]:
                payload = json.loads(results[-1]["content"].split("\n", 1)[1])
                expected = range(1, 9) if len(results) == 1 else range(9, 10)
                assert payload == {"messages": [{"agent_id": ids[peer],
                    "message": f'whole {round_number}/{index}: "quoted"\\tail\nline'} for index in expected], "more_pending": False}, payload
                if len(results) == 1:
                    assert wait_for(f, lambda: (f.workspace / f"sent-nine-{round_number}").exists())
                    return tool("receive_messages", {}, "native-receive-nine")
                return tool("send_message", {"agent_id": ids[peer], "message": f"reply {round_number}: native -> local"}, "native-reply")
            if len(results) == 3:
                assert "status=ok;" in results[-1]["content"]
                assert json.loads(results[-1]["content"].split("\n", 1)[1]) == {"sent": True}
                (f.workspace / f"ack-sent-{round_number}").write_text("actual reply acknowledged")
                return tool("receive_messages", {}, "native-empty")
            assert len(results) == 4, len(results)
            assert json.loads(results[-1]["content"].split("\n", 1)[1]) == {"messages": [], "more_pending": False}
        final_results[token] = True
        return text("CHILD_DONE_" + token)

    def responder(request, f):
        names = {item["function"]["name"] for item in request["tools"]}
        messages = request["messages"]
        task = [m["content"] for m in messages if m["role"] == "user"][-1]
        if "delegate" in names:
            round_number = int(task.split()[-1])
            if messages[-1]["role"] != "tool":
                return tool("delegate", {"tasks": [{"task": f"child {role} {round_number}"} for role in roles]}, f"delegate-{round_number}")
            content = messages[-1]["content"]
            if mode == "fanout-limit":
                assert "status=limit;" in content and "side_effects_possible=false;" in content
                assert not list(f.workspace.glob("effect-*"))
                return text("PARENT_REFUSED")
            assert not crash, "crashed attempt must not silently resume provider"
            members = members_from(content)
            assert len(members) == len(roles)
            assert len({m["agent_id"] for m in members}) == len(roles)
            assert len({m["session"] for m in members}) == len(roles)
            for role, member in zip(roles, members):
                token = f"{role}-{round_number}"
                cancelled = mode == "fanout-cancel" or (mode == "cancel" and role == "native")
                assert member["status"] == (4 if cancelled else 2), member
                assert not member["side_effects_possible"], member
                if cancelled:
                    assert not (f.workspace / ("effect-" + token)).exists()
                else:
                    assert member["result"] == "CHILD_DONE_" + token, member
                    if messaging:
                        assert final_results[token]
                    else:
                        assert (f.workspace / ("effect-" + token)).read_text() == "once"
            return text(f"PARENT_DONE_{round_number}")
        assert "delegate" not in names and {"send_message", "receive_messages"} <= names
        _, role, round_number = task.split()
        round_number = int(round_number)
        token = f"{role}-{round_number}"
        with f.lock:
            child_calls[token] = child_calls.get(token, 0) + 1
        if messaging:
            assert "run_command" not in names
            return chat(request, f, role, round_number)
        assert "run_command" in names
        if mode == "fanout-cancel" or (mode == "cancel" and role == "native") or (mode == "crash-before" and role == "native"):
            (f.workspace / (f"entered-{token}" if fanout else "native-entered")).write_text("actual native provider request")
            f.stop.wait(30)
            return None
        if messages[-1]["role"] == "tool":
            assert "status=ok;" in messages[-1]["content"]
            assert (f.workspace / ("effect-" + token)).read_text() == "once"
            if fanout and role != "native-b":
                predecessor = "native-b" if role == "native-a" else "native-a"
                assert wait_for(f, lambda: (f.workspace / f"terminal-{predecessor}-{round_number}").exists())
            return text("CHILD_DONE_" + token)
        command = f"printf once >> effect-{token}; printf effect-complete"
        if mode == "crash-after" and role == "native":
            command = f"printf once >> effect-{token}; printf '%s %s' $$ $PPID > native-entered; sleep 2; printf ended > native-command-ended"
        return tool("run_command", {"command": command}, "effect-" + token)

    with TLSFixture(build, responder, max_requests=64) as fixture:
        fixture.run(harness, executable, extra=(mode,), timeout=65)
        expected = {"success": 12, "cancel": 5, "crash-before": 4, "crash-after": 4,
            "fanout": 16, "fanout-cancel": 5, "fanout-limit": 2, "messaging": 23}[mode]
        assert len(fixture.requests) == expected, (mode, len(fixture.requests), child_calls)
        if crash:
            assert child_calls["native-1"] == 1, child_calls
            assert (fixture.workspace / "effect-native-1").exists() == (mode == "crash-after")
            if mode == "crash-after":
                assert (fixture.workspace / "effect-native-1").read_text() == "once"
                command_pids = (fixture.workspace / "native-entered").read_text().split()
                assert len(command_pids) == 2 and all(int(pid) > 0 for pid in command_pids)
            # Normal REAL resume must refuse the actual uncertain checkpoint
            # before any provider/tool continuation; no fencing assertion or
            # recovery override is supplied to turn this into fresh work.
            session = fixture.registry()["root_session"]
            count = len(fixture.requests)
            env = {key: value for key, value in os.environ.items() if not key.startswith("COG_")}
            env.update(COG_NATIVE_KEY="synthetic-native", CURL_CA_BUNDLE=str(fixture.cert),
                TMPDIR=str(Path(build).resolve() / "tmp"))
            resumed = subprocess.run([str(Path(executable).resolve()), "--backend", "real", "--model", "fixture/model",
                "--credential-ref", "env:COG_NATIVE_KEY", "--base-url", fixture.url, "--workspace", str(fixture.workspace),
                "--store", str(fixture.store), "--resume", session, "--prompt", "must not replay"],
                env=env, capture_output=True, text=True, timeout=10)
            assert resumed.returncode == 1 and "indeterminate" in resumed.stderr, (resumed.returncode, resumed.stdout, resumed.stderr)
            assert len(fixture.requests) == count and not fixture.errors, "unsafe resume sent a request"
        print("finite actual TLS scenario:", mode, "PASS")


if __name__ == "__main__":
    main()

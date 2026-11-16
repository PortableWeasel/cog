#!/usr/bin/env python3
"""Offline actual provider/effect/history oracles for canonical scenarios 11/17."""
from pathlib import Path
import json
import sys
import time
from headless_fixture import TLSFixture, tool, text


def main():
    harness, executable, build = map(Path, sys.argv[1:4])
    mode = sys.argv[4]
    assert mode in {"reconnect", "migration", "migration-timeout"}

    def responder(request, fixture):
        messages = request["messages"]
        users = [m["content"] for m in messages if m["role"] == "user"]
        task = users[-1]
        if mode == "reconnect":
            assert users == ["stream 1", "stream 2", "stream 3", "stream 4", "policy"][:len(users)], users
            assert len(users) <= 5
            if task.startswith("stream"):
                if task == "stream 1":
                    until = time.monotonic() + 5
                    while not (fixture.workspace / "offline").exists():
                        assert time.monotonic() < until and not fixture.stop.wait(0.01), "offline barrier"
                return text("STREAM_DONE_" + task.split()[-1])
            assert task == "policy"
            if messages[-1]["role"] == "tool":
                assert "status=ok" in messages[-1]["content"], messages[-1]
                assert (fixture.workspace / "effect-policy").read_text() == "once"
                return text("POLICY_DONE")
            return tool("run_command", {"command": "printf once >> effect-policy; printf policy-effect"}, "policy-effect")
        number = int(task.removeprefix("turn "))
        assert task == f"turn {number}" and 0 <= number <= (0 if mode.endswith("timeout") else 4)
        assert users == [f"turn {i}" for i in range(number + 1)], users
        old_tools = [m for m in messages if m["role"] == "tool"]
        at_tool = messages[-1]["role"] == "tool"
        assert len(old_tools) == number + int(at_tool), (number, len(old_tools))
        calls = [c for m in messages for c in m.get("tool_calls", [])]
        assert [c["id"] for c in calls] == [f"move-effect-{i}" for i in range(number + int(at_tool))], calls
        for i in range(number + int(at_tool)):
            assert (fixture.workspace / f"effect-{i}").read_text() == "once", i
            assert "status=ok" in old_tools[i]["content"], old_tools[i]
        # Every old assistant completion is still in the exact target history;
        # a move never replays the old user prompt or regenerates its tool call.
        completed = [m["content"] for m in messages if m["role"] == "assistant" and m.get("content", "").startswith("TURN_DONE_")]
        assert completed == [f"TURN_DONE_{i}" for i in range(number)], completed
        if at_tool:
            return text(f"TURN_DONE_{number}")
        return tool("run_command", {"command": f"printf once >> effect-{number}; printf effect-{number}"}, f"move-effect-{number}")

    with TLSFixture(build, responder, max_requests=32) as fixture:
        fixture.run(harness, executable, extra=(mode,), timeout=100 if mode == "migration" else 60)
        expected = 6 if mode == "reconnect" else 2 if mode.endswith("timeout") else 10
        assert len(fixture.requests) == expected, len(fixture.requests)
        if mode == "reconnect":
            assert (fixture.workspace / "effect-policy").read_text() == "once"
            records = [json.loads(line) for line in (fixture.workspace / "controller.ndjson").read_text().splitlines()]
            replacements = [r for r in records if r["type"] == "replacement"]
            assert len({r["session"] for r in replacements}) == 1
            assert len({r["generation"] for r in replacements}) == 1
            sequences = [int(r["sequence"]) for r in replacements]
            assert sequences == sorted(sequences)
            assert any(r["policies"] for r in replacements)
            assert any(any(row[3] == "POLICY_DONE" for row in r["items"]) and not r["policies"] for r in replacements)
            results = [r for r in records if r["type"] == "command-result"]
            assert [r["input"] for r in results] == [str(i) for i in range(1, 7)], results
            assert all(r["result"] == 0 for r in results)
            watched = [json.loads(line) for line in (fixture.workspace / "watch.ndjson").read_text().splitlines()]
            assert len([r for r in watched if r["type"] == "command-result" and r["result"] != 0]) == 2
        else:
            for i in range(1 if mode.endswith("timeout") else 5):
                assert (fixture.workspace / f"effect-{i}").read_text() == "once"


if __name__ == "__main__":
    main()

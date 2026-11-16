#!/usr/bin/env python3
"""Exercise reusable TLS support with the unchanged O4 single-mode oracle."""
from pathlib import Path
import json
import sys
from headless_fixture import TLSFixture, text, tool


def respond(request, fixture):
    names = {entry["function"]["name"] for entry in request["tools"]}
    messages = request["messages"]
    task = [entry["content"] for entry in messages if entry["role"] == "user"][-1]
    if "delegate" in names:
        assert task == "delegate round 1", task
        if messages[-1]["role"] == "tool":
            for kind in ("local", "native"):
                assert (fixture.workspace / f"effect-{kind}-1").read_text() == "once"
                assert "CHILD_DONE_" + kind + "-1" in messages[-1]["content"]
            members = json.loads(messages[-1]["content"].split("\n", 1)[1])["members"]
            assert len(members) == 2 and all(member["status"] == 2 for member in members)
            return text("PARENT_DONE_1")
        return tool("delegate", {"tasks": [{"task": f"child {kind} 1"}
            for kind in ("local", "native")]}, "delegate-1")
    assert "run_command" in names and "delegate" not in names
    _, kind, round_number = task.split()
    token = f"{kind}-{round_number}"
    if messages[-1]["role"] == "tool":
        assert "status=ok" in messages[-1]["content"]
        assert (fixture.workspace / ("effect-" + token)).read_text() == "once"
        return text("CHILD_DONE_" + token)
    return tool("run_command", {"command": f"printf once >> effect-{token}; printf effect-complete"},
        "effect-" + token)


if __name__ == "__main__":
    harness, executable, build = map(Path, sys.argv[1:4])
    with TLSFixture(build, respond, max_requests=6) as fixture:
        fixture.run(harness, executable, extra=("single",))
        assert len(fixture.requests) == 6
        metadata = fixture.registry()
        assert metadata["root_session"]
        assert metadata["members"] and all(member["agent"] > 0 and member["session"]
            for member in metadata["members"])
        assert any(member["peer"] > 0 for member in metadata["members"])

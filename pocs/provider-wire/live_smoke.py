#!/usr/bin/env python3
"""Explicit two-request synthetic smoke; credential remains in this process only.
No retry, no automatic model substitution, no real tool execution. Offline CTest
never imports/runs this file. Refuse to overwrite evidence without manual action.
"""
import argparse
import datetime
import json
from pathlib import Path
import sys
import urllib.error
import urllib.request

BASE = "https://openrouter.ai/api/v1"
MODEL = "deepseek/deepseek-v4-flash-0731"
ROOT = Path(__file__).resolve().parent


def credential():
    with (Path.home() / ".pi/agent/auth.json").open() as f:
        entry = json.load(f).get("openrouter", {})
    for field in ("key", "apiKey", "api_key", "access"):
        key = entry.get(field)
        if isinstance(key, str) and key:
            return key
    raise RuntimeError("OpenRouter credential unavailable")


def decode_sse(raw):
    # Live capture helper only. Production candidate framing is the C PoC.
    text = raw.decode("utf-8").replace("\r\n", "\n").replace("\r", "\n")
    for frame in text.split("\n\n"):
        data = [line[5:].removeprefix(" ") for line in frame.split("\n") if line.startswith("data:")]
        if data:
            value = "\n".join(data)
            yield value if value == "[DONE]" else json.loads(value)


def sanitized(raw, key):
    if key.encode() in raw:
        raise RuntimeError("Response contained credential; refused persistence")
    frames = []
    for item in decode_sse(raw):
        if item == "[DONE]":
            frames.append("data: [DONE]\n\n")
            continue
        if "error" in item:
            raise RuntimeError("Remote stream error; body not persisted")
        # Strip transient generation/provider identifiers, retain wire semantics.
        if "id" in item:
            item["id"] = "gen-synthetic-redacted"
        if "created" in item:
            item["created"] = 0
        item.pop("provider", None)
        item.pop("system_fingerprint", None)
        frames.append("data: " + json.dumps(item, ensure_ascii=False, separators=(",", ":")) + "\n\n")
    return "".join(frames)


def post(key, payload):
    body = json.dumps(payload, ensure_ascii=False).encode()
    request = urllib.request.Request(BASE + "/chat/completions", data=body,
        headers={"Authorization": "Bearer " + key, "Content-Type": "application/json"}, method="POST")
    try:
        with urllib.request.urlopen(request, timeout=120) as response:
            status = response.status
            raw = response.read(1024 * 1024 + 1)
            content_type = response.headers.get("Content-Type", "")
    except urllib.error.HTTPError as exc:
        raise RuntimeError("HTTP status " + str(exc.code) + "; body not persisted") from None
    if len(raw) > 1024 * 1024 or "text/event-stream" not in content_type:
        raise RuntimeError("Unexpected response size/content type; body not persisted")
    return status, raw


def facts(raw):
    result = {"finish_reason": None, "usage": None, "done": False, "tools": {}, "text": ""}
    for item in decode_sse(raw):
        if item == "[DONE]":
            result["done"] = True
            continue
        if item.get("usage"):
            result["usage"] = item["usage"]
        for choice in item.get("choices", []):
            result["finish_reason"] = choice.get("finish_reason") or result["finish_reason"]
            delta = choice.get("delta", {})
            result["text"] += delta.get("content") or ""
            for t in delta.get("tool_calls") or []:
                slot = result["tools"].setdefault(t["index"], {"id": "", "name": "", "arguments": ""})
                slot["id"] += t.get("id") or ""
                slot["name"] += (t.get("function") or {}).get("name") or ""
                slot["arguments"] += (t.get("function") or {}).get("arguments") or ""
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--live", action="store_true", required=True)
    parser.parse_args()
    summary_path = ROOT / "fixtures/live-summary.json"
    if summary_path.exists():
        raise RuntimeError("Evidence already exists; refusing additional live requests")
    now = datetime.datetime.now(datetime.timezone.utc).isoformat()
    with urllib.request.urlopen(BASE + "/models", timeout=30) as response:
        catalog = json.load(response)
    model = next((m for m in catalog["data"] if m["id"] == MODEL), None)
    if not model or not all(p in model.get("supported_parameters", []) for p in ("tools", "tool_choice")):
        raise RuntimeError("Configured catalog model/tool capabilities unavailable; no substitution")
    (ROOT / "fixtures/catalog.json").write_text(json.dumps({"observed_utc": now,
        "source": BASE + "/models", "model": {k: model[k] for k in
        ("id", "name", "context_length", "supported_parameters", "pricing")}}, indent=2) + "\n")
    key = credential()
    tools = [{"type": "function", "function": {"name": "echo", "description": "Return synthetic test input unchanged; no side effects.",
        "parameters": {"type": "object", "properties": {"value": {"type": "string"}}, "required": ["value"], "additionalProperties": False}}}]
    messages = [{"role": "user", "content": 'Call echo once with value equal to café "wire" followed by a newline and 😀. Do not explain.'}]
    common = {"model": MODEL, "stream": True, "max_tokens": 512, "reasoning": {"enabled": False},
              "provider": {"require_parameters": True}, "stream_options": {"include_usage": True}}
    summary = {"observed_utc": now, "model": MODEL, "max_requests": 3, "output_token_limit_each": 512,
               "requests": [], "retries": 0, "tool_execution": "synthetic echo in memory only"}
    try:
        for turn in ("tool", "answer"):
            payload = dict(common, messages=messages)
            if turn == "tool":
                payload.update(tools=tools, tool_choice={"type": "function", "function": {"name": "echo"}})
            # Count attempted requests before I/O; there is no retry path.
            record = {"turn": turn, "attempted": True}
            summary["requests"].append(record)
            summary_path.write_text(json.dumps(summary, indent=2) + "\n")
            status, raw = post(key, payload)
            record["http_status"] = status
            safe = sanitized(raw, key)
            f = facts(safe.encode())
            record.update({k: f[k] for k in ("finish_reason", "usage", "done")})
            if not f["done"] or not f["usage"]:
                raise RuntimeError("Incomplete stream/usage")
            (ROOT / ("fixtures/live-" + turn + ".sse")).write_text(safe)
            (ROOT / ("fixtures/request-" + turn + ".json")).write_text(json.dumps(payload, ensure_ascii=False, indent=2) + "\n")
            if turn == "tool":
                if f["finish_reason"] != "tool_calls" or set(f["tools"]) != {0}:
                    raise RuntimeError("Expected exactly one synthetic tool call")
                t = f["tools"][0]
                args = json.loads(t["arguments"])
                if t["name"] != "echo" or args != {"value": 'café "wire"\n😀'}:
                    raise RuntimeError("Tool arguments did not match synthetic input")
                messages += [{"role": "assistant", "content": f["text"] or None,
                    "tool_calls": [{"id": t["id"], "type": "function", "function": {"name": t["name"], "arguments": t["arguments"]}}]},
                    {"role": "tool", "tool_call_id": t["id"], "content": json.dumps(args, ensure_ascii=False)},
                    {"role": "user", "content": "Reply with exactly WIRE_OK."}]
            elif f["finish_reason"] != "stop" or f["text"].strip() != "WIRE_OK":
                raise RuntimeError("Expected synthetic final answer")
        summary["success"] = True
    finally:
        summary_path.write_text(json.dumps(summary, indent=2) + "\n")
    print("Live synthetic smoke PASS: two requests; fixtures and usage saved without credentials")


if __name__ == "__main__":
    try:
        main()
    except Exception as exc:
        # Do not print arbitrary HTTP response bodies, requests, or exceptions.
        print("Live synthetic smoke failed (" + type(exc).__name__ + "); inspect sanitized summary", file=sys.stderr)
        sys.exit(1)

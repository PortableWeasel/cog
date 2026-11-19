#!/usr/bin/env python3
"""Prove tool-call correlation and idle dispatcher supervision on Linux."""

import argparse
import asyncio
from dataclasses import dataclass
import json
import os
from pathlib import Path
import re
import secrets
import signal
import subprocess
import sys
import tempfile
import urllib.error
import urllib.request

ENDPOINT = "https://openrouter.ai/api/v1/chat/completions"
DEFAULT_MODEL = "deepseek/deepseek-v4-flash"
WORKER = "import secrets, sys; sys.stdin.buffer.read(); print(secrets.token_hex(8))"
MAX_JOBS = 4


def require(condition, message):
    if not condition:
        raise ValueError(message)


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        require(key not in result, "duplicate JSON argument")
        result[key] = value
    return result


@dataclass(frozen=True)
class ToolTemplate:
    name: str
    description: str
    argument: str
    pattern: str

    def declaration(self):
        return {"type": "function", "function": {
            "name": self.name,
            "description": self.description,
            "parameters": {
                "type": "object",
                "properties": {self.argument: {"type": "string", "pattern": self.pattern}},
                "required": [self.argument],
                "additionalProperties": False,
            },
        }}

    def validate(self, raw):
        require(isinstance(raw, str) and len(raw) <= 512, "invalid argument payload")
        args = json.loads(raw, object_pairs_hook=unique_object)
        require(isinstance(args, dict) and args.keys() == {self.argument},
                "unexpected argument fields")
        value = args[self.argument]
        require(isinstance(value, str) and re.fullmatch(self.pattern, value),
                "argument failed template validation")
        return value


START = ToolTemplate(
    "start_background",
    "Launch a synthetic background job and immediately return its job_id and running state.",
    "label", r"^[a-z][a-z0-9_-]{0,31}$",
)
STATUS = ToolTemplate(
    "background_status",
    "Read the current state of an existing job by its job_id without waiting. "
    "A completed job includes its result.",
    "job_id", r"^job-[0-9a-f]{12}$",
)


class Job:
    def __init__(self, job_id, label):
        self.job_id = job_id
        self.label = label
        self.done = asyncio.get_running_loop().create_future()
        self.output = tempfile.TemporaryFile()
        self.outcome = {"job_id": job_id, "state": "running"}
        self.pidfd = None
        try:
            self.process = subprocess.Popen(
                [sys.executable, "-I", "-S", "-c", WORKER],
                stdin=subprocess.PIPE, stdout=self.output,
                stderr=subprocess.DEVNULL, env={},
            )
        except BaseException:
            self.output.close()
            raise

        try:
            self.pidfd = os.pidfd_open(self.process.pid)
            asyncio.get_running_loop().add_reader(self.pidfd, self._reap)
        except BaseException:
            # The direct child is still ours and has never been reaped.
            self.process.kill()
            self.process.wait()
            self.process.stdin.close()
            self.output.close()
            if self.pidfd is not None:
                os.close(self.pidfd)
            raise

    def release(self):
        self.process.stdin.close()

    def _reap(self):
        asyncio.get_running_loop().remove_reader(self.pidfd)
        try:
            code = self.process.wait()  # pidfd readiness means exit already happened.
            outcome = {"job_id": self.job_id, "exit_code": code}
            if code == 0:
                self.output.seek(0)
                result = self.output.read(64).decode("ascii").strip()
                require(re.fullmatch("[0-9a-f]{16}", result), "bad worker result")
                outcome.update(state="completed", result=result)
            else:
                outcome.update(state="failed")
            self.outcome = outcome
            print(f"reap  {self.job_id} pid={self.process.pid} exit={code}", flush=True)
        except (OSError, ValueError) as error:
            self.done.set_exception(error)
        else:
            self.done.set_result(None)
        finally:
            os.close(self.pidfd)
            self.pidfd = None
            self.process.stdin.close()
            self.output.close()

    async def close(self):
        if not self.done.done():
            try:
                signal.pidfd_send_signal(self.pidfd, signal.SIGKILL)
            except ProcessLookupError:
                pass  # Exited already. The reader still owns the reap.
        await asyncio.shield(self.done)


class Dispatcher:
    def __init__(self, complete):
        self.complete = complete
        self.jobs = {}
        self.history = [{"role": "system", "content":
                         "Use the provided tools when asked to start or inspect a job. "
                         "When asked for an exact reply, output only the requested value."}]
        self.calls = set()
        self.bindings = {START.name: (START, self.start),
                         STATUS.name: (STATUS, self.status)}
        self.busy = False

    def start(self, label):
        require(len(self.jobs) < MAX_JOBS, "job limit reached")
        job_id = "job-" + secrets.token_hex(6)
        require(job_id not in self.jobs, "duplicate job identity")
        job = Job(job_id, label)
        self.jobs[job_id] = job
        print(f"spawn {job_id} label={label} pid={job.process.pid} pidfd={job.pidfd}", flush=True)
        return job.outcome

    def status(self, job_id):
        return self.jobs[job_id].outcome if job_id in self.jobs else {
            "job_id": job_id, "state": "unknown",
        }

    def dispatch(self, call):
        require(call.get("type") == "function", "invalid call type")
        identity = call.get("id")
        require(isinstance(identity, str) and identity and identity not in self.calls,
                "missing or duplicate provider call ID")
        function = call["function"]
        binding = self.bindings.get(function["name"])
        require(binding is not None, "unadvertised tool")
        template, handler = binding
        value = template.validate(function["arguments"])
        result = handler(value)
        self.calls.add(identity)
        return {"role": "tool", "tool_call_id": identity, "content": json.dumps(result)}

    async def prompt(self, text):
        require(not self.busy, "a prompt is already in flight")
        self.busy = True
        try:
            messages = self.history + [{"role": "user", "content": text}]
            tools = [template.declaration() for template, _ in self.bindings.values()]
            choice = await self.complete(messages, tools, "auto")
            message = choice["message"]
            calls = message.get("tool_calls") or []
            tool_name = None
            if choice["finish_reason"] == "tool_calls":
                require(len(calls) == 1, "expected one complete tool call")
                call = calls[0]
                tool_name = call["function"]["name"]
                assistant = {key: message[key] for key in
                             ("content", "tool_calls", "reasoning_details") if key in message}
                result_message = self.dispatch(call)
                messages += [dict(assistant, role="assistant"), result_message]
                # Publish the complete call/result pair before provider continuation.
                self.history = messages
                choice = await self.complete(messages, tools, "none")
                message = choice["message"]
            reply = message.get("content")
            require(choice["finish_reason"] == "stop" and
                    not message.get("tool_calls") and isinstance(reply, str),
                    "expected a complete text reply")
            self.history = messages + [{"role": "assistant", "content": reply}]
            return reply.strip(), tool_name
        finally:
            self.busy = False

    async def close(self):
        await asyncio.gather(*(job.close() for job in self.jobs.values()))


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, request, fp, code, msg, headers, newurl):
        return None


class OpenRouter:
    def __init__(self, model):
        self.model = model
        self.key = os.environ.get("OPENROUTER_API_KEY")
        if not self.key:
            config = Path.home() / ".pi/agent/auth.json"
            if config.is_file():
                entry = json.loads(config.read_text()).get("openrouter", {})
                self.key = entry.get("key") or entry.get("apiKey") or entry.get("api_key")
        require(isinstance(self.key, str) and self.key, "OpenRouter API key unavailable")
        self.requests = 0

    def post(self, messages, tools, tool_choice):
        payload = {
            "model": self.model, "messages": messages, "tools": tools,
            "tool_choice": tool_choice, "max_tokens": 256,
            "temperature": 0, "reasoning": {"enabled": False},
            "provider": {"require_parameters": True, "allow_fallbacks": False},
        }
        request = urllib.request.Request(
            ENDPOINT, data=json.dumps(payload).encode(),
            headers={"Authorization": "Bearer " + self.key, "Content-Type": "application/json"},
        )
        try:
            with urllib.request.build_opener(NoRedirect()).open(request, timeout=30) as response:
                raw = response.read(65537)
        except urllib.error.HTTPError as error:
            raise ValueError(f"OpenRouter returned HTTP {error.code}") from None
        except (urllib.error.URLError, TimeoutError):
            raise ValueError("OpenRouter request failed or timed out") from None
        require(len(raw) <= 65536, "provider response exceeds limit")
        body = json.loads(raw)
        choices = body.get("choices", [])
        require(len(choices) == 1, "provider returned no usable choice")
        return choices[0]

    async def __call__(self, messages, tools, tool_choice):
        self.requests += 1
        print(f"request {self.requests} model={self.model} tool_choice={tool_choice}", flush=True)
        return await asyncio.to_thread(self.post, messages, tools, tool_choice)


def child_is_live(job):
    if job.pidfd is None or job.done.done():
        return False
    return os.waitid(os.P_PIDFD, job.pidfd, os.WEXITED | os.WNOHANG | os.WNOWAIT) is None


async def prove(complete):
    dispatcher = Dispatcher(complete)
    try:
        job_id, tool = await dispatcher.prompt(
            "Start a new synthetic background job with label 'proof'. "
            "After the tool returns, reply with exactly its job_id.")
        require(tool == START.name and job_id in dispatcher.jobs and not dispatcher.busy,
                "expected an idle dispatcher and a launch receipt")
        job = dispatcher.jobs[job_id]
        require(job.label == "proof" and child_is_live(job), "expected a live child with label proof")

        print("idle  dispatcher accepted an unrelated prompt with child still running", flush=True)
        answer, tool = await dispatcher.prompt(
            "Unrelated question. What is 19 + 23? Reply with just the number.")
        require(answer == "42" and tool is None and not dispatcher.busy and child_is_live(job),
                "unrelated prompt must finish while the child is still running")
        print("reply unrelated prompt answered 42 without a tool", flush=True)

        state, tool = await dispatcher.prompt(
            "Inspect the background job from the first turn. Use its job_id. "
            "After the tool returns, reply with exactly its state.")
        require(tool == STATUS.name and state == "running" and child_is_live(job),
                "status prompt must finish while the child is still running")
        print("reply status prompt confirmed running", flush=True)

        job.release()
        await asyncio.wait_for(asyncio.shield(job.done), timeout=5)
        require(job.pidfd is None and job.process.returncode == 0, "expected a reaped child")
        result, tool = await dispatcher.prompt(
            "Inspect the same job again. After the tool returns, "
            "reply with exactly the result field.")
        require(tool == STATUS.name and result == job.outcome["result"],
                "reply did not reproduce the worker result")
        print(f"PASS four prompts, final worker result {result}", flush=True)
    finally:
        await dispatcher.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--live", action="store_true", required=True)
    parser.add_argument("--model", default=DEFAULT_MODEL)
    args = parser.parse_args()
    try:
        require(hasattr(os, "pidfd_open") and hasattr(os, "P_PIDFD") and
                hasattr(signal, "pidfd_send_signal"),
                "Python and Linux with pidfd support are required")
        asyncio.run(prove(OpenRouter(args.model)))
    except (OSError, ValueError, KeyError, TypeError, AttributeError, TimeoutError,
            asyncio.TimeoutError) as error:
        print(f"FAIL {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())

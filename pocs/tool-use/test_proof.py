"""Offline regression checks for the experiment, with real child processes."""

import asyncio
import json
import os
import signal
import unittest

from proof import Dispatcher, START, STATUS, prove


def tool_call(call_id, name, arguments):
    return {"id": call_id, "type": "function",
            "function": {"name": name, "arguments": arguments}}


class ScriptedProvider:
    def __init__(self, check):
        self.check = check
        self.receipts = []
        self.requests = 0

    async def __call__(self, history, tools, choice):
        self.requests += 1
        self.check.assertEqual(tools, [START.declaration(), STATUS.declaration()])
        json.dumps(history)  # Check the wire values remain serializable.
        if choice == "auto":
            if self.requests == 3:
                self.check.assertIn("19 + 23", history[-1]["content"])
                return {"finish_reason": "stop", "message": {"content": "42"}}
            if not self.receipts:
                name, args = START.name, '{"label":"proof"}'
            else:
                name = STATUS.name
                args = json.dumps({"job_id": self.receipts[0]["job_id"]})
            call = tool_call(f"call-{self.requests}", name, args)
            return {"finish_reason": "tool_calls", "message": {"tool_calls": [call]}}

        self.check.assertEqual(choice, "none")
        call = history[-2]["tool_calls"][0]
        self.check.assertEqual(history[-1]["role"], "tool")
        self.check.assertEqual(history[-1]["tool_call_id"], call["id"])
        receipt = json.loads(history[-1]["content"])
        self.check.assertNotIn("pid", receipt)
        self.check.assertNotIn("pidfd", receipt)
        self.receipts.append(receipt)
        answer = (receipt["job_id"], receipt["state"], receipt.get("result"))[len(self.receipts) - 1]
        return {"finish_reason": "stop", "message": {"content": answer}}


class ProofTest(unittest.IsolatedAsyncioTestCase):
    async def test_round_trips(self):
        provider = ScriptedProvider(self)
        await prove(provider)
        self.assertEqual(provider.requests, 7)
        self.assertEqual([r["state"] for r in provider.receipts],
                         ["running", "running", "completed"])

    async def test_schema_failure_precedes_launch(self):
        dispatcher = Dispatcher(None)
        bad_arguments = (
            '{"label":"ok","command":"other"}', '{"label":"a","label":"b"}',
            '{"label":"UPPERCASE"}', '{"label":"test\\n"}', '{"label":7}', '[]', '{',
        )
        for raw in bad_arguments:
            with self.subTest(raw=raw), self.assertRaises(ValueError):
                dispatcher.dispatch(tool_call("id", START.name, raw))
        with self.assertRaises(ValueError):
            dispatcher.dispatch(tool_call("id", "unknown", "{}"))
        self.assertFalse(dispatcher.jobs)
        self.assertFalse(dispatcher.calls)

    async def test_cancel_and_reap_two_live_children(self):
        dispatcher = Dispatcher(None)
        watched = []
        try:
            for index in range(2):
                call = tool_call(f"call-{index}", START.name, '{"label":"proof"}')
                receipt = json.loads(dispatcher.dispatch(call)["content"])
                job = dispatcher.jobs[receipt["job_id"]]
                watched.append((job, os.dup(job.pidfd)))
                with self.assertRaises(ValueError):
                    dispatcher.dispatch(call)
            self.assertEqual(len(dispatcher.jobs), 2)
            self.assertNotEqual(watched[0][0].process.pid, watched[1][0].process.pid)
            for _, fd in watched:
                self.assertIsNone(os.waitid(os.P_PIDFD, fd, os.WEXITED | os.WNOHANG | os.WNOWAIT))
            await asyncio.wait_for(dispatcher.close(), timeout=5)
            await dispatcher.close()
            for job, fd in watched:
                self.assertEqual(job.outcome["exit_code"], -signal.SIGKILL)
                self.assertEqual(job.outcome["state"], "failed")
                self.assertIsNone(job.pidfd)
                with self.assertRaises(ChildProcessError):
                    os.waitid(os.P_PIDFD, fd, os.WEXITED | os.WNOHANG)
        finally:
            for _, fd in watched:
                os.close(fd)
            await dispatcher.close()


    async def test_cancelled_close_waiter_leaves_reap_active(self):
        dispatcher = Dispatcher(None)
        receipt = dispatcher.start("proof")
        job = dispatcher.jobs[receipt["job_id"]]
        close_task = asyncio.create_task(job.close())
        try:
            await asyncio.sleep(0)
            close_task.cancel()
            with self.assertRaises(asyncio.CancelledError):
                await close_task
            await asyncio.wait_for(dispatcher.close(), timeout=5)
            self.assertEqual(job.process.returncode, -signal.SIGKILL)
            self.assertIsNone(job.pidfd)
        finally:
            await dispatcher.close()


if __name__ == "__main__":
    unittest.main()

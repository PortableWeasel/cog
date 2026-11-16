# Native headless integration fixture

`run_headless_native.py` supplies a loopback TLS provider, a temporary certificate,
synthetic credential, durable store and workspace. `test_headless_native` drives
actual coordinator/core/executor APIs and launches the built `cog-cli --worker`
through the real orchestrator. Both child placements use real provider adapters,
net transport, tool execution and persistence. No live provider or paid call is
used. All fixture/build outputs are bounded under the selected build directory.

The provider emits a two-member delegate call. One child is local and one runs in
the authenticated native process. Each successful child runs an actual workspace
command once; the server checks the file effect before emitting its final text.
The parent receives factual member results, saves its TOOL result, then receives
its final provider response. The harness requires group retirement and checked
runtime shutdown. The maintained modes share this path:

- `cli-headless-native-single`: one group, six actual TLS requests, two exact-once
  command effects and factual completion, followed by cleanup and shutdown.
- `cli-headless-native`: two groups across two parent turns; twelve requests,
  four distinct command effects and actual physical slot reuse.
- `cli-headless-native-limit`: two-child output bound 4096 deliberately fails
  PREPARE. The JSON worst-case bound leaves 160 bytes/member, below the admitted
  minimum256. Require explicit LIMIT, no child execution/effects and clean close.
  Successful cases use8192, allowing501 bytes/member under the same bound.
- `cli-headless-native-cancel`: wait until the native child's actual TLS request
  enters the server, then admit cancellation twice through the real control core.
  Require factual CANCELLED with no native command effect, local sibling success,
  parent saved result and complete group cleanup. The server releases its held
  response on teardown; it does not supply a fabricated child result.
- `cli-headless-native-handoff`: after a completed group, request four alternating
  native/local moves. Verify each actual store owner epoch/instance and primary
  placement after phase completion; repeated physical slots must accept each new
  executor's mutation serials while retaining stream receipt watermarks.

A registered test is an oracle, not a passing claim. Exact heads and per-profile
results live in ticket50fad6e and its bug tickets. Native reuse/handoff/cancel
failures must be resolved and independently reviewed before whole-O4 acceptance.
Scoped private leaf tests cannot replace these native joins. Compiler logs and
runtime failure logs remain in checkout-local `build-*` directories.

The `handoff-timeout` fault mode retains the default three-second move budget.
After actual source release and owner CAS, it pauses the exact still-owned worker
before target guard acquisition. The helper rejects PID<=0, reaped/lost ownership,
and a child which `waitid(WNOWAIT)` reports exited; it never consumes the
orchestrator's wait status or signals a default PID/process group. Expected timeout
requires frozen admission, source authority still released, no target guard/route/
activation, and the actual stored next owner. Cleanup resumes that exact child or
uses the orchestrator's managed-ID kill, drives bounded reap, and requires consumed
runtime close before declaring the expected timeout a passing oracle.

The Python wrapper deliberately starts the harness in its own session. Its final
process-group fallback is used only while the leader is still an owned unreaped
child with a verified positive PID. A completed `Popen.wait()` disables that
fallback, so a reaped leader's numeric PGID is never signalled later. Normal and
expected-failure paths use the actual runtime/orchestrator cleanup above.

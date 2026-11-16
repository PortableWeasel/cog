# O2 private IPC readiness prerequisite

Ticket5fcaef7, dependent sourcee55f22c. O1 is complete at O base0aa7654.
This private IPC-only amendment is separate from core/agent body implementation;
no public header change, endpoint allocation, mutation or replay-engine change.

## Pass1 — exact read-only boundary

cog_ipc_internal_ready reports whether ONE endpoint can advance without waiting.
The caller combines all owned peers, queues and deadlines. Listener readiness
means pending acceptance only; the helper never walks or services accepted peers.
For established streams: retained complete RX or retained terminal failure is
immediately actionable; otherwise poll read readiness only when input reservation
allows a new frame, plus write readiness only when copied TX is queued. An idle
writable socket alone is false, avoiding an unbounded busy-spin in core step.

For handshake: native pending connect waits for writable/error; connected client
awaiting its kernel snapshot has a local capture unit ready; identity receiver
waits readable/error; identity sender waits writable/error. This is readiness,
not authority: it never captures credentials or advances identity bytes. Core
still captures proof immediately after the one-unit handshake publication and
BEFORE another service unit regardless of event/arena queue pressure.

Only poll(timeout0) may touch the kernel. Valid query returns OK and writes a
boolean. Null, unbound and permanent poll failure preserve caller output;
transient poll failure returns AGAIN unchanged, allowing a later bounded retry.
No wait, endpoint mutation, allocation, callback or process-global state.

## Pass2 — bounded-unit and pressure traces

Trace all20 at the affected IPC boundary:01/03/04/09/10 need writable only for
accepted copied TX or pending identity;02/06/12 preserve finite retry/deadline
ownership outside this helper;05/07/16/20 report terminal/retained work even after
physical EOF;08/11/17 and R13/R14 keep proof/lease identities separate from
readiness;13/14/15/18/19 add no persistence/effect/observer behavior here.

A frame's last payload read and its publication are separate existing units.
Amendment: if rx_length_known and rx_partial equals the declared slot length,
publication is immediately ready without another kernel read. This includes a
zero-length frame. A parsed prefix with prefix_used nonzero similarly owns a
local bookkeeping unit before its payload read. Retained RX takes priority over
poll, including a full queue: the caller may consume its head without another
read, while core event/arena saturation is handled by the core scheduler.

Exact truth table before any poll:

| Endpoint condition | ready/result |
|---|---|
| null endpoint/output | NULL_PTR, output unchanged |
| fd absent with no terminal failure | INVALID_STATE, output unchanged |
| retained terminal failure or complete RX | true/OK, no syscall |
| established parsed prefix bookkeeping or completed payload awaiting publication | true/OK, no syscall |
| pending connect | poll OUT + implicit error/hangup |
| native connect complete, capture not attempted | true/OK, no syscall |
| client identity receive | poll IN + implicit error/hangup |
| accepted identity transmit | poll OUT + implicit error/hangup |
| established incomplete RX | poll IN + implicit error/hangup |
| established with queued TX | additionally poll OUT |
| idle established, no queued TX/no inbound data | false/OK even if fd writable |
| listener | poll IN + implicit error/hangup, no child traversal |

Multiple interests are queried once at timeout0. Poll error/hangup/invalid-fd
readiness means a later ordinary service unit can discover the existing terminal
result; the query itself does not fail or close the endpoint for revents bits.
Only a failed poll syscall produces IO or transient AGAIN. Output commits only
after successful validation/query, including a ready=false result.

## Pass3 — authority, nonmutation and boundedness audit

Rechecked the truth table against ipc_handshake/read_unit/write_unit and the
single-unit service dispatch. Queued ready work may require alternating past an
idle direction, but remains bounded actionable work and must not authorize sleep.
No attempt to publish, receive, send, capture proof or advance a clock is hidden
inside this query. It reports potential progress, never promises a syscall's
later success in the presence of external peer changes. A readiness race falls
back to the existing nonblocking AGAIN result without changed authority.

The core separately applies endpoint deadlines and its constructor finite idle
ceiling. Lack of a due timer never authorizes unbounded sleep. A due timer is not
an IPC readiness query side effect. Listener IDs retain their existing scope of
one listen-to-disconnect fabric, not a stronger new-to-free object identity.
Neither this helper nor rebegin may overwrite an already admitted event proof.

Planned focused tests: idle writable socket false; queued copied TX true; drained
TX false; actual inbound prefix true; partial prefix/payload with no more bytes
false; parsed-prefix and payload-publication local units true; retained/full RX
and terminal EOF true; connecting/client/accepted fragmented handshake interests;
listener pending acceptance vs idle listener; exact output preservation for null,
unbound, injected poll IO/EINTR; endpoint byte/queue/identity state unchanged after
every query; unrelated accepted peer does not make an idle listener ready.
A loopback socket may change kernel readiness concurrently, so tests assert local
nonmutation and concrete queued/readable facts, not frozen external fd state.

Verification: private core_ipc.h compiles standalone/repeated include under full
strict GCC/Clang C11 and G++/Clang++ C++11 (4/4). No body has changed. Separate
IPC helper implementation/runtime approval and O integration are required before
dependent core bodies, in addition to the core/agent private-layout gate.

## Writer implementation evidence

Private header gate passed88946e8. Implementation checkpointf79ea01, final test
snapshot correction1debd18 (memcpy preserves structure padding for the exact
nonmutation oracle). This adds only the helper body to ipc.c, its dedicated
unit fixture and additive unit registration; no core/agent/app/orchestrator body
or public API changed. No endpoint layout crosses the IPC/core boundary.

The helper performs at most one timeout0 poll and no allocation, queue advance,
credential query, clock sampling or mutation. Native pending-connect readiness
uses actual socket writability; a deterministic test-only connect wrapper reports
EINPROGRESS after a successful real Unix connect so the existing SO_ERROR and
native-capture stages can be exercised. A send wrapper fragments the actual
identity into two-byte writes. These are explicit fault/scheduling controls, not
a claim that AF_UNIX naturally selected EINPROGRESS on this run.

Actual service inspection corrected a test assumption: the current read unit
retires parsed-prefix bookkeeping in that same prefix-read unit and publishes
zero-length frames immediately. The fixture checks these real transitions,
partial prefixes/payloads with no new bytes, and the genuinely separate nonempty
payload-complete → publication unit. The helper also safely recognizes the
private buffered-prefix condition described by the header; no production frame
engine behavior was changed to fabricate such a state. Socket EOF and retained
terminal state remain readiness only; the existing service reports the error.

Exact focused selection in strict GCC14.2 Release, strict Clang19.1.7 Release,
and separate Clang ASan+UBSan/LSan: **8/8 each** — unit-ipc, unit-ipc-critic,
unit-ipc-nonblocking, unit-ipc-replay, unit-ipc-replay-critic,
unit-ipc-authenticated, unit-ipc-o1-review and unit-ipc-readiness. Separate GCC
Debug TSan: **6/6**, the same set excluding only the two replay-only cases.
Existing independent-endpoint threaded fixtures run unchanged. All production
and test targets use project strict warnings/Werror; Release assertions retain
-UNDEBUG. Sanitizers use detect_leaks=1/halt_on_error=1 and UBSan stack traces;
TSan uses halt_on_error=1 in its separate build.

Builds/logs: `/tmp/cog-ready-{gcc,clang,asan,tsan}` and matching -config.log,
-build.log, -tests.log. Accepted CURL_LIBRARY and COG_NET_CURL_LIBRARY both point
to the frozen shipped curl-build/install/lib/libcurl.so, COG_NET_CURL_BUILD_DIR
points to the true shipped curl-build, with its installed include and admitted
COG_TEST_CARES_BUILD_DIR. No dependency runtime bytes were modified or installed.
The first GCC invocation omitted building the already registered O1 critic test;
its NOT RUN was corrected by building the existing target, not excluding it.

Four independent temporary O3 mutations were rejected by actual assertions:
idle write interest without queued TX, missing local frame publication readiness,
output mutation on poll failure, and endpoint direction mutation during query.
Sources/binaries/diagnostics remain `/tmp/cog-ready-negatives`; production source
was never overwritten. Every successful query snapshots endpoint/queue bytes;
error tests prove caller outputs and endpoint bytes stay unchanged for IO/EINTR/
EAGAIN. These are scoped IPC checks, not completed cooperative core/O acceptance.

| Final receipt | SHA256 |
|---|---|
| cog-ready-gcc-tests.log | 153ee6410ef1d8acc00e2327a8f517c737aa15b3293ff19dd69cc6b7d3172fb1 |
| cog-ready-clang-tests.log | a526aac2fc1efe4dad2cd67c4bc11f94f3f968bba9a8897e55f1ddec2c956694 |
| cog-ready-asan-tests.log | 8ef7a68d5b0e42f01a79163bc9ea01b360e6656c8e32359e7acd9a7dcf38d8e9 |
| cog-ready-tsan-tests.log | 6616eec5e99cb581f098877d8e770cf7b3efefd1c97fef6e63706db32a7bda4d |

Independent source/runtime review and O-branch integration remain required
before dependent core bodies. Main and the accepted frozen profile are untouched;
no paid requests or gate closure occurred.

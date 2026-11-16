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

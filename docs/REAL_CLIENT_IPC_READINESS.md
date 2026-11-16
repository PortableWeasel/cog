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

# Canonical projection and policy resync (N pass1)

`src/projection.h` owns the ONE coordinator CGR1 ring. An executor's standalone
display_sequence is input provenance, not a wire replay cursor. Executing apps
retain actual accepted text/history/incomplete aggregates through K/L; the
projection never manufactures conversation from child status, task text or a
route. Only coordinator-committed copied CGD1 data becomes visible wire state.

## Mutation barrier and exact append

Headless execution uses the existing static state machine with a private
PUBLICATION state and one-dispatch core slices. Save/stage user input before
provider send; retain accepted TEXT; save terminal assistant/tool result before
next tool/model. Each visible change marks one immutable pending view, yields
before the next ordinary effect, and disables direct app fwrite. Between slices
execution_copy_view copies the complete actual app snapshot. Local capture calls
projection_commit directly. Remote primary encodes EXECUTOR_VIEW with its exact
current owner tuple and nonreused mutation serial, retaining bytes until exact
VIEW_COMMIT. Same serial/different bytes is PROTOCOL; exact retry returns the same
committed cursor. A stale source owner after handoff cannot publish a new view.

projection_commit validates all source data and prepares complete CGV1 bytes in
constructor scratch under its known next sequence, then existing replay_append
copies ONCE and assigns exactly that sequence. Pointer/anchor publication after
append is infallible. Any encode/capacity/sequence/append failure leaves prior
ring/view unchanged, retains pending app data, freezes ordinary admission and
reports explicit failure. It never reports successful partial snapshot. Existing
cancel/provider/tool drain and absolute deadlines continue while publication is
blocked; lost UI/projection feed cannot strand teardown. Final accepted data
stays in actual durable/incomplete app state even if no wire view could commit.

No client output socket backpressures the canonical ring. Each bounded client
owns copied send/resync frames. A slow client gets explicit eviction GAP and a
full committed snapshot at one anchor, then requests DATA after it; another GAP
repeats resync. Accepted input/task/tool effects are never invoked for display.
An app result being durable, a child semantic update accepted, a command copied,
a canonical view committed and a client render acknowledged are distinct facts.

## CGV1 literal replacement body

Every integer is big-endian; strings use u32 length then non-NUL UTF-8 bytes.
Magic CGV1(4), nested-CGD1 length u32 and exact CGD1 bytes, active configuration
u64, published configuration u64, agent count u16, agent rows, policy count u16,
policy rows. No padding/native pointers/trailing bytes. CGD1 has the EXACT outer
CGR1 generation/sequence and configured root session ID. Both configuration IDs
are nonzero, published>=active; no view claims future publication was activated.

Agent row order is ascending admitted agent ID, no duplicates: agent ID u64,
attempt u64, group u64, pinned config generation u64, session string, status u8,
outcome signed i32, status text string, result text string. IDs/attempt/config
nonzero; status/outcome obey existing CGA1 predicates. Nonterminal result empty;
terminal result is the copied actual accepted notification, not invented history.
Core notification copies/outbox lifetime precede local ACK; UI ring success is
not required to dispose a failed feed during drain, but copied result ownership
is retained until parent result persistence or consuming teardown with failure.

Policy rows are sorted by(session UTF-8 bytes,assistant node,call ordinal), no
duplicates: session string, assistant node u64, zero-based immutable call ordinal
u16, prompt nonce u64, pinned config generation u64, absolute deadline u64, static
tool name string. Node/nonce/config/deadline nonzero. Session names root or one
currently admitted child row. Ordinal resolves exact original tool-call order,
including literal IDs containing whitespace/newline/@-looking bytes; it never
selects a filtered pending list or changes a full original ID. Tool name is the
known descriptor name, no arguments/credentials/opaque values. Nonce is fresh
monotonic coordinator identity, checked exhaustion before prompt/STARTED/effects.

Constructor proves worst-case root display+bounded agent status/results+all
root/child policy rows fit ONE projection/frame and all snapshot/send arenas.
No truncation of text/status/results to make it fit. CGV1 decoder validates all
lengths/scalars/ordering/identity/UTF-8/anchors BEFORE writes; small caller arenas
return only required counts/bytes and preserve every other byte/metadata.

## Approval and incomplete-input races

An executor emits ASK through a retained private prompt state instead of reading
stdin. No prompt means no ALLOW. Missing controller does not auto-approve: wait
under the original finite turn/tool deadline or deny/cancel. After projection
commit, POLICY includes exact observed canonical anchor/session/node/immutable
ordinal/nonreused nonce and decision. Coordinator checks current controller and
still-current prompt, then owner rechecks actual PROPOSED call and guard before
ALLOW. Exact prompt retransmission is not a persistent grant. Parent approval of
delegate never changes child global policy. Child ASK is independent and visible
after UI reconnect/GAP. Stale/prequeued/cross-session/cross-attempt ALLOW fails
without STARTED/effect; no policy callback authority crosses a worker boundary.

Ordinary SUBMIT queued during active work retains continuation_authorized=false.
If that work fails/incomplete, it stays queued and cannot fork/switch/send under
an old receipt. Only fresh CONTINUE matching CURRENT incomplete leaf and view can
authorize K's explicit branch decision. Accepted CONTINUE dedup remains stable
through disconnect/lost receipt, never forks twice. Handoff rejects pending input,
policy, partial view/result, child intent or future reload/notice; it cannot hide
an unresolved execution by serializing an older replay-safe ancestor.

## Client rendering/retirement

Install CGV1 conversation+controls together after complete decode. Reject another
session or regressing same-generation cursor. Render TTY as escaped bounded full
replacement; nonTTY explicitly NDJSON replacement records. Cursor moves only
after checked complete output/flush. Output saturation/failure is a client driver
failure, not a successful render/approval/command receipt. The server stays alive
on client detach. Same live client retains exact command IDs across reconnect;
server restart is a new runtime/replay incarnation and makes old command outcome
uncertain rather than authorizing automatic resend. New client first resyncs and
requires fresh explicit input. No unbounded local history or leaked socket frames.

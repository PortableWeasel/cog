# O8 projection/client implementation layout

Ticketd592ad6 under9d57bf5. BaseO25a5936 with approved N, O1/O5/O6 and O4 layout.
This declares private storage before projection.c/host_client.c bodies; no public
API, library/replay rewrite, process/task execution or shared app/CMake edit.

## Pass1 — fixed owners and minimum interfaces

`view_internal.h` owns the two opaque concrete layouts. Projection has its own
existing CGR1 ring and two independent complete encoded+decoded banks. Client has
one explicit reusable Q endpoint, separate receive/envelope/replay/command arenas,
two full decode/view banks, one full render arena and fixed input buffering. Each
bank owns rows/text/wire; no app snapshot, IPC decode or prior-bank pointer survives
commit. FILEs/input/signal fd are declared caller borrows. Each named new/free is
the only allocation boundary; reconnect reuses the endpoint and all arenas.

Projection constructor checks declared worst CGV1 bytes: max_display_bytes +452
(one policy with127-byte session/255-byte name plus outer/config/count overhead)
+ max_agents*(174 + max_status_bytes + max_result_bytes). Status/result maxima
include NUL; bounds/count/overflow are checked before allocating. The full view
fits projection_bytes and a single replay record (replay_bytes). Item capacity
max_display_bytes/14 and per-bank text capacity projection_bytes cover every
possible valid CGD1/CGV1 row/text copy under that byte ceiling. CGR1 DATA type1 is
CGV1 replacement; other nonzero types are not admitted by this client profile.

Client reserves max(projection_bytes+512,65536+512) for each maximum Q frame and
independent decoded storage; the fixed512 overhead covers CGL1/CGR1 and both
127-byte root IDs. Each render arena is checked6*projection_bytes+16384, covering
JSON escape expansion, row metadata/numeric identities and fixed framing. No
successful truncation. Input line payload is at most65535 bytes, explicit LIMIT
on saturation; original K/L still owns actual root admission/history bounds.

Client immutable COMMAND/CANCEL requests are independent; a separate HELLO
exchange cannot acknowledge/retire pending command receipts. HELLO is role/
discovery traffic even when it shares COMMAND lane numbering. A new higher HELLO
wire is NOT proof the prior effect-bearing result was consumed. Only its exact
COMMAND_ACK/ACK-of-ACK sequence reclaims that request. Known reconnect preserves
session/runtime/lease and exact pending command bytes/IDs; no new lease or server
incarnation may silently retarget them. Watch/role loss still permits exact old
result lookup/ACK only, never fresh mutation authority.

Initial implementation role/phase boundaries: CONNECTING (one-unit native proof
barrier), DISCOVERING (exact HELLO response), ONLINE (fully installed view before
input), DETACHING (finite pending outcome/cleanup), DONE. Signal/EOF means client
detach; explicit /shutdown is the only shutdown command. A full immutable candidate
is validated and rendered+flushed before publishing its cursor/control nonce.
Malformed Q transport remains existing fatal client driver behavior; unknown/
forbidden well-framed server control is refused, no implicit execution authority.

Body work remains gated on independent targeted header review. Next passes trace
commit rollback, discovery/reconnect/HELLO vs receipt, resync/render/EOF and bounded
resource/unwind edges before freezing this layout.

## Pass2 — retention and whole-scenario boundary traces

1. **01/04/15 canonical append:** copy actual source with configured root and new
   coordinator anchor into inactive encode/decode bank. Validate all size/status/
   policy correlations and decode successfully BEFORE one replay_append. Any
   stage/append failure leaves current bank, sequence and ring intact. Success
   has no fallible work left: verify assigned counter and publish bank index.
   Source anchors are not the canonical counter, no provider/tool call occurs.
2. **11/R13 GAP:** matching DATA must be next contiguous canonical sequence and
   type1 CGV1 with exact root/generation/sequence. Mismatch/GAP sets explicit
   awaiting_resync; only then RESYNC_VIEW may replace at sequence>=GAP.last.
   Decode in independent candidate arrays/text, render full escaped replacement
   and flush successfully, THEN install view+nonce/cursor and retain a RESYNC
   command/CURSOR ACK. A second GAP repeats this path. Partial output fails IO
   with old cursor/control state and no input; socket send-OK is not render.
3. **11/R13 discovery/receipt8eb0e14:** root is learned only after same-effective-
   UID native proof and exact HELLO result. Added one bounded nonsecret
   cli_client_hello_record per O4 logical client, three512-byte constructor slices
   and original nonce/request/result/ACK correlation. HELLO's higher wire is NOT
   command consumption. Drop connection with COMMANDn still pending, exchange
   known-lease HELLOm/ACK independently, then retry SAME n only. Lost HELLO result
   or ACK retains original bytes; late/duplicate HELLO never frees n or silently
   replaces lease/root/runtime. Its own ACK-of-ACK retires HELLO only. Expired role
   may serve exact old mutation result read-only, never authorizes fresh input.
   Bounds/ID exhaustion refuse before admitting new requests; no cache growth.
4. **03/07/09/10/R14 status:** projection takes copied real CGA accepted registry
   data only. Child status/result never becomes fabricated conversation, task or
   receiver mailbox input. Client has no worker/profile kinds or executor/store
   calls. Unknown/stale server envelope cannot acquire effect authority.
5. **05/20 EOF/signal:** read raw input only after installed view; retain observed
   anchor from the byte-read batch through line buffering. A queued old POLICY/
   CONTINUE cannot acquire a later prompt/failure anchor by delayed parsing.
   Final unterminated line is parsed once, then both COMMAND/CANCEL retained
   final results and ACK-of-ACK drain under one finite deadline. Signal/timeout
   before known outcome is INDETERMINATE; known outcome with missing receipt
   cleanup is IO. EOF never becomes a server shutdown or ALLOW. No pending
   command means immediate client detach. Other clients/server keep executing.
6. **02/06/12/16 finite errors:** reuse explicit endpoint on reconnect; no wait,
   service-time constructor, mutable flag changes or retry effect. Transport
   timeout/backpressure stays separate from application command result. Current
   request correlation precedes watermarks, so interleaved HELLO/REPLAY replies
   cannot discard an older still-pending effect result. Primary error survives
   checked close/output/endpoint cleanup.
7. **08/13/14/17/18/19 boundaries:** active/published generation, history/fork/
   policy state are copied actual committed data only; client reload/handoff uses
   existing typed command, never local model/config/store/URL/secret resolution.
   No log sink/new net observer/opaque data is introduced. Server incarnation or
   root change terminates old pending operation as uncertain, never retargets it
   to a replacement identity. Same-runtime replay cannot invoke an effect twice.

The coordinated CONTROL comment/doc exception and O4 HELLO metadata are necessary
implementation joins, not new public APIs/lanes/opcodes. Fixed root trust, leases,
exact ordinary command retirement and all existing expiry/admission checks remain.

## Pass3 — bounds, static dispatch and final cleanup consistency

The ring has one private appender. Local sequence+1 is checked before preparing
inactive state; existing replay_append's monotonic contract guarantees the same
assigned value on success. Assert that invariant rather than adding a fallible
post-append decode/copy or claiming rollback after a successful append. Every
other fallible encode/validation/capacity step occurs before append. Ring memory
is separate from both banks and source input. Commit and render never call app,
provider, tools, config, store or a mutable registry. CGV1 codec remains O5-owned.

Client proof capture uses O1's private single-unit service boundary: after
handshake readiness, get_peer_id+peer_info and cache SAME-effective-UID proof
before any additional IO/EOF. Aggregate endpoint_service is not the initial proof
barrier. Already accepted complete frames remain tied to that connection's cached
proof across EOF. Reconnect closes/reuses the explicit endpoint only after copied
request/view state is retained. No numeric peer/PID/name alone grants authority.

Per-step owner guard rejects recursion; one finite receive/input/render/retained
send offer at a time, actual copied backlog reports immediate eligibility and
idle returns a finite max_wait (not an infinite sleep or permanently writable
spin). Checked monotonic clock and a min-only reconnect/detach deadline bound
failure paths. Poll0/one bounded read never changes inherited fd flags. Output
uses the explicitly borrowed checked host stream; JSON escapes all untrusted
control bytes, then one checked complete write+flush. Embedding FILE preconditions
are caller-owned; installed main already supplies bounded host adapters. No new
FILE, output thread or hidden output queue is introduced.

The finite client text grammar maps existing typed opcodes only: ordinary text
is SUBMIT; `/continue NODE TEXT`, `/fork NODE`, `/allow NONCE`, `/deny NONCE`,
`/cancel [AGENT]`, `/reload model|base-url|credential-ref VALUE`, `/handoff local`
or `/handoff INCARNATION NUMBER`, `/shutdown`, `/resync`. Nonce resolution uses
only the fully rendered PRIMARY prompt; session/node/ordinal are copied from that
exact row, never an implicit persistent grant. Observed anchor remains the one
captured when its input bytes were read. `/history` and `/tools` render the
current committed view locally, `/quit`/`/exit` detach, unknown slash syntax is a
sanitized checked local refusal, never a model prompt. Watch permits only local
view/resync/detach; disallowed mutation is refused. No offline recovery/session
query, credential resolver or worker selector is invented. New ordinary input
while its single command slot is busy is explicitly refused; CANCEL remains
independent. No silently dropped/truncated line or unlimited client input queue.

One monotonic input serial correlates final input result across independent
COMMAND/CANCEL replies; late earlier result cannot overwrite the final line's
known outcome. EOF waits exact results+ACK-of-ACK under its original finite bound;
a known nonOK final command is reported as that result, missing receipt cleanup
as IO, unknown outcome as INDETERMINATE. Interactive nonOK command results are
checked output metadata, not fresh effects or mutation of server state. No client
result is confused with later model/task completion. Read-only retained duplicate
lookup uses original bytes and never renews a role. Other server runtime/root
always ends the old client (pending outcome uncertain), requiring a fresh client
and explicit new input rather than an undeclared reattachment protocol.

Constructor unwind frees only new owned endpoint/arrays/arenas; caller FILE/fds
are never closed or mutated. Close consumes even failed/unbegun client; no effect
owner exists to leak/drain. Projection_free similarly consumes both banks/ring.
Per-client HELLO cache in O4 is bounded by actual client count<=16 (metadata and
three512-byte slices per client); helper planning must count these actual bytes.
Retire only matching HELLO proof on exact cleanup/successor, never COMMAND/CANCEL.
No unbounded tombstones, reused IDs or automatic request re-identification.

No public header/ABI, CGL/CGR/CGV layout, library transport behavior or execution
capability changes. Three targeted passes complete; exact strict header checks
and independent final review are the remaining gate before either O8 body.

Verification at the frozen pass3 checkpoint:16/16 full-strict GCC14/Clang19 C11
and G++14/Clang++19 C++11 standalone plus combined/reversed include checks pass
for view_internal.h, coordinated host_internal.h and control.h. Diff-check clean;
only four private-header/doc paths plus this layout document changed since base,
no .c/shared CMake/config/accepted-profile edits. Independent final gate pending.

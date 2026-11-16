# O1 private endpoint implementation passes

Parent e7c11c7, source7b5592d; approved N contracts7eb24f1, O base0ae8213.
This freezes private layout/helper responsibilities before ipc.c bodies. Public
ipc.h behavior remains the contract; no replay engine/core/app/orchestrator edits.

## Pass1 — layout and ownership

ipc_internal.h declares existing endpoint/frame/route storage plus one immutable
native credential snapshot and its capture result. All memory remains constructor
reserved. Linux SO_PEERCRED captures PID/UID/GID from the actual connected fd;
no HELLO or identity prefix supplies credentials. Native PID must be positive,
getsockopt length exact; failed/unsupported capture is recorded for fail-closed
query while preserving optional generic unauthenticated transport.

Capture happens at accept before endpoint publication; a connecting endpoint
captures after native connect readiness, before publishing complete identity.
Query copies only on OK, with current matching assigned peer_id. Listener lookup
resolves its actual intrusive child list. Unknown/failed/disconnected link gives
NOT_FOUND; unbound endpoint INVALID_STATE; matching handshake-incomplete link
AGAIN; zero ID INVALID_ARG. A connected endpoint cannot query another link.
Directed send resolves one live accepted endpoint, requires complete handshake,
and uses existing whole-frame copied queue admission/cursor. No fake route.

Wiping is opt-in and uses volatile stores. Successful TX/RX retirement wipes the
whole slot before descriptor reuse; short writes and receive OVERFLOW preserve
retained bytes. Physical failure wipes abandoned TX/partial RX while keeping all
complete RX frames readable. Explicit disconnect/free/constructor unwind wipes
all allocated payload buffers before release, including never-used capacity.

## Pass2 — scenario and boundary retrace

Rechecked N pass2/all20 against this IPC-only delta. Scenarios01/03/04/09/10
use accepted native identity, complete handshake, exact fabric-scoped directed
send and copied FIFO admission. Scenario07 retains copied proof before physical
EOF; IPC query does not resurrect a closed link. Scenarios05/16/20 preserve
complete RX while abandoning partial input/output and later wipe on teardown.
Scenarios08/11/17 and R13/R14 preserve exact connection IDs across logical
leases; replacement accepted connections get new IDs, never reused by a stale
query/directed send. Scenarios02/06/12/13/14/15/18/19 introduce no IPC interface
changes: bounded service/queue pressure, no hidden retry, no proof from stored
metadata and no credential logging remain existing obligations.

Concrete trace: two peers share listener, client1 queues a secret then mutates
its source; service delivers the original copied bytes to only peer1. Full queue
returns AGAIN with no slot/descriptor mutation, retry after drain admits once.
Wrong fabric/numeric ID has no cross-listener authority. Directed send before
identity handshake completes is INVALID_STATE, not implicit handshake progress.

Complete-frame+EOF trace: RX publishes frame A, begins partial B, observes EOF.
Failure wipes B and all abandoned TX, retains A and cached establishment facts;
OVERFLOW retry still sees A unchanged, successful recv wipes only its slot.
The complete queue may be full: failure must not wipe its head as a presumed
partial slot. If physical failure occurs after the last payload read but before
publication, that frame is not yet committed and is abandoned. Core O2 owns its
by-value proof before IPC_CONNECTED/MESSAGE admission and endpoint removal;
a fresh post-EOF peer_info query intentionally returns NOT_FOUND.

Amendments: initialize native_peer_result to AGAIN explicitly (calloc zero is
OK and cannot mean captured credentials). Connected-side pre-publication query
returns AGAIN; once an ID is published, mismatched IDs return NOT_FOUND. Capture
is attempted once after native connection succeeds, even when the identity
prefix is fragmented. Accepted-side known ID is still query-AGAIN until its
identity handshake completes. Capture failure never changes caller output.

Test matrix: actual parent/child socket PID/UID/GID in both directions; false
wire identity/PID payload cannot alter proof; query null/zero/unbound/unknown/
handshake/error/unsupported and unchanged output; foreign listener and retired
ID; exact directed output/no broadcast; copied source and capacity retry;
short write, complete+partial EOF, overflow preservation, all slot retirement,
explicit abandonment and constructor unwind with optimizer-safe wipe oracles.

## Pass3 — final implementation consistency review

Rewalked all preceding paths against public ipc.h and core/event proof contracts.
No public behavior change is proposed. The private header is a planned relocation
of ipc.c's existing layout, not a second endpoint implementation; bodies will
include it only after independent approval. No exported/helper ABI is added.

Native capture stores only a kernel connection-establishment snapshot. Query
performs no syscall or allocation, returns the latched IO/UNSUPPORTED only for
a live published exact link, and never copies on failure. The byte-order identity
handshake remains unchanged and contains no native credentials. O2 separately
captures/retains its protected event proof before ownership can end; these facts
do not establish managed-worker/role authorization. Directed send delegates only
after checking listener, live peer and complete handshake; queue fullness accepts
nothing, oversized input returns LIMIT, no route mutation or implicit service.

All existing checked constructor products remain unchanged except the fixed
endpoint sizeof. Cleanup checks allocated pointers individually, so partial
constructor failure can wipe exactly the allocated configured capacity without
reading uninitialized bytes. A volatile clearing helper must not be optimized
away under Release; tests inspect buffers at actual retirement and allocator
release, rather than trusting memset source presence. Frame descriptors are
cleared with retired slots; partial writes retain their cursor and complete RX
OVERFLOW retains its descriptor. Opt-out keeps existing payload behavior.

Failure cleanup is idempotent: re-observing the same terminal error cannot wipe
committed RX or lose its error precedence. Explicit disconnect resets native
snapshot/capture state and all framing state before reuse. Listener disconnect
still detaches rather than owns/frees separately owned accepted endpoints; each
accepted endpoint retains its independent cleanup duty and inherited wipe flag.
No changes to Q fatal framing propagation, replay storage, core event ingress,
application role authorization or orchestrator lifetime are included.

Verification: the private header compiles standalone and under repeated include
with full strict GCC/Clang C11 and G++/Clang++ C++11 (4/4). Existing source bodies
are byte-for-byte unchanged. Independent private-layout gate remains required
before implementation. Focused runtime validation later includes existing IPC
and replay suites plus new native-auth/retirement/fault tests; no claim is made
that unfinished O core/orchestrator targets or the full release suite pass.

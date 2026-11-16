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

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

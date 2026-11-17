# IPC runtime contract — Stage 14.Q

The public declarations in `include/cog/ipc.h` are authoritative. This replaces
the Stage 12 blocking transport profile. Q implements all IPC API declarations;
headless CGR1 replay and CGA1/CGH1 application protocols belong to N/O.

`endpoint_new_ex` checks all products before allocating fixed input/output frame
slots and route storage. Defaults are 1 MiB payload, 8 input slots, 8 output
slots, 16 service units, 64 routes, 1000 ms handshake. Each slot reserves one
maximum-size payload plus framing bytes. No operation grows buffers or arrays.
Accepted endpoints use an explicit constructor with the listener's limits and
matching caller-owned destruction. Listener linkage is intrusive; the caller
bounds accepted endpoint admission. Independent endpoint instances share no
mutable state.

All descriptors have O_NONBLOCK and FD_CLOEXEC. A successful accept assigns a
nonzero listener-scoped identity, never reused during that listener lifetime.
The listener sends that identity as eight big-endian bytes; connect returns AGAIN
until service receives it. The client obtains the same ID through get_peer_id. Independent listeners may
both assign ID1; cross-fabric identity is (owning endpoint/fabric, peer_id).
Exhaustion rejects further accepts with LIMIT before consuming a connection.
A pending connect must not be repeated. Service's first monotonic timestamp
anchors the finite connect/identity deadline; decreasing clocks fail INVALID_ARG.
An uncompleted handshake at its deadline fails IO. An all-zero identity fails
PROTOCOL. Accepted endpoints send identity before application frames.

Application frames are four-byte unsigned big-endian length plus payload.
Empty frames are valid. Declared lengths above the constructor ceiling fail
LIMIT before receiving payload. Incomplete prefix/payload EOF fails PROTOCOL;
a normal peer close fails IO. Complete retained input survives a later peer
failure and can still be received before the terminal error. Errors are sticky
until disconnect; disconnect clears cursors and queues for explicit reuse.

Send OK copies exactly one complete frame. AGAIN accepts nothing when output
slots are full; LIMIT accepts nothing for oversized input. The caller may mutate
or free a message after OK. Bounded service flushes partial writes with retained
cursors and MSG_NOSIGNAL. Each unit performs at most one syscall, bounded to
64 KiB for payload I/O, or one frame publication. Read/write selection alternates.
A full input queue pauses reads while writes and unrelated peers still progress.

Receive never performs socket I/O. AGAIN means no complete retained frame.
OVERFLOW sets only len to the required bytes, writes no destination bytes, and
retains the same complete frame across further service. A larger-buffer retry
consumes it once. Constructor receive slots bound complete plus partial frames.

Broadcast first checks every current healthy peer's output queue, then copies
one frame to each; AGAIN leaves every peer unchanged. Later peer errors do not
undo accepted frames. Listener service rotates a bounded number of peer units;
peer failures remain visible through those endpoints and do not kill the listener.

Routes copy up to 127-byte session IDs into constructor storage. Re-registering
updates a mapping; a full route table returns OVERFLOW. Stale routes remain
until explicit unregister, and routing to a missing peer returns IO. Unknown
sessions return NOT_FOUND. Listener free detaches accepted endpoints without
closing them; accepted free unlinks itself before releasing its socket/storage.

`private/core_ipc.h` is an uninstalled integration boundary owned by IPC: role
query, one bounded service unit, and allocation-free refusal of one excess
pending connection. Core never knows the endpoint layout. No link cycle is added.

Validation: migrated `test_ipc` and `test_ipc_critic` retain their functional and
threaded fixtures using explicit test-only drivers. `test_ipc_nonblocking` directly
asserts asynchronous admission, partial prefixes/payloads, overflow retry, copied
ownership, slow-peer partial writes, timeout, invalid clocks, checked arithmetic
and transactional broadcast. See `tix show bcd9c0a` for commands.

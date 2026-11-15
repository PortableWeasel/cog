# ipc.h Contract

`libraries/cog-ipc/include/cog/ipc.h` — the IPC fabric transport layer over
Unix domain sockets: message buffers, endpoint lifecycle, framed send/recv,
poll, peer ids, broadcast, and session routing. Implemented by
`libraries/cog-ipc/src/ipc.c`.

This document records the executable contract verified by
`tests/unit/test_ipc.c`. It intentionally does **not** expand the public
API; it only documents what the header already declares and the concrete
behaviour the implementation pins down.

## Status

Implemented. `src/ipc.c` provides every function declared in `ipc.h`.
Guard: `COG_IPC_H`. The header includes `<cog/result.h>` and the standard
`<stddef.h>` / `<stdint.h>`.

## Framing

Each message is a single length-prefixed frame:

```
+----------------------------+
| payload length (uint32be)  |
+----------------------------+
| payload bytes              |
+----------------------------+
```

- The length field is big-endian and capped at `COG_IPC_MAX_PAYLOAD`
  (65535 bytes, `UINT16_MAX`).
- A zero length is a valid empty frame.
- `send` rejects a payload larger than the maximum with
  `COG_ERROR_INVALID_ARG`.
- `recv` reads the header, then the payload, looping over partial reads.
  If the declared length exceeds the caller's `msg->capacity` (or the
  maximum), the connection is closed and `COG_ERROR_OVERFLOW` is returned
  to avoid desynchronising the stream.

## Public types

| Type | Kind | Notes |
|------|------|-------|
| `cog_peer_id_t` | `uint64_t` | Peer identifier; `COG_PEER_ID_INVALID` (0) means none. |
| `cog_ipc_message_t` | `struct` | Caller-owned payload buffer (`data`/`len`/`capacity`). |
| `cog_ipc_poll_event_t` | `enum` | `COG_IPC_POLL_IN` / `COG_IPC_POLL_OUT` / `COG_IPC_POLL_ERR`. |
| `cog_ipc_endpoint_t` | `struct cog_ipc_endpoint` | Opaque endpoint. |

## Ownership and threading

- The caller owns every endpoint and message buffer. `cog_ipc_message_new`
  / `cog_ipc_message_free` and `cog_ipc_endpoint_new` /
  `cog_ipc_endpoint_free` are the only constructors/destructors.
- A listener owns its peer table (pointers to accepted endpoints) and its
  session table. An accepted endpoint keeps a back-pointer to its listener
  so that disconnecting it removes it from the table.
- The file descriptor of an accepted connection is owned by the accepted
  endpoint; the listener's peer table holds only a pointer. When a listener
  is freed or disconnected it detaches its accepted endpoints (clears their
  back-pointer) without closing their fds.
- A single endpoint is not thread-safe. Distinct endpoints may be used by
  distinct threads without locking.

## Functions

### `cog_ipc_message_new` / `cog_ipc_message_free`

```c
cog_result_t cog_ipc_message_new(cog_ipc_message_t *msg, size_t capacity);
cog_result_t cog_ipc_message_free(cog_ipc_message_t *msg);
```

| Return | Condition |
|--------|-----------|
| `COG_OK` | Success. |
| `COG_ERROR_NULL_PTR` | `msg` is NULL (`new`). |
| `COG_ERROR_INVALID_ARG` | `capacity` is zero (`new`). |
| `COG_ERROR_NOMEM` | Allocation failed (`new`). |

`new` sets `data` to a `capacity`-byte buffer, `len` to 0, `capacity` to
`capacity`. On failure `msg` is zeroed. `free` releases the buffer and
zeroes `msg`; `free(NULL)` is a no-op returning `COG_OK`.

### `cog_ipc_endpoint_new` / `cog_ipc_endpoint_free`

```c
cog_result_t cog_ipc_endpoint_new(cog_ipc_endpoint_t **out);
cog_result_t cog_ipc_endpoint_free(cog_ipc_endpoint_t *endpoint);
```

| Return | Condition |
|--------|-----------|
| `COG_OK` | Success. |
| `COG_ERROR_NULL_PTR` | `out` / `endpoint` is NULL. |
| `COG_ERROR_NOMEM` | Allocation failed (`new`). |

`new` produces an unbound endpoint. `free` disconnects a bound/connected
endpoint first, then releases it. `free(NULL)` returns
`COG_ERROR_NULL_PTR` (the header documents this return; it is not a
silent no-op).

### `cog_ipc_endpoint_listen`

```c
cog_result_t cog_ipc_endpoint_listen(cog_ipc_endpoint_t *endpoint, const char *path);
```

| Return | Condition |
|--------|-----------|
| `COG_OK` | Bound and listening. |
| `COG_ERROR_NULL_PTR` | `endpoint` or `path` is NULL. |
| `COG_ERROR_INVALID_STATE` | `endpoint` is already associated with a socket. |
| `COG_ERROR_IO` | Socket create/bind/listen failed, or the path is too long. |

On success the endpoint is a listener; a stale socket file at `path` is
removed first. The path is stored and unlinked on disconnect/free.

### `cog_ipc_endpoint_accept`

```c
cog_result_t cog_ipc_endpoint_accept(cog_ipc_endpoint_t *listener,
                                     cog_ipc_endpoint_t **client,
                                     cog_peer_id_t *peer_id);
```

| Return | Condition |
|--------|-----------|
| `COG_OK` | A new connected endpoint was created. |
| `COG_ERROR_NULL_PTR` | `listener` or `client` is NULL. |
| `COG_ERROR_INVALID_STATE` | `listener` is not listening. |
| `COG_ERROR_AGAIN` | No connection pending and the listener is non-blocking. |
| `COG_ERROR_IO` | `accept(2)` failed. |
| `COG_ERROR_NOMEM` | Endpoint allocation or peer-table growth failed. |

On success `*client` is owned by the caller and added to the listener's
peer table. `peer_id` (if non-NULL) receives the assigned id.

### `cog_ipc_endpoint_connect`

```c
cog_result_t cog_ipc_endpoint_connect(cog_ipc_endpoint_t *endpoint,
                                      const char *path,
                                      cog_peer_id_t *peer_id);
```

| Return | Condition |
|--------|-----------|
| `COG_OK` | Connected. |
| `COG_ERROR_NULL_PTR` | `endpoint` or `path` is NULL. |
| `COG_ERROR_INVALID_STATE` | `endpoint` is already associated with a socket. |
| `COG_ERROR_IO` | Socket create/connect failed, or the path is too long. |

On success the endpoint is connected with a local peer id of 1.

### `cog_ipc_endpoint_send`

```c
cog_result_t cog_ipc_endpoint_send(cog_ipc_endpoint_t *endpoint,
                                   const cog_ipc_message_t *msg);
```

| Return | Condition |
|--------|-----------|
| `COG_OK` | Frame queued/transmitted. |
| `COG_ERROR_NULL_PTR` | `endpoint` or `msg` is NULL. |
| `COG_ERROR_INVALID_ARG` | `msg->data` is NULL, `msg->len > msg->capacity`, or `msg->len` exceeds the max payload. |
| `COG_ERROR_INVALID_STATE` | `endpoint` is not connected. |
| `COG_ERROR_IO` | The write failed. |
| `COG_ERROR_AGAIN` | The write would block on a non-blocking endpoint. |

### `cog_ipc_endpoint_recv`

```c
cog_result_t cog_ipc_endpoint_recv(cog_ipc_endpoint_t *endpoint,
                                   cog_ipc_message_t *msg);
```

| Return | Condition |
|--------|-----------|
| `COG_OK` | A complete frame was received; `msg->len` is the payload length. |
| `COG_ERROR_NULL_PTR` | `endpoint` or `msg` is NULL. |
| `COG_ERROR_INVALID_ARG` | `msg->data` is NULL. |
| `COG_ERROR_INVALID_STATE` | `endpoint` is not connected. |
| `COG_ERROR_IO` | The read failed or the remote closed the connection. |
| `COG_ERROR_AGAIN` | No complete frame available on a non-blocking endpoint. |
| `COG_ERROR_OVERFLOW` | The next frame exceeds `msg->capacity` (or the max payload); the connection is closed. |

On any failure the endpoint is disconnected (returns to the unbound state).

### `cog_ipc_endpoint_poll`

```c
cog_result_t cog_ipc_endpoint_poll(cog_ipc_endpoint_t *endpoint,
                                   int events, int timeout_ms, int *revents);
```

| Return | Condition |
|--------|-----------|
| `COG_OK` | One or more requested events are ready. |
| `COG_ERROR_NULL_PTR` | `endpoint` is NULL. |
| `COG_ERROR_INVALID_ARG` | `events` is zero. |
| `COG_ERROR_INVALID_STATE` | `endpoint` is unbound. |
| `COG_ERROR_AGAIN` | Timeout expired (or poll was interrupted). |
| `COG_ERROR_IO` | `poll(2)` failed. |

`revents` (if non-NULL) receives the ready `cog_ipc_poll_event_t` bits.

### `cog_ipc_endpoint_disconnect`

```c
cog_result_t cog_ipc_endpoint_disconnect(cog_ipc_endpoint_t *endpoint);
```

| Return | Condition |
|--------|-----------|
| `COG_OK` | Socket closed; endpoint returned to the unbound state. |
| `COG_ERROR_NULL_PTR` | `endpoint` is NULL. |
| `COG_ERROR_INVALID_STATE` | `endpoint` is not associated with a socket. |

For a listener, accepted endpoints are detached (their back-pointer is
cleared) and the socket path is unlinked. For an accepted endpoint, it is
removed from its listener's peer table.

### `cog_ipc_endpoint_get_peer_id`

```c
cog_result_t cog_ipc_endpoint_get_peer_id(const cog_ipc_endpoint_t *endpoint,
                                          cog_peer_id_t *peer_id);
```

| Return | Condition |
|--------|-----------|
| `COG_OK` | `*peer_id` receives the peer id. |
| `COG_ERROR_NULL_PTR` | `endpoint` or `peer_id` is NULL. |
| `COG_ERROR_INVALID_STATE` | `endpoint` has no peer id (unbound or listening). |

### `cog_ipc_broadcast`

```c
cog_result_t cog_ipc_broadcast(cog_ipc_endpoint_t *endpoint,
                               const cog_ipc_message_t *msg);
```

| Return | Condition |
|--------|-----------|
| `COG_OK` | Sent to all live peers (best-effort; dead peers are dropped). |
| `COG_ERROR_NULL_PTR` | `endpoint` or `msg` is NULL. |
| `COG_ERROR_INVALID_STATE` | `endpoint` is not a listener. |
| `COG_ERROR_INVALID_ARG` | `msg->data` is NULL, `msg->len > msg->capacity`, or `msg->len` exceeds the max payload. |

### `cog_ipc_register_session`

```c
cog_result_t cog_ipc_register_session(cog_ipc_endpoint_t *endpoint,
                                      const char *session_id,
                                      cog_peer_id_t peer_id);
```

| Return | Condition |
|--------|-----------|
| `COG_OK` | Mapping registered (or updated if the id already exists). |
| `COG_ERROR_NULL_PTR` | `endpoint` or `session_id` is NULL. |
| `COG_ERROR_INVALID_STATE` | `endpoint` is not a listener. |
| `COG_ERROR_INVALID_ARG` | `session_id` is too long (>= 128 bytes). |
| `COG_ERROR_OVERFLOW` | The session table (64 entries) is full. |

### `cog_ipc_send_to_session`

```c
cog_result_t cog_ipc_send_to_session(cog_ipc_endpoint_t *endpoint,
                                     const char *session_id,
                                     const cog_ipc_message_t *msg);
```

| Return | Condition |
|--------|-----------|
| `COG_OK` | Frame sent to the peer hosting the session. |
| `COG_ERROR_NULL_PTR` | `endpoint`, `session_id`, or `msg` is NULL. |
| `COG_ERROR_INVALID_ARG` | `msg->data` is NULL, `msg->len > msg->capacity`, or `msg->len` exceeds the max payload. |
| `COG_ERROR_NOT_FOUND` | No peer hosts the given session. |
| `COG_ERROR_IO` | The peer hosting the session is gone. |

### `cog_ipc_unregister_session`

```c
cog_result_t cog_ipc_unregister_session(cog_ipc_endpoint_t *endpoint,
                                        const char *session_id);
```

| Return | Condition |
|--------|-----------|
| `COG_OK` | Mapping removed. |
| `COG_ERROR_NULL_PTR` | `endpoint` or `session_id` is NULL. |
| `COG_ERROR_NOT_FOUND` | The session is not registered. |

## Build integration

`cog-ipc` links the header-only `cog::result` target (exposed by cog-core)
for its `<cog/result.h>` include path and C11 requirement.  It does **not**
link the `cog-core` archive, so the target graph stays acyclic
(`cog::core -> cog::ipc -> cog::result`); the previous `cog::core` link
produced a static-library/export cycle.  `cog-result` is exported in its
own `cog-result-targets.cmake` set, so an installed consumer loads the
sets in dependency order (`cog-result`, then `cog-ipc`/`cog-obs`, then
`cog-core`) with no circular reference.

## Behavioural notes

- **Blocking mode.** Endpoints are blocking. `send` and `recv` block until
  the operation completes or fails; the `COG_ERROR_AGAIN` returns they
  document are unreachable through the current API because no endpoint can
  be placed in non-blocking mode. A `recv` that has read a partial frame
  blocks until the rest of that frame arrives, so a peer that stalls
  mid-frame can block the caller indefinitely. `poll` remains non-blocking
  and its timeout behaviour is exactly as documented.
- **SIGPIPE safety.** Writes use `send(2)` with `MSG_NOSIGNAL`, so sending
  to a peer whose remote end closed returns an error (EPIPE) instead of
  raising SIGPIPE and terminating the process under the default signal
  disposition. The library installs no signal handler.
- **Direction of data flow.** A connected endpoint (`connect`) and the
  accepted endpoint (`accept`) are the two ends of one full-duplex socket.
  Data written to one end is read from the other. Broadcast and
  session-routing send from the listener to the accepted endpoints, so the
  data arrives on the connected (client) side.
- **Overflow closes the link.** An oversized or malformed frame is treated
  as a protocol violation: `recv` disconnects the endpoint and returns
  `COG_ERROR_OVERFLOW`.
- **Peer ids.** A listener assigns incrementing ids starting at 1 to each
  accepted connection. A connected endpoint is assigned id 1. Ids are
  unique for the lifetime of the connection.
- **Session table bounds.** The table holds at most 64 mappings; the 65th
  registration returns `COG_ERROR_OVERFLOW`. Re-registering an existing id
  updates its peer mapping and never overflows.
- **No global state.** All state lives in caller-allocated endpoint
  contexts. There is no dynamic registration and no global constructor.
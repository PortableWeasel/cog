# cog-ipc {#page_cog_ipc}

## What it does

`cog-ipc` is the Unix-domain-socket fabric that lets a core, a UI, and
auxiliary cores talk to each other. It provides:

- framed, length-delimited **messages** over nonblocking sockets,
- **listeners, accepted peers, and clients** with explicit ownership,
- **session routing** and **broadcast** to many peers,
- a bounded **replay buffer** for UI reconnect,
- peer **identity** assignment and validation.

Everything is nonblocking. There is no hidden thread, no blocking accept, and
no unbounded buffer.

Headers: `ipc.h`.

## Key ideas

**Frames, not byte streams.** `cog_ipc_endpoint_send` copies one whole frame
into reserved output storage exactly once; `cog_ipc_endpoint_service` flushes
partial writes. `cog_ipc_endpoint_recv` returns only complete frames and
retains partial prefixes internally.

**Driven by `service`.** `cog_ipc_endpoint_service(endpoint, now_ms,
&deadline)` advances framing, handshakes, and output without waiting. The core
calls it at fairness points; standalone users call it in their own loop.

**`AGAIN` means "nothing now".** It never hides dropped data. A full input
queue pauses that peer only, not unrelated endpoints.

**Limits are reserved at construction.** `max_frame_bytes`, `receive_frames`,
`send_frames`, `service_quantum`, `max_routes`, and `connect_timeout_ms` are
fixed when the endpoint is created. Oversized frames fail with `COG_ERROR_LIMIT`
before allocation.

**Peer IDs are stable.** Every accepted connection gets an identifier that
routing and diagnostics use.

## Getting started: a loopback round trip

```c
#include <cog/ipc.h>

#include <stdint.h>
#include <string.h>

void ipc_round_trip(void);

void ipc_round_trip(void)
{
    cog_ipc_endpoint_config_t config = {0};
    cog_ipc_endpoint_t *listener = NULL;
    cog_ipc_endpoint_t *server = NULL;
    cog_ipc_endpoint_t *client = NULL;
    cog_ipc_message_t out = {0};
    cog_ipc_message_t in = {0};
    cog_peer_id_t peer = 0;
    uint64_t deadline = 0;

    if (cog_ipc_endpoint_new_ex(&listener, &config) != COG_OK ||
        cog_ipc_endpoint_listen(listener, "/tmp/cog-demo.sock") != COG_OK ||
        cog_ipc_endpoint_new(&client) != COG_OK) {
        goto done;
    }

    /* connect() may return AGAIN while the handshake is pending. */
    while (cog_ipc_endpoint_connect(client, "/tmp/cog-demo.sock", NULL) ==
           COG_ERROR_AGAIN) {
        (void)cog_ipc_endpoint_service(client, 0, &deadline);
    }

    if (cog_ipc_endpoint_accept(listener, &server, &peer) != COG_OK) {
        goto done;
    }

    if (cog_ipc_message_new(&out, 64) != COG_OK) {
        goto done;
    }
    memcpy(out.data, "ping", 4);
    out.len = 4;
    if (cog_ipc_endpoint_send(client, &out) != COG_OK) {
        goto done;
    }

    if (cog_ipc_message_new(&in, 64) != COG_OK) {
        goto done;
    }
    while (cog_ipc_endpoint_recv(server, &in) == COG_ERROR_AGAIN) {
        (void)cog_ipc_endpoint_service(server, 0, &deadline);
        (void)cog_ipc_endpoint_service(client, 0, &deadline);
    }
    /* in.len == 4 and in.data holds "ping" */

done:
    (void)cog_ipc_message_free(&in);
    (void)cog_ipc_message_free(&out);
    (void)cog_ipc_endpoint_free(server);
    (void)cog_ipc_endpoint_free(client);
    (void)cog_ipc_endpoint_free(listener);
}
```

In production the service calls belong in the core's `service_fn`, not in a
tight loop as shown here. The loop is only to make the handshake and frame
exchange deterministic in a small example.

## Routing and broadcast

After peers are connected you can address them by session rather than by
socket:

- `cog_ipc_register_session` binds a session id to a peer,
- `cog_ipc_send_to_session` delivers to that peer,
- `cog_ipc_unregister_session` removes the binding,
- `cog_ipc_broadcast` fans a frame out to every connected peer.

Routes are bounded by `max_routes` and are owned by the endpoint.

## Reconnect and replay

`cog_ipc_replay_t` is a bounded ring of encoded records. A UI that
disconnects can reconnect and ask for everything after a sequence number with
`cog_ipc_replay_copy_after`. Records are encoded and decoded with
`cog_ipc_replay_encode` / `cog_ipc_replay_decode`, so the buffer can also be
persisted if needed.

## Beyond the basics

- **Passing an endpoint to the core.** `cog_core_config_t.ipc_endpoint`
  transfers ownership on a successful `cog_core_new`; the core drains it and
  emits `COG_EVENT_IPC_*` events.
- **Discovery and diagnostics.** Poll events include `COG_IPC_POLL_IN`,
  `COG_IPC_POLL_OUT`, and `COG_IPC_POLL_ERR`; connection events are mirrored
  into @ref page_cog_obs "cog-obs" for the UI.
- **Backpressure.** A peer that stops reading fills its own output queue; the
  endpoint returns `AGAIN` to the producer without blocking others.

## See also

- @ref page_cog_core — the consumer of IPC events
- @ref page_cog_orchestrator — spawning and supervising aux cores
- `pocs/ipc-fabric` — the original proof of concept

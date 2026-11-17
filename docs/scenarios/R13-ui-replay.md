# R13: UI detach, bounded replay gap and active-display resynchronization

Stage 14.D pass 2; baseline header pass 1 `29b41e8`, amended by this pass.
This is a contract trace, **not an executable conformance pass**.

## Sequence

```mermaid
sequenceDiagram
    CLI->>Replay: cog_ipc_replay_append(ring,type,payload,len,sequence)
    UI->>IPC: CGR1 CURSOR(session,generation,last_rendered)
    CLI->>Replay: cog_ipc_replay_copy_after(ring,generation,after,record,storage,capacity,required)
    alt generation mismatch or evicted prefix
    Replay-->>CLI: GAP including retained window
    CLI->>CLI: cog_cli_app_display_snapshot(app,snapshot)
    CLI->>IPC: cog_cli_display_snapshot_encode(snapshot,wire,capacity,required)
    IPC-->>UI: CGD1 full visible state plus session/generation/through_sequence
    UI->>UI: decode, replace display atomically, anchor cursor
    else contiguous window
    CLI->>IPC: cog_ipc_replay_encode(record,wire,capacity,required)
    IPC-->>UI: DATA; render once then advance cursor
    end
```

## Exact public coverage

Each symbol below is declared in the linked public header. Local application
decisions in the diagram are not invented library functions.

| Step | Exact calls, types, events or fields | Declaring header |
|---|---|---|
| Bounded replay generation/cursor | `cog_ipc_replay_new`, `cog_ipc_replay_append`, `cog_ipc_replay_copy_after`, `cog_ipc_replay_record_t`, `COG_IPC_REPLAY_DATA`, `COG_IPC_REPLAY_GAP`, `COG_IPC_REPLAY_CURSOR` | [libraries/cog-ipc/include/cog/ipc.h](../../libraries/cog-ipc/include/cog/ipc.h) |
| Portable replay codec | `cog_ipc_replay_encode`, `cog_ipc_replay_decode` | [libraries/cog-ipc/include/cog/ipc.h](../../libraries/cog-ipc/include/cog/ipc.h) |
| Atomic full display state/codec | `cog_cli_app_display_snapshot`, `cog_cli_display_snapshot_t`, `cog_cli_display_item_t`, `cog_cli_display_snapshot_encode`, `cog_cli_display_snapshot_decode` | [applications/cog-cli/include/cog_cli/cli.h](../../applications/cog-cli/include/cog_cli/cli.h) |
| Nonblocking transport | `cog_ipc_endpoint_new_ex`, `cog_ipc_endpoint_service`, `cog_ipc_endpoint_send`, `cog_ipc_endpoint_recv` | [libraries/cog-ipc/include/cog/ipc.h](../../libraries/cog-ipc/include/cog/ipc.h) |
| Connection events | `COG_EVENT_IPC_CONNECTED`, `COG_EVENT_IPC_DISCONNECTED` | [libraries/cog-core/include/cog/event.h](../../libraries/cog-core/include/cog/event.h) |


## Ownership and invariants

- Replay ring copies bounded display records; eviction reports GAP, never silently presents a contiguous stream. Identity is (session_id,generation,sequence). Empty ring mismatch returns GAP with first=last=sequence=0.
- GAP cannot be repaired by provider copy_history: it may reject incomplete/unresolved turns and has no active text. Instead app owns complete visible display plus active aggregate and returns an atomic replay anchor with a bounded scratch snapshot, excluding opaque/credential data.
- Client validates session identity, decodes full snapshot transactionally, replaces display and then requests data after through_sequence. New gap during snapshot transfer triggers another snapshot; never re-execute model/tools to reconstruct output.
- App snapshot views are encoded/copied before any later app mutation/service. UI cursor advances only after successful render; reconnect duplicates at/below cursor are ignored.

## Failure and capacity branches

- Replay copy/codec/recv destination OVERFLOW changes required size only and preserves source. Output send AGAIN accepts no frame, so caller retains exact encoded bytes for retry; send OK accepts once despite future partial writes.
- Slow peer sends only prefix or never reads: bounded nonblocking endpoint_service must still permit unrelated net, timer, tool and cancellation progress. Snapshot LIMIT is an explicit failed resync, never partial state publication.

## Evidence and gap disposition

Original11 baseline drop-oldest buffer test cannot prove generation-aware GAP or active snapshot; new codecs remain declarations.

**Required future executable evidence:** N/O/Q: detach during active stream/tool round, ring eviction/restart-before-first-record, cross-session stale snapshot, tiny buffers and slow prefix/output peer tests.

**Amendment references:** D11,D12,D13 in pass-2 register (`tix show 345a536`).

## Pass-3 IPC ownership retrace (E02)

Core constructor settings `cog_core_config_t.ipc_max_frame_bytes` and
`cog_core_config_t.ipc_payload_bytes` bound admitted frames and queued payload
bytes ([core.h](../../libraries/cog-core/include/cog/core.h)). Reserve an event
slot and owned arena bytes before endpoint receive; if unavailable, consume
nothing and service/dispatch other work. Bytes survive until handler return.
An endpoint-retained frame above the core ceiling closes only that peer with
reported LIMIT, rather than an endless OVERFLOW loop. Accepted endpoint
constructors remain explicit and peer-count-bounded; private intrusive listener
links need no additional public allocation surface. Test just-below/exact/above
frame limits, full arena/full event queue, and caller payload mutation after enqueue.

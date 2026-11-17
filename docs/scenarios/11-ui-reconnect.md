# Scenario 11: UI disconnect → bounded buffer → reconnect → replay

O9 executable update: the canonical CTest uses the actual host/client sockets,
real GAP/replacement, retained ASK and lost-final command retry without repeated
effects. Historical buffer/queue assertions remain in `component-ui_buffer`.
See the joined evidence and exact limits (`tix show f63652b`).
The contract/design-pass history below is retained as such.

Stage 14.D pass-2 retrace from public-contract baseline `29b41e8`.
This replaces the earlier schematic SVG as the normative call trace. The SVG is
retained as historical design material and is not current API coverage.

## Sequence diagram

```mermaid
sequenceDiagram
    participant UI
    participant App
    participant Provider
    UI-->>App: IPC disconnected
    loop Headless model processing continues
      Provider-->>App: typed stream delta
      App->>App: cog_ipc_replay_append(copied payload,sequence)
    end
    UI->>App: encoded CURSOR(session,generation,last_sequence)
    App->>App: cog_ipc_replay_decode(cursor)
    App->>App: cog_ipc_replay_copy_after(generation,last_sequence)
    alt Requested sequence retained
      App->>App: cog_ipc_replay_encode(DATA)
      App-->>UI: endpoint_send encoded DATA; deduplicate lifetime/sequence
    else Window overflow / gap
      App->>App: cog_ipc_replay_encode(GAP with window bounds)
      App-->>UI: explicit GAP
      App->>App: cog_cli_app_display_snapshot atomically anchors through_sequence
      App->>App: cog_cli_display_snapshot_encode(CGD1) before service mutation
      App-->>UI: full CGD1 display snapshot including active/incomplete text
      UI->>UI: decode CGD1 and replace display; cursor = generation/through_sequence
    end
    Note over UI,App: Replay never executes tools or retries model turns
```

## Exact public-header coverage

Names below are exact declared functions, types, enum members or struct fields;
application-only policy/envelope steps are identified explicitly rather than
presented as library calls. Each header path names the declaring public file.

| Trace step | Exact declaration / member | Declaring header |
|---|---|---|
| Connection transitions | `COG_EVENT_IPC_DISCONNECTED`; `COG_EVENT_IPC_CONNECTED` | [libraries/cog-core/include/cog/event.h](../../libraries/cog-core/include/cog/event.h) |
| Connect / accept / identify | `cog_ipc_endpoint_connect`; `cog_ipc_endpoint_accept`; `cog_ipc_endpoint_get_peer_id` | [libraries/cog-ipc/include/cog/ipc.h](../../libraries/cog-ipc/include/cog/ipc.h) |
| Send replay frame | `cog_ipc_message_t`; `cog_ipc_endpoint_send`; `cog_ipc_send_to_session` | [libraries/cog-ipc/include/cog/ipc.h](../../libraries/cog-ipc/include/cog/ipc.h) |
| Bounded replay owner | `cog_ipc_replay_t`; `cog_ipc_replay_new`; `cog_ipc_replay_append`; `cog_ipc_replay_free` | [libraries/cog-ipc/include/cog/ipc.h](../../libraries/cog-ipc/include/cog/ipc.h) |
| Transactional cursor read | `cog_ipc_replay_copy_after`; `cog_ipc_replay_record_t.generation`; `cog_ipc_replay_record_t.sequence`; `cog_ipc_replay_record_t.first_sequence`; `cog_ipc_replay_record_t.last_sequence` | [libraries/cog-ipc/include/cog/ipc.h](../../libraries/cog-ipc/include/cog/ipc.h) |
| Defined portable envelope | `cog_ipc_replay_encode`; `cog_ipc_replay_decode`; `COG_IPC_REPLAY_DATA`; `COG_IPC_REPLAY_GAP`; `COG_IPC_REPLAY_CURSOR` | [libraries/cog-ipc/include/cog/ipc.h](../../libraries/cog-ipc/include/cog/ipc.h) |
| Headless service | `cog_core_run`; `cog_core_config_t.service_fn`; `cog_core_config_t.service_interval` | [libraries/cog-core/include/cog/core.h](../../libraries/cog-core/include/cog/core.h) |
| Provider progress remains independent | `cog_provider_poll`; `cog_provider_next` | [libraries/cog-provider/include/cog/provider_vtable.h](../../libraries/cog-provider/include/cog/provider_vtable.h) |
| Atomic visible resynchronization source | `cog_cli_app_display_snapshot`; `cog_cli_display_snapshot_t.session_id`; `cog_cli_display_snapshot_t.generation`; `cog_cli_display_snapshot_t.through_sequence`; `cog_cli_display_snapshot_t.items`; `cog_cli_display_item_t.status`; `cog_cli_display_item_t.text` | [applications/cog-cli/include/cog_cli/cli.h](../../applications/cog-cli/include/cog_cli/cli.h) |
| Portable full-display codec | `cog_cli_display_snapshot_encode`; `cog_cli_display_snapshot_decode` | [applications/cog-cli/include/cog_cli/cli.h](../../applications/cog-cli/include/cog_cli/cli.h) |
| Bounded nonblocking frame service | `cog_ipc_endpoint_new_ex`; `cog_ipc_endpoint_config_t.max_frame_bytes`; `cog_ipc_endpoint_config_t.receive_frames`; `cog_ipc_endpoint_config_t.send_frames`; `cog_ipc_endpoint_config_t.service_quantum`; `cog_ipc_endpoint_service` | [libraries/cog-ipc/include/cog/ipc.h](../../libraries/cog-ipc/include/cog/ipc.h) |

## Ownership, errors and adversarial branches

- Every endpoint has constructor-reserved frame queues and bounded nonblocking service. A peer that sends a partial length prefix or stalls halfway through payload retains its own cursor and cannot block unrelated peers. Send OK copies one frame exactly once; retry only AGAIN, which accepted nothing. Receive OVERFLOW reports required bytes and retains the same complete frame across service calls.
- Provider progress and durable history cannot depend on the UI staying connected. Application buffers are bounded and have stable session/sequence identity.
- Dropping an old display frame must produce an explicit replay gap; it cannot silently claim the UI received a complete stream or replay an external effect.
- Reconnect validates session, nonreused stream generation and last accepted sequence before replay. Duplicate replay is deduplicated by `(session,generation,sequence)`, not text equality or a sequence reused after restart. Matching-generation cursor beyond tail is INVALID_ARG; mismatched generation or evicted prefix yields explicit GAP.
- Constructor limits both entries and payload bytes. One oversized record/sequence wrap returns LIMIT atomically; ordinary append evicts only enough oldest records. `copy_after` does not consume or mutate a shared cursor, and destination OVERFLOW writes only required capacity.
- CGR1 encodes fixed-width big-endian scalar fields and exact session/payload bytes; never send native C struct/pointers. Decode rejects bad version/kind, UTF-8, bounds or trailing bytes without publishing partial output.
- On GAP, obtain the atomic display snapshot anchored at `(generation,through_sequence)`, including active/incomplete visible text even where provider copy_history refuses replay. Encode CGD1 before app mutation/service invalidates its scratch view. UI verifies the snapshot session ID matches its requested session, decodes into owned item/text arrays, atomically replaces its display and resumes DATA after that exact anchor; if another gap occurs, snapshot again. It never renders opaque continuation or uses this display state to authorize execution.
- Snapshot/codec capacity failure is explicit and publishes no partial display. Provider history is not a display snapshot and cannot substitute for unresolved/active aggregates.
- Borrowed provider/IPC storage is copied before buffering. A slow UI cannot hold transport views forever and starve unrelated work.

## Gap analysis and release amendment

**O11-G1 resolved by pass-2 bounded replay and CGR1 codec contracts:** constructor-owned ring, explicit lifetime/cursor identity, transactional copying, portable encode/decode and GAP window metadata cover reconnect without hidden loss. CGD1 full-display snapshots atomically bind visible state, active/incomplete text and replay anchor; this closes the history-vs-display gap and must complete before later display deltas. No remaining public contract gap is claimed; runtime implementation and adversarial reconnect tests remain gated.

## Executable evidence and required follow-up

Existing baseline test: [`applications/cog-cli/tests/test_11_ui_reconnect.c`](../../applications/cog-cli/tests/test_11_ui_reconnect.c). It exists as historical
engine/mock application evidence. It was not rerun in this design-only pass and
does not prove the new mandatory headers, real transport or durable-tool behavior.

Future conformance requirements: Window exhaustion emits gap, reconnect sequence deduplication, late/stale UI handshake, headless completion and bounded memory under slow reader.

Header tracing is complete only where the gap analysis says so. Production
implementation and executable scenario migration remain gated by pass 3 and the
independent final review; no passing runtime result is inferred from these tables.

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

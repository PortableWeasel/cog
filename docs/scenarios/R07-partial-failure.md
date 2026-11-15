# R07: Accepted partial output followed by network loss

Stage 14.D pass 2; baseline header pass 1 `29b41e8`, amended by this pass.
This is a contract trace, **not an executable conformance pass**.

## Sequence

```mermaid
sequenceDiagram
    CLI->>Provider: cog_provider_next(provider,TEXT,storage,capacity,required)
    CLI->>CLI: display and retain accepted text
    Provider->>Net: cog_net_peek_request(net,id,terminal_error)
    CLI->>Provider: cog_provider_next(provider,FINISH_error,storage,capacity,required)
    CLI->>Provider: cog_provider_get_error(provider,error)
    CLI->>Session: cog_session_append(session,INCOMPLETE_assistant,node)
    CLI->>Store: cog_store_save(store,session)
    Note over CLI,Provider: no transparent resend; explicit new user continuation required
```

## Exact public coverage

Each symbol below is declared in the linked public header. Local application
decisions in the diagram are not invented library functions.

| Step | Exact calls, types, events or fields | Declaring header |
|---|---|---|
| Retained terminal | `cog_net_peek_request`, `cog_net_response_t` | [libraries/cog-net/include/cog/net.h](../../libraries/cog-net/include/cog/net.h) |
| Copy/error | `cog_provider_next`, `cog_provider_get_error` | [libraries/cog-provider/include/cog/provider_vtable.h](../../libraries/cog-provider/include/cog/provider_vtable.h) |
| Partial/outcome metadata | `cog_provider_error_t`, `cog_provider_delta_t` | [libraries/cog-provider/include/cog/provider_request.h](../../libraries/cog-provider/include/cog/provider_request.h) |
| Incomplete history | `COG_MESSAGE_INCOMPLETE`, `cog_message_t` | [libraries/cog-core/include/cog/message.h](../../libraries/cog-core/include/cog/message.h) |
| Persistence/replay guard | `cog_session_append`, `cog_session_copy_history` | [libraries/cog-core/include/cog/session.h](../../libraries/cog-core/include/cog/session.h) |
| Save | `cog_store_save` | [libraries/cog-core/include/cog/session_store.h](../../libraries/cog-core/include/cog/session_store.h) |


## Ownership and invariants

- An accepted text delta is enough to disable transparent retry even if no durable complete assistant exists yet. Header output_accepted must not be interpreted as only already-rendered text.
- Keep exactly one incomplete assistant aggregate; unfinished tool fragments never become executable. Ordinary copy_history refuses unsafe incomplete paths.
- A new explicit continuation must acknowledge the incomplete turn; it cannot silently reconstruct an earlier request or rerun old tools.

## Failure and capacity branches

- Network loss while next destination is too small must retain the earlier selected text sequence before the error FINISH. Retry copying the same delta, then observe terminal failure once.
- Cancel after a terminal completion is already queued does not create a second FINISH. Save failure reports incomplete state and blocks automatic next request.

## Evidence and gap disposition

Protocol PoC tests preserve partial text on remote error; baseline original16/20 tests do not prove typed retained terminal ordering.

**Required future executable evidence:** F/H/K/L: loss at every fragment boundary and destination overflow/cancel races with exact sequence assertions.

**Amendment references:** D02,D03 in [pass-2 register](../REAL_CLIENT_SCENARIO_PASS2.md).

## Pass-3 explicit continuation and queued text (E05)

An explicitly authorized continuation forks from the failed model turn's last
replay-safe ancestor, preserves the incomplete aggregate on its original branch,
and appends the new user input. It never relabels a failed wire turn COMPLETE or
replays partial tools/opaque data; unresolved effects use E01 recovery instead.
Automatic queued prompts cannot make that decision. FIFO auto-injection leases
the original backing text until its handler returns (or copies into another
reserved owner); successful enqueue alone is not permission to overwrite it.
Future tests enqueue a second prompt before dispatching the first and verify
unchanged text, plus explicit continuation/cancel branch isolation.

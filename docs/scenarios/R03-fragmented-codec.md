# R03: Fragmented SSE, UTF-8, tool identity and opaque data

Stage 14.D pass 2; baseline header pass 1 `29b41e8`, amended by this pass.
This is a contract trace, **not an executable conformance pass**.

## Sequence

```mermaid
sequenceDiagram
    CLI->>Provider: cog_provider_poll(provider,now,deadline)
    Provider->>Net: cog_net_peek_request(net,id,response)
    Provider->>Provider: frame bytes; bounded strict JSON and identity assembly
    Provider->>Net: cog_net_ack(net,id,sequence) after bytes copied
    CLI->>Provider: cog_provider_next(provider,delta,storage,capacity,required)
    Note over Provider,CLI: TEXT may stream; split ID/name buffered until complete turn
    Provider-->>CLI: TOOL_BEGIN -> TOOL_ARGUMENTS -> TOOL_END -> OPAQUE -> FINISH
```

## Exact public coverage

Each symbol below is declared in the linked public header. Local application
decisions in the diagram are not invented library functions.

| Step | Exact calls, types, events or fields | Declaring header |
|---|---|---|
| Shared net retained bytes | `cog_net_peek_request`, `cog_net_ack`, `cog_net_response_t` | [libraries/cog-net/include/cog/net.h](../../libraries/cog-net/include/cog/net.h) |
| Private parser composition | `cog_openrouter_config_t`, `cog_openrouter_provider_vtable` | [libraries/cog-provider/include/cog/openrouter.h](../../libraries/cog-provider/include/cog/openrouter.h) |
| Poll/copy exact retained item | `cog_provider_poll`, `cog_provider_next` | [libraries/cog-provider/include/cog/provider_vtable.h](../../libraries/cog-provider/include/cog/provider_vtable.h) |
| Typed order/limits | `cog_provider_delta_t`, `cog_provider_limits_t`, `COG_PROVIDER_DELTA_TOOL_BEGIN`, `COG_PROVIDER_DELTA_TOOL_ARGUMENTS`, `COG_PROVIDER_DELTA_TOOL_END`, `COG_PROVIDER_DELTA_OPAQUE` | [libraries/cog-provider/include/cog/provider_request.h](../../libraries/cog-provider/include/cog/provider_request.h) |
| Opaque owner and bytes | `cog_message_opaque_t` | [libraries/cog-core/include/cog/message.h](../../libraries/cog-core/include/cog/message.h) |


## Ownership and invariants

- Synthetic fragments: index0 ID "call_" then "7"; name "edit_" then "file"; arguments may start before the second identity fragment. No partial/guessed BEGIN is published. After validated wire turn, exactly one BEGIN establishes call_7/edit_file, then arguments and END. Two interleaved indices retain independent assembly.
- EOF or finish before a required nonempty identity/complete strict argument object fails. Duplicate IDs/keys, invalid UTF-8, ambiguous continuation metadata or unknown required opaque format fail closed.
- Each frame is committed once even if it expands beyond queue entry count. Private bounded cursor resumes expansion as next frees slots; an independent FINISH slot guarantees terminal error/cancel after earlier retained items.

## Failure and capacity branches

- Every byte split of UTF-8, CRLF, comment and multiline data is legal transport fragmentation. A frame above max_frame_bytes or a single delta above retention capacity fails LIMIT; queue fullness alone pauses without dropping.
- Opaque fixture in fixtures/real-client-contracts.json specifies text/summary reconstruction and discrete encrypted entries. It is synthetic protocol metadata, not requested/rendered model reasoning.

## Evidence and gap disposition

pocs/provider-wire tests already establish strict JSON/SSE, Unicode/OOM/depth and actual repeated usage-finish shape; they do not implement new typed queue/opaque reconstruction.

**Required future executable evidence:** H: split-identity fixture, typed expansion cursor, malformed/duplicate-key/depth/OOM and opaque stream reconstruction tests.

**Amendment references:** D02,D03,D04,D05 in [pass-2 register](../REAL_CLIENT_SCENARIO_PASS2.md).

## Pass-3 opaque preservation retrace (E04)

The codec retains explicit `reasoning_details: []` presence. Merged text/summary
blocks preserve non-payload members as a union; conflicting repeated JSON values
fail UNSUPPORTED instead of discarding unknown continuation metadata. Absence
does not erase a previous member. These rules are pinned in
[openrouter.h](../../libraries/cog-provider/include/cog/openrouter.h). Future
fixtures cover absent vs empty, signature-only suffixes and unknown equal/conflicting
members, then compare exact generated opaque bytes through save/load/fork/replay.

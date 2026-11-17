# cog-provider implementation contract

Public headers are authoritative. All nine static callbacks are mandatory;
generic connect rejects an incomplete table and sets its output to NULL on
failure. Generic request validation checks nested message structure, finite
sampling bounds and UTF-8 model identifiers. Instances embed the public base.

OpenRouter construction borrows one immutable endpoint/config generation and a
live cog-net context. It checks HTTPS authority/port and credential line safety,
reserves request serialization, independent strict JSON pools, SSE line/frame,
tool/opaque assembly, one retained ordinary delta and an independent terminal
slot. The configured entry count is a ceiling; this implementation uses one
ordinary entry plus its resumable final expansion cursor. Small counts work even
for frames expanding into many final tool/opaque/usage items. No Cog runtime
allocation occurs. Third-party transport allocation follows the explicit net
contract. All retained state is released on disconnect.

Requests are serialized completely before cog_net_request; net copies the body
and credential headers before return. No borrowed nested request pointer survives
send. Unsupported namespace/format, malformed JSON, duplicate keys and invalid
history fail before network effects. The adapter selects exactly the requested
registered model, disables reasoning generation, and never retries/substitutes.
Continuation is replayed separately under the pinned namespace/format.

SSE accepts arbitrary byte fragmentation, CR/LF/CRLF, comments and multiline data.
Strict yyjson decoding uses explicit fixed allocators and depth preflight. Every
nested object rejects duplicate decoded keys; invalid UTF-8/surrogates fail.
A frame validates before exposing its text. Tool identity/arguments accumulate
privately across interleaved slots; final complete ID/name and object arguments
are validated after finish+[DONE]+EOF. Only then does the resumable cursor emit
one BEGIN/ARGUMENTS/END per tool, complete opaque bytes, cumulative usage and one
successful FINISH. Duplicate tool IDs, conflicting terminal metadata, nonempty
post-finish data or unknown continuation types fail closed. Partial earlier
text is retained on failure and cannot authorize a tool.

Streaming opaque text/summary updates merge consecutive unsigned blocks, retain signature
closure and the union of equal non-payload metadata. Encrypted entries stay
discrete. Explicit empty-array presence is preserved. Unknown/conflicting merged
members fail UNSUPPORTED. Nonstream responses already contain their final array:
entry boundaries/order and every value are retained without fragment merging,
including adjacent entries with equal or differing metadata. All output pointer
lifetimes belong to caller storage.

next is a pure transactional copy: OVERFLOW changes only required, retains
sequence/item and writes neither storage nor metadata. A configured retention
ceiling instead yields terminal LIMIT. Full retention pauses consumption of only
this provider's net ID. Cancellation discards active transport without waiting,
preserves an already retained item and adds one ABORTED FINISH. Errors contain
fixed sanitized messages and explicit HTTP/kind/retry/output/replay metadata;
peer text, credentials and request bodies never become diagnostics.

Legacy receive refuses structured pending items without consuming them. The
mock's narrow typed script contract is documented in
`tix show 055211c`; it uses the same generic vtable surface.

Verification: unit-provider, unit-provider-critic, unit-openrouter,
unit-mock-provider, integration-core-provider; integration-openrouter-https when
the audited transport profile is supplied. The historical HTTP-only net fake
used implicit lifecycle and consume-on-poll APIs and was retired. Actual HTTPS
composition replaces that obsolete coverage. H's core integration fixture remains
a legacy text-flow test, not K complete durable conversation conformance.

No-wire preflight validates ordered call/result history and adapter compatibility
without accepting a turn. COMPLETE requires every group closed; PENDING_TOOLS
permits only the final assistant group with at least one missing result. IDs
match within their group and unique results may arrive in any order. Normal
send always revalidates COMPLETE. OpenRouter reserves independent equal-sized
parser/writer/request scratch at construction so preflight cannot overwrite the
accepted request, retained continuation document, deltas, clock or terminal
error. Mock fixture preflight uses an independent 128 KiB arena and deliberately
accepts arbitrary structurally valid opaque fixture data; it is not a protocol
compatibility oracle. No preflight path allocates or touches the transport.

The approved recognized opaque entry kinds share one inbound/outbound validator:
actual-kind payloads are strings when present; common id is string/null, format
is string, and index is a finite number. Optional fields remain optional and
unknown members remain lossless, including payload-like names on other kinds.
Null signature is outside the pinned profile (UNSUPPORTED); other malformed
recognized kinds are PROTOCOL. No base64, index-range or format-value policy is
introduced; the approved profile clarification is archived on `tix show a81767d`.

## Prompt compiler extension

`provider_prompt.h` and the static `cog_openrouter_prompt_template` add pure,
bounded preparation. They do not change the existing provider request/vtable
layout or authorize network/effect work. The joined instruction gate is accepted
(`tix show 04800f5`); `unit-provider-prompt` checks the implemented compiler.

The constructor reserves scratch and two aligned output lanes. Compile validates
identity, counts, strict input and model/token ceilings, renders into the inactive
lane, checks all transitive returned ranges and unchanged history/settings, then
publishes. Every failure preserves the previous request and output parameter.
No compile-time allocation, source loading, provider callbacks or credential
resolution is permitted. Destruction ends all borrowed views without I/O.

Contract tests must cover constructor overflow, exact capacity edges, invalid
model/template/provider bindings, duplicate tool names (including denied rows),
ASK guidance, denied omission, strict object schemas and decoded duplicate keys,
quotes/newlines/Unicode escaping, exact framed fragment order, copied history
roles/calls/results/opaque bytes/usage, output-token and logical byte ceilings,
and every codec range violation. An intentionally invalid static test renderer
must not publish storage outside the candidate lane or mutate preserved settings.
Successful requests remain valid after caller input storage changes. Test
success/error/success alternation to verify two-lane ownership.

Integration must use the same prepared input for send, restored-tool preflight,
reload and staged handoff. No compiler test alone proves runtime publication or
execution eligibility; those belong to runtime/store scenarios.

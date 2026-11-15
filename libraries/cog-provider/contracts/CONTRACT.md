# cog-provider implementation contract

Public headers are authoritative. All eight static callbacks are mandatory;
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

Opaque text/summary updates merge consecutive unsigned blocks, retain signature
closure and the union of equal non-payload metadata. Encrypted entries stay
discrete. Explicit empty-array presence is preserved. Unknown/conflicting merged
members fail UNSUPPORTED. All output pointer lifetimes belong to caller storage.

next is a pure transactional copy: OVERFLOW changes only required, retains
sequence/item and writes neither storage nor metadata. A configured retention
ceiling instead yields terminal LIMIT. Full retention pauses consumption of only
this provider's net ID. Cancellation discards active transport without waiting,
preserves an already retained item and adds one ABORTED FINISH. Errors contain
fixed sanitized messages and explicit HTTP/kind/retry/output/replay metadata;
peer text, credentials and request bodies never become diagnostics.

Legacy receive refuses structured pending items without consuming them. The
mock's narrow typed script contract is documented in
`docs/REAL_CLIENT_H_MOCK_AMENDMENT.md`; it uses the same generic vtable surface.

Verification: unit-provider, unit-provider-critic, unit-openrouter,
unit-mock-provider, integration-core-provider; integration-openrouter-https when
the audited transport profile is supplied. The historical HTTP-only net fake
used implicit lifecycle and consume-on-poll APIs and was retired. Actual HTTPS
composition replaces that obsolete coverage. H's core integration fixture remains
a legacy text-flow test, not K complete durable conversation conformance.

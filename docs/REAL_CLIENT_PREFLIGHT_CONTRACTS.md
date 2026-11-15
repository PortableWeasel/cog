# L/H no-wire provider preflight amendment

Design gate `f7d9933`, linked to L0 `65ccb86`; baseline `fa56242` includes the
independently approved L selectors. No production implementation is authorized
until three targeted passes and an independent final review pass. This document
amends only the narrow provider compatibility boundary; unaffected L work may
continue against the approved selector contract.

## Pass 1: goals, blocks, source audit and initial contract

- Reject unsupported persisted continuation before resumed PROPOSED tool effects.
- Accept supported opaque bytes without protocol-specific CLI branching.
- Check the actual selected prefix and current model/settings/declared limits.
- Preserve turn acceptance, clock, retained output and terminal error evidence.
- Keep normal send eligibility separate from a staged pre-tool prefix.

The functional blocks are the app's bounded public session views, generic static
provider dispatch, and adapter-owned protocol/constructor workspaces. Store,
ledger and toolkit keep their existing roles. Every block serves one goal; no
capability registry, protocol switch in CLI or new persistent representation is
needed. [Boundary diagram](provider-preflight.svg).

The source audit found no existing safe public operation: request_validate calls
message_validate for structural data only; model flags do not describe opaque
payload grammar; the OpenRouter codec validates pinned namespace/json-v1 content
privately during send. Calling send then cancel accepts wire work, and ordinary
copy_history refuses unresolved calls. Blanket refusal of valid opaque content
would violate lossless continuation support. These facts resolve the research
question without writing an implementation or making any wire call.

The minimal amendment adds `cog_provider_preflight_stage_t`, one generic
`cog_provider_preflight(provider,request,stage)` and one required static vtable
callback `preflight`. COMPLETE selects ordinary complete history; PENDING_TOOLS
permits the final assistant call group before results exist. The request struct
has no dry-run flag, so staged permission cannot accidentally reach send.

Return values report input validation directly. preflight does not publish or
replace get_error: that remains a sanitized retained TERMINAL TURN error whose
message lifetime ends at the next send/disconnect, never at preflight. Neither
success nor failure accepts a request, performs transport/service, advances a
sequence/clock, consumes output, retains inputs or allocates. Scratch reuse must
preserve every observable retained lifetime even after an invalid input.

The app supplies its real current model/generation/max_tokens/tools declaration
and a bounded selected root-to-active array from public node/message views. It
checks store/ledger replay eligibility separately; no synthetic TOOL result is
inserted to satisfy an encoder. Compatibility is checked before STARTED/save/
ALLOW for restored PROPOSED work. Later send checks the completed request again,
since future tool output may exhaust a bound or require separate validation.

## Delivery strategy (gated)

Provider dispatch/history checks, OpenRouter adapter, mock fixture and every
custom static vtable must migrate together to the required ninth callback.
Missing required callbacks fail before connect invokes implementation code.
No silent structural-only fallback is allowed. Existing limits/workspaces serve
preflight; no runtime allocation, dependency initialization or global state is
introduced. CLI owns only the generic selected-prefix call, not parser details.

After independent header acceptance, implement a coherent provider migration
with unit/adapter/error-preservation/zero-wire tests, then integrate the dependent
L PROPOSED path with real supported and unsupported opaque fixtures. Source
writing and executable evidence remain gated; this header commit proves neither.

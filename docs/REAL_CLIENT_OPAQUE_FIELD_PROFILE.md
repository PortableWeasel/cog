# Recognized opaque field profile clarification

Header gate `a81767d`, implementation review `396a690`, baseline `416d67a`.
This is a narrow header/docs amendment. No production source is changed here;
three targeted passes and a different independent final reviewer precede runtime
correction. The preflight API, stage rules, constructor limits and on-wire
representation remain unchanged.

## Pass 1: goal, source audit and minimal boundary

The selected provider must reject malformed recognized opaque metadata before
restored PROPOSED effects while preserving supported unknown members. The shared
validator currently rejects malformed text/summary/signature values but accepts
encrypted data objects/numbers and common id numbers, format objects and index
strings. A local real-adapter PENDING_TOOLS repro returns OK for these values and
copies them into the encoded request without any network call.

The functional boundary remains the adapter-owned shared inbound/outbound
continuation validator; the [existing diagram](provider-preflight.svg) applies.
No generic CLI protocol branching, parser replacement, new model capability or
second request representation is introduced.

The [official Chat Completions reasoning-details shape](https://openrouter.ai/docs/guides/best-practices/reasoning-tokens#common-fields)
identifies the common id/format/index kinds and the recognized text/summary/
encrypted data members. These known fields are not exempted by the promise to
preserve unknown metadata. The implementation accepts a pinned subset rather
than all future provider shapes: null signatures in official examples remain
outside current supported merge/closure semantics. They must be identified as
UNSUPPORTED, not mislabeled malformed JSON or silently accepted.

Initial explicit profile:

| Recognized field | Accepted kind when present |
|---|---|
| type | One supported reasoning.text / reasoning.summary / reasoning.encrypted string |
| id | String or null |
| format | String, including unfamiliar string values |
| index | Finite JSON number |
| text on reasoning.text | String |
| summary on reasoning.summary | String |
| data on reasoning.encrypted | String |
| signature | Non-null string; null remains UNSUPPORTED |

Members other than type remain optional, preserving signature-only and payload-
absent fragments already admitted by the pinned profile. No new requirement to
supply id/format/index/data is added. Existing strict JSON/UTF-8/duplicate-key
validation and constructor byte/depth ceilings still apply.

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


## Pass 2: targeted field and state traces

| Case | Local outcome before effects | Preserved boundary |
|---|---|---|
| encrypted data string, including empty/arbitrary synthetic text | OK within limits | No base64 decode or secret interpretation |
| encrypted data object/array/number/bool/null | PROTOCOL | No STARTED/save/tool start or wire probe |
| common id string or null | OK within limits | Presence and exact JSON value retained |
| common id object/array/number/bool | PROTOCOL | No reinterpretation or guessed ID |
| common format any string | OK within limits | No new allowlist for unfamiliar formats |
| common format null/object/array/number/bool | PROTOCOL | No silent dropping |
| common index finite number, including fractional/negative | OK within limits | No invented integer/nonnegative/range policy |
| common index string/null/object/array/bool | PROTOCOL | No coercion |
| own-kind text/summary string, including empty | OK within limits | Existing text-fragment merge contract |
| own-kind text/summary number/bool/object/array/null | PROTOCOL | No malformed payload enters merge |
| signature non-null string, including empty | OK within limits | Existing presence closes a text/summary block |
| signature null | UNSUPPORTED | Official protocol shape outside pinned merge/closure profile |
| signature other nonstring kind | PROTOCOL | No guess at cryptographic meaning |
| recognized optional member absent | Existing supported behavior | No new mandatory id/format/index/payload |
| unknown member nested object/array/scalar | Existing supported behavior | Unknown discrete metadata remains lossless |
| a type-specific payload name on another supported kind | Existing supported behavior | It is unknown for that kind, not misclassified |
| same bad field through restored preflight and incoming stream | Same recognized-field decision | Tool events remain gated by valid FINISH |
| preflight rejected while old turn error exists | Direct validation result only | Prior get_error/message/clock/retained data unchanged |

The pass-2 retrace found a scope risk: merely saying data/index are typed could
be read as adding base64 decoding, integer/range checks, a format-value allowlist
or mandatory keys. The header now explicitly excludes those changes. It also
preserves unfamiliar members, including a payload-like name on a different
supported entry kind. Only recognized fields for the actual kind and the stated
common fields receive these kind checks.

The same check applies to inbound entry reconstruction, outbound send and local
preflight. A source fixture must demonstrate both paths, including invalid
recognized fields never reaching tool readiness and a valid unknown member
round-tripping unchanged. No new generic dispatch rule, request stage, storage
codec, model feature, workspace operation or dependency allocation is needed.


## Pass 3: consistency and independent-review handoff

The final targeted pass checked the field table and eighteen traces against the
existing public preflight and OpenRouter contracts. No function, struct field,
callback or constructor limit is added. The dependency/ownership diagram stays
unchanged. Both stages use the same known-member decisions; normal send remains
COMPLETE-only, and store/guard/policy/ledger still govern any effect admission.

The error taxonomy is explicit: malformed recognized member kinds are PROTOCOL;
unknown discriminator/profile semantics and a null signature are UNSUPPORTED;
existing constructor representation/depth exhaustion remains LIMIT. Absence is
not the same as null for signature, format or payload members. A null id remains
a permitted exact value. Accepted unknown members are neither normalized away
nor reclassified as recognized fields. No cryptographic validation is claimed.

A future nullable-signature extension must separately define streaming null ->
string transitions, block closure and member-union/equality behavior, then follow
the same header-first gate. This clarification does not grant that extension.
The direct source defect remains the grouped first implementation-review cycle;
these comments do not claim that source is already corrected.

Required implementation oracles are bounded field-kind tables through both the
actual no-wire encoder and incoming reconstruction, a supported opaque request
still accepted, unknown-member JSON preservation, explicit null-signature
UNSUPPORTED, and unchanged preflight state/allocation/net counters on rejection.
Existing no-effect and terminal-ready ordering assertions remain intact.

Strict standalone C11/C++11 header checks and diff validation precede handoff to
a DIFFERENT independent final reviewer. Runtime correction starts only after
that review passes. No production source or paid/network request was used here;
reading the public protocol documentation was not a generation request.

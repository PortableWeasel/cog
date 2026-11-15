# Provider no-wire preflight implementation evidence

Implementation ticket `396a690`, based on approved header checkpoint `495567a`.
The narrow [accepted contract](REAL_CLIENT_PREFLIGHT_CONTRACTS.md) remains the
interface. Independent implementation review and integration are still required.

Generic dispatch requires all nine callbacks before implementation connect. It
validates structural requests and ordered assistant call groups without storage
allocation. COMPLETE closes every group. PENDING_TOOLS accepts only the final
open group, including already-recorded unique results in either order. IDs are
local to a group; textual reuse in a later group is supported. Ordinary send
always preflights COMPLETE again, so staged validation cannot authorize a wire
turn. Legacy mock send also enters that generic boundary.

OpenRouter uses its actual send encoder and compatibility checks with independent,
equal-sized constructor-reserved parser, writer and request scratch. This keeps
an idle provider's accepted request and retained continuation document intact.
Active includes retained deltas and queued FINISH. Preflight never services,
cancels or drains them. Model/generation, current advertised capabilities,
strict JSON/UTF-8/duplicate keys, pinned opaque semantics and representation
ceilings remain adapter-owned. Historical tools remain representable when no
current tools are advertised. The shared continuation validator now also rejects
malformed known text/summary/signature value kinds consistently with inbound
validation; supported unknown members in discrete opaque entries are preserved.

Mock preflight uses an independent constructor-reserved 128 KiB copy arena and
the identical representation as send. Arbitrary structural fixture opaque,
models and generations are accepted as documented. Preflight does not consume
scripted send failures or replace last_request, errors, cursors or counters.
All six existing static table sites migrated without removing old callbacks;
the configuration-only identity descriptor remains deliberately undispatched.

## Executable checks

The expanded existing suites cover all subsets and both orders of results for
three calls; closed ID reuse; orphan, duplicate, wrong-group and crossed-group
results; non-COMPLETE messages; null/stage/required-callback failures; and pending
history rejected by ordinary send. Real codec cases cover supported opaque,
unknown namespace/format/type/required keys, malformed JSON/UTF-8/duplicate keys,
known-field type errors, tool declarations/arguments, models, generation and
current capabilities. Request bytes and mock arena boundaries are exact; parser,
writer, JSON depth, opaque and output-token ceilings are independently exercised.
Future tool output can still fail completed send after an earlier pending prefix
fit. Existing constructor failure injection now includes every new allocation.

Link-wrapped malloc/calloc/realloc checks prohibit allocation during real and mock
preflight. All five real-adapter net entry points are counted: every tested
preflight return leaves the counter unchanged. Retained text/sequence,
cancellation, queued FINISH, last_request pointers/content, script state and last
terminal get_error values/message lifetimes survive rejected and successful
preflight. The admitted-curl offline HTTPS integration now preflights its actual
pending assistant/opaque prefix before supplying the result; active request
count stays unchanged and the fixture still admits exactly two wire turns.

Validation on 2026-11-15:

- Strict GCC full target build; focused 9/9 checks passed.
- Strict Clang full target build; focused 9/9 checks passed.
- Clang ASan/UBSan full target build; focused 9/9 checks passed with leak detection
  and halt-on-error enabled.
- GCC full admitted-curl CTest: 96/102 passed. The six failures are in untouched
  legacy orchestrator/IPC fixtures: unit-orchestrator,
  integration-orchestrator-ipc, integration-orchestrator-stress and scenarios
  07/09/10. Their source still assumes the earlier synchronous peer-admission
  profile. This evidence does not claim that the full release gate passes.
- git diff whitespace check passed. No paid or live service requests occurred.

The focused set is unit-provider, unit-provider-critic, unit-openrouter,
unit-mock-provider, cli-mock-provider, cli-configuration,
cli-configuration-memory, integration-core-provider and
integration-openrouter-https. All builds use the existing admitted c-ares curl
at `/tmp/cog-net-maintained/build`, its shared library and installed headers.
No concurrency or dependency lifecycle behavior changes; preflight remains an
operation on the provider's owner thread.

## Review cycle 1: recognized opaque member kinds

Independent review found that the first implementation did not validate encrypted
`data` or the common `id`/`format`/`index` kinds. The separately approved
[recognized-field profile](REAL_CLIENT_OPAQUE_FIELD_PROFILE.md), gate `a81767d`
at `3e237a8`, now precedes this correction in the implementation history.

One shared entry validator serves inbound reconstruction and outbound/preflight.
It checks the actual entry kind's text, summary or data payload as a string;
common id as string/null, format as string and index as a finite JSON number.
Optional fields remain optional. Null signature is consistently UNSUPPORTED;
other nonstring signatures are PROTOCOL. Encrypted data remains uninterpreted,
negative/fractional indices and unfamiliar format strings remain supported, and
unknown members, including payload-like names on other kinds, remain opaque.

The writer's bounded 109-case table exercises these decisions through both
preflight stages and incoming reconstruction. Additional checks preserve nested
unknown JSON and replay the exact resulting opaque bytes, refuse malformed data
before any TOOL_BEGIN/ARGUMENTS/END, and preserve a prior terminal error across
PROTOCOL and UNSUPPORTED preflight returns. Allocation and all net entry-point
counters remain unchanged for every preflight case. The independent critic's
separate finite-state model (`1f27e90`) checks 55,987 histories and was left
untouched by the writer's revision.

Strict GCC and Clang full target builds and the focused ten-test set pass;
Clang ASan/UBSan with leak detection also passes all ten. The ten-test set adds
unit-provider-preflight-critic to the nine cases above. The final additional
terminal-error field cases were rebuilt and rerun in unit-openrouter under all
three profiles. No new full-suite or release-gate success is claimed; the earlier
six unrelated orchestrator fixture failures remain recorded above. This is an
implementation revision awaiting the different independent reviewer's verdict.

/** @file runtime_instructions.h
 * @brief Draft original static instruction-control tools and scope admission.
 * No dynamic registration or independent invocation: these descriptors use the
 * owning runtime's strict JSON, policy, ledger and publication machinery.
 */
#ifndef COG_RUNTIME_INSTRUCTIONS_H
#define COG_RUNTIME_INSTRUCTIONS_H
#include <cog/tool.h>
#ifdef __cplusplus
extern "C" {
#endif
/** @brief activate_skill, exact literal schema:
 * {"type":"object","properties":{"name":{"type":"string"}},
 * "required":["name"],"additionalProperties":false}
 * Catalog grammar applies. Must be eligible with current tools. Already active
 * is successful no-op, no new binding. Missing NOT_FOUND; metadata UNSUPPORTED;
 * inaccessible DENIED. Host explicitly binds ASK/ALLOW/DENY; skills cannot grant.
 */
extern const cog_tool_t cog_runtime_activate_skill_tool;
/** @brief read_skill_resource, exact literal schema:
 * {"type":"object","properties":{"skill":{"type":"string"},
 * "path":{"type":"string"}},"required":["skill","path"],
 * "additionalProperties":false}
 * Skill must be active. Path normalized strictly beneath its package directory;
 * no absolute/dot/dotdot/symlink traversal, script execution or automatic includes.
 * Coordinator reader supplies bounded bytes. Workers use authenticated logical
 * skill/path against pinned context; never reopen coordinator source paths.
 * Missing NOT_FOUND; escape DENIED; invalid UTF-8 PROTOCOL; capacity LIMIT.
 */
extern const cog_tool_t cog_runtime_read_skill_resource_tool;
/** @brief Existing delegate descriptor schema amendment for named roles.
 * Extend each tasks[] item with optional agent (string, exact catalog name) and
 * skills (array of unique strings). Retain required task, minItems1/maxItems16,
 * additionalProperties:false at item and outer levels. DISABLED rejects these
 * additions UNSUPPORTED; absent fields preserve old context-free CGB1 behavior.
 * PINNED resolves each via runtime_child_select before original delegate STARTED;
 * no new descriptor or duplicate child scheduler is introduced.
 */

/** @section runtime_instruction_admission Common control and scope contract
 * Both descriptors are original static pointers shared by prompt/dispatch.
 * Validate complete JSON/schema and policy before STARTED. Existing finite
 * request/tool/elapsed budgets apply; no recursive activation or reader callbacks.
 * Candidate preparation uses explicit async constructor/reader reservations;
 * bounded polling never allocates/grows. After a source read completes, yield
 * out of poll before the owner preparation phase invokes named catalog/snapshot
 * constructors and runtime_instruction_prepare. These explicit constructors may
 * allocate bounded storage and always have matching destruction; no allocator
 * runs hidden inside source/tool polling. Source bytes/host paths never diagnostics.
 * Successful controls return only bounded metadata (skill/resource identity and
 * selected snapshot IDs), not source bodies in UI-visible tool output. Loaded
 * resource bytes enter instruction fragments through the replacement snapshot.
 *
 * A context-changing control closes remaining unstarted calls in its assistant
 * group as identified no-effect DENIED results. Previously STARTED effects drain
 * under original context. Successful control result, terminal ledger rows and
 * replacement binding commit in ONE guarded checkpoint. Failed preparation
 * records failed identified result and keeps old context. Save failure freezes
 * admission. Next request is bounded continuation, never replay of old group.
 * Duplicate transport uses original receipts/call ID, never reinvokes operation.
 * Native operations stage a coordinator-validated candidate; the coordinator
 * assigns catalog/snapshot IDs and authenticates parent epoch/call/group identity.
 * Executor commits exact result/ledger/binding atomically and returns COMMIT;
 * coordinator verifies correlation before READY permits bounded continuation.
 * Partial transfer, stale candidate or cancellation never publishes/executes.
 * Detailed transport receipt lifetime follows worker capsule protocol; a hash
 * alone is integrity evidence, not authentication or durable commit proof.
 *
 * Before read/search/edit effects, read AGENTS chain for each declared target:
 * containing directory for files, target directory for directory/search; command
 * uses declared cwd. V1 multiple target rows must resolve to the SAME directory
 * scope; distinct scopes fail UNSUPPORTED before any effect rather than merging
 * sibling AGENTS chains into a single-scope manifest. No-follow bounded
 * reader compares retained exact source identities/bytes, not hash metadata alone.
 * Changed/new scope denies ENTIRE unstarted proposal group without STARTED,
 * prepares replacement, and checkpoints denials+binding before continuation.
 * Inaccessible/malformed scope fails closed, never treated as absent. External
 * changes after admission remain filesystem races; this is not OS confinement
 * or knowledge of arbitrary command paths. No silent restored-context reread:
 * explicit resumed operation scope checking precedes its new effects only.
 *
 * Host replacements share runtime_instruction_prepare/publish. Explicit catalog
 * refresh may discover new files; ordinary restore uses durable exact bytes.
 * Core generic context/worker capsule contracts still govern persistence/transfer.
 */
#ifdef __cplusplus
}
#endif
#endif

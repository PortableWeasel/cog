/** @file runtime.h
 * @brief Draft frontend-neutral cooperative runtime; no implementation yet.
 * One owner thread; no recursive calls from callbacks, terminal IO, environment
 * discovery, signal mutation, implicit dependency init or mutable globals.
 * Constructors reserve bounded storage. Service never grows runtime pools.
 * Named instruction snapshot/catalog/prepare constructors run in a separate
 * owner preparation phase after source polling has yielded terminal data, never
 * inside reader/tool poll callbacks. Their finite configured reservations are
 * explicit allocation boundaries, with matching destructors on every path.
 * Existing explicit tool/provider/IPC constructors retain their documented
 * allocation boundaries. Required NULLs return NULL_PTR. Errors leave outputs
 * unchanged unless specified. All IDs are nonzero, nonreused and overflow-checked.
 * Snapshot identity is (owning session,catalog generation,snapshot ID). Child
 * sessions have separate domains. Coordinator allocates above complete durable
 * history high-water, never just selected branch. Live candidates burn IDs;
 * uncertain publication freezes rather than replay/reissue. Host prepare validates
 * supplied ID above high-water; model/native candidates use coordinator allocation.
 * No process-global catalog or snapshot allocator is introduced.
 */
#ifndef COG_RUNTIME_H
#define COG_RUNTIME_H
#include <cog/result.h>
#include <cog/session.h>
#include <cog/session_store.h>
#include <cog/provider.h>
#include <cog/net.h>
#include <cog/provider_prompt.h>
#include <cog/instructions.h>
#include <cog/tool.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct cog_runtime cog_runtime_t;
typedef struct cog_runtime_instruction_change cog_runtime_instruction_change_t;
/** @brief Explicit startup action; restore never implicitly continues work. */
typedef enum cog_runtime_mode {
    COG_RUNTIME_NEW = 0, COG_RUNTIME_RESUME, COG_RUNTIME_RECOVER, COG_RUNTIME_INSPECT
} cog_runtime_mode_t;
/** @brief Explicit instruction compatibility policy; never inferred from files. */
typedef enum cog_runtime_instruction_mode {
    COG_RUNTIME_INSTRUCTIONS_DISABLED = 0, /**< Legacy context-free execution;
       instructions/sources/workspace_root_id/scopes NULL, scope_count0. Existing
       CGB1 profile remains valid. Null store/mock usage keeps existing limits:
       no durable workspace effects without a guarded store. No instruction tools. */
    COG_RUNTIME_INSTRUCTIONS_PINNED, /**< NEW requires initial snapshot and owned
       context-aware store. RESUME requires NULL snapshot and exact durable
       binding; missing context UNSUPPORTED. Offline inspect may view it without
       provider. Source-free resume allowed; any later scope/resource need lacking
       configured roots is DENIED before effect, not implicit discovery. */
    COG_RUNTIME_INSTRUCTIONS_ADOPT_LEGACY /**< Explicit NEW-like initial snapshot
       for RESUME of context-free safe history only. Owned/context-capable store
       and existing legacy fencing rules required; no pending/STARTED/uncertain
       calls or incomplete turn allowed. Commit initial binding before fresh
       input; no rebase of prior proposals, auto continuation or source scan.
       Already context-bound history returns INVALID_STATE. */
} cog_runtime_instruction_mode_t;
/** @brief Finite nonzero bounds, except max_retries may be zero. */
typedef struct cog_runtime_limits {
    size_t max_input_bytes, max_queued_inputs, max_view_bytes;
    size_t max_context_bytes, max_tool_output_bytes, max_instruction_bytes;
    size_t max_events, max_dispatch_per_step;
    uint32_t max_output_tokens, max_tool_rounds, max_turn_ms, max_tool_ms;
    uint32_t max_retries, max_retry_elapsed_ms;
} cog_runtime_limits_t;
/** @brief Resolved generation. Runtime constructs/owns connected provider.
 * Endpoint, ALL nested fields/config/transport and template remain immutable
 * borrows until runtime_close consumes runtime, even after superseding reload.
 * Caller clears secrets only then. No file/env/credential resolver callbacks.
 */
typedef struct cog_runtime_provider_config {
    const cog_endpoint_t *endpoint;
    const char *model_id; /**< Copied exact registered model, no fallback. */
    const cog_provider_prompt_template_t *prompt_template;
    cog_provider_prompt_limits_t compiler_limits;
} cog_runtime_provider_config_t;
/** @brief Original static table/user borrows through close; bindings copied.
 * Prompt and execution use identical descriptor pointers; duplicate names or
 * descriptor mismatch fail construction. Async descriptors required. Bindings
 * reference members of table or the exact cog_runtime_activate_skill_tool and
 * cog_runtime_read_skill_resource_tool objects. Those two builtin controls use
 * internal singleton static tables; including them never grants a policy
 * implicitly. Copies of their descriptors do not preserve their identity. */
typedef struct cog_runtime_tool_config {
    const cog_tool_table_t *table;
    void *user;
    const cog_prompt_tool_binding_t *bindings;
    size_t binding_count;
} cog_runtime_tool_config_t;
/** @brief Static mapping from original descriptor to declared scope property.
 * Paths are top-level JSON string fields checked by ordinary strict validation.
 * A descriptor may have several rows for multiple targets; V1 requires all
 * resolved directory scopes equal or returns UNSUPPORTED before effects. Unmapped external
 * tools are refused when instruction scope enforcement is configured; the two
 * instruction controls are exempt. NULL path_key uses explicit default_scope.
 */
typedef struct cog_runtime_scope_binding {
    const cog_tool_t *descriptor;
    const char *path_key;
    const char *default_scope; /**< Used only for absent optional key or NULL key;
       NULL means a missing key is INVALID_ARG. Empty means workspace root. */
    bool file_target; /**< Strip final component; false directory/command cwd. */
} cog_runtime_scope_binding_t;
/** @brief Optional native workers; NULL disables them. Strings copied.
 * Existing authenticated bootstrap/receipt/handoff used; no shell/PATH search.
 * Only builtin serializable provider/tool profiles supported; arbitrary native
 * implementation_config/user pointers fail UNSUPPORTED before launching. */
typedef struct cog_runtime_workers {
    const char *executable; /**< Trusted absolute worker-capable executable. */
    const char *socket_path; /**< Dedicated worker fabric, never UI listener. */
    const cog_net_config_t *net_config; /**< Explicit immutable transport template
       borrowed through close; initialized host dependencies and fixed memory
       profile, observer/user NULL. Each executor constructs its own transport. */
    size_t frame_bytes; /**< Trusted full CGL frame capacity. */
    size_t instruction_bytes; /**< Complete CGIM capacity per executor; zero only
       in DISABLED mode. Constructor proves CGB2 plus CGL fits frame_bytes and
       accounts every candidate/retained/codec copy multiplied by all slots. */
    size_t workers; /**< 1..8 physical slots, no automatic failover/replay. */
    size_t children; /**< 1..16 including retained terminal attempts. */
    uint64_t request_budget; /**< Finite aggregate worst-case request credit. */
    uint32_t tool_mask; /**< read/search/edit/command bits 1/2/4/8; intersect parent. */
    uint32_t shutdown_ms; /**< Finite; timeout is not proof effects undone. */
} cog_runtime_workers_t;
/** @brief Explicit constructor inputs. Offline modes need no provider/tools,
 * sources/workers/dependency runtime. No frontend-derived defaults.
 * V1 max_queued_inputs is1..8 and max_input_bytes<=max_context_bytes; larger
 * unsupported reservations fail LIMIT before effects. store.backend explicitly
 * set to cog_store_null_backend selects nondurable DISABLED mode; NULL means
 * the default file backend, never implicit no-store. */
typedef struct cog_runtime_config {
    cog_runtime_mode_t mode;
    cog_runtime_instruction_mode_t instruction_mode;
    const char *session_id; /**< Copied 1..127 bytes, existing store ID grammar. */
    bool previous_processes_fenced; /**< Required for recovery/legacy adoption;
       operator assertion only, does not stop or independently verify writers. */
    cog_session_config_t session; /**< V1 execution reservation maxima: nodes1024,
       calls per message16; zero selects these runtime maxima. Larger values fail
       LIMIT. Smaller explicit reservations and payload/context arenas are honored. */
    cog_store_config_t store; /**< Runtime creates/frees store; custom context
       borrowed through close. PINNED/ADOPT require owned context-aware checkpoints;
       DISABLED preserves legacy null-store/no-tools and explicit fixture behavior. */
    cog_runtime_limits_t limits;
    cog_runtime_provider_config_t provider;
    cog_runtime_tool_config_t tools;
    const char *workspace_root_id; /**< Logical source root for scoped tool paths;
       NULL disables scoped tools, never disables checks for enabled workspace. */
    const cog_runtime_scope_binding_t *scopes; /**< Copied array/strings. */
    size_t scope_count;
    const cog_instruction_snapshot_t *instructions; /**< PINNED NEW required, deep-copy
       manifest at new. PINNED RESUME requires NULL; ADOPT requires candidate;
       DISABLED and offline modes require NULL. See instruction_mode rules. */
    const cog_instruction_source_config_t *sources; /**< Optional explicit roots,
       copied/opened by independent reader constructor. Required for configured
       workspace scope/resource access. No implicit home/source discovery. */
    cog_instruction_limits_t instruction_limits;
    const cog_runtime_workers_t *workers;
} cog_runtime_config_t;
/** @brief Slice status, returned even after valid-call driver error. Continue
 * stepping to drain, preserve first failure. done does not imply effect undo. */
typedef struct cog_runtime_step_info {
    bool done, input_ready, publication_pending;
    size_t dispatched;
    uint64_t next_deadline_ms; /**< UINT64_MAX if none. */
    uint32_t max_wait_ms; /**< Caller must not sleep beyond this finite bound. */
    cog_result_t outcome;
} cog_runtime_step_t;
/** @brief Visible row, excludes provider opaque state. */
typedef struct cog_runtime_view_item {
    uint64_t node_id;
    cog_message_role_t role;
    cog_message_status_t status;
    const char *text;
    size_t text_bytes;
} cog_runtime_view_item_t;
/** @brief Approval identity; echo exact copied fields when answering. */
typedef struct cog_runtime_policy {
    uint64_t serial, assistant_node_id, configuration_generation;
    uint64_t instruction_catalog_generation, instruction_snapshot_id, deadline_ms;
    const char *call_id, *tool_name, *arguments;
    size_t argument_bytes;
} cog_runtime_policy_t;
/** @brief Immutable publication borrows survive step/drain until ack or close.
 * Separate reserved arena retains later mutations. Optional ASK is not consent. */
typedef struct cog_runtime_view_info {
    uint64_t serial, active_generation, published_generation;
    uint64_t instruction_catalog_generation, instruction_snapshot_id;
    const char *session_id;
    const cog_runtime_view_item_t *items;
    size_t item_count;
    const cog_runtime_policy_t *policy;
    cog_result_t outcome;
    bool continuation_required;
} cog_runtime_view_t;

/** @brief Construct quiescent owner, without model/tool work.
 * @param[out] out Empty slot, NULL on failure. @param[in] config Explicit input.
 * @return OK; NULL_PTR; INVALID_ARG mode/IDs/bounds; NOT_FOUND model/session;
 * DENIED fencing/policy; UNSUPPORTED backend/template/native profile; LIMIT;
 * NOMEM; IO; PROTOCOL malformed context; AGAIN competing owner; INDETERMINATE.
 * PINNED NEW commits initial empty session+binding before publication. RESUME validates
 * retained context/template/tools without source reread. RECOVER is guard-fenced
 * offline recovery only; INSPECT never acquires execution authority. Failed
 * checkpoint freezes rather than guessing/retrying a new identity. Partial
 * resources unwind once. Offline modes expose retained view without begin.
 */
cog_result_t cog_runtime_new(cog_runtime_t **out, const cog_runtime_config_t *config);
/** @brief Begin cooperative drive, no dispatch/input/launch here.
 * @param[in,out] runtime Quiescent owner. @return OK; NULL_PTR; INVALID_STATE
 * begun/ended/offline/shutdown. An ended runtime cannot restart. */
cog_result_t cog_runtime_begin(cog_runtime_t *runtime);
/** @brief Fair bounded dispatch/service/drain without wait or terminal IO.
 * @param[in,out] runtime Begun owner. @param[out] step Slice status.
 * @return OK; AGAIN no progress; NULL_PTR; INVALID_STATE invalid drive; first
 * driver failure. Named candidate constructors may run at the documented owner
 * preparation boundary; reader/tool poll functions never allocate. Errors freeze admission and request drain: keep stepping
 * until done, then close. Publication backpressure never blocks cancel/deadlines
 * or overwrites retained output. Each executor gets its fixed round-robin quantum.
 */
cog_result_t cog_runtime_step(cog_runtime_t *runtime, cog_runtime_step_t *step);
/** @brief Copy fresh explicit input exactly once; queue retains for later turn.
 * @param[in,out] runtime Owner. @param[in] text UTF-8/NUL-free.
 * @param[in] bytes 1..max_input_bytes. @param[in] queue Later-turn selection.
 * @return OK; NULL_PTR; INVALID_ARG text; LIMIT bytes; OVERFLOW queue; AGAIN
 * publication/context change; INVALID_STATE offline/shutdown; INDETERMINATE.
 * Prior queued input cannot authorize failed/cancelled continuation. Fresh
 * queue=false selects safe ancestor and retains original failed aggregate.
 */
cog_result_t cog_runtime_submit(cog_runtime_t *runtime, const char *text, size_t bytes, bool queue);
/** @brief Cancel current turn, retaining accepted output/results.
 * @param[in,out] runtime Owner. @return OK; NULL_PTR; INVALID_STATE offline/ended;
 * OVERFLOW queue. No fresh input, automatic replay or promise of effect undo. */
cog_result_t cog_runtime_cancel(cog_runtime_t *runtime);
/** @brief Freeze admission and request drain, also valid before begin.
 * @param[in,out] runtime Owner. @return OK; NULL_PTR. Idempotent, owner-thread
 * only, not signal-safe. Pre-begin shutdown completes without launching work. */
cog_result_t cog_runtime_shutdown(cog_runtime_t *runtime);
/** @brief Borrow retained complete view; no implicit approval.
 * @param[in] runtime Owner. @param[out] view Borrowed immutable data.
 * @return OK; NULL_PTR; AGAIN no publication; LIMIT view capacity. Caller must
 * copy/render successfully before ack; borrowed policy must be copied for answer.
 */
cog_result_t cog_runtime_view(const cog_runtime_t *runtime, cog_runtime_view_t *view);
/** @brief Acknowledge successful delivery to authorized host/controller.
 * @param[in,out] runtime Owner. @param[in] serial Exact retained view ID.
 * @return OK exact last duplicate idempotent; NULL_PTR; INVALID_STATE stale.
 * Retires borrow. Ordinary send/effect/next-input cannot cross this barrier until
 * ack; cancel/drain can. Ack is neither ALLOW nor durable checkpoint evidence.
 */
cog_result_t cog_runtime_ack(cog_runtime_t *runtime, uint64_t serial);
/** @brief Answer acknowledged ASK under exact immutable identity.
 * @param[in,out] runtime Owner. @param[in] policy Deep copy from acknowledged view.
 * @param[in] allow Explicit decision. @return OK; NULL_PTR; INVALID_STATE stale,
 * unpublished or cancelled; DENIED expired/child; INDETERMINATE unsafe ledger.
 * Compare all fields/bytes; enforce original descriptor policy and guard, then
 * persist STARTED before effects. No matching by name or input-text inference.
 */
cog_result_t cog_runtime_answer(cog_runtime_t *runtime, const cog_runtime_policy_t *policy, bool allow);
/** @brief Explicit constructor operation preparing resolved future generation.
 * @param[in,out] runtime Owner. @param[in] provider Immutable new settings.
 * @param[in] instructions Optional matching future snapshot, deep-copied during
 * preparation. Required in PINNED mode if model/template ID/revision changes;
 * DISABLED requires NULL. Same-model/template refresh may reuse current context.
 * Candidate may not increase tool permissions, budgets or source-root authority.
 * @return OK; NULL_PTR; INVALID_ARG nonincreasing generation; AGAIN future pending;
 * INVALID_STATE offline/shutdown; LIMIT 64 eligible attempts/capacity; NOMEM;
 * UNSUPPORTED compatibility; provider preflight/cleanup errors. Failed candidate
 * leaves old generation unless cleanup failure freezes admission. Successful
 * candidate endpoint borrows persist until close; failed borrow ends at return.
 * Current whole turn stays pinned. Prepare the provider and matching snapshot
 * as ONE candidate; pure preflight uses that pair, never current context with a
 * future model. At safe idle, checkpoint the replacement context binding first,
 * then switch provider/context/core references together and disconnect old once.
 * Failed/uncertain checkpoint freezes admission and retains evidence; no send
 * uses a mismatched pair. This call only prepares/publishes the future candidate;
 * it performs no input/send/store/policy/source mutation itself. Deferred owner
 * activation performs the guarded checkpoint before exposing the new pair.
 */
cog_result_t cog_runtime_reload(cog_runtime_t *runtime,
    const cog_runtime_provider_config_t *provider,
    const cog_instruction_snapshot_t *instructions);
/** @brief Select safe same-session fork point and its historical context.
 * @param[in,out] runtime Owner. @param[in] node_id Existing ID,0 active leaf.
 * @return OK; NULL_PTR; NOT_FOUND; AGAIN active/queued/view/change; INVALID_STATE
 * unsafe/offline; INDETERMINATE. No ancestor rewrite or new branch until fresh input.
 */
cog_result_t cog_runtime_fork(cog_runtime_t *runtime, uint64_t node_id);
/** @brief Recovery-only operator reconciliation, never executes tools.
 * @param[in,out] runtime RECOVER owner. @param[in] node_id Exact assistant ID.
 * @param[in] call_id Exact call. @param[in] outcome Verified SUCCEEDED/FAILED/
 * CANCELLED or ACKNOWLEDGED preserving uncertainty. @param[in] evidence UTF-8.
 * @param[in] bytes 1..4095. @return OK after durable result+audit; NULL_PTR;
 * INVALID_ARG; NOT_FOUND; INVALID_STATE closed/wrong mode; LIMIT; IO;
 * INDETERMINATE. Failure freezes reconciliation; later explicit RESUME required.
 */
cog_result_t cog_runtime_reconcile(cog_runtime_t *runtime, uint64_t node_id,
    const char *call_id, cog_tool_execution_state_t outcome, const char *evidence, size_t bytes);
/** @brief Bind an explicit named role/skill choice to one proposed child task.
 * @param[in,out] runtime Parent owner, PINNED/ADOPT with pinned catalog.
 * @param[in] assistant_node_id Original delegate assistant group.
 * @param[in] call_id Exact unstarted delegate call ID.
 * @param[in] task_index Zero-based task ordinal in immutable validated arguments.
 * @param[in] agent_name Exact catalog role, NULL inherits parent role.
 * @param[in] skills Additional exact skill names, NULL iff count0.
 * @param[in] count Bounded number; duplicates INVALID_ARG.
 * @return OK copied once; NULL_PTR; NOT_FOUND call/role/skill; INVALID_ARG ordinal;
 * INVALID_STATE started/closed/non-delegate; PROTOCOL conflicting repeated choice;
 * DENIED unavailable capability; UNSUPPORTED model/template; LIMIT; NOMEM.
 * Explicit preparation constructor, no spawn or STARTED. Resolve from parent's
 * pinned catalog; intersect parent effective original bindings, role restrictions,
 * child ceiling and remaining deadlines/credits before admission. Child ASK denies
 * noninteractively; no recursive delegation. Model delegate task optional agent
 * and skills fields use the SAME path; a differing host/model choice is refused,
 * never overwritten. Resolve exact bytes locally and in native capsule, with
 * coordinator-assigned context IDs, before child launch. No child source reread.
 */
cog_result_t cog_runtime_child_select(cog_runtime_t *runtime,
    uint64_t assistant_node_id, const char *call_id, size_t task_index,
    const char *agent_name, const char *const *skills, size_t count);
/** @brief Explicit constructor for replacement instruction context.
 * @param[out] out Owned candidate, NULL on failure. @param[in,out] runtime Owner.
 * @param[in] snapshot Complete candidate deep copied; original static descriptors
 * remain borrowed through candidate, and after publish through runtime close.
 * @return OK; NULL_PTR; INVALID_STATE offline/frozen; INVALID_ARG nonfresh ID;
 * LIMIT; NOMEM; DENIED capability increase; UNSUPPORTED model/template/schema;
 * PROTOCOL. Successful prepare reserves/burns the fresh snapshot ID in the live
 * owner high-water and captures exact branch/context/generation. Failure burns
 * no ID. No store/source/provider IO; abandoned successful candidates do not
 * make their reserved IDs reusable in this live owner.
 * Refresh, activation and resource additions share this path. No file can grant
 * ALLOW. Explicit constructor may allocate; not a side effect or publication.
 */
cog_result_t cog_runtime_instruction_prepare(cog_runtime_instruction_change_t **out,
    cog_runtime_t *runtime, const cog_instruction_snapshot_t *snapshot);
/** @brief Publish prepared replacement after closing old proposal group.
 * @param[in,out] runtime Matching owner. @param[in,out] change Consumed/NULL on OK.
 * @return OK; NULL_PTR; AGAIN active effect/provider/unclosed group/view;
 * INVALID_STATE stale branch/generation/context; LIMIT; IO/INDETERMINATE freezes
 * admission. Validate before writes; terminal result/ledger/new context binding
 * save atomically. Admitted effects keep old context; next bounded continuation
 * uses replacement without replay. Publish/ack new view before next send.
 */
cog_result_t cog_runtime_instruction_publish(cog_runtime_t *runtime, cog_runtime_instruction_change_t **change);
/** @brief Discard unpublished candidate without changing runtime.
 * @param[in,out] change Owned slot, NULL pointee allowed. @return OK; NULL_PTR.
 * A valid candidate is always consumed and pointer cleared. */
cog_result_t cog_runtime_instruction_change_free(cog_runtime_instruction_change_t **change);
/** @brief Checked close after drain or before begin.
 * @param[in,out] runtime Owned slot, NULL pointee allowed; NULL on consumption.
 * @param[in] primary Prior failure preserved. @return primary/first cleanup error;
 * NULL_PTR; INVALID_STATE undrained. Late guard-release refusal can retain a
 * cleanup-only nonNULL owner, never executable. Detach consumed resources once;
 * all endpoint/transport/tool/store borrows end only at complete consumption.
 * No blocking wait, replay, signal changes or logging to terminal.
 */
cog_result_t cog_runtime_close(cog_runtime_t **runtime, cog_result_t primary);
#ifdef __cplusplus
}
#endif
#endif

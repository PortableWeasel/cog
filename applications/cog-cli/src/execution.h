/**
 * @file execution.h
 * @brief Private one-shot application executor, local borrow and staged handoff.
 *
 * Design N pass1. The ordinary public app_new/run/free contract remains valid.
 * The installed multi-context host uses this resolved admission/drive boundary
 * rather than nested public RESUME constructors or a second state machine.
 * One owner thread, static K/L dispatch and Q core stepping; no bodies yet.
 */
#ifndef COG_CLI_EXECUTION_H
#define COG_CLI_EXECUTION_H
#include "configuration.h"
#include "instruction_transfer.h"
#include "observation.h"
#include "control.h"
#include "projection.h"
#include <cog/agent.h>
#include <cog/orchestrator.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum cog_cli_execution_kind {
    COG_CLI_EXECUTION_ROOT = 0,
    COG_CLI_EXECUTION_LOCAL_CHILD,
    COG_CLI_EXECUTION_REMOTE_CHILD,
    COG_CLI_EXECUTION_STAGED
} cog_cli_execution_kind_t;

/** All task/config/identity authority is validated by the host before this call.
 * A local child uses a live registry borrow; remote/staged modes never accept
 * native session pointers or a string ID as proof of an execution lease.
 */
typedef struct cog_cli_execution_admission {
    cog_cli_execution_kind_t kind;
    cog_cli_configuration_t *configuration; /**< Owned resolved generation,
       consumed on EVERY constructor return. Caller detaches its pointer before
       call. No file/env/argv rerun or credential resolver callback. */
    cog_session_t *local_session; /**< LOCAL_CHILD only, exact borrowed object;
       app never frees it. Borrow remains the host's on constructor failure. */
    const char *session_id; /**< REMOTE_CHILD/STAGED exact copied identity. */
    const char *parent_session_id; /**< Child only: exact immutable parent link,
       already copied in registry admission, not an arbitrary worker choice. */
    const char *task; /**< LOCAL/REMOTE_CHILD only, bounded copied task admitted
       once after initial checkpoint/guard. Never inherited root prompt/queue. */
    const char *provenance; /**< Child-only host-generated bounded SYSTEM context
       (≤4095 UTF-8 bytes, no NUL): real parent call/group/attempt/members. Copied
       and saved with initial child checkpoint before any task/model/tool. */
    size_t provenance_len;
    uint64_t configuration_generation; /**< Pinned parent admission generation,
       nonzero; no relabel on later parent reload. */
    uint32_t reload_attempts_used; /**< STAGED exact inherited logical R12 count,
       0..64 including FAILED attempts. ROOT/CHILD0. Constructor tree seeds
       effective values but never resets this remaining global attempt budget. */
    uint64_t absolute_deadline_ms; /**< Finite child monotonic deadline; capped
       by remaining parent turn/tool deadline, never restarted on receipt/retry.
       ROOT/STAGED zero: existing per-turn K deadline begins on fresh input. */
    cog_store_owner_t expected, next; /**< STAGED only: exact CGH1 tuples. */
    cog_store_checkpoint_t checkpoint; /**< STAGED exact nonzero generation. */
    cog_cli_observer_t *observer; /**< Borrowed bridge through app_free/net_free. */
    cog_cli_instruction_capsule_t instructions; /**< Borrowed only during new;
       constructor validates/restores into owned snapshot before returning. Empty
       tuple means explicit instruction-free mode. Child snapshot already reflects
       policy/limit intersection. STAGED matches exact checkpoint CGIM attachment,
       including complete bytes/identity, not latest context or source rereads. */
} cog_cli_execution_admission_t;

/**
 * @brief Construct an executor from resolved, intersected immutable options.
 * @param[out] out App, NULL on failure; match existing consuming app_free.
 * @param[in,out] admission Consumes configuration and sets its pointer NULL on
 * every return; all other inputs borrowed only through call unless noted.
 * @return OK; NULL_PTR; INVALID_ARG mode/identity; LIMIT; NOMEM; IO;
 * INDETERMINATE; UNSUPPORTED; existing J/G/preflight admission failures.
 * Instruction-aware construction restores CGIM against actual descriptors and
 * template without source reads. Initial child checkpoint includes context before
 * task/model/tool. STAGED compares exact attachment before PREPARED; mismatch
 * fails, never repairs/adopts latest. Every failed construction frees partial
 * snapshot ownership; caller capsule remains borrowed and unconsumed.
 * ROOT uses normal initial checkpoint/explicit RESUME logic once. LOCAL_CHILD
 * uses the exact supplied session and creates its guarded initial checkpoint;
 * remote child creates/guards its copied identity and exact parent link, refusing
 * all existing identities. Neither child reads stdin/queue/selectors or delegates.
 * STAGED loads ONLY checkpoint into a fresh owned tree and constructs workspace,
 * provider/config/codec resources plus pure provider preflight. It performs NO
 * owner CAS, guard acquisition, save, branch switch, prompt/queue enqueue, model,
 * tool or UI publication. Startup prompt/queue are explicitly cleared/consumed,
 * never replayed on activation. Expected durable owner/checkpoint are checked;
 * coordinator alone later commits CAS. Any failure consumes partial resources,
 * preserves durable authority/data, and never thaws a source or starts work.
 */
cog_result_t cog_cli_execution_new(cog_cli_app_t **out,
    cog_cli_execution_admission_t *admission);

/** @brief Begin private nonrecursive driving of an ordinary ACTIVE executor.
 * @param[in,out] app Owner, not staged, not running/driven from another stack.
 * @return OK; NULL_PTR; INVALID_STATE. Same run lifecycle/leases; no dispatch.
 * STAGED may begin only after separately guarded activation (declared in the
 * staged-handoff subset); successful construction alone never grants execution.
 */
cog_result_t cog_cli_execution_begin(cog_cli_app_t *app);

/** @brief One bounded K dispatch/service slice, without idle/stdin/wait hooks.
 * @param[in,out] app Begun app, one owner. @param[out] info Core step information.
 * @return OK; AGAIN; NULL_PTR; INVALID_STATE; first driver failure. Continues
 * provider/tool/process drain after failure until info.done; no new work then.
 * Applies existing whole-turn reload pinning, durable result barriers and
 * accepted-output retention. Host input/policy/projection are explicit bounded
 * commands, not competing FILE readers. No yield discards an input/event lease.
 * Once core step reports done, call drive_end ONCE then enter private DRAINING;
 * never call core_step again or restart it to finish cleanup. The direct bounded
 * drain helper below drives retained provider/tool work independently.
 */
cog_result_t cog_cli_execution_step(cog_cli_app_t *app, cog_core_step_info_t *info);

/** @brief One direct cancel/drain unit after core driver ownership ends.
 * @param[in,out] app DRAINING executor. @param[out] info done only after actual
 * provider/tool/process/result work disposed or explicit frozen recovery error.
 * @return OK; AGAIN; NULL_PTR; INVALID_STATE wrong phase; retained primary error.
 * Poll at most one provider delta/tool unit and close at most one unstarted call
 * per invocation; no positive poll/sleep/while-until-done, allocation or next
 * model/tool admission. Preserve actual accepted incomplete text/result and
 * existing durable barriers. Public run/drain may loop the SAME helper with its
 * existing bounded host wait; multi-context host never invokes that loop.
 */
cog_result_t cog_cli_execution_drain_step(cog_cli_app_t *app,
    cog_core_step_info_t *info);

/** @brief End a done drive and return retained primary status.
 * @param[in,out] app Done executor, still owned until app_free.
 * @return OK; NULL_PTR; INVALID_STATE not done/reentrant; first driver failure.
 * Ending does not return the registry borrow, release result or grant a restart.
 * Host returns exact local borrow after subsequent consuming app_free completes.
 */
cog_result_t cog_cli_execution_end(cog_cli_app_t *app);

/** @brief Checked consuming destruction, shared by public app_free wrapper.
 * @param[in,out] app NULL pointee OK; set NULL after consuming all resources.
 * @param[in] primary Existing failure wins over all later cleanup results.
 * @return primary or first cleanup error; NULL_PTR; preflight INVALID_STATE
 * leaves live/undrained app untouched. Never-begun/staged quiescent app can be
 * consumed. A late nonconsuming guard-release refusal retains a cleanup-only
 * app, with every already-consumed subordinate pointer cleared. Return actual
 * refusal unless primary already failed; the nonNULL output then identifies
 * remaining ownership regardless of result precedence. No execution may resume;
 * after outstanding guard refusal is resolved, retry only checked destruction.
 * Consuming guard cleanup errors detach the guard and continue teardown; on
 * complete consumption output is NULL even when primary/cleanup error is returned.
 * Destroys retained tool handles, then installed delegation context while app
 * lives, workspace/core/provider/config/obs/store/session in declared order.
 * Every provider disconnect consumes once on every return; secret cleared after
 * return. Borrowed exact local session NOT destroyed; caller returns its stored
 * registry borrow only after this consumption. Bridge remains until net_free.
 * Public app_free keeps its consuming void/sanitized-report contract; runtime
 * uses this checked result rather than losing cleanup error in a void wrapper.
 */
cog_result_t cog_cli_execution_close(cog_cli_app_t **app, cog_result_t primary);

/**
 * @brief Freeze a complete idle source and save its guarded exact checkpoint.
 * @param[in,out] app Ordinary active owner, no current drive callback recursion.
 * @param[in] expected Exact admitted current owner.
 * @param[out] checkpoint New nonzero checkpoint, unchanged on failure.
 * @return OK; NULL_PTR; AGAIN unsafe active/queued/pending state; INVALID_STATE
 * frozen-without-matching-success/recovery/staged/stale owner; LIMIT; IO;
 * INDETERMINATE/store error.
 * Host first checks no child attempts/results/intents/message/policy borrows.
 * App checks no provider turn, retained delta/output/event, execution, STARTED/
 * uncertain ledger, pending result/assistant, input, future reload/notice or
 * incomplete path. No active-handle transfer or cancellation disguised as idle.
 * Freeze input before guarded checkpoint_save. OK retains original guard and
 * immutable checkpoint until release; exact duplicate request returns same
 * checkpoint, never saves again. Any save error freezes without guessing its
 * commit outcome. Source may not continue because a route still names it.
 */
cog_result_t cog_cli_execution_quiesce(cog_cli_app_t *app,
    cog_store_owner_t expected, cog_store_checkpoint_t *checkpoint);

/** @brief Release only the quiesced source's exact guard after target PREPARED.
 * @param[in,out] app Frozen source. @param[in] checkpoint Retained exact token.
 * @return OK; NULL_PTR; INVALID_STATE correlation/guard still borrowed; IO or
 * other consuming guard cleanup error. OK detaches the guard and records
 * successful release. INVALID_STATE retains a live guard. Consuming error also
 * detaches it, retains failure and leaves the source frozen, WITHOUT recording
 * successful release or granting CAS. Never retry a consumed handle or turn a
 * repeated call into OK after failed release. A later error cannot resurrect
 * the released epoch; checked close must still consume remaining resources. */
cog_result_t cog_cli_execution_release_source(cog_cli_app_t *app,
    cog_store_checkpoint_t checkpoint);

/** @brief Acquire next guard for a PREPARED target after exact committed CAS.
 * @param[in,out] app STAGED exact-checkpoint app; not driven/published.
 * @param[in] next Retained expected next owner. @param[in] checkpoint Exact token.
 * @return OK; NULL_PTR; INVALID_STATE tuple/checkpoint/stage mismatch; G error.
 * Verify owner_read, acquire normal guard and remain STAGED. No save, input,
 * provider send or tool start. OK permits READY, never ACTIVATE by itself.
 */
cog_result_t cog_cli_execution_stage_guard(cog_cli_app_t *app,
    cog_store_owner_t next, cog_store_checkpoint_t checkpoint);

/** @brief Activate guarded target only after trusted exact ACTIVATE correlation.
 * @param[in,out] app Guarded STAGED app. @param[in] next Exact retained owner.
 * @param[in] checkpoint Exact loaded/published token.
 * @return OK (exact duplicate idempotent, no new input); NULL_PTR; INVALID_STATE.
 * Host must already have checked authenticated coordinator role/incarnation,
 * phase identity and successful exact publish_owner; a decoded tuple is not
 * proof. Activation changes only admission state, no save/model/tool/input replay.
 * Startup prompt/queue remain consumed; fresh explicit input is required.
 */
cog_result_t cog_cli_execution_stage_activate(cog_cli_app_t *app,
    cog_store_owner_t next, cog_store_checkpoint_t checkpoint);

/** @brief Apply explicit coordinator-resolved R12 candidate at owning executor.
 * @param[in,out] app Ordinary primary owner, not child/staged/frozen.
 * @param[in,out] candidate Owned already-resolved generation, consumed and NULL
 * on EVERY return. net belongs to this app; no resolver authority is transferred.
 * @param[in] attempt Authoritative eligible coordinator attempt1..64, greater
 * than previously observed; candidate generation must equal attempt+1. Local
 * failed prepares may consume IDs without reaching worker; never require every
 * unused generation to arrive. Preserve count through handoff. Reject stale/
 * over-budget before provider work, exact wire retry answered from cached result.
 * @param[out] published Generation only on success, unchanged on failure.
 * @return Existing R12 errors, INVALID_ARG if any field outside the THREE
 * reloadable fields differs. Same candidate tree validation/provider prepare/
 * preflight/commit/rollback/publication and checked cleanup ordering as app_reload.
 * No extra re-resolution, callbacks, store/policy/workspace/limit changes or
 * implicit activation. At most current/future logical generations, pinned whole
 * turn; copied coordinator mirror is bounded transport state, never a third
 * generation or provider. Cleanup error freezes the owner and reaches runtime.
 */
cog_result_t cog_cli_execution_reload_resolved(cog_cli_app_t *app,
    cog_cli_configuration_t **candidate, uint32_t attempt, uint64_t *published);

/** @brief Borrow one actual retained executor view between dispatch slices.
 * @param[in,out] app Begun primary executor with publication_pending.
 * @param[out] mutation_serial Exact nonreused local serial, not CGR cursor.
 * @param[out] view Complete actual history+active/incomplete snapshot.
 * @return OK; NULL_PTR; AGAIN no pending view; LIMIT actual view cannot fit.
 * View borrows a DEDICATED frozen copy arena, distinct from actual mutable drain
 * buffers. It is immutable until exact commit or explicit failed-feed teardown;
 * This publication barrier applies to PRIMARY conversation. Child conversation
 * remains actual durable K state but is not a root CGD1 projection; children
 * publish bounded CGA status/result, never wait for an undeclared conversation
 * ACK. Child ASK is noninteractive identified no-effect DENY before STARTED.
 * later drain mutations set newer_view_dirty and cannot overwrite this copy.
 * A private static PUBLICATION state gates headless effects: save/stage new user,
 * accepted TEXT, terminal assistant/result and policy/control changes, then
 * yield before provider send/tool start/next input. Split those phases INSIDE
 * existing input/finish/STARTED handlers; one-dispatch alone is insufficient.
 * One-dispatch headless slices
 * leave no second ordinary handler between mutation and capture. Do not fwrite
 * uncommitted headless text. Existing standalone rendering remains unchanged.
 * Control/cancel/provider/tool drain and deadlines still make bounded progress;
 * lost projection feed cannot strand teardown or permit further ordinary work.
 */
cog_result_t cog_cli_execution_copy_view(cog_cli_app_t *app,
    uint64_t *mutation_serial, cog_cli_display_snapshot_t *view);

/** @brief Commit only the exact pending view after canonical replay append.
 * @param[in,out] app Owner. @param[in] mutation_serial Exact copied view.
 * @param[in] generation Canonical coordinator lifetime. @param[in] sequence
 * Exact committed CGR1 sequence, neither guessed nor raw app display counter.
 * @return OK (exact duplicate idempotent); NULL_PTR; INVALID_STATE mismatch;
 * PROTOCOL conflicting duplicate. No allocation/effect/input in this call;
 * static publication event resumes next ordinary action on a later owner slice.
 * A newer_view_dirty marker forces capture/commit of newer actual drain state
 * before ordinary work resumes. Lost parent/projection feed may end teardown
 * only with retained error and durable incomplete state, never forged commit.
 * Identity exhaustion freezes without dropping accepted output; source app view
 * remains inspectable/durable even if no successful wire publication occurred.
 */
cog_result_t cog_cli_execution_commit_view(cog_cli_app_t *app,
    uint64_t mutation_serial, uint64_t generation, uint64_t sequence);

/** @brief Copy actual current ASK metadata, no approval or prompt output.
 * @param[in] app Owner. @param[out] serial Nonreused executor prompt serial.
 * @param[out] view Nonce0 until coordinator assignment, exact session/node/
 * ordinal/generation/deadline/static descriptor name. Copy before next mutation.
 * @return OK; NULL_PTR; AGAIN no current prompt. No allocation or state changes.
 * Worker POLICY_STATE carries this structure; do not encode it in status text.
 */
cog_result_t cog_cli_execution_copy_policy(const cog_cli_app_t *app,
    uint64_t *serial, cog_cli_policy_view_t *view);

/** @brief Copy real active/published R12 generation identities, never infer from
 * a publication ACK. @param[in] app Owner. @param[out] active Current pinned
 * provider generation; @param[out] published Future if present else current.
 * @param[out] attempts_used Logical attempts including failed/discarded candidates.
 * @return OK; NULL_PTR. No callbacks/allocation/mutation. Remote view/delegate
 * frames carry these facts so old-turn delegation clones the OLD pinned mirror. */
cog_result_t cog_cli_execution_copy_configuration(const cog_cli_app_t *app,
    uint64_t *active, uint64_t *published, uint32_t *attempts_used);

/** @brief Apply one independently role-checked/deduped executor command.
 * @param[in,out] app Current owner, outside callbacks; never staged/frozen source.
 * @param[in] command Bounded decoded body. @param[in] owner Exact current tuple.
 * @param[in] executor_serial POLICY prompt serial; CONTINUE/FORK actual source
 * mutation serial;0 otherwise. Coordinator validates global CGR anchor; owner
 * validates this actual executor version, not unrelated agent/control ring IDs.
 * @return OK admission once; NULL_PTR; INVALID_STATE stale owner/state/prompt;
 * DENIED inappropriate child command; OVERFLOW/AGAIN nothing accepted; command
 * validation/IO/LIMIT error. Caller retains exact original/result in bounded
 * dedup cache so retry never repeats this admission. Root supports SUBMIT/
 * CONTINUE/POLICY/CANCEL/FORK; child CANCEL only (ASK denies noninteractively).
 * Coordinator-only RELOAD/
 * HANDOFF/SHUTDOWN/RESYNC do not become model text. Check full byte validity and
 * same-session safe fork or current incomplete leaf; no queued authorization.
 * POLICY checks real PROPOSED call+guard before any STARTED/effect. Controller
 * authorization linearizes at coordinator's valid-lease forward(send OK), not
 * remote apply time; once forwarded exact grant may finish after lease expiry.
 * Owner does not query/reconstruct coordinator role state. Cancellation/deadline/
 * changed call still refuses it; old grant never becomes replacement's decision.
 * No command
 * can change fixed child policy or use a nonce for another prompt/attempt.
 */
cog_result_t cog_cli_execution_apply_command(cog_cli_app_t *app,
    const cog_cli_command_t *command, cog_store_owner_t owner,
    uint64_t executor_serial);

#ifdef __cplusplus
}
#endif
#endif

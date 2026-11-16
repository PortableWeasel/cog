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
#include "observation.h"
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
    uint64_t configuration_generation; /**< Pinned parent admission generation,
       nonzero; no relabel on later parent reload. */
    uint64_t absolute_deadline_ms; /**< Finite child monotonic deadline; capped
       by remaining parent turn/tool deadline, never restarted on receipt/retry.
       ROOT/STAGED zero: existing per-turn K deadline begins on fresh input. */
    cog_store_owner_t expected, next; /**< STAGED only: exact CGH1 tuples. */
    cog_store_checkpoint_t checkpoint; /**< STAGED exact nonzero generation. */
    cog_cli_observer_t *observer; /**< Borrowed bridge through app_free/net_free. */
} cog_cli_execution_admission_t;

/**
 * @brief Construct an executor from resolved, intersected immutable options.
 * @param[out] out App, NULL on failure; match existing consuming app_free.
 * @param[in,out] admission Consumes configuration and sets its pointer NULL on
 * every return; all other inputs borrowed only through call unless noted.
 * @return OK; NULL_PTR; INVALID_ARG mode/identity; LIMIT; NOMEM; IO;
 * INDETERMINATE; UNSUPPORTED; existing J/G/preflight admission failures.
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
 */
cog_result_t cog_cli_execution_step(cog_cli_app_t *app, cog_core_step_info_t *info);

/** @brief End a done drive and return retained primary status.
 * @param[in,out] app Done executor, still owned until app_free.
 * @return OK; NULL_PTR; INVALID_STATE not done/reentrant; first driver failure.
 * Ending does not return the registry borrow, release result or grant a restart.
 * Host returns exact local borrow after subsequent consuming app_free completes.
 */
cog_result_t cog_cli_execution_end(cog_cli_app_t *app);

#ifdef __cplusplus
}
#endif
#endif

/** @file handoff.h
 * @brief Private one-at-a-time quiescent primary-session handoff coordinator.
 * Single owner. G retains all durable authority; routes/control messages grant
 * no lease. No live child/handle or cross-host migration, no old-epoch rollback.
 */
#ifndef COG_CLI_HANDOFF_H
#define COG_CLI_HANDOFF_H
#include "host_runtime.h"
#include "execution.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct cog_cli_handoff cog_cli_handoff_t;
/** @brief Reserve fixed phase/retry/correlation storage, no handoff effects.
 * @param[out] out NULL on failure, match close. @param[in,out] runtime Owner
 * outlives this coordinator. @param[in,out] orchestrator Worker fabric owner.
 * @param[in,out] catalog Independently owned fixed shared-store context, outlives
 * orchestrator+handoff, never borrows an executor's soon-retired store pointer.
 * @param[in] timeout_ms Finite whole-transaction deadline, no retry extension.
 * @return OK; NULL_PTR; INVALID_ARG; NOMEM; LIMIT. No source freeze/CAS/fork.
 */
cog_result_t cog_cli_handoff_new(cog_cli_handoff_t **out,
    cog_cli_host_runtime_t *runtime, cog_orchestrator_t *orchestrator,
    cog_store_t *catalog, uint32_t timeout_ms);
/** @brief Admit explicit move of idle complete primary to healthy worker/local.
 * @param[in,out] handoff Owner; @param[in] target Managed worker or both0 local.
 * @return OK; NULL_PTR; AGAIN unsafe pending work; INVALID_STATE same target,
 * frozen/recovery/already migrating; NOT_FOUND stale managed ID; LIMIT identity/
 * epoch; IO entropy/profile. Outputs/effects unchanged on preflight refusal.
 * Settle all R12 current/future/publication/notice ownership first. Reject child
 * attempts/retained results, pending policy/input/message/view, active provider/
 * tool/partial ledger. Reserve target profile/app and phase/control identity
 * budget; next.epoch exactly expected+1, fresh nonzero target instance. No counter
 * reset of global child/request/command limits. No route implies a live guard.
 */
cog_result_t cog_cli_handoff_request(cog_cli_handoff_t *handoff,
    cog_worker_id_t target);
/** @brief Advance one bounded phase/ACK/G operation on actual owner thread.
 * @param[in,out] handoff Owner; @param[out] done True quiescent/no pending phase;
 * @param[out] deadline_ms Earliest absolute retry/deadline, no unbounded host wait.
 * @return OK; AGAIN; NULL_PTR; primary phase/store/driver error. Error preserves
 * ownership/phase for explicit cleanup, not guessed rollback. Source checkpoint
 * then PREPARED, source guard release, exact CAS, target acquire/READY, exact route
 * publication then ACTIVATE; each duplicate echoes same source/target/tuple/token.
 * Post-CAS route retry keeps same admitted owner. ACTIVATE ACK is outer reply_to
 * correlated and cached by target; lost ACK retry never reloads latest, repeats
 * CAS/acquire/save or replays startup input. Forward no fresh input until ACK.
 * Abort/failure never thaws source. Initial profile requires host shutdown/close,
 * then fresh explicit --resume (or --recover --fenced for uncertain effects),
 * existing L fresh+1 epoch/guard admission; there is NO in-place reclaim opcode.
 * INDETERMINATE stays frozen for explicit L
 * recovery, no owner_read guess or lost-ACK automatic task reconstruction.
 */
cog_result_t cog_cli_handoff_step(cog_cli_handoff_t *handoff, bool *done,
    uint64_t *deadline_ms);
/** @brief Freeze/cancel one phase without rolling back committed authority.
 * @param[in,out] handoff Owner. @return OK; NULL_PTR. Idempotent; keep stepping
 * until owned target/source resources cleaned and no pending send/guard borrow.
 * No automatic thaw, resume/save/model call or replacement task. */
cog_result_t cog_cli_handoff_abort(cog_cli_handoff_t *handoff);
/** @brief Consume idle/disposed coordinator, leave live one on refusal.
 * @param[in,out] handoff NULL pointee OK; @param[in] primary First error.
 * @return primary/cleanup error; NULL_PTR; INVALID_STATE live phase/borrow.
 * Does not destroy borrowed runtime/orchestrator/catalog or silently release a
 * live executor's guard. Named staged app cleanup follows consumed-handle rules. */
cog_result_t cog_cli_handoff_close(cog_cli_handoff_t **handoff,
    cog_result_t primary);
#ifdef __cplusplus
}
#endif
#endif

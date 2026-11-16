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
/** Application-private CGH1 role/phase mapping (see
 * docs/REAL_CLIENT_HANDOFF_PHASES.md): coordinator QUIESCE to source receives
 * CHECKPOINTED. After dedicated purpose3 profile ACK, CHECKPOINTED to target
 * performs exact staged construction and receives PREPARED. Coordinator then
 * forwards PREPARED to the frozen source; source releases its exact guard and
 * echoes PREPARED only after successful actual release. Those two PREPARED
 * replies are distinguished by exact request wire, managed peer and phase.
 * Only then coordinator CASes; COMMIT goes ONLY to target for guard acquisition,
 * receives READY, then exact route publication precedes ACTIVATE/ACTIVATE echo.
 * Every remote reply is HANDOFF lane with outer reply_to naming its request;
 * exact native binding/session/source/target/expected/next/checkpoint and current
 * retained phase must match. Send-OK never substitutes for an operation ACK.
 * Local endpoints use actual direct operation returns, never fabricated replies.
 * Duplicate retained phases replay their immutable reply without save/release/
 * CAS/acquire/activation repetition. Unknown retired phases cannot resurrect work.
 * No failure, timeout or ABORT restores source admission or old owner authority.
 * After successful target ACTIVATE/echo, request disposal of the frozen old
 * source with a fresh HANDOFF-lane ABORT carrying the SAME transaction tuple
 * and checkpoint. This is resource retirement, never rollback or target abort.
 * ABORT reply echoes reply_to only AFTER actual executor/handle/guard/provider/
 * configuration/net/observer cleanup succeeded; request_shutdown/send-OK/reap
 * alone cannot produce this receipt. Retain exact request/final reply in the
 * existing slot HANDOFF cache OUTSIDE the disposed app until consumption.
 * Coordinator cannot reuse the old physical executor slot before matching
 * successful final ABORT receipt (local source requires actual checked close).
 * Physical resource reuse is NOT receipt retirement: preserve HANDOFF key,
 * counters/original/reply through reuse. Subsequent PROFILE/bootstrap may use
 * disposed resources but NEVER consumes/overwrites that distinct lane proof;
 * only a higher valid SAME HANDOFF stream request after final consumption or
 * native lifetime retirement can retire it. No unrelated-key slot rebind while
 * the proof is retained; pressure refuses new work rather than erasing proof.
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

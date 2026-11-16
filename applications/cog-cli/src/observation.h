/** @file observation.h @brief Private bounded host capture and app timing. */
#ifndef COG_CLI_OBSERVATION_H
#define COG_CLI_OBSERVATION_H
#include <cog/obs.h>
#include <cog/net.h>
#include <cog_cli/cli.h>

#define CLI_OBSERVATION_LABEL_BYTES 128U
typedef struct cog_cli_observer cog_cli_observer_t;
/** Copied metadata only; strings already screened by the configuration owner.
 * MOCK/OPENROUTER select static names; no URL, credential, payload or call ID. */
typedef struct cog_cli_observation_scope {
    uint64_t request, configuration_generation; /**< App observation identity, not core event serial. */
    uint32_t attempt; /**< Zero-based application retry count. */
    bool retry, mock, session_available, model_available;
    char session[CLI_OBSERVATION_LABEL_BYTES], model[CLI_OBSERVATION_LABEL_BYTES];
} cog_cli_observation_scope_t;

/** Explicit constructor: reserve rows=request_slots and queue=4*slots+2 after
 * checked multiply/add/byte arithmetic. No later allocation or row eviction.
 * @return OK; NULL_PTR; INVALID_ARG for zero slots; LIMIT overflow; NOMEM.
 * *out=NULL on failure. No callbacks/obs/transport/global initialization. */
cog_result_t cog_cli_observer_new(cog_cli_observer_t **out, size_t request_slots);
/** Release constructor storage after every borrowing net is destroyed and
 * target/domain clear completed. NULL is a no-op; never drives a net context. */
void cog_cli_observer_free(cog_cli_observer_t *observer);
/** Borrow obs after app_new, before drive; NULL detaches only after all rows and
 * queued events have been cleared. No target destruction or ownership transfer.
 * Unfiltered typed DEBUG logs require the shared observation_fields vocabulary.
 * @return OK; NULL_PTR for observer; INVALID_STATE for unsafe retarget/detach. */
cog_result_t cog_cli_observer_bind(cog_cli_observer_t *observer, cog_obs_t *obs);
/** Owner prepares one fixed copied scope immediately before provider_send.
 * Required NULL arguments return NULL_PTR. Drain/check previous work first.
 * Strings are bounded/validated, never retained
 * by pointer. Failed preparation admits no wire work. @return OK or sticky error. */
cog_result_t cog_cli_observer_prepare(cog_cli_observer_t *observer,
    const cog_net_ctx_t *net, const cog_cli_observation_scope_t *scope);
/** Net constructor callback: checked monotonic callback-time stamp plus fixed
 * numeric/scope copy only. No obs, formatting, stdio, allocation or net reentry.
 * OPEN requires a bound target and matching prepared scope. Every error is sticky and returned so
 * net independently disables its feed. @return OK; explicit clock/capacity/state error. */
cog_result_t cog_cli_observer_capture(void *user, const cog_net_ctx_t *net,
    const cog_net_observation_t *observation);
/** Owner drain after EVERY provider/net operation, including cleanup, before
 * another operation/reload/retirement. Emits existing obs events/diagnostics with
 * callback timestamps and owned row labels. OPEN then admitted RETRY share time.
 * Terminal rows can be diag-cleared before reuse only after request_status says
 * NOT_FOUND (release). Active and terminal-retained rows never evict.
 * Every borrowed diagnostic string is cleared before its owning row is reused.
 * Assigns nonzero never-reused connection IDs across context/request keys.
 * Clears prepared scope after draining. On failure latch first deferred error,
 * never retry partially delivered records, discard remaining queued values as
 * an explicitly incomplete feed; cleanup may still release requests/clear rows.
 * @return NULL_PTR for observer; otherwise OK or first sticky bridge error;
 * no runtime allocation. */
cog_result_t cog_cli_observer_drain(cog_cli_observer_t *observer);
/** Query bridge-local callback/deferred error; never writes net observer_status.
 * @return NULL_PTR or first bridge error/OK. No callbacks or mutation. */
cog_result_t cog_cli_observer_status(const cog_cli_observer_t *observer);
/** Drain then clear all rows for a still-live net with no owned request IDs.
 * @pre Owner released ALL IDs, including any unobserved after feed loss.
 * The helper checks active_count and every known row's request_status; no public
 * operation can enumerate unknown retained IDs, so this ownership precondition
 * cannot be inferred from a zero active wire count.
 * Existing feed error stays latched, but rows/diagnostic borrows still clear.
 * Call before net destruction/memory reuse and before unbinding/freeing obs.
 * @return NULL_PTR for required pointers; OK/sticky error; INVALID_STATE if a
 * known ID remains owned or a wire is active. Never clears those rows early. */
cog_result_t cog_cli_observer_clear(cog_cli_observer_t *observer, const cog_net_ctx_t *net);
/** Irreversibly end the obs-target borrow during final app destruction.
 * If ordinary clear/unbind is refused, latch INVALID_STATE as an incomplete-feed
 * error, discard pending values, clear all published diagnostic borrows while
 * target is alive, then NULL target/scope. Retain numeric domain keys for later
 * release/clear; do NOT claim reusable domain or cancel/release foreign IDs.
 * Future callbacks return the sticky failure without touching the old target.
 * Host bridge memory still outlives all borrowing net contexts. App/obs may now
 * be consumed even if unrelated retained IDs remain. NOT_FOUND diagnostic rows
 * already cleared by their owner need no retry. @return first sticky error/OK;
 * NULL_PTR for observer. Owner thread only; never resumes the lost feed. */
cog_result_t cog_cli_observer_end_target(cog_cli_observer_t *observer);

/** One immutable allowlisted typed field vocabulary; match app obs constructor. */
const cog_obs_field_table_t *cog_cli_observation_fields(void);
/** Owner admission checkpoint: drain own bridge, inspect net callback status and
 * app deferred timing/sink error independently. Preserve a nonOK primary result.
 * Normalize observation AGAIN to fatal IO at the app/core drive boundary while
 * preserving raw bridge/net status; AGAIN must never become a no-progress retry.
 * NULL app returns NULL_PTR. During existing cancel/drain, latch secondary errors without interrupting
 * durable cleanup; app_run merges them only after work cleanup finishes. */
cog_result_t cog_cli_observation_check(cog_cli_app_t *app, cog_result_t primary);
/** Prepare copied pinned scope, checked start time and static provider span;
 * caller sets admitted provider state before checking a successful send's OPEN.
 * Burn a nonreused private observation request ID, including failed sends;
 * core event serial and application retry-budget semantics remain unchanged.
 * No send/tool/session mutation. @return NULL_PTR; OK or first observation error. */
cog_result_t cog_cli_observation_provider_begin(cog_cli_app_t *app);
/** End one timed attempt once, including failed send or consumed semantic FINISH.
 * Typed duration/status/HTTP and known-usage snapshot only. Never changes work
 * result, retries or ledger. Failed trace_end is not retried during cleanup.
 * NULL app is a no-op. */
void cog_cli_observation_provider_end(cog_cli_app_t *app, cog_result_t outcome, int http_status);
/** Checked timer/span immediately before admitted execution start.
 * @return NULL_PTR; OK or first observation error. */
cog_result_t cog_cli_observation_tool_begin(cog_cli_app_t *app);
/** Terminal-once duration through reap, or explicit no-execution record when no
 * tool timer was started. Static recognized tool name/node/original ordinal,
 * no call ID/arguments/output. Identity/once state suppresses duplicate terminal
 * records if durable insertion fails and a terminal handle must stay retained.
 * Latches obs errors but never skips durable result. NULL app is a no-op. */
void cog_cli_observation_tool_end(cog_cli_app_t *app, cog_result_t outcome,
    bool effects_possible, bool truncated);
/** After all providers release IDs, drain/clear/unbind before app obs is freed.
 * Host retains bridge storage until net_free completes. Returns primary first. */
cog_result_t cog_cli_observation_detach(cog_cli_app_t *app, cog_result_t primary);
#endif

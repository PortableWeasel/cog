/**
 * @file provider_vtable.h
 * @brief Public provider base and static, nonblocking dispatch contract.
 *
 * An implementation embeds cog_provider_t as its FIRST member, initializes
 * base.vtable to its static const table before publishing it, and returns
 * &implementation->base from connect. No private Cog header or guessed prefix
 * is required. The implementation owns its constructor allocation and matching
 * disconnect. Never copy or change a live base or dynamically register tables.
 *
 * Threading: one owner thread per instance, no signal-safe calls. Independent
 * instances may run on different threads; immutable tables may be shared.
 * No callbacks may recursively drive/disconnect the same provider.
 */
#ifndef COG_PROVIDER_VTABLE_H
#define COG_PROVIDER_VTABLE_H
#include <cog/provider_request.h>
#include <cog/result.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct cog_endpoint cog_endpoint_t;
typedef struct cog_provider_vtable cog_provider_vtable_t;

/** @brief Supported public first-member base for provider implementations. */
typedef struct cog_provider {
    const cog_provider_vtable_t *vtable; /**< Immutable static dispatch table. */
} cog_provider_t;

/**
 * @brief Reserve instance/workspaces; do not wait for DNS/connect/HTTP.
 * @param[in] endpoint Immutable generation, credential and config kept alive
 * until disconnect. Constructor validates backend capability and all limits.
 * @param[out] out_provider NULL on failure; owned instance on COG_OK.
 * @return NULL_PTR, INVALID_ARG (including missing required static callbacks),
 * UNSUPPORTED, NOMEM or COG_OK. Generic connect validates all nine callbacks
 * before invoking implementation connect. No wire effects
 * on failure. Called only on owner thread; no lazy dependency global init.
 */
typedef cog_result_t (*cog_provider_connect_fn)(const cog_endpoint_t *endpoint,
                                               cog_provider_t **out_provider);
/**
 * @brief Release instance including active requests and all owned buffers.
 * @param[in,out] provider NULL is COG_OK. A non-NULL handle is consumed on
 * every return, including cleanup error; never retry or use it afterward.
 * @return COG_OK, or backend cleanup error after resources are released.
 * @note Owner thread, outside callbacks; never sends/retries or waits for peer.
 * Uses the explicitly selected bounded-cancellation dependency backend.
 */
typedef cog_result_t (*cog_provider_disconnect_fn)(cog_provider_t *provider);
/** @brief History eligibility checked by no-wire request preflight. */
typedef enum cog_provider_preflight_stage {
    COG_PROVIDER_PREFLIGHT_COMPLETE = 0, /**< Every identified call has its result. */
    COG_PROVIDER_PREFLIGHT_PENDING_TOOLS /**< Exactly one final assistant call group remains open. */
} cog_provider_preflight_stage_t;

/**
 * @brief Validate a known request without accepting or starting a turn.
 * @param[in,out] provider Idle connected instance; prior FINISH consumed.
 * @param[in] request Borrowed complete selected history and current request
 * settings, including nested message/opaque bytes and advertised tools.
 * @param[in] stage COMPLETE for ordinary send eligibility; PENDING_TOOLS for
 * the selected final assistant call group awaiting tool execution/results.
 * Every message must be COMPLETE and structurally valid. A TOOL result must
 * uniquely match by tool_call_id a call in its immediately preceding assistant call group;
 * TOOL name is descriptive only. Result order may differ from call order. A later non-TOOL
 * message requires that prior group to be fully resolved. COMPLETE requires
 * every group closed. PENDING_TOOLS requires exactly one open group belonging
 * to the last assistant in the prefix, with one or more missing results; only
 * its unique matching already-recorded TOOL results may follow it. No synthetic
 * results, orphan/duplicate results or unrelated later message is permitted.
 * Reusing a textual call ID in a distinct, earlier closed group is valid.
 * @return OK when the known input fits this implementation's protocol/profile;
 * NULL_PTR; INVALID_ARG for unknown stage/structural values; INVALID_STATE for
 * an active turn, non-COMPLETE message or invalid stage/history association;
 * NOT_FOUND for unknown configured model;
 * UNSUPPORTED for model capability or continuation namespace/format/semantics;
 * PROTOCOL for malformed strict protocol data; LIMIT for constructor ceilings.
 * @post On every return no turn is accepted, no request/sequence/time advances,
 * and no transport, service, tool, application callback or external effect occurs. Existing
 * retained output and last terminal error snapshot/lifetimes stay unchanged.
 * No input is retained, modified, logged or rendered; no runtime allocation.
 * @note Uses only constructor-reserved workspace. Validate selected history,
 * model/generation, capabilities, tool declarations and all provider-owned
 * message/opaque syntax using the same compatibility/limit rules as send.
 * Success validates only currently supplied bytes: future tool output can
 * still exhaust limits. It grants no policy, guard, ledger or effect admission.
 * Caller still validates replay safety; recheck the completed request at send.
 * Owner thread, outside callbacks; no recursive call on this instance. Active
 * rejection includes a queued terminal until FINISH is consumed; preflight must
 * not poll/cancel/drain to manufacture an idle instance. Required callback and
 * unknown-stage failures are INVALID_ARG, never a structural-only fallback.
 */
typedef cog_result_t (*cog_provider_preflight_fn)(cog_provider_t *provider,
    const cog_provider_request_t *request, cog_provider_preflight_stage_t stage);

/**
 * @brief Copy validated request transitively and start one asynchronous turn.
 * @param[in,out] provider Idle instance (prior FINISH consumed).
 * @param[in] request Borrowed only until return, including nested messages,
 * tools, opaque bytes, model and strings. Caller may free/change after return.
 * @return COG_OK; NULL_PTR; INVALID_ARG/UNSUPPORTED/PROTOCOL/NOT_FOUND before network effects;
 * LIMIT if request/workspace cannot fit; INVALID_STATE if a turn is active.
 * @post Failure leaves instance idle and sends nothing. No runtime Cog allocation.
 * Send requires COMPLETE history and rejects an open group with INVALID_STATE
 * before network effects; preflight PENDING_TOOLS never authorizes send. The
 * generic structural-only request_validate does not grant history eligibility.
 * @note Owner thread; no blocking I/O or automatic retry/model substitution.
 */
typedef cog_result_t (*cog_provider_send_fn)(cog_provider_t *provider,
                                            const cog_provider_request_t *request);
/**
 * @brief Legacy text receive, nonblocking; typed clients use next().
 * @param[in,out] provider Instance with a turn; services no blocking wait.
 * @param[in,out] response Destination. OVERFLOW changes only content_len to
 * required text bytes; destination text and pending item stay unchanged.
 * @return COG_OK, AGAIN, NULL_PTR, INVALID_STATE, OVERFLOW, UNSUPPORTED for a
 * structured pending item, or terminal outcome. Error detail via error().
 * @note Owner thread. Do not mix receive/next within a turn.
 */
typedef cog_result_t (*cog_provider_receive_fn)(cog_provider_t *provider,
                                               cog_provider_response_t *response);
/**
 * @brief Advance transport/parser without waiting, with a bounded work quantum.
 * @param[in,out] provider Connected instance, including idle.
 * @param[in] now_ms Caller monotonic milliseconds, nondecreasing per instance.
 * @param[out] out_next_deadline_ms Absolute next timer; UINT64_MAX if none.
 * @return COG_OK for progress/no progress, NULL_PTR or INVALID_ARG for backwards
 * time. Per-request failures become one retained FINISH and error snapshot;
 * a fatal driver failure is returned and must be propagated by the core hook.
 * @note Owner thread; never sleeps or invokes tools. Poll even with queue work.
 * Shared-net adapters receive the same monotonic hook timestamp, service only
 * bounded work and drain only their own IDs. Hook aggregates minimum deadlines
 * and rotates adapters fairly; one blocked adapter cannot suppress another.
 */
typedef cog_result_t (*cog_provider_poll_fn)(cog_provider_t *provider,
                                            uint64_t now_ms,
                                            uint64_t *out_next_deadline_ms);
/**
 * @brief Cancel the current turn idempotently without waiting for the peer.
 * @param[in,out] provider Instance. A completed/idle turn is a COG_OK no-op.
 * @return COG_OK or NULL_PTR. Detaches network work, preserves queued deltas,
 * then enqueues exactly one FINISH/ABORTED; no post-cancel wire output/retry.
 * @note Owner thread; completion already queued wins, no second terminal item.
 */
typedef cog_result_t (*cog_provider_cancel_fn)(cog_provider_t *provider);
/**
 * @brief Copy the oldest retained typed delta, with transactional capacity retry.
 * @param[in,out] provider Instance with a turn.
 * @param[out] out_delta Metadata; unchanged unless COG_OK.
 * @param[out] storage Caller bytes; may be NULL iff capacity is zero.
 * @param[in] capacity Writable storage bytes; includes string terminators.
 * @param[out] out_required Exact complete storage requirement (may be zero).
 * @return COG_OK consumes exactly one item; AGAIN if none ready (required=0);
 * OVERFLOW sets only required, writes no storage and retains SAME item/sequence;
 * NULL_PTR/INVALID_ARG for invalid pointers; INVALID_STATE before send or after
 * terminal consumed. Failed/cancelled turns deliver FINISH with outcome.
 * @note No allocation or service side effects. Owner thread. Lifetime of all
 * output pointers is caller storage, independent of provider destruction.
 */
typedef cog_result_t (*cog_provider_next_fn)(cog_provider_t *provider,
                                            cog_provider_delta_t *out_delta,
                                            void *storage, size_t capacity,
                                            size_t *out_required);
/**
 * @brief Snapshot sanitized detail of the most recent terminal turn error.
 * @param[in] provider Connected instance.
 * @param[out] out_error Shallow snapshot; message valid until next send/free.
 * @return COG_OK; NOT_FOUND if no error; NULL_PTR on NULL. No mutation/wait.
 * @note Owner thread; caller copies strings before persisting or sharing.
 * Preflight validation returns its own result directly and neither creates nor
 * clears this terminal-turn error snapshot or shortens its message lifetime.
 */
typedef cog_result_t (*cog_provider_error_fn)(const cog_provider_t *provider,
                                             cog_provider_error_t *out_error);

/** @brief All nine callbacks are required; table is static const. */
struct cog_provider_vtable {
    cog_provider_connect_fn connect;
    cog_provider_disconnect_fn disconnect;
    cog_provider_send_fn send;
    cog_provider_receive_fn receive;
    cog_provider_poll_fn poll;
    cog_provider_cancel_fn cancel;
    cog_provider_next_fn next;
    cog_provider_error_fn error;
    cog_provider_preflight_fn preflight;
};

/** @copydoc cog_provider_connect_fn */
cog_result_t cog_provider_connect(const cog_endpoint_t *endpoint,
                                  cog_provider_t **out_provider);
/** @copydoc cog_provider_disconnect_fn */
cog_result_t cog_provider_disconnect(cog_provider_t *provider);
/** @copydoc cog_provider_preflight_fn */
cog_result_t cog_provider_preflight(cog_provider_t *provider,
    const cog_provider_request_t *request, cog_provider_preflight_stage_t stage);
/** @copydoc cog_provider_send_fn */
cog_result_t cog_provider_send(cog_provider_t *provider,
                               const cog_provider_request_t *request);
/** @copydoc cog_provider_receive_fn */
cog_result_t cog_provider_receive(cog_provider_t *provider,
                                  cog_provider_response_t *response);
/** @copydoc cog_provider_poll_fn */
cog_result_t cog_provider_poll(cog_provider_t *provider, uint64_t now_ms,
                               uint64_t *out_next_deadline_ms);
/** @copydoc cog_provider_cancel_fn */
cog_result_t cog_provider_cancel(cog_provider_t *provider);
/** @copydoc cog_provider_next_fn */
cog_result_t cog_provider_next(cog_provider_t *provider,
                               cog_provider_delta_t *out_delta,
                               void *storage, size_t capacity,
                               size_t *out_required);
/** @copydoc cog_provider_error_fn */
cog_result_t cog_provider_get_error(const cog_provider_t *provider,
                                cog_provider_error_t *out_error);
#ifdef __cplusplus
}
#endif
#endif /* COG_PROVIDER_VTABLE_H */

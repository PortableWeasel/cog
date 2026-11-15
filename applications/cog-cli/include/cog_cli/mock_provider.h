/**
 * @file mock_provider.h
 * @brief Deterministic scripted mock LLM provider for the cog CLI and
 *        scenario harness.
 *
 * The mock provider is a complete @c cog_provider_vtable_t implementation
 * that never touches the network.  It is driven by a static script of
 * steps (chunks, errors, cancellation) and exposes an observable state
 * snapshot so tests can assert exactly what the provider was asked to do.
 *
 * It is deliberately **not** part of cog-provider: it lives in the CLI
 * application tree so production provider logic stays free of test
 * scaffolding.  Scenario tests (Stages 12.6b–12.6e) link @c cog::cli and
 * use this header to inject chunks/errors and observe provider state.
 *
 * Legacy steps and typed delta fixtures share the mandatory eight-callback
 * provider contract. Requests are copied transitively into a constructor-owned
 * 128 KiB arena; LIMIT leaves the instance idle. Scripts/nested fixture strings
 * remain borrowed until disconnect. next() reports exact required capacity and
 * retains the same item/sequence on overflow. Cancellation preserves a retained
 * item before exactly one ABORTED FINISH. No callback sleeps or allocates.
 * chunk_delay_ms is serviced using monotonic poll timestamps. Legacy receive
 * can combine the last text chunk with STOP; after completion the turn is idle.
 * Typed fixtures emit a separate FINISH. Do not mix receive/next in one turn.
 * Threading: one owner thread; static descriptors may be shared.
 */
#ifndef COG_CLI_MOCK_PROVIDER_H
#define COG_CLI_MOCK_PROVIDER_H

#include <cog/endpoint.h>
#include <cog/provider.h>
#include <cog/provider_request.h>
#include <cog/result.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Kind of one scripted mock step.
 */
typedef enum cog_mock_step_kind {
    COG_MOCK_STEP_CHUNK = 0,   /**< Emit @p chunk as a streaming delta. */
    COG_MOCK_STEP_ERROR,       /**< Receive fails with @p error_code and
                                    the attached retry metadata. */
    COG_MOCK_STEP_CANCEL,      /**< Receive returns @c COG_ERROR_ABORTED. */
    COG_MOCK_STEP_COMPLETE,    /**< Receive returns an empty STOP
                                    completion. */
} cog_mock_step_kind_t;

/**
 * @brief One scripted step.
 *
 * Only the fields relevant to @p kind are read.  All string pointers are
 * borrowed and must outlive the provider instance that references them.
 */
typedef struct cog_mock_step {
    cog_mock_step_kind_t kind; /**< Step kind. */
    const char *chunk;         /**< Delta text for @c COG_MOCK_STEP_CHUNK. */
    cog_result_t error_code;   /**< Error code for @c COG_MOCK_STEP_ERROR. */
    int http_status;           /**< HTTP status for @c COG_MOCK_STEP_ERROR
                                    (e.g. 429, 500); zero if not
                                    applicable. */
    bool retryable;            /**< Transient flag for
                                    @c COG_MOCK_STEP_ERROR. */
    uint32_t retry_after_ms;   /**< Suggested backoff for
                                    @c COG_MOCK_STEP_ERROR. */
    const char *error_message; /**< Human-readable message for
                                    @c COG_MOCK_STEP_ERROR. */
} cog_mock_step_t;

/**
 * @brief Scripted behaviour for one mock provider instance.
 *
 * The struct is copied by value into the provider at connect time, so a
 * caller may build it on the stack.  The @p steps array and all strings
 * it references are borrowed and must outlive the provider.
 */
typedef struct cog_mock_script {
    const cog_mock_step_t *steps; /**< Step array; must be non-NULL when
                                       @p step_count is non-zero. */
    size_t step_count;            /**< Number of steps. */
    size_t repeat;                /**< Extra replays after the first pass;
                                       0 = single pass, @c SIZE_MAX =
                                       infinite. */
    cog_result_t send_error;      /**< If non-@c COG_OK, every send fails
                                       with this code (scripted send
                                       failure). */
    uint32_t chunk_delay_ms;      /**< Simulated latency between chunks;
                                       zero = none.  The poll schedules this
                                       delay without sleeping. */
} cog_mock_script_t;

/**
 * @brief Observable snapshot of a mock provider instance.
 *
 * Filled by @c cog_mock_provider_state.  All counters are cumulative over
 * the provider's lifetime.
 */
typedef struct cog_mock_state {
    size_t send_count;            /**< Successful sends. */
    size_t receive_count;         /**< Receive calls (any outcome). */
    size_t chunk_count;           /**< Chunks emitted. */
    size_t error_count;           /**< Scripted error steps delivered. */
    size_t cancel_count;          /**< @c COG_ERROR_ABORTED receives. */
    bool disconnected;            /**< True after disconnect. */
    bool cancel_requested;        /**< True while a one-shot cancel is
                                       armed. */
    size_t step_index;            /**< Next step index. */
    const cog_provider_request_t *last_request; /**< Pointer to the mock's
                                       copy of the most recent request
                                       struct; valid until the next send.
                                       All nested pointer fields belong to the
                                       mock request arena. */
    cog_provider_error_t last_error; /**< Copy of the most recent scripted
                                       error metadata. */
} cog_mock_state_t;

/**
 * @brief Static vtable for the mock provider.
 *
 * Wire it into a @c cog_endpoint_t descriptor:
 * @code
 * static const cog_endpoint_t ep = {
 *     .name = "mock",
 *     .vtable = &cog_mock_provider_vtable,
 *     .implementation_config = &script,   // script is copied at connect
 * };
 * @endcode
 */
extern const cog_provider_vtable_t cog_mock_provider_vtable;

/**
 * @brief Ready-made endpoint for the mock provider with a default script
 *        (one chunk: "mock response").
 *
 * Convenient for smoke runs and tests that do not need a custom script.
 */
extern const cog_endpoint_t cog_mock_endpoint;

/**
 * @brief Create a mock provider instance with an explicit script.
 *
 * @param[in]  script Script to drive; may be NULL for the default script
 *                    (one chunk, "mock response").  The struct is copied;
 *                    the steps array and its strings are borrowed.
 * @param[out] out    Uninitialised pointer; set to the new provider on
 *                    success.
 * @return COG_OK on success.
 * @return COG_ERROR_NULL_PTR if @p out is NULL.
 * @return COG_ERROR_INVALID_ARG if @p script is non-NULL with a NULL
 *         @p steps pointer and a non-zero @p step_count.
 * @return COG_ERROR_NOMEM if allocation fails.
 *
 * @post On success, @p *out is a valid provider owned by the caller and
 *       must be released with @c cog_provider_disconnect.
 */
cog_result_t cog_mock_provider_new(const cog_mock_script_t *script,
                                   cog_provider_t **out);

/** @brief Construct a typed fixture provider. Deltas and nested strings are
 * borrowed until disconnect. FINISH is required and ends each scripted turn.
 * Requests are copied into a fixed 128 KiB constructor-owned arena; LIMIT is
 * returned atomically when they exceed it. No runtime allocation or sleeping.
 * @param deltas Immutable typed event script. @param count Event count.
 * @param out Owned provider on success, NULL on failure.
 * @return COG_OK, NULL_PTR, INVALID_ARG or NOMEM.
 */
cog_result_t cog_mock_provider_new_deltas(const cog_provider_delta_t *deltas,
                                         size_t count, cog_provider_t **out);

/**
 * @brief Connect a mock provider through the standard endpoint path.
 *
 * Reads the script from @p endpoint->implementation_config (interpreted as a
 * @c cog_mock_script_t, copied by value) so the standard
 * @c cog_provider_connect flow works unchanged.  A NULL @p implementation_config
 * selects the default script.
 *
 * @param[in]  endpoint Endpoint whose @p implementation_config carries the script.
 * @param[out] out      Uninitialised pointer; set to the new provider on
 *                      success.
 * @return COG_OK on success.
 * @return COG_ERROR_NULL_PTR if @p endpoint or @p out is NULL.
 * @return COG_ERROR_NOMEM if allocation fails.
 */
cog_result_t cog_mock_provider_connect(const cog_endpoint_t *endpoint,
                                       cog_provider_t **out);

/**
 * @brief Disconnect and free a mock provider instance.
 *
 * @param[in,out] provider Provider to release; may be NULL (no-op).
 * @return COG_OK.
 */
cog_result_t cog_mock_provider_disconnect(cog_provider_t *provider);

/**
 * @brief Send a request to the mock provider.
 *
 * Records the request, increments @c send_count, arms a fresh script pass
 * when the previous pass is exhausted and repeats remain, and clears any
 * armed one-shot cancel.  Fails with the scripted @c send_error when set.
 *
 * @param[in,out] provider Mock provider instance.
 * @param[in]     request  Request to record; must be non-NULL.
 * @return COG_OK on success.
 * @return COG_ERROR_NULL_PTR if @p provider or @p request is NULL.
 * @return COG_ERROR_INVALID_STATE if the provider is disconnected.
 * @return The scripted @c send_error otherwise.
 */
cog_result_t cog_mock_provider_send(cog_provider_t *provider,
                                    const cog_provider_request_t *request);

/**
 * @brief Receive the next scripted response or chunk.
 *
 * Consumes one step on successful delivery. Overflow retains it for retry.
 * See the header overview for the exact
 * semantics of chunks, errors, cancellation, and exhaustion.
 *
 * @param[in,out] provider Mock provider instance.
 * @param[in,out] response Caller-provided response buffer.
 * @return COG_OK on success.
 * @return COG_ERROR_NULL_PTR if @p provider or @p response is NULL.
 * @return COG_ERROR_INVALID_STATE if no request is in flight.
 * @return COG_ERROR_OVERFLOW if a chunk does not fit in
 *         @p response->content_capacity.
 * @return COG_ERROR_ABORTED for a cancel step or an armed one-shot
 *         cancel.
 * @return The scripted error code for an error step.
 */
cog_result_t cog_mock_provider_receive(cog_provider_t *provider,
                                       cog_provider_response_t *response);

/**
 * @brief Arm a one-shot cancellation.
 *
 * The next receive returns @c COG_ERROR_ABORTED and the flag clears
 * itself, so a later turn is not stuck cancelled.
 *
 * @param[in,out] provider Mock provider instance.
 * @return COG_OK on success.
 * @return COG_ERROR_NULL_PTR if @p provider is NULL.
 */
cog_result_t cog_mock_provider_request_cancel(cog_provider_t *provider);

/**
 * @brief Snapshot the observable state of a mock provider.
 *
 * @param[in]  provider Mock provider instance.
 * @param[out] out      Receives the snapshot.
 * @return COG_OK on success.
 * @return COG_ERROR_NULL_PTR if @p provider or @p out is NULL.
 */
cog_result_t cog_mock_provider_state(cog_provider_t *provider,
                                     cog_mock_state_t *out);

#ifdef __cplusplus
}
#endif

#endif /* COG_CLI_MOCK_PROVIDER_H */

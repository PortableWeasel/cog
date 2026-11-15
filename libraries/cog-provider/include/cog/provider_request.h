/**
 * @file provider_request.h
 * @brief Provider request and response types.
 *
 * A request describes a single LLM call: model identifier, conversation
 * history, sampling parameters, and whether the response should be
 * streamed.  A response is written into a caller-provided buffer so that
 * no hidden allocation occurs.
 *
 * Threading model: request and response structs are caller-owned.  A
 * provider may read the request only for the duration of the call and
 * must not retain pointers to it afterwards.  The caller must not modify
 * a response while a provider is writing to it.
 */
#ifndef COG_PROVIDER_REQUEST_H
#define COG_PROVIDER_REQUEST_H

#include <cog/message.h>
#include <cog/result.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Reason a provider response finished.
 */
typedef enum cog_finish_reason {
    COG_FINISH_REASON_NONE = 0,      /**< Response is incomplete (streaming). */
    COG_FINISH_REASON_STOP,          /**< Natural stop. */
    COG_FINISH_REASON_LENGTH,        /**< Token limit reached. */
    COG_FINISH_REASON_TOOL_CALLS,    /**< Model requested tool calls. */
    COG_FINISH_REASON_ERROR,         /**< Provider reported an error. */
    COG_FINISH_REASON_CONTENT_FILTER, /**< Provider refused/filtered output. */
    COG_FINISH_REASON_CANCELLED,     /**< Explicit cancellation. */
} cog_finish_reason_t;

/**
 * @brief Request sent to a provider.
 *
 * Callers should zero-initialise this struct to obtain defaults; adding
 * fields later will not break callers that zero-init.
 */
typedef struct cog_provider_request {
    /**
     * @brief Model identifier.
     *
     * Must be non-NULL and NUL-terminated.  The provider resolves this
     * to an endpoint-specific model id.
     */
    const char *model_id;

    /**
     * @brief Conversation messages.
     *
     * May be NULL only when @p message_count is zero.
     */
    const cog_message_t *messages;

    /**
     * @brief Number of messages in @p messages.
     */
    size_t message_count;

    /**
     * @brief Request a streaming response.
     *
     * When true, @c cog_provider_receive returns partial deltas until
     * @p is_complete is set.
     */
    bool stream;

    /**
     * @brief Sampling temperature.
     *
     * Provider-specific; zero selects a sensible default.
     */
    float temperature;

    /**
     * @brief Maximum tokens to generate.
     *
     * Real adapters require a finite nonzero value. Zero is legacy/mock only;
     * no implicit context truncation or model substitution.
     */
    uint32_t max_tokens;

    /**
     * @brief Optional JSON description of available tools.
     *
     * May be NULL.  Provider-specific format (e.g. JSON Schema array).
     */
    const char *tools_json;

    /**
     * @brief Length of @p tools_json in bytes.
     */
    size_t tools_json_len;

    /** Must match the endpoint's immutable generation when send validates it.
     * Zero is permitted only for legacy/mock generations; values are not reused
     * while an older generation remains live. */
    uint64_t configuration_generation;
} cog_provider_request_t;

/**
 * @brief Legacy text-only response. Use cog_provider_next for structured turns.
 *
 * Receiving a tool/opaque delta through this interface returns UNSUPPORTED
 * without consuming it. OVERFLOW retains the same text delta for retry;
 * content_len reports required bytes excluding NUL and no text is written.
 * A provider's configured retention ceiling instead fails the request with
 * LIMIT; a larger caller destination cannot recover that terminal failure.
 *
 * The caller provides the @p content buffer.  The provider writes the
 * generated text (or delta) into it and reports whether more data is
 * expected.
 */
typedef struct cog_provider_response {
    /**
     * @brief Caller-provided buffer that receives the response text.
     *
     * May be NULL only when @p content_capacity is zero.
     */
    char *content;

    /**
     * @brief Size of @p content in bytes, including space for a NUL
     *        terminator.
     */
    size_t content_capacity;

    /**
     * @brief Length of the response text in bytes, excluding the
     *        terminator.  Filled by the provider.
     */
    size_t content_len;

    /**
     * @brief Why the response finished, if known.
     *
     * Filled by the provider.
     */
    cog_finish_reason_t finish_reason;

    /**
     * @brief True when this is the final response or chunk.
     *
     * For single-turn requests this is true after the first receive.
     * For streaming requests the caller should loop until this is true.
     */
    bool is_complete;
} cog_provider_response_t;

/** @brief Exactly one typed event is delivered by each successful next(). */
typedef enum cog_provider_delta_kind {
    COG_PROVIDER_DELTA_TEXT = 0,
    COG_PROVIDER_DELTA_TOOL_BEGIN,
    COG_PROVIDER_DELTA_TOOL_ARGUMENTS,
    COG_PROVIDER_DELTA_TOOL_END,
    COG_PROVIDER_DELTA_OPAQUE,
    COG_PROVIDER_DELTA_USAGE,
    COG_PROVIDER_DELTA_FINISH
} cog_provider_delta_kind_t;

/**
 * @brief Typed, ordered view copied into next()'s caller-provided storage.
 *
 * Sequence starts at one for each send and is contiguous. tool_index is a
 * request-local bounded slot; TOOL_BEGIN establishes nonempty ID/name once,
 * ARGUMENTS appends bytes, END means the assembled object is syntactically
 * valid (tool schema/policy still pending). No tool runs before FINISH with
 * TOOL_CALLS and COG_OK, after wire terminal/EOF validation. FINISH occurs
 * exactly once and outcome reports errors/cancel; usage is optional and never
 * guessed. USAGE is a replacement cumulative snapshot, never an increment;
 * repeated identical finish/usage frames must not double-count. Conflicting
 * terminal metadata or nonempty post-finish content fails PROTOCOL before
 * successful FINISH. OPAQUE carries one complete namespace/format blob; implementations
 * assemble any wire fragments privately within configured limits. All pointer
 * members refer to caller storage and survive later provider service calls.
 */
typedef struct cog_provider_delta {
    cog_provider_delta_kind_t kind;
    uint64_t sequence;
    size_t tool_index;
    const char *tool_call_id; /**< BEGIN/ARGUMENTS/END identity. */
    const char *tool_name;    /**< BEGIN name; NULL otherwise. */
    const char *data;         /**< TEXT or ARGUMENTS bytes; may be empty. */
    size_t data_len;
    cog_message_opaque_t opaque;
    cog_usage_t usage;
    cog_finish_reason_t finish_reason;
    cog_result_t outcome;     /**< FINISH only; COG_OK, ABORTED or failure. */
} cog_provider_delta_t;

/**
 * @brief Constructor-owned provider workspace ceilings, not growth hints.
 *
 * Every field must be nonzero. connect reserves all Cog-owned request copies,
 * strict JSON parser/writer pools, depth preflight, event retention and turn
 * assembly storage. No runtime Cog malloc/realloc. Checked size arithmetic and
 * impossible layouts fail INVALID_ARG; reservation failure returns NOMEM.
 * Peer data above a ceiling terminates with LIMIT and retains an incomplete
 * turn. A full event queue pauses transport; it never drops or reorders data.
 */
typedef struct cog_provider_limits {
    size_t request_bytes;
    size_t parser_bytes;
    size_t writer_bytes;
    size_t max_json_depth;
    size_t max_frame_bytes;
    size_t retained_delta_bytes;
    size_t retained_delta_count;
    size_t max_text_bytes;
    size_t max_tool_calls;
    size_t max_tool_id_bytes;
    size_t max_tool_name_bytes;
    size_t max_tool_arguments_bytes;
    size_t max_opaque_bytes;
} cog_provider_limits_t;

/**
 * @brief Validate that a request is well-formed.
 *
 * @param[in] request Request to validate.
 * @return COG_OK on success.
 * @return COG_ERROR_NULL_PTR if @p request is NULL.
 * @return COG_ERROR_INVALID_ARG if @p request->model_id is NULL, if
 *         @p request->messages is NULL while @p request->message_count
 *         is non-zero, or if @p request->tools_json is NULL while
 *         @p request->tools_json_len is non-zero, model_id is empty, a nested
 *         message is structurally invalid or sampling
 *         parameters are nonfinite/out of range. Provider-specific protocol/schema
 *         validation and finite output/context limits are enforced before send.
 * @note No allocation. Does not retain any input or initiate network activity.
 *
 * Threading: the request must not be modified during the call.
 */
cog_result_t cog_provider_request_validate(const cog_provider_request_t *request);

/**
 * @brief Validate that a response struct is ready to receive data.
 *
 * @param[in] response Response to validate.
 * @return COG_OK on success.
 * @return COG_ERROR_NULL_PTR if @p response is NULL.
 * @return COG_ERROR_INVALID_ARG if @p response->content is NULL while
 *         @p response->content_capacity is non-zero.
 *
 * Threading: the response must not be modified during the call.
 */
cog_result_t cog_provider_response_validate(const cog_provider_response_t *response);

/** @brief Stable sanitized provider failure classification. */
typedef enum cog_provider_error_kind {
    COG_PROVIDER_ERROR_NONE = 0,
    COG_PROVIDER_ERROR_DNS,
    COG_PROVIDER_ERROR_TLS,
    COG_PROVIDER_ERROR_CONNECT,
    COG_PROVIDER_ERROR_TIMEOUT,
    COG_PROVIDER_ERROR_HTTP,
    COG_PROVIDER_ERROR_PROTOCOL,
    COG_PROVIDER_ERROR_CAPACITY,
    COG_PROVIDER_ERROR_CANCELLED,
    COG_PROVIDER_ERROR_DEPENDENCY
} cog_provider_error_kind_t;

/**
 * @brief Detailed error information from a provider call.
 *
 * Used with COG_EVENT_PROVIDER_ERROR to describe what went wrong.
 */
typedef struct cog_provider_error {
    /** Error code from the provider or transport. */
    cog_result_t code;

    /** HTTP status code (if applicable, e.g. 429, 500). */
    int http_status;

    /** Human-readable error message. */
    const char *message;

    /** If true, the error is transient and the request may be retried
     *  after a backoff period (e.g. rate limit, temporary network issue). */
    bool retryable;

    /** Suggested retry delay in milliseconds (from Retry-After header).
     *  Zero if not specified. */
    uint32_t retry_after_ms;

    /** Accepted output makes transparent retry unsafe even if retryable. */
    bool output_accepted;
    /** True only before any accepted output or tool side effect. The
     * application owns the sole elapsed-time/attempt retry budget. */
    bool replay_safe;
    /** Sanitized stable protocol/transport classification. */
    cog_provider_error_kind_t kind;
} cog_provider_error_t;

#ifdef __cplusplus
}
#endif

#endif /* COG_PROVIDER_REQUEST_H */

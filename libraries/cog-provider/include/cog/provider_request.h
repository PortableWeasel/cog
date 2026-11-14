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
     * Zero leaves the choice to the provider.
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
} cog_provider_request_t;

/**
 * @brief Response or streaming chunk received from a provider.
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

/**
 * @brief Validate that a request is well-formed.
 *
 * @param[in] request Request to validate.
 * @return COG_OK on success.
 * @return COG_ERROR_NULL_PTR if @p request is NULL.
 * @return COG_ERROR_INVALID_ARG if @p request->model_id is NULL, if
 *         @p request->messages is NULL while @p request->message_count
 *         is non-zero, or if @p request->tools_json is NULL while
 *         @p request->tools_json_len is non-zero.
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

#ifdef __cplusplus
}
#endif

#endif /* COG_PROVIDER_REQUEST_H */

/**
 * @file toolkit_result.h
 * @brief Result types and validation helpers for toolkit execution.
 *
 * A @c cog_toolkit_result_t describes the outcome of a single tool
 * invocation.  For parallel execution, @c cog_toolkit_execute_all returns
 * an opaque @c cog_toolkit_result_collection_t that owns copies of the
 * individual results and provides indexed access to them.
 *
 * Threading model: result values and collections are not thread-safe.
 * They should be consumed on the same thread that performed the execution.
 */
#ifndef COG_TOOLKIT_RESULT_H
#define COG_TOOLKIT_RESULT_H

#include <cog/result.h>

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Result of a single tool execution.
 *
 * This is a value type.  For a single synchronous invocation the caller
 * provides the output buffer and the toolkit writes the result string
 * into it.  For collections returned by @c cog_toolkit_execute_all the
 * collection owns the buffers and the @p output pointer refers to memory
 * inside the collection.
 */
typedef struct cog_toolkit_result {
    /**
     * @brief Outcome of the tool invocation.
     *
     * @c COG_OK means the tool ran successfully and produced a result.
     * Any other value indicates failure; see @p error_message for a
     * human-readable explanation.
     */
    cog_result_t status;

    /**
     * @brief Pointer to the NUL-terminated result string.
     *
     * May be NULL on failure or when the tool produced no output.  The
     * pointed-to memory is owned by the caller-provided buffer for
     * single invocations, or by the result collection for parallel
     * invocations.
     */
    char *output;

    /**
     * @brief Length of @p output in bytes, excluding the NUL terminator.
     *
     * Zero when @p output is NULL or empty.
     */
    size_t output_len;

    /**
     * @brief Optional human-readable error message.
     *
     * May be NULL when @p status is @c COG_OK or when no additional
     * detail is available.  The pointed-to string is owned by the same
     * owner as @p output.
     */
    const char *error_message;
    const char *tool_call_id; /**< Stable correlation ID; borrowed/copied with owner. */
    bool truncated; /**< Explicit capture truncation at configured ceiling. */
    bool side_effects_possible; /**< Failure/overflow never implies safe replay. */
    int exit_status;
    bool exit_status_known;
} cog_toolkit_result_t;

/**
 * @brief Opaque collection of results from parallel tool calls.
 *
 * Created by @c cog_toolkit_execute_all and destroyed with
 * @c cog_toolkit_result_collection_free.
 */
typedef struct cog_toolkit_result_collection cog_toolkit_result_collection_t;

/**
 * @brief Return the number of results in a collection.
 *
 * @param[in] collection Collection to query; may be NULL.
 * @return Number of results, or zero if @p collection is NULL.
 *
 * Threading: not thread-safe.
 */
size_t cog_toolkit_result_collection_count(const cog_toolkit_result_collection_t *collection);

/**
 * @brief Access a single result from a collection.
 *
 * @param[in]  collection Collection to query.
 * @param[in]  index       Zero-based index of the result to retrieve.
 * @param[out] out_result  Receives a pointer to the result on success;
 *                         unchanged on failure.  May be NULL if the
 *                         caller only wants to validate the index.
 * @return COG_OK on success.
 * @return COG_ERROR_NULL_PTR if @p collection is NULL.
 * @return COG_ERROR_INVALID_ARG if @p index is out of range.
 *
 * @post On success, @p *out_result points to a result owned by
 *       @p collection.  The pointer remains valid until the collection is
 *       freed.
 *
 * Threading: not thread-safe.
 */
cog_result_t cog_toolkit_result_collection_get(const cog_toolkit_result_collection_t *collection,
                                               size_t index,
                                               const cog_toolkit_result_t **out_result);

/**
 * @brief Destroy a result collection.
 *
 * @param[in,out] collection Collection to destroy; may be NULL (no-op).
 *
 * @post All storage owned by @p collection is released, including any
 *       copied result strings.  @p collection must not be used after this
 *       call.
 *
 * Threading: not thread-safe.
 */
void cog_toolkit_result_collection_free(cog_toolkit_result_collection_t *collection);

/**
 * @brief Return true if a result indicates success.
 *
 * @param[in] result Result to inspect; may be NULL.
 * @return true if @p result is non-NULL and @p result->status is
 *         @c COG_OK, false otherwise.
 *
 * Threading: not thread-safe.
 */
bool cog_toolkit_result_is_ok(const cog_toolkit_result_t *result);

/**
 * @brief Validate a result against an expected output schema.
 *
 * The schema string must contain a single JSON object; trailing
 * whitespace after the object is ignored, but any other trailing
 * characters cause the schema to be treated as malformed.  A NULL or
 * empty schema is treated as "no validation required" and the function
 * returns @c COG_OK.
 *
 * @param[in] result Result to validate.
 * @param[in] schema Schema string; may be NULL or empty.
 * @return COG_OK if validation passes or no schema was supplied.
 * @return COG_ERROR_NULL_PTR if @p result is NULL.
 * @return COG_ERROR_INVALID_ARG if @p result->output is NULL or if the
 *         schema is malformed.
 * @return COG_ERROR_INVALID_STATE if @p result->status is not @c COG_OK.
 *
 * Threading: not thread-safe.
 */
cog_result_t cog_toolkit_result_validate_schema(const cog_toolkit_result_t *result,
                                                const char *schema);

#ifdef __cplusplus
}
#endif

#endif /* COG_TOOLKIT_RESULT_H */

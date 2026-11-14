/**
 * @file tool.h
 * @brief Tool descriptor table and tool function contract.
 *
 * Tools are plain C functions exposed through a static const table.
 * The core uses the table to resolve tool names invoked by handlers; no
 * dynamic registration is supported.
 *
 * Threading model: the tool table is read-only after construction and may
 * be shared by multiple core instances.  Individual tool implementations
 * must be re-entrant and must not introduce their own global mutable
 * state.
 */
#ifndef COG_TOOL_H
#define COG_TOOL_H

#include <cog/event.h>
#include <cog/result.h>

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declaration to break the core/tool include cycle. */
typedef struct cog_core cog_core_t;

/**
 * @brief Tool implementation signature.
 *
 * The caller supplies the result buffer, so no hidden allocation occurs
 * and ownership never transfers.  If the buffer is too small the tool
 * reports the required length via @p out_result_len (when non-NULL) and
 * returns @c COG_ERROR_OVERFLOW.
 *
 * @param[in,out] core             Core instance that requested the tool.
 * @param[in]     request          Tool request payload (e.g. JSON arguments).
 * @param[in]     request_len      Length of @p request in bytes.
 * @param[out]    result           Caller-provided buffer that receives the
 *                                 result string.  May be NULL only when
 *                                 @p result_capacity is zero.
 * @param[in]     result_capacity  Size of @p result in bytes, including
 *                                 space for a NUL terminator.
 * @param[out]    out_result_len   On success or @c COG_ERROR_OVERFLOW, set
 *                                 to the length of the result string in
 *                                 bytes (excluding the terminator).  May be
 *                                 NULL if the caller does not need the
 *                                 length.
 * @return COG_OK on success.
 * @return COG_ERROR_NULL_PTR if a required pointer argument is NULL.
 * @return COG_ERROR_INVALID_ARG if the request is malformed.
 * @return COG_ERROR_NOT_FOUND if the tool is not applicable to the
 *         request.
 * @return COG_ERROR_OVERFLOW if the result does not fit in
 *         @p result_capacity.
 * @return Another error code on failure.
 *
 * @pre @p core is a fully initialised core instance.
 * @pre @p request is valid for the duration of the call.
 * @pre If @p result is non-NULL, @p result_capacity is greater than zero.
 *
 * @post On success, @p result contains a NUL-terminated result string
 *       and ownership remains with the caller.
 * @post On failure, @p result is unchanged.
 */
typedef cog_result_t (*cog_tool_fn)(cog_core_t *core,
                                     const char *request,
                                     size_t request_len,
                                     char *result,
                                     size_t result_capacity,
                                     size_t *out_result_len);

/**
 * @brief Descriptor for a single tool.
 *
 * Rows are assembled into a static const array and supplied to the core
 * through @c cog_core_config_t.
 */
typedef struct cog_tool {
    const char *name;        /**< Tool name used for dispatch.  Non-NULL,
                                  NUL-terminated, unique within the table. */
    const char *description; /**< Human-readable description; may be NULL. */
    const char *input_schema; /**< Input validation schema (e.g. JSON Schema);
                                  may be NULL. */
    cog_tool_fn handler;     /**< Tool implementation.  Non-NULL. */
} cog_tool_t;

/**
 * @brief Static tool table wrapper.
 *
 * This is a convenience type for passing a pointer/count pair.  The
 * underlying array must remain valid for the lifetime of any core that
 * references it.
 */
typedef struct cog_tool_table {
    const cog_tool_t *tools; /**< Pointer to first tool descriptor. */
    size_t count;            /**< Number of descriptors.  May be zero. */
} cog_tool_table_t;

/**
 * @brief Look up a tool by name in a tool table.
 *
 * @param[in]  table      Tool table to search.
 * @param[in]  name       Tool name to look up; NUL-terminated.
 * @param[out] out_tool   Receives a pointer to the matching descriptor on
 *                        success; unchanged on failure.  May be NULL if
 *                        only presence is required.
 * @return COG_OK on success.
 * @return COG_ERROR_NULL_PTR if @p table or @p name is NULL.
 * @return COG_ERROR_NOT_FOUND if no tool matches @p name.
 *
 * Threading: the table must not be modified during the call.
 */
cog_result_t cog_tool_table_lookup(const cog_tool_table_t *table,
                                   const char *name,
                                   const cog_tool_t **out_tool);

#ifdef __cplusplus
}
#endif

#endif /* COG_TOOL_H */

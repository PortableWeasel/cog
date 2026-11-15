/**
 * @file toolkit_internal.h
 * @brief Private layout shared between the cog-toolkit translation units.
 *
 * This header is NOT part of the public contract.  It reveals the private
 * layout of @c cog_toolkit_result_collection_t so that @c toolkit_exec.c
 * can allocate and populate a collection, and it declares the private
 * per-call timeout accessor implemented in @c toolkit_call.c.  Public
 * consumers only ever see the opaque typedefs from the public headers.
 */
#ifndef COG_TOOLKIT_INTERNAL_H
#define COG_TOOLKIT_INTERNAL_H

#include <cog/toolkit_call.h>
#include <cog/toolkit_result.h>

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Private layout of a result collection.
 *
 * @p results is an array of @p count entries.  @p storage is a single
 * heap block holding every output string and error message, laid out
 * contiguously; each @c cog_toolkit_result_t in @p results points into
 * @p storage.  Both are freed by @c cog_toolkit_result_collection_free.
 */
struct cog_toolkit_result_collection {
    size_t count;
    cog_toolkit_result_t *results;
    char *storage;
};

/**
 * @brief Private accessor for the per-call timeout.
 *
 * Implemented in @c toolkit_call.c.  Returns the timeout recorded by
 * @c cog_toolkit_call_set_timeout, or zero if none was set.
 */
uint32_t cog_toolkit_call_timeout_ms(const cog_toolkit_call_t *call);

#ifdef __cplusplus
}
#endif

#endif /* COG_TOOLKIT_INTERNAL_H */

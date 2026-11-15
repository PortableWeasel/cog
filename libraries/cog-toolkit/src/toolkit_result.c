/**
 * @file toolkit_result.c
 * @brief Implementation of the result types and validation helpers
 *        declared in cog/toolkit_result.h.
 *
 * A @c cog_toolkit_result_t is a value type describing the outcome of a
 * single tool invocation.  A @c cog_toolkit_result_collection_t is an
 * opaque collection that owns copies of the individual results (their
 * output strings and error messages) and provides indexed access.
 *
 * The collection is allocated by @c cog_toolkit_execute_all in
 * @c toolkit_exec.c using the private layout in @c toolkit_internal.h;
 * this module provides the query and destruction functions.
 *
 * Schema validation implements a small, documented JSON Schema subset:
 * the @c type keyword (object/array/string/number/integer/boolean/null)
 * and, for objects, the @c required keyword (an array of property names).
 * Unknown keywords are ignored.  A NULL or empty schema means "no
 * validation required".
 *
 * The module holds no global mutable state.
 *
 * Ticket: 8d52c1a (Stage 12.4b — Implement toolkit)
 */

#include "cog/toolkit_result.h"
#include "toolkit_internal.h"
#include "toolkit_json.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/* Maximum number of required-property names a schema may list.  Schemas
 * that exceed this are rejected as malformed/unsupported. */
#define COG_TOOLKIT_SCHEMA_MAX_REQUIRED 64U

/* ------------------------------------------------------------------ */
/* Result collection                                                   */
/* ------------------------------------------------------------------ */

size_t cog_toolkit_result_collection_count(
    const cog_toolkit_result_collection_t *collection)
{
    return collection != NULL ? collection->count : 0U;
}

cog_result_t cog_toolkit_result_collection_get(
    const cog_toolkit_result_collection_t *collection,
    size_t index,
    const cog_toolkit_result_t **out_result)
{
    if (collection == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    if (index >= collection->count) {
        return COG_ERROR_INVALID_ARG;
    }
    if (out_result != NULL) {
        *out_result = &collection->results[index];
    }
    return COG_OK;
}

void cog_toolkit_result_collection_free(
    cog_toolkit_result_collection_t *collection)
{
    if (collection == NULL) {
        return;
    }
    free(collection->results);
    free(collection->storage);
    free(collection);
}

bool cog_toolkit_result_is_ok(const cog_toolkit_result_t *result)
{
    return result != NULL && result->status == COG_OK;
}

cog_result_t cog_toolkit_result_validate_schema(
    const cog_toolkit_result_t *result, const char *schema)
{
    if (result == NULL) { return COG_ERROR_NULL_PTR; }
    if (result->status != COG_OK) { return COG_ERROR_INVALID_STATE; }
    if (result->output == NULL) { return COG_ERROR_INVALID_ARG; }
    if (schema == NULL || *schema == '\0') { return COG_OK; }
    return cog_toolkit_json_validate_output(result->output, result->output_len, schema);
}

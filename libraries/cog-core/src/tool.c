/**
 * @file tool.c
 * @brief Implementation of the tool table lookup contract declared in
 *        cog/tool.h.
 *
 * Tools are plain C functions exposed through a static const table.  The
 * core resolves tool names invoked by handlers against this table; no
 * dynamic registration is supported.  This module provides the single
 * pure lookup helper that @c cog_core_run uses to resolve a tool name to
 * its descriptor.
 *
 * The module holds no global state: the table is caller-provided and
 * read-only.  Lookup is a deterministic linear scan in table order, so
 * the first matching descriptor always wins.
 *
 * Ticket: 62a438e (Stage 12.2c — Implement tool.c)
 */

#include "cog/tool.h"

#include <string.h>

cog_result_t cog_tool_table_lookup(const cog_tool_table_t *table,
                                   const char *name,
                                   const cog_tool_t **out_tool)
{
    size_t i;

    if (table == NULL || name == NULL) {
        return COG_ERROR_NULL_PTR;
    }

    /* A NULL tools pointer is only legal when the table is empty (count
     * zero).  A non-NULL tools pointer with count zero is likewise an
     * empty table.  Either way an empty table can never contain a
     * match. */
    if (table->tools == NULL && table->count != 0U) {
        return COG_ERROR_NULL_PTR;
    }
    if (table->count == 0U) {
        return COG_ERROR_NOT_FOUND;
    }

    /* Deterministic linear scan: the first descriptor in table order
     * whose name matches is selected.  Names are compared as
     * NUL-terminated strings, so a NULL name field in a row is simply
     * never matched by a well-formed query. */
    for (i = 0U; i < table->count; ++i) {
        if (table->tools[i].name != NULL &&
            strcmp(table->tools[i].name, name) == 0) {
            if (out_tool != NULL) {
                *out_tool = &table->tools[i];
            }
            return COG_OK;
        }
    }

    return COG_ERROR_NOT_FOUND;
}

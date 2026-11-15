/**
 * @file test_tool.c
 * @brief Contract test for the tool.h contract (tool.c module).
 *
 * tool.h declares the tool descriptor type (@c cog_tool_t), the static
 * tool table wrapper (@c cog_tool_table_t), the tool implementation
 * signature (@c cog_tool_fn), and one function,
 * @c cog_tool_table_lookup, implemented in tool.c.  This test pins:
 *
 *   - the shape of cog_tool_t (name, description, input_schema, handler);
 *   - the shape of cog_tool_table_t (tools pointer + count);
 *   - the cog_tool_fn signature (assignable and invocable through a
 *     test-defined tool);
 *   - lookup success: the first matching descriptor in table order is
 *     returned by pointer;
 *   - lookup not-found: COG_ERROR_NOT_FOUND for an absent name;
 *   - lookup presence-only: out_tool may be NULL;
 *   - NULL table / NULL name handling: COG_ERROR_NULL_PTR;
 *   - empty table (count zero, tools NULL or non-NULL): NOT_FOUND;
 *   - malformed table (tools NULL with count > 0): COG_ERROR_NULL_PTR;
 *   - out_tool is unchanged on failure;
 *   - the tool descriptor fields are readable through the returned
 *     pointer (name/description/input_schema/handler).
 *
 * This module does not test core.c tool dispatch or toolkit.c; those
 * modules own their own contracts.  Nothing is tested here that is not
 * either stated in tool.h or guaranteed by C11 semantics.
 *
 * Ticket: 62a438e (Stage 12.2c — Implement tool.c)
 */

#include "cog/tool.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#ifndef COG_TOOL_H
#error "COG_TOOL_H include guard missing"
#endif

/* ------------------------------------------------------------------ */
/* Test tool implementations.                                          */
/* ------------------------------------------------------------------ */

/** A minimal tool: echoes a fixed result string. */
static cog_result_t echo_tool(cog_core_t *core,
                              const char *request,
                              size_t request_len,
                              char *result,
                              size_t result_capacity,
                              size_t *out_result_len)
{
    static const char kReply[] = "echo:ok";
    const size_t need = sizeof(kReply) - 1U; /* excluding NUL */

    (void)core;
    (void)request;
    (void)request_len;

    if (result_capacity < sizeof(kReply)) {
        if (out_result_len != NULL) {
            *out_result_len = need;
        }
        return COG_ERROR_OVERFLOW;
    }
    (void)memcpy(result, kReply, sizeof(kReply));
    if (out_result_len != NULL) {
        *out_result_len = need;
    }
    return COG_OK;
}

/** A tool that reports a NULL core as a precondition violation. */
static cog_result_t strict_tool(cog_core_t *core,
                                const char *request,
                                size_t request_len,
                                char *result,
                                size_t result_capacity,
                                size_t *out_result_len)
{
    (void)request;
    (void)request_len;
    (void)result;
    (void)result_capacity;
    (void)out_result_len;

    if (core == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    return COG_OK;
}

/* ------------------------------------------------------------------ */
/* Compile-time checks.                                                */
/* ------------------------------------------------------------------ */

/* cog_tool_t must expose name, description, input_schema, handler. */
_Static_assert(_Generic(((cog_tool_t){0}).name, const char * : 1, default : 0),
               "cog_tool_t.name must be const char *");
_Static_assert(_Generic(((cog_tool_t){0}).description, const char * : 1,
                        default : 0),
               "cog_tool_t.description must be const char *");
_Static_assert(_Generic(((cog_tool_t){0}).input_schema, const char * : 1,
                        default : 0),
               "cog_tool_t.input_schema must be const char *");
_Static_assert(_Generic(((cog_tool_t){0}).handler, cog_tool_fn : 1,
                        default : 0),
               "cog_tool_t.handler must be cog_tool_fn");

/* cog_tool_table_t must expose tools and count. */
_Static_assert(_Generic(((cog_tool_table_t){0}).tools,
                        const cog_tool_t * : 1, default : 0),
               "cog_tool_table_t.tools must be const cog_tool_t *");
_Static_assert(_Generic(((cog_tool_table_t){0}).count, size_t : 1,
                        default : 0),
               "cog_tool_table_t.count must be size_t");

/* ------------------------------------------------------------------ */
/* Test table.                                                         */
/* ------------------------------------------------------------------ */

static const cog_tool_t kTools[] = {
    {
        .name = "echo",
        .description = "Echo a fixed reply.",
        .input_schema = NULL,
        .handler = echo_tool,
    },
    {
        .name = "strict",
        .description = NULL,
        .input_schema = "{\"type\":\"object\"}",
        .handler = strict_tool,
    },
};

static const cog_tool_table_t kTable = {
    .tools = kTools,
    .count = sizeof(kTools) / sizeof(kTools[0]),
};

/* A table with a NULL-name row and duplicate names, to pin the
 * documented lookup semantics: a NULL name field is never matched, and
 * the first descriptor in table order wins. */
static const cog_tool_t kEdgeTools[] = {
    {
        .name = NULL,          /* malformed row: never matched */
        .description = NULL,
        .input_schema = NULL,
        .handler = echo_tool,
    },
    {
        .name = "dup",
        .description = NULL,
        .input_schema = NULL,
        .handler = echo_tool,
    },
    {
        .name = "dup",         /* duplicate: first match must win */
        .description = NULL,
        .input_schema = NULL,
        .handler = strict_tool,
    },
};

static const cog_tool_table_t kEdgeTable = {
    .tools = kEdgeTools,
    .count = sizeof(kEdgeTools) / sizeof(kEdgeTools[0]),
};

/* ------------------------------------------------------------------ */
/* Runtime checks.                                                     */
/* ------------------------------------------------------------------ */

/** Lookup success: first matching descriptor returned by pointer. */
static int verify_lookup_success(void)
{
    const cog_tool_t *tool = NULL;
    cog_result_t r = cog_tool_table_lookup(&kTable, "echo", &tool);

    if (r != COG_OK) {
        return 1;
    }
    if (tool == NULL) {
        return 1;
    }
    if (tool != &kTools[0]) {
        return 1;
    }
    if (tool->handler != echo_tool) {
        return 1;
    }
    return 0;
}

/** Lookup success: second row, and descriptor fields are readable. */
static int verify_lookup_second_row(void)
{
    const cog_tool_t *tool = NULL;
    cog_result_t r = cog_tool_table_lookup(&kTable, "strict", &tool);

    if (r != COG_OK) {
        return 1;
    }
    if (tool != &kTools[1]) {
        return 1;
    }
    if (tool->handler != strict_tool) {
        return 1;
    }
    if (tool->description != NULL) {
        return 1;
    }
    if (tool->input_schema == NULL) {
        return 1;
    }
    return 0;
}

/** Lookup not-found: absent name returns COG_ERROR_NOT_FOUND. */
static int verify_lookup_not_found(void)
{
    const cog_tool_t *tool = (const cog_tool_t *)&kTools[0];
    cog_result_t r = cog_tool_table_lookup(&kTable, "no-such-tool", &tool);

    if (r != COG_ERROR_NOT_FOUND) {
        return 1;
    }
    /* out_tool must be unchanged on failure. */
    if (tool != &kTools[0]) {
        return 1;
    }
    return 0;
}

/** Presence-only lookup: out_tool may be NULL. */
static int verify_lookup_presence_only(void)
{
    cog_result_t r = cog_tool_table_lookup(&kTable, "echo", NULL);

    if (r != COG_OK) {
        return 1;
    }
    r = cog_tool_table_lookup(&kTable, "no-such-tool", NULL);
    if (r != COG_ERROR_NOT_FOUND) {
        return 1;
    }
    return 0;
}

/** NULL table / NULL name: COG_ERROR_NULL_PTR. */
static int verify_null_args(void)
{
    const cog_tool_t *tool = NULL;

    if (cog_tool_table_lookup(NULL, "echo", &tool) != COG_ERROR_NULL_PTR) {
        return 1;
    }
    if (cog_tool_table_lookup(&kTable, NULL, &tool) != COG_ERROR_NULL_PTR) {
        return 1;
    }
    if (cog_tool_table_lookup(NULL, NULL, &tool) != COG_ERROR_NULL_PTR) {
        return 1;
    }
    return 0;
}

/** Empty table (tools NULL, count 0): NOT_FOUND. */
static int verify_empty_table_null_tools(void)
{
    const cog_tool_table_t empty = { .tools = NULL, .count = 0U };
    const cog_tool_t *tool = NULL;

    if (cog_tool_table_lookup(&empty, "echo", &tool) != COG_ERROR_NOT_FOUND) {
        return 1;
    }
    if (tool != NULL) {
        return 1;
    }
    return 0;
}

/** Empty table (tools non-NULL, count 0): NOT_FOUND. */
static int verify_empty_table_nonnull_tools(void)
{
    const cog_tool_table_t empty = { .tools = kTools, .count = 0U };
    const cog_tool_t *tool = NULL;

    if (cog_tool_table_lookup(&empty, "echo", &tool) != COG_ERROR_NOT_FOUND) {
        return 1;
    }
    if (tool != NULL) {
        return 1;
    }
    return 0;
}

/** Malformed table (tools NULL, count > 0): COG_ERROR_NULL_PTR. */
static int verify_malformed_table(void)
{
    const cog_tool_table_t bad = { .tools = NULL, .count = 1U };
    const cog_tool_t *tool = NULL;

    if (cog_tool_table_lookup(&bad, "echo", &tool) != COG_ERROR_NULL_PTR) {
        return 1;
    }
    if (tool != NULL) {
        return 1;
    }
    return 0;
}

/** A NULL name field in a row is never matched by a query. */
static int verify_null_name_row_skipped(void)
{
    const cog_tool_t *tool = NULL;
    cog_result_t r = cog_tool_table_lookup(&kEdgeTable, "echo", &tool);

    /* "echo" is the handler of the NULL-name row, but the row's name
     * field is NULL, so it must not match. */
    if (r != COG_ERROR_NOT_FOUND) {
        return 1;
    }
    if (tool != NULL) {
        return 1;
    }
    return 0;
}

/** First descriptor in table order wins when names are duplicated. */
static int verify_first_match_wins(void)
{
    const cog_tool_t *tool = NULL;
    cog_result_t r = cog_tool_table_lookup(&kEdgeTable, "dup", &tool);

    if (r != COG_OK) {
        return 1;
    }
    if (tool != &kEdgeTools[1]) {
        return 1;
    }
    if (tool->handler != echo_tool) {
        return 1;
    }
    return 0;
}

/** cog_tool_fn is assignable and invocable through the pointer. */
static int verify_fn_pointer(void)
{
    static long dummy_core_storage;
    char buf[16];
    size_t len = 0U;
    cog_tool_fn fn = echo_tool;
    cog_result_t r = fn((cog_core_t *)&dummy_core_storage, NULL, 0U,
                        buf, sizeof(buf), &len);

    if (r != COG_OK) {
        return 1;
    }
    if (len != 7U) {
        return 1;
    }
    if (buf[0] != 'e' || buf[7] != '\0') {
        return 1;
    }
    return 0;
}

/** Overflow reporting: too-small result buffer returns COG_ERROR_OVERFLOW. */
static int verify_overflow_reporting(void)
{
    static long dummy_core_storage;
    char small[4];
    size_t len = 0U;
    cog_result_t r = echo_tool((cog_core_t *)&dummy_core_storage, NULL, 0U,
                               small, sizeof(small), &len);

    if (r != COG_ERROR_OVERFLOW) {
        return 1;
    }
    if (len != 7U) {
        return 1;
    }
    return 0;
}

/** NULL core detection: a tool may report COG_ERROR_NULL_PTR. */
static int verify_tool_null_core(void)
{
    cog_result_t r = strict_tool(NULL, NULL, 0U, NULL, 0U, NULL);

    if (r != COG_ERROR_NULL_PTR) {
        return 1;
    }
    return 0;
}

int main(void)
{
    unsigned failures = 0U;

#define CHECK(label, expr)                                                   \
    do {                                                                     \
        if ((expr) != 0) {                                                   \
            (void)fprintf(stderr, "FAIL: %s\n", label);                      \
            ++failures;                                                      \
        }                                                                    \
    } while (0)

    CHECK("lookup success", verify_lookup_success());
    CHECK("lookup second row", verify_lookup_second_row());
    CHECK("lookup not found", verify_lookup_not_found());
    CHECK("lookup presence only", verify_lookup_presence_only());
    CHECK("null args", verify_null_args());
    CHECK("empty table null tools", verify_empty_table_null_tools());
    CHECK("empty table nonnull tools", verify_empty_table_nonnull_tools());
    CHECK("malformed table", verify_malformed_table());
    CHECK("null name row skipped", verify_null_name_row_skipped());
    CHECK("first match wins", verify_first_match_wins());
    CHECK("fn pointer", verify_fn_pointer());
    CHECK("overflow reporting", verify_overflow_reporting());
    CHECK("tool null core", verify_tool_null_core());

#undef CHECK

    if (failures == 0U) {
        (void)printf("test_tool: ok\n");
    }
    return failures == 0U ? 0 : 1;
}

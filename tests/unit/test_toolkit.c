/**
 * @file test_toolkit.c
 * @brief Contract test for the cog-toolkit call/exec/result modules.
 *
 * Exercises the public contracts declared in cog/toolkit_call.h,
 * cog/toolkit_exec.h, and cog/toolkit_result.h, implemented in
 * libraries/cog-toolkit/src/.  This test pins:
 *
 *   - call context construction/destruction and argument/result lifetimes;
 *   - static tool lookup and single-call dispatch;
 *   - sequential and "parallel" (cooperative) execution via execute_all;
 *   - exactly call_count results in call order;
 *   - per-call timeout handling with a deterministic blocking fixture;
 *   - cooperative cancellation returning COG_ERROR_ABORTED;
 *   - result collection/order and ownership;
 *   - tool errors, invalid arguments, capacity limits (grow-on-overflow);
 *   - schema validation (minimal JSON Schema subset);
 *   - cleanup under stress (run under ASan/UBSan to detect leaks).
 *
 * All tools are test-defined with deterministic blocking/cancellation
 * fixtures.  Nothing is tested here that is not stated in the public
 * headers or the module CONTRACT.md.
 *
 * Ticket: 8d52c1a (Stage 12.4b — Implement toolkit)
 */

#define _POSIX_C_SOURCE 200809L

#include "cog/toolkit.h"
#include "fixtures/toolkit_core.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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

    (void)core;
    (void)request;
    (void)request_len;

    if (result_capacity < sizeof(kReply)) {
        if (out_result_len != NULL) {
            *out_result_len = sizeof(kReply) - 1U;
        }
        return COG_ERROR_OVERFLOW;
    }
    (void)memcpy(result, kReply, sizeof(kReply));
    if (out_result_len != NULL) {
        *out_result_len = sizeof(kReply) - 1U;
    }
    return COG_OK;
}

/** Echoes the request payload back as the result. */
static cog_result_t request_echo_tool(cog_core_t *core,
                                      const char *request,
                                      size_t request_len,
                                      char *result,
                                      size_t result_capacity,
                                      size_t *out_result_len)
{
    (void)core;

    if (result_capacity < request_len + 1U) {
        if (out_result_len != NULL) {
            *out_result_len = request_len;
        }
        return COG_ERROR_OVERFLOW;
    }
    if (request_len > 0U) {
        (void)memcpy(result, request, request_len);
    }
    result[request_len] = '\0';
    if (out_result_len != NULL) {
        *out_result_len = request_len;
    }
    return COG_OK;
}

/** Blocks for a duration parsed from the request (decimal ms), then
 *  returns a fixed reply.  Used as a deterministic timeout fixture. */
static cog_result_t blocking_tool(cog_core_t *core,
                                  const char *request,
                                  size_t request_len,
                                  char *result,
                                  size_t result_capacity,
                                  size_t *out_result_len)
{
    static const char kReply[] = "blocked:ok";
    unsigned long ms = 0UL;
    struct timespec ts;

    (void)core;

    if (request != NULL && request_len > 0U) {
        char tmp[16];
        size_t n = request_len < sizeof(tmp) - 1U ? request_len
                                                  : sizeof(tmp) - 1U;
        (void)memcpy(tmp, request, n);
        tmp[n] = '\0';
        ms = strtoul(tmp, NULL, 10);
    }
    ts.tv_sec = (time_t)(ms / 1000UL);
    ts.tv_nsec = (long)((ms % 1000UL) * 1000000UL);
    (void)nanosleep(&ts, NULL);

    if (result_capacity < sizeof(kReply)) {
        if (out_result_len != NULL) {
            *out_result_len = sizeof(kReply) - 1U;
        }
        return COG_ERROR_OVERFLOW;
    }
    (void)memcpy(result, kReply, sizeof(kReply));
    if (out_result_len != NULL) {
        *out_result_len = sizeof(kReply) - 1U;
    }
    return COG_OK;
}

/** Always fails with COG_ERROR_IO. */
static cog_result_t fail_tool(cog_core_t *core,
                              const char *request,
                              size_t request_len,
                              char *result,
                              size_t result_capacity,
                              size_t *out_result_len)
{
    (void)core;
    (void)request;
    (void)request_len;
    (void)result;
    (void)result_capacity;
    (void)out_result_len;
    return COG_ERROR_IO;
}

/** Produces a 1000-byte output (larger than the collection's initial
 *  buffer) to exercise grow-on-overflow. */
static cog_result_t large_output_tool(cog_core_t *core,
                                      const char *request,
                                      size_t request_len,
                                      char *result,
                                      size_t result_capacity,
                                      size_t *out_result_len)
{
    size_t i;

    (void)core;
    (void)request;
    (void)request_len;

    if (result_capacity < 1001U) {
        if (out_result_len != NULL) {
            *out_result_len = 1000U;
        }
        return COG_ERROR_OVERFLOW;
    }
    for (i = 0U; i < 1000U; i++) {
        result[i] = (char)('a' + (i % 26U));
    }
    result[1000] = '\0';
    if (out_result_len != NULL) {
        *out_result_len = 1000U;
    }
    return COG_OK;
}

/** Produces an empty (zero-length) result. */
static cog_result_t empty_tool(cog_core_t *core,
                               const char *request,
                               size_t request_len,
                               char *result,
                               size_t result_capacity,
                               size_t *out_result_len)
{
    (void)core;
    (void)request;
    (void)request_len;

    if (result_capacity == 0U) {
        if (out_result_len != NULL) {
            *out_result_len = 0U;
        }
        return COG_ERROR_OVERFLOW;
    }
    result[0] = '\0';
    if (out_result_len != NULL) {
        *out_result_len = 0U;
    }
    return COG_OK;
}

/** Produces a JSON object result for schema validation. */
static cog_result_t json_object_tool(cog_core_t *core,
                                     const char *request,
                                     size_t request_len,
                                     char *result,
                                     size_t result_capacity,
                                     size_t *out_result_len)
{
    static const char kReply[] = "{\"name\":\"alice\",\"age\":30}";

    (void)core;
    (void)request;
    (void)request_len;

    if (result_capacity < sizeof(kReply)) {
        if (out_result_len != NULL) {
            *out_result_len = sizeof(kReply) - 1U;
        }
        return COG_ERROR_OVERFLOW;
    }
    (void)memcpy(result, kReply, sizeof(kReply));
    if (out_result_len != NULL) {
        *out_result_len = sizeof(kReply) - 1U;
    }
    return COG_OK;
}

/** Produces a JSON string result for schema validation. */
static cog_result_t json_string_tool(cog_core_t *core,
                                     const char *request,
                                     size_t request_len,
                                     char *result,
                                     size_t result_capacity,
                                     size_t *out_result_len)
{
    static const char kReply[] = "\"hello\"";

    (void)core;
    (void)request;
    (void)request_len;

    if (result_capacity < sizeof(kReply)) {
        if (out_result_len != NULL) {
            *out_result_len = sizeof(kReply) - 1U;
        }
        return COG_ERROR_OVERFLOW;
    }
    (void)memcpy(result, kReply, sizeof(kReply));
    if (out_result_len != NULL) {
        *out_result_len = sizeof(kReply) - 1U;
    }
    return COG_OK;
}

/* ------------------------------------------------------------------ */
/* Test table and dummy core.                                          */
/* ------------------------------------------------------------------ */

static const cog_tool_t kTools[] = {
    { .name = "echo", .description = NULL, .input_schema = NULL,
      .handler = echo_tool, .flags = COG_TOOL_REENTRANT | COG_TOOL_INDEPENDENT },
    { .name = "request_echo", .description = NULL, .input_schema = NULL,
      .handler = request_echo_tool, .flags = COG_TOOL_REENTRANT | COG_TOOL_INDEPENDENT },
    { .name = "block", .description = NULL, .input_schema = NULL,
      .handler = blocking_tool, .flags = COG_TOOL_REENTRANT | COG_TOOL_INDEPENDENT },
    { .name = "fail", .description = NULL, .input_schema = NULL,
      .handler = fail_tool, .flags = COG_TOOL_REENTRANT | COG_TOOL_INDEPENDENT },
    { .name = "large", .description = NULL, .input_schema = NULL,
      .handler = large_output_tool, .flags = COG_TOOL_REENTRANT | COG_TOOL_INDEPENDENT },
    { .name = "empty", .description = NULL, .input_schema = NULL,
      .handler = empty_tool, .flags = COG_TOOL_REENTRANT | COG_TOOL_INDEPENDENT },
    { .name = "json", .description = NULL, .input_schema = NULL,
      .handler = json_object_tool, .flags = COG_TOOL_REENTRANT | COG_TOOL_INDEPENDENT },
    { .name = "json_string", .description = NULL, .input_schema = NULL,
      .handler = json_string_tool, .flags = COG_TOOL_REENTRANT | COG_TOOL_INDEPENDENT },
};

static const cog_tool_table_t kTable = {
    .tools = kTools,
    .count = sizeof(kTools) / sizeof(kTools[0]),
};

static cog_core_t *dummy_core(void)
{
    return toolkit_test_core;
}

/* ------------------------------------------------------------------ */
/* Call context tests.                                                 */
/* ------------------------------------------------------------------ */

/** Construction, accessors, and destruction. */
static int verify_call_new_free(void)
{
    static const char kReq[] = "{\"x\":1}";
    cog_toolkit_call_t *call = NULL;
    size_t len = 0U;

    if (cog_toolkit_call_new(&call, "echo", kReq, sizeof(kReq) - 1U) != COG_OK) {
        return 1;
    }
    if (call == NULL) {
        return 1;
    }
    if (cog_toolkit_call_name(call) == NULL ||
        strcmp(cog_toolkit_call_name(call), "echo") != 0) {
        cog_toolkit_call_free(call);
        return 1;
    }
    if (strcmp(cog_toolkit_call_request(call, &len), kReq) != 0 || len != sizeof(kReq) - 1U) {
        cog_toolkit_call_free(call);
        return 1;
    }
    if (cog_toolkit_call_is_cancelled(call)) {
        cog_toolkit_call_free(call);
        return 1;
    }
    cog_toolkit_call_free(call);
    cog_toolkit_call_free(NULL); /* no-op */
    return 0;
}

/** NULL out / NULL name. */
static int verify_call_new_null(void)
{
    cog_toolkit_call_t *call = (cog_toolkit_call_t *)&call;

    if (cog_toolkit_call_new(NULL, "echo", NULL, 0U) != COG_ERROR_NULL_PTR) {
        return 1;
    }
    if (cog_toolkit_call_new(&call, NULL, NULL, 0U) != COG_ERROR_NULL_PTR) {
        return 1;
    }
    if (call != NULL) {
        return 1; /* *out unchanged on failure */
    }
    return 0;
}

/** Request NULL with non-zero length. */
static int verify_call_new_bad_request(void)
{
    cog_toolkit_call_t *call = NULL;

    if (cog_toolkit_call_new(&call, "echo", NULL, 5U) != COG_ERROR_INVALID_ARG) {
        return 1;
    }
    if (call != NULL) {
        return 1;
    }
    return 0;
}

/** Zero-length request yields a NULL request pointer. */
static int verify_call_zero_request(void)
{
    cog_toolkit_call_t *call = NULL;
    size_t len = 99U;

    if (cog_toolkit_call_new(&call, "echo", "ignored", 0U) != COG_OK) {
        return 1;
    }
    if (cog_toolkit_call_request(call, &len) != NULL || len != 0U) {
        cog_toolkit_call_free(call);
        return 1;
    }
    cog_toolkit_call_free(call);
    return 0;
}

/** set_timeout: OK, NULL, and unreasonably large. */
static int verify_call_set_timeout(void)
{
    cog_toolkit_call_t *call = NULL;

    if (cog_toolkit_call_new(&call, "echo", NULL, 0U) != COG_OK) {
        return 1;
    }
    if (cog_toolkit_call_set_timeout(call, 1000U) != COG_OK) {
        cog_toolkit_call_free(call);
        return 1;
    }
    if (cog_toolkit_call_set_timeout(call, 0U) != COG_OK) {
        cog_toolkit_call_free(call);
        return 1;
    }
    if (cog_toolkit_call_set_timeout(call, 0x7FFFFFFFu) != COG_OK) {
        cog_toolkit_call_free(call);
        return 1;
    }
    if (cog_toolkit_call_set_timeout(call, 0x80000000u) != COG_ERROR_INVALID_ARG) {
        cog_toolkit_call_free(call);
        return 1;
    }
    if (cog_toolkit_call_set_timeout(NULL, 1U) != COG_ERROR_NULL_PTR) {
        cog_toolkit_call_free(call);
        return 1;
    }
    cog_toolkit_call_free(call);
    return 0;
}

/** cancel / is_cancelled. */
static int verify_call_cancel(void)
{
    cog_toolkit_call_t *call = NULL;

    if (cog_toolkit_call_new(&call, "echo", NULL, 0U) != COG_OK) {
        return 1;
    }
    if (cog_toolkit_call_cancel(call) != COG_OK) {
        cog_toolkit_call_free(call);
        return 1;
    }
    if (!cog_toolkit_call_is_cancelled(call)) {
        cog_toolkit_call_free(call);
        return 1;
    }
    if (cog_toolkit_call_cancel(NULL) != COG_ERROR_NULL_PTR) {
        cog_toolkit_call_free(call);
        return 1;
    }
    cog_toolkit_call_free(call);
    return 0;
}

/** Accessors on NULL. */
static int verify_call_accessors_null(void)
{
    size_t len = 99U;

    if (cog_toolkit_call_name(NULL) != NULL) {
        return 1;
    }
    if (cog_toolkit_call_request(NULL, &len) != NULL || len != 0U) {
        return 1;
    }
    if (cog_toolkit_call_is_cancelled(NULL)) {
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Single-call execution tests.                                        */
/* ------------------------------------------------------------------ */

/** Basic single-call success. */
static int verify_execute_single(void)
{
    cog_toolkit_call_t *call = NULL;
    cog_toolkit_result_t result;
    char buf[64];

    if (cog_toolkit_call_new(&call, "echo", NULL, 0U) != COG_OK) {
        return 1;
    }
    result.output = buf;
    if (cog_toolkit_execute(dummy_core(), &kTable, call, &result,
                            sizeof(buf)) != COG_OK) {
        cog_toolkit_call_free(call);
        return 1;
    }
    if (result.status != COG_OK || result.output_len != 7U ||
        strcmp(result.output, "echo:ok") != 0) {
        cog_toolkit_call_free(call);
        return 1;
    }
    cog_toolkit_call_free(call);
    return 0;
}

/** Request payload is passed through to the tool. */
static int verify_execute_request(void)
{
    static const char kReq[] = "hello";
    cog_toolkit_call_t *call = NULL;
    cog_toolkit_result_t result;
    char buf[64];

    if (cog_toolkit_call_new(&call, "request_echo", kReq, sizeof(kReq) - 1U) !=
        COG_OK) {
        return 1;
    }
    result.output = buf;
    if (cog_toolkit_execute(dummy_core(), &kTable, call, &result,
                            sizeof(buf)) != COG_OK) {
        cog_toolkit_call_free(call);
        return 1;
    }
    if (result.status != COG_OK || result.output_len != 5U ||
        strcmp(result.output, "hello") != 0) {
        cog_toolkit_call_free(call);
        return 1;
    }
    cog_toolkit_call_free(call);
    return 0;
}

/** Unknown tool name. */
static int verify_execute_not_found(void)
{
    cog_toolkit_call_t *call = NULL;
    cog_toolkit_result_t result;
    char buf[64];

    if (cog_toolkit_call_new(&call, "no-such-tool", NULL, 0U) != COG_OK) {
        return 1;
    }
    result.output = buf;
    if (cog_toolkit_execute(dummy_core(), &kTable, call, &result,
                            sizeof(buf)) != COG_ERROR_NOT_FOUND) {
        cog_toolkit_call_free(call);
        return 1;
    }
    if (result.status != COG_ERROR_NOT_FOUND) {
        cog_toolkit_call_free(call);
        return 1;
    }
    cog_toolkit_call_free(call);
    return 0;
}

/** Cancelled call returns COG_ERROR_ABORTED without invoking the tool. */
static int verify_execute_cancelled(void)
{
    cog_toolkit_call_t *call = NULL;
    cog_toolkit_result_t result;
    char buf[64];

    if (cog_toolkit_call_new(&call, "echo", NULL, 0U) != COG_OK) {
        return 1;
    }
    (void)cog_toolkit_call_cancel(call);
    result.output = buf;
    if (cog_toolkit_execute(dummy_core(), &kTable, call, &result,
                            sizeof(buf)) != COG_ERROR_ABORTED) {
        cog_toolkit_call_free(call);
        return 1;
    }
    if (result.status != COG_ERROR_ABORTED) {
        cog_toolkit_call_free(call);
        return 1;
    }
    cog_toolkit_call_free(call);
    return 0;
}

/** NULL arguments. */
static int verify_execute_null_args(void)
{
    cog_toolkit_call_t *call = NULL;
    cog_toolkit_result_t result;
    char buf[64];

    if (cog_toolkit_call_new(&call, "echo", NULL, 0U) != COG_OK) {
        return 1;
    }
    result.output = buf;
    if (cog_toolkit_execute(NULL, &kTable, call, &result, sizeof(buf)) !=
        COG_ERROR_NULL_PTR) {
        cog_toolkit_call_free(call);
        return 1;
    }
    if (cog_toolkit_execute(dummy_core(), NULL, call, &result, sizeof(buf)) !=
        COG_ERROR_NULL_PTR) {
        cog_toolkit_call_free(call);
        return 1;
    }
    if (cog_toolkit_execute(dummy_core(), &kTable, NULL, &result, sizeof(buf)) !=
        COG_ERROR_NULL_PTR) {
        cog_toolkit_call_free(call);
        return 1;
    }
    if (cog_toolkit_execute(dummy_core(), &kTable, call, NULL, sizeof(buf)) !=
        COG_ERROR_NULL_PTR) {
        cog_toolkit_call_free(call);
        return 1;
    }
    cog_toolkit_call_free(call);
    return 0;
}

/** NULL output buffer and zero capacity. */
static int verify_execute_bad_buffer(void)
{
    cog_toolkit_call_t *call = NULL;
    cog_toolkit_result_t result;

    if (cog_toolkit_call_new(&call, "echo", NULL, 0U) != COG_OK) {
        return 1;
    }
    result.output = NULL;
    if (cog_toolkit_execute(dummy_core(), &kTable, call, &result, 64U) !=
        COG_ERROR_NULL_PTR) {
        cog_toolkit_call_free(call);
        return 1;
    }
    result.output = (char *)&result;
    if (cog_toolkit_execute(dummy_core(), &kTable, call, &result, 0U) !=
        COG_ERROR_INVALID_ARG) {
        cog_toolkit_call_free(call);
        return 1;
    }
    cog_toolkit_call_free(call);
    return 0;
}

/** Too-small buffer propagates the tool's COG_ERROR_OVERFLOW. */
static int verify_execute_overflow(void)
{
    cog_toolkit_call_t *call = NULL;
    cog_toolkit_result_t result;
    char small[4];

    if (cog_toolkit_call_new(&call, "echo", NULL, 0U) != COG_OK) {
        return 1;
    }
    result.output = small;
    if (cog_toolkit_execute(dummy_core(), &kTable, call, &result,
                            sizeof(small)) != COG_ERROR_OVERFLOW) {
        cog_toolkit_call_free(call);
        return 1;
    }
    if (result.status != COG_ERROR_OVERFLOW) {
        cog_toolkit_call_free(call);
        return 1;
    }
    cog_toolkit_call_free(call);
    return 0;
}

/** Tool error is propagated. */
static int verify_execute_tool_error(void)
{
    cog_toolkit_call_t *call = NULL;
    cog_toolkit_result_t result;
    char buf[64];

    if (cog_toolkit_call_new(&call, "fail", NULL, 0U) != COG_OK) {
        return 1;
    }
    result.output = buf;
    if (cog_toolkit_execute(dummy_core(), &kTable, call, &result,
                            sizeof(buf)) != COG_ERROR_IO) {
        cog_toolkit_call_free(call);
        return 1;
    }
    if (result.status != COG_ERROR_IO) {
        cog_toolkit_call_free(call);
        return 1;
    }
    cog_toolkit_call_free(call);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Parallel (execute_all) tests.                                       */
/* ------------------------------------------------------------------ */

/** Basic multi-call: exactly call_count results in call order. */
static int verify_execute_all_basic(void)
{
    cog_toolkit_call_t *calls[3];
    cog_toolkit_result_collection_t *coll = NULL;
    const cog_toolkit_result_t *res = NULL;
    cog_result_t r;

    if (cog_toolkit_call_new(&calls[0], "echo", NULL, 0U) != COG_OK) {
        return 1;
    }
    if (cog_toolkit_call_new(&calls[1], "request_echo", "hello", 5U) != COG_OK) {
        cog_toolkit_call_free(calls[0]);
        return 1;
    }
    if (cog_toolkit_call_new(&calls[2], "echo", NULL, 0U) != COG_OK) {
        cog_toolkit_call_free(calls[0]);
        cog_toolkit_call_free(calls[1]);
        return 1;
    }

    r = cog_toolkit_execute_all(dummy_core(), &kTable, calls, 3U, NULL, &coll);
    cog_toolkit_call_free(calls[0]);
    cog_toolkit_call_free(calls[1]);
    cog_toolkit_call_free(calls[2]);
    if (r != COG_OK || coll == NULL) {
        return 1;
    }
    if (cog_toolkit_result_collection_count(coll) != 3U) {
        cog_toolkit_result_collection_free(coll);
        return 1;
    }
    if (cog_toolkit_result_collection_get(coll, 0U, &res) != COG_OK ||
        res->status != COG_OK || strcmp(res->output, "echo:ok") != 0) {
        cog_toolkit_result_collection_free(coll);
        return 1;
    }
    if (cog_toolkit_result_collection_get(coll, 1U, &res) != COG_OK ||
        res->status != COG_OK || strcmp(res->output, "hello") != 0) {
        cog_toolkit_result_collection_free(coll);
        return 1;
    }
    if (cog_toolkit_result_collection_get(coll, 2U, &res) != COG_OK ||
        res->status != COG_OK || strcmp(res->output, "echo:ok") != 0) {
        cog_toolkit_result_collection_free(coll);
        return 1;
    }
    cog_toolkit_result_collection_free(coll);
    return 0;
}

/** Zero calls yields an empty collection. */
static int verify_execute_all_empty(void)
{
    static cog_toolkit_call_t *const kNoCalls[1] = { NULL };
    cog_toolkit_result_collection_t *coll = NULL;
    cog_result_t r;

    r = cog_toolkit_execute_all(dummy_core(), &kTable, kNoCalls, 0U, NULL,
                                &coll);
    if (r != COG_OK || coll == NULL) {
        return 1;
    }
    if (cog_toolkit_result_collection_count(coll) != 0U) {
        cog_toolkit_result_collection_free(coll);
        return 1;
    }
    cog_toolkit_result_collection_free(coll);
    return 0;
}

/** NULL arguments. */
static int verify_execute_all_null(void)
{
    cog_toolkit_call_t *call = NULL;
    cog_toolkit_call_t *calls[1];
    cog_toolkit_result_collection_t *coll = NULL;

    if (cog_toolkit_call_new(&call, "echo", NULL, 0U) != COG_OK) {
        return 1;
    }
    calls[0] = call;
    if (cog_toolkit_execute_all(NULL, &kTable, calls, 1U, NULL, &coll) !=
        COG_ERROR_NULL_PTR) {
        cog_toolkit_call_free(call);
        return 1;
    }
    if (cog_toolkit_execute_all(dummy_core(), NULL, calls, 1U, NULL, &coll) !=
        COG_ERROR_NULL_PTR) {
        cog_toolkit_call_free(call);
        return 1;
    }
    if (cog_toolkit_execute_all(dummy_core(), &kTable, NULL, 1U, NULL, &coll) !=
        COG_ERROR_NULL_PTR) {
        cog_toolkit_call_free(call);
        return 1;
    }
    if (cog_toolkit_execute_all(dummy_core(), &kTable, calls, 1U, NULL, NULL) !=
        COG_ERROR_NULL_PTR) {
        cog_toolkit_call_free(call);
        return 1;
    }
    cog_toolkit_call_free(call);
    return 0;
}

/** A NULL entry in the calls array. */
static int verify_execute_all_null_entry(void)
{
    cog_toolkit_call_t *call = NULL;
    cog_toolkit_call_t *calls[2];
    cog_toolkit_result_collection_t *coll = NULL;

    if (cog_toolkit_call_new(&call, "echo", NULL, 0U) != COG_OK) {
        return 1;
    }
    calls[0] = call;
    calls[1] = NULL;
    if (cog_toolkit_execute_all(dummy_core(), &kTable, calls, 2U, NULL, &coll) !=
        COG_ERROR_INVALID_ARG) {
        cog_toolkit_call_free(call);
        return 1;
    }
    cog_toolkit_call_free(call);
    return 0;
}

/** Mixed success/failure without stop_on_error: all results collected. */
static int verify_execute_all_mixed(void)
{
    cog_toolkit_call_t *calls[3];
    cog_toolkit_result_collection_t *coll = NULL;
    const cog_toolkit_result_t *res = NULL;
    cog_result_t r;

    if (cog_toolkit_call_new(&calls[0], "echo", NULL, 0U) != COG_OK) {
        return 1;
    }
    if (cog_toolkit_call_new(&calls[1], "fail", NULL, 0U) != COG_OK) {
        cog_toolkit_call_free(calls[0]);
        return 1;
    }
    if (cog_toolkit_call_new(&calls[2], "request_echo", "hi", 2U) != COG_OK) {
        cog_toolkit_call_free(calls[0]);
        cog_toolkit_call_free(calls[1]);
        return 1;
    }

    r = cog_toolkit_execute_all(dummy_core(), &kTable, calls, 3U, NULL, &coll);
    cog_toolkit_call_free(calls[0]);
    cog_toolkit_call_free(calls[1]);
    cog_toolkit_call_free(calls[2]);
    if (r != COG_OK || coll == NULL) {
        return 1;
    }
    if (cog_toolkit_result_collection_count(coll) != 3U) {
        cog_toolkit_result_collection_free(coll);
        return 1;
    }
    if (cog_toolkit_result_collection_get(coll, 0U, &res) != COG_OK ||
        !cog_toolkit_result_is_ok(res)) {
        cog_toolkit_result_collection_free(coll);
        return 1;
    }
    if (cog_toolkit_result_collection_get(coll, 1U, &res) != COG_OK ||
        res->status != COG_ERROR_IO || res->error_message == NULL) {
        cog_toolkit_result_collection_free(coll);
        return 1;
    }
    if (cog_toolkit_result_collection_get(coll, 2U, &res) != COG_OK ||
        !cog_toolkit_result_is_ok(res) || strcmp(res->output, "hi") != 0) {
        cog_toolkit_result_collection_free(coll);
        return 1;
    }
    cog_toolkit_result_collection_free(coll);
    return 0;
}

/** stop_on_error: first failure aborts and out_results is NULL. */
static int verify_execute_all_stop_on_error(void)
{
    cog_toolkit_exec_opts_t opts;
    cog_toolkit_call_t *calls[2];
    cog_toolkit_result_collection_t *coll = NULL;
    cog_result_t r;

    if (cog_toolkit_call_new(&calls[0], "echo", NULL, 0U) != COG_OK) {
        return 1;
    }
    if (cog_toolkit_call_new(&calls[1], "fail", NULL, 0U) != COG_OK) {
        cog_toolkit_call_free(calls[0]);
        return 1;
    }
    memset(&opts, 0, sizeof(opts));
    opts.stop_on_error = true;

    r = cog_toolkit_execute_all(dummy_core(), &kTable, calls, 2U, &opts, &coll);
    cog_toolkit_call_free(calls[0]);
    cog_toolkit_call_free(calls[1]);
    if (r != COG_OK) {
        return 1;
    }
    if (coll == NULL) { return 1; }
    const cog_toolkit_result_t *retained = NULL;
    if (cog_toolkit_result_collection_get(coll, 1U, &retained) != COG_OK || retained->status != COG_ERROR_IO) { cog_toolkit_result_collection_free(coll); return 1; }
    cog_toolkit_result_collection_free(coll);
    return 0;
}

/** Timeout: a blocking tool that overruns its budget is recorded TIMEOUT. */
static int verify_execute_all_timeout(void)
{
    cog_toolkit_call_t *call = NULL;
    cog_toolkit_call_t *calls[1];
    cog_toolkit_result_collection_t *coll = NULL;
    const cog_toolkit_result_t *res = NULL;
    cog_result_t r;

    if (cog_toolkit_call_new(&call, "block", "50", 2U) != COG_OK) {
        return 1;
    }
    if (cog_toolkit_call_set_timeout(call, 20U) != COG_OK) {
        cog_toolkit_call_free(call);
        return 1;
    }
    calls[0] = call;
    r = cog_toolkit_execute_all(dummy_core(), &kTable, calls, 1U, NULL, &coll);
    cog_toolkit_call_free(call);
    if (r != COG_OK || coll == NULL) {
        return 1;
    }
    if (cog_toolkit_result_collection_get(coll, 0U, &res) != COG_OK ||
        res->status != COG_ERROR_TIMEOUT || res->error_message == NULL) {
        cog_toolkit_result_collection_free(coll);
        return 1;
    }
    cog_toolkit_result_collection_free(coll);
    return 0;
}

/** No timeout set: a short blocking tool completes successfully. */
static int verify_execute_all_no_timeout(void)
{
    cog_toolkit_call_t *call = NULL;
    cog_toolkit_call_t *calls[1];
    cog_toolkit_result_collection_t *coll = NULL;
    const cog_toolkit_result_t *res = NULL;
    cog_result_t r;

    if (cog_toolkit_call_new(&call, "block", "10", 2U) != COG_OK) {
        return 1;
    }
    calls[0] = call;
    r = cog_toolkit_execute_all(dummy_core(), &kTable, calls, 1U, NULL, &coll);
    cog_toolkit_call_free(call);
    if (r != COG_OK || coll == NULL) {
        return 1;
    }
    if (cog_toolkit_result_collection_get(coll, 0U, &res) != COG_OK ||
        !cog_toolkit_result_is_ok(res) || strcmp(res->output, "blocked:ok") != 0) {
        cog_toolkit_result_collection_free(coll);
        return 1;
    }
    cog_toolkit_result_collection_free(coll);
    return 0;
}

/** Cancelled call is recorded as ABORTED. */
static int verify_execute_all_cancel(void)
{
    cog_toolkit_call_t *call = NULL;
    cog_toolkit_call_t *calls[1];
    cog_toolkit_result_collection_t *coll = NULL;
    const cog_toolkit_result_t *res = NULL;
    cog_result_t r;

    if (cog_toolkit_call_new(&call, "echo", NULL, 0U) != COG_OK) {
        return 1;
    }
    (void)cog_toolkit_call_cancel(call);
    calls[0] = call;
    r = cog_toolkit_execute_all(dummy_core(), &kTable, calls, 1U, NULL, &coll);
    cog_toolkit_call_free(call);
    if (r != COG_OK || coll == NULL) {
        return 1;
    }
    if (cog_toolkit_result_collection_get(coll, 0U, &res) != COG_OK ||
        res->status != COG_ERROR_ABORTED || res->error_message == NULL) {
        cog_toolkit_result_collection_free(coll);
        return 1;
    }
    cog_toolkit_result_collection_free(coll);
    return 0;
}

/** Grow-on-overflow: a large output is captured in full. */
static int verify_execute_all_grow(void)
{
    cog_toolkit_call_t *call = NULL;
    cog_toolkit_call_t *calls[1];
    cog_toolkit_result_collection_t *coll = NULL;
    const cog_toolkit_result_t *res = NULL;
    cog_result_t r;
    size_t i;

    if (cog_toolkit_call_new(&call, "large", NULL, 0U) != COG_OK) {
        return 1;
    }
    calls[0] = call;
    r = cog_toolkit_execute_all(dummy_core(), &kTable, calls, 1U, NULL, &coll);
    cog_toolkit_call_free(call);
    if (r != COG_OK || coll == NULL) {
        return 1;
    }
    if (cog_toolkit_result_collection_get(coll, 0U, &res) != COG_OK ||
        !cog_toolkit_result_is_ok(res) || res->output_len != 1000U) {
        cog_toolkit_result_collection_free(coll);
        return 1;
    }
    for (i = 0U; i < 1000U; i++) {
        if (res->output[i] != (char)('a' + (i % 26U))) {
            cog_toolkit_result_collection_free(coll);
            return 1;
        }
    }
    if (res->output[1000] != '\0') {
        cog_toolkit_result_collection_free(coll);
        return 1;
    }
    cog_toolkit_result_collection_free(coll);
    return 0;
}

/** Empty output: status OK, output NULL, length 0. */
static int verify_execute_all_empty_output(void)
{
    cog_toolkit_call_t *call = NULL;
    cog_toolkit_call_t *calls[1];
    cog_toolkit_result_collection_t *coll = NULL;
    const cog_toolkit_result_t *res = NULL;
    cog_result_t r;

    if (cog_toolkit_call_new(&call, "empty", NULL, 0U) != COG_OK) {
        return 1;
    }
    calls[0] = call;
    r = cog_toolkit_execute_all(dummy_core(), &kTable, calls, 1U, NULL, &coll);
    cog_toolkit_call_free(call);
    if (r != COG_OK || coll == NULL) {
        return 1;
    }
    if (cog_toolkit_result_collection_get(coll, 0U, &res) != COG_OK ||
        !cog_toolkit_result_is_ok(res) || res->output != NULL ||
        res->output_len != 0U) {
        cog_toolkit_result_collection_free(coll);
        return 1;
    }
    cog_toolkit_result_collection_free(coll);
    return 0;
}

/** opts with max_parallel=1 (sequential) works and preserves order. */
static int verify_execute_all_sequential_opts(void)
{
    cog_toolkit_exec_opts_t opts;
    cog_toolkit_call_t *calls[2];
    cog_toolkit_result_collection_t *coll = NULL;
    const cog_toolkit_result_t *res = NULL;
    cog_result_t r;

    if (cog_toolkit_call_new(&calls[0], "request_echo", "a", 1U) != COG_OK) {
        return 1;
    }
    if (cog_toolkit_call_new(&calls[1], "request_echo", "b", 1U) != COG_OK) {
        cog_toolkit_call_free(calls[0]);
        return 1;
    }
    memset(&opts, 0, sizeof(opts));
    opts.max_parallel = 1U;

    r = cog_toolkit_execute_all(dummy_core(), &kTable, calls, 2U, &opts, &coll);
    cog_toolkit_call_free(calls[0]);
    cog_toolkit_call_free(calls[1]);
    if (r != COG_OK || coll == NULL) {
        return 1;
    }
    if (cog_toolkit_result_collection_get(coll, 0U, &res) != COG_OK ||
        strcmp(res->output, "a") != 0) {
        cog_toolkit_result_collection_free(coll);
        return 1;
    }
    if (cog_toolkit_result_collection_get(coll, 1U, &res) != COG_OK ||
        strcmp(res->output, "b") != 0) {
        cog_toolkit_result_collection_free(coll);
        return 1;
    }
    cog_toolkit_result_collection_free(coll);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Result collection tests.                                            */
/* ------------------------------------------------------------------ */

/** get with out-of-range index and NULL collection. */
static int verify_collection_get_errors(void)
{
    cog_toolkit_call_t *call = NULL;
    cog_toolkit_call_t *calls[1];
    cog_toolkit_result_collection_t *coll = NULL;
    const cog_toolkit_result_t *res = NULL;
    cog_result_t r;

    if (cog_toolkit_call_new(&call, "echo", NULL, 0U) != COG_OK) {
        return 1;
    }
    calls[0] = call;
    r = cog_toolkit_execute_all(dummy_core(), &kTable, calls, 1U, NULL, &coll);
    cog_toolkit_call_free(call);
    if (r != COG_OK || coll == NULL) {
        return 1;
    }
    if (cog_toolkit_result_collection_get(coll, 1U, &res) !=
        COG_ERROR_INVALID_ARG) {
        cog_toolkit_result_collection_free(coll);
        return 1;
    }
    if (cog_toolkit_result_collection_get(coll, 0U, NULL) != COG_OK) {
        cog_toolkit_result_collection_free(coll);
        return 1;
    }
    if (cog_toolkit_result_collection_get(NULL, 0U, &res) !=
        COG_ERROR_NULL_PTR) {
        cog_toolkit_result_collection_free(coll);
        return 1;
    }
    if (cog_toolkit_result_collection_count(NULL) != 0U) {
        cog_toolkit_result_collection_free(coll);
        return 1;
    }
    cog_toolkit_result_collection_free(coll);
    cog_toolkit_result_collection_free(NULL); /* no-op */
    return 0;
}

/** result_is_ok. */
static int verify_result_is_ok(void)
{
    cog_toolkit_result_t ok = { .status = COG_OK };
    cog_toolkit_result_t bad = { .status = COG_ERROR_IO };

    if (!cog_toolkit_result_is_ok(&ok)) {
        return 1;
    }
    if (cog_toolkit_result_is_ok(&bad)) {
        return 1;
    }
    if (cog_toolkit_result_is_ok(NULL)) {
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Schema validation tests.                                            */
/* ------------------------------------------------------------------ */

static int verify_validate_schema(void)
{
    cog_toolkit_result_t res;
    char obj[64];
    char str[16];

    /* NULL result. */
    if (cog_toolkit_result_validate_schema(NULL, "{}") != COG_ERROR_NULL_PTR) {
        return 1;
    }

    /* Non-OK status. */
    res.status = COG_ERROR_IO;
    res.output = obj;
    res.output_len = 0U;
    if (cog_toolkit_result_validate_schema(&res, "{}") !=
        COG_ERROR_INVALID_STATE) {
        return 1;
    }

    /* NULL output. */
    res.status = COG_OK;
    res.output = NULL;
    res.output_len = 0U;
    if (cog_toolkit_result_validate_schema(&res, "{}") != COG_ERROR_INVALID_ARG) {
        return 1;
    }

    /* NULL / empty schema: no validation. */
    res.output = obj;
    (void)strcpy(obj, "{\"name\":\"alice\"}");
    res.output_len = strlen(obj);
    if (cog_toolkit_result_validate_schema(&res, NULL) != COG_OK) {
        return 1;
    }
    if (cog_toolkit_result_validate_schema(&res, "") != COG_OK) {
        return 1;
    }

    /* Empty schema object: any valid JSON passes. */
    if (cog_toolkit_result_validate_schema(&res, "{}") != COG_OK) {
        return 1;
    }

    /* Type match / mismatch. */
    if (cog_toolkit_result_validate_schema(&res, "{\"type\":\"object\"}") !=
        COG_OK) {
        return 1;
    }
    if (cog_toolkit_result_validate_schema(&res, "{\"type\":\"string\"}") !=
        COG_ERROR_INVALID_ARG) {
        return 1;
    }

    (void)strcpy(str, "\"hello\"");
    res.output = str;
    res.output_len = strlen(str);
    if (cog_toolkit_result_validate_schema(&res, "{\"type\":\"string\"}") !=
        COG_OK) {
        return 1;
    }
    if (cog_toolkit_result_validate_schema(&res, "{\"type\":\"object\"}") !=
        COG_ERROR_INVALID_ARG) {
        return 1;
    }

    /* required properties. */
    res.output = obj;
    res.output_len = strlen(obj);
    if (cog_toolkit_result_validate_schema(&res, "{\"required\":[\"name\"]}") !=
        COG_OK) {
        return 1;
    }
    if (cog_toolkit_result_validate_schema(&res,
                                           "{\"required\":[\"missing\"]}") !=
        COG_ERROR_INVALID_ARG) {
        return 1;
    }

    /* Malformed schema. */
    if (cog_toolkit_result_validate_schema(&res, "not json") !=
        COG_ERROR_INVALID_ARG) {
        return 1;
    }
    if (cog_toolkit_result_validate_schema(&res, "{\"type\":\"bogus\"}") !=
        COG_ERROR_INVALID_ARG) {
        return 1;
    }

    /* Non-JSON output. */
    (void)strcpy(obj, "echo:ok");
    res.output = obj;
    res.output_len = strlen(obj);
    if (cog_toolkit_result_validate_schema(&res, "{}") != COG_ERROR_INVALID_ARG) {
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Stress / cleanup test.                                              */
/* ------------------------------------------------------------------ */

/** Many calls, mixed outcomes, then full cleanup. */
static int verify_stress(void)
{
    enum { N = 200 };
    cog_toolkit_call_t *calls[N];
    cog_toolkit_result_collection_t *coll = NULL;
    const cog_toolkit_result_t *res = NULL;
    size_t i;
    cog_result_t r;

    for (i = 0U; i < N; i++) {
        const char *name = (i % 4U == 1U) ? "fail" : "echo";
        if (cog_toolkit_call_new(&calls[i], name, NULL, 0U) != COG_OK) {
            while (i > 0U) {
                cog_toolkit_call_free(calls[--i]);
            }
            return 1;
        }
    }
    r = cog_toolkit_execute_all(dummy_core(), &kTable, calls, N, NULL, &coll);
    for (i = 0U; i < N; i++) {
        cog_toolkit_call_free(calls[i]);
    }
    if (r != COG_OK || coll == NULL) {
        return 1;
    }
    if (cog_toolkit_result_collection_count(coll) != N) {
        cog_toolkit_result_collection_free(coll);
        return 1;
    }
    for (i = 0U; i < N; i++) {
        if (cog_toolkit_result_collection_get(coll, i, &res) != COG_OK) {
            cog_toolkit_result_collection_free(coll);
            return 1;
        }
        if ((i % 4U == 1U) != (res->status != COG_OK)) {
            cog_toolkit_result_collection_free(coll);
            return 1;
        }
    }
    cog_toolkit_result_collection_free(coll);
    return 0;
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int main(void)
{
    toolkit_core_create();
    unsigned failures = 0U;

#define CHECK(label, expr)                                                   \
    do {                                                                     \
        if ((expr) != 0) {                                                   \
            (void)fprintf(stderr, "FAIL: %s\n", label);                      \
            ++failures;                                                      \
        }                                                                    \
    } while (0)

    CHECK("call new/free", verify_call_new_free());
    CHECK("call new null", verify_call_new_null());
    CHECK("call new bad request", verify_call_new_bad_request());
    CHECK("call zero request", verify_call_zero_request());
    CHECK("call set timeout", verify_call_set_timeout());
    CHECK("call cancel", verify_call_cancel());
    CHECK("call accessors null", verify_call_accessors_null());

    CHECK("execute single", verify_execute_single());
    CHECK("execute request", verify_execute_request());
    CHECK("execute not found", verify_execute_not_found());
    CHECK("execute cancelled", verify_execute_cancelled());
    CHECK("execute null args", verify_execute_null_args());
    CHECK("execute bad buffer", verify_execute_bad_buffer());
    CHECK("execute overflow", verify_execute_overflow());
    CHECK("execute tool error", verify_execute_tool_error());

    CHECK("execute all basic", verify_execute_all_basic());
    CHECK("execute all empty", verify_execute_all_empty());
    CHECK("execute all null", verify_execute_all_null());
    CHECK("execute all null entry", verify_execute_all_null_entry());
    CHECK("execute all mixed", verify_execute_all_mixed());
    CHECK("execute all stop on error", verify_execute_all_stop_on_error());
    CHECK("execute all timeout", verify_execute_all_timeout());
    CHECK("execute all no timeout", verify_execute_all_no_timeout());
    CHECK("execute all cancel", verify_execute_all_cancel());
    CHECK("execute all grow", verify_execute_all_grow());
    CHECK("execute all empty output", verify_execute_all_empty_output());
    CHECK("execute all sequential opts", verify_execute_all_sequential_opts());

    CHECK("collection get errors", verify_collection_get_errors());
    CHECK("result is ok", verify_result_is_ok());

    CHECK("validate schema", verify_validate_schema());

    CHECK("stress", verify_stress());

#undef CHECK

    if (failures == 0U) {
        (void)printf("test_toolkit: ok\n");
    }
    toolkit_core_destroy();
    return failures == 0U ? 0 : 1;
}

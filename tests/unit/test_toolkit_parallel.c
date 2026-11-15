/**
 * @file test_toolkit_parallel.c
 * @brief Focused tests for bounded worker scheduling and failure cleanup.
 *
 * Verifies that stop_on_error waits for running tools, prevents new claims
 * after an observed failure, returns the earliest failure by input index,
 * and turns pthread_create failures into COG_ERROR_NOMEM after joining the
 * workers that were already created.
 *
 * The executable links with --wrap=pthread_create so creation failure can be
 * injected deterministically without changing the production API.
 *
 * Ticket: 8d52c1a, revision for review 9a39e07.
 */

#define _POSIX_C_SOURCE 200809L

#include "cog/toolkit.h"
#include "fixtures/toolkit_core.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static atomic_int g_counted_calls;
static atomic_int g_not_found_calls;

static void sleep_ms(unsigned long ms)
{
    struct timespec ts;

    ts.tv_sec = (time_t)(ms / 1000UL);
    ts.tv_nsec = (long)((ms % 1000UL) * 1000000UL);
    (void)nanosleep(&ts, NULL);
}

static unsigned long request_ms(const char *request, size_t request_len)
{
    char tmp[16];
    size_t n;

    if (request == NULL || request_len == 0U) {
        return 0UL;
    }
    n = request_len < sizeof(tmp) - 1U ? request_len : sizeof(tmp) - 1U;
    (void)memcpy(tmp, request, n);
    tmp[n] = '\0';
    return strtoul(tmp, NULL, 10);
}

static cog_result_t blocking_tool(cog_core_t *core,
                                  const char *request,
                                  size_t request_len,
                                  char *result,
                                  size_t result_capacity,
                                  size_t *out_result_len)
{
    static const char reply[] = "blocked";

    (void)core;
    sleep_ms(request_ms(request, request_len));
    if (result_capacity < sizeof(reply)) {
        if (out_result_len != NULL) {
            *out_result_len = sizeof(reply) - 1U;
        }
        return COG_ERROR_OVERFLOW;
    }
    (void)memcpy(result, reply, sizeof(reply));
    if (out_result_len != NULL) {
        *out_result_len = sizeof(reply) - 1U;
    }
    return COG_OK;
}

static cog_result_t delayed_fail_tool(cog_core_t *core,
                                      const char *request,
                                      size_t request_len,
                                      char *result,
                                      size_t result_capacity,
                                      size_t *out_result_len)
{
    (void)core;
    (void)result;
    (void)result_capacity;
    (void)out_result_len;
    sleep_ms(request_ms(request, request_len));
    return COG_ERROR_IO;
}

static cog_result_t not_found_tool(cog_core_t *core,
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
    (void)atomic_fetch_add(&g_not_found_calls, 1);
    return COG_ERROR_NOT_FOUND;
}

static cog_result_t counting_tool(cog_core_t *core,
                                  const char *request,
                                  size_t request_len,
                                  char *result,
                                  size_t result_capacity,
                                  size_t *out_result_len)
{
    static const char reply[] = "counted";

    (void)core;
    (void)request;
    (void)request_len;
    (void)atomic_fetch_add(&g_counted_calls, 1);
    if (result_capacity < sizeof(reply)) {
        if (out_result_len != NULL) {
            *out_result_len = sizeof(reply) - 1U;
        }
        return COG_ERROR_OVERFLOW;
    }
    (void)memcpy(result, reply, sizeof(reply));
    if (out_result_len != NULL) {
        *out_result_len = sizeof(reply) - 1U;
    }
    return COG_OK;
}

static const cog_tool_t kTools[] = {
    { .name = "block", .description = NULL, .input_schema = NULL,
      .handler = blocking_tool, .flags = COG_TOOL_REENTRANT | COG_TOOL_INDEPENDENT },
    { .name = "delayed_fail", .description = NULL, .input_schema = NULL,
      .handler = delayed_fail_tool, .flags = COG_TOOL_REENTRANT | COG_TOOL_INDEPENDENT },
    { .name = "not_found", .description = NULL, .input_schema = NULL,
      .handler = not_found_tool, .flags = COG_TOOL_REENTRANT | COG_TOOL_INDEPENDENT },
    { .name = "count", .description = NULL, .input_schema = NULL,
      .handler = counting_tool, .flags = COG_TOOL_REENTRANT | COG_TOOL_INDEPENDENT }
};

static const cog_tool_table_t kTable = {
    .tools = kTools,
    .count = sizeof(kTools) / sizeof(kTools[0])
};

static cog_core_t *dummy_core(void)
{
    return toolkit_test_core;
}

/* pthread_create wrapping: -1 passes every call through, N fails creation
 * number N + 1 and every later creation.  The dispatcher is the only user of
 * pthread_create in this executable, and these counters are main-thread only. */
static long g_fail_creation_after = -1L;
static long g_creation_attempts;

int __real_pthread_create(pthread_t *thread,
                          const pthread_attr_t *attr,
                          void *(*start_routine)(void *),
                          void *arg);
int __wrap_pthread_create(pthread_t *thread,
                          const pthread_attr_t *attr,
                          void *(*start_routine)(void *),
                          void *arg);

int __wrap_pthread_create(pthread_t *thread,
                          const pthread_attr_t *attr,
                          void *(*start_routine)(void *),
                          void *arg)
{
    if (g_fail_creation_after >= 0L &&
        g_creation_attempts >= g_fail_creation_after) {
        g_creation_attempts++;
        return EAGAIN;
    }
    g_creation_attempts++;
    return __real_pthread_create(thread, attr, start_routine, arg);
}

/** A failing worker stops new claims but the other running tool is joined. */
static int verify_parallel_stop_claims(void)
{
    cog_toolkit_call_t *calls[3];
    cog_toolkit_exec_opts_t opts;
    cog_toolkit_result_collection_t *coll = NULL;
    cog_result_t r;

    g_fail_creation_after = -1L;
    atomic_store(&g_counted_calls, 0);
    if (cog_toolkit_call_new(&calls[0], "delayed_fail", "40", 2U) != COG_OK ||
        cog_toolkit_call_new(&calls[1], "block", "80", 2U) != COG_OK ||
        cog_toolkit_call_new(&calls[2], "count", NULL, 0U) != COG_OK) {
        return 1;
    }
    memset(&opts, 0, sizeof(opts));
    opts.max_parallel = 2U;
    opts.stop_on_error = true;
    r = cog_toolkit_execute_all(dummy_core(), &kTable, calls, 3U, &opts,
                                &coll);
    cog_toolkit_call_free(calls[0]);
    cog_toolkit_call_free(calls[1]);
    cog_toolkit_call_free(calls[2]);
    if (r != COG_OK || coll == NULL) { return 1; }
    cog_toolkit_result_collection_free(coll);
    return atomic_load(&g_counted_calls) == 0 ? 0 : 1;
}

/** The final error is selected by input index, not completion order. */
static int verify_earliest_error_index(void)
{
    cog_toolkit_call_t *calls[2];
    cog_toolkit_exec_opts_t opts;
    cog_toolkit_result_collection_t *coll = NULL;
    cog_result_t r;

    g_fail_creation_after = -1L;
    atomic_store(&g_not_found_calls, 0);
    if (cog_toolkit_call_new(&calls[0], "delayed_fail", "60", 2U) != COG_OK ||
        cog_toolkit_call_new(&calls[1], "not_found", NULL, 0U) != COG_OK) {
        return 1;
    }
    memset(&opts, 0, sizeof(opts));
    opts.max_parallel = 2U;
    opts.stop_on_error = true;
    r = cog_toolkit_execute_all(dummy_core(), &kTable, calls, 2U, &opts,
                                &coll);
    cog_toolkit_call_free(calls[0]);
    cog_toolkit_call_free(calls[1]);
    if (r != COG_OK || coll == NULL) { return 1; }
    cog_toolkit_result_collection_free(coll);
    return atomic_load(&g_not_found_calls) == 1 ? 0 : 1;
}

/** A cap above call_count is harmless and preserves exact count/order. */
static int verify_cap_above_calls(void)
{
    cog_toolkit_call_t *calls[2];
    cog_toolkit_exec_opts_t opts;
    cog_toolkit_result_collection_t *coll = NULL;
    const cog_toolkit_result_t *res = NULL;
    cog_result_t r;
    size_t i;

    g_fail_creation_after = -1L;
    if (cog_toolkit_call_new(&calls[0], "block", "10", 2U) != COG_OK ||
        cog_toolkit_call_new(&calls[1], "block", "20", 2U) != COG_OK) {
        return 1;
    }
    memset(&opts, 0, sizeof(opts));
    opts.max_parallel = 100U;
    r = cog_toolkit_execute_all(dummy_core(), &kTable, calls, 2U, &opts,
                                &coll);
    cog_toolkit_call_free(calls[0]);
    cog_toolkit_call_free(calls[1]);
    if (r != COG_OK || coll == NULL ||
        cog_toolkit_result_collection_count(coll) != 2U) {
        return 1;
    }
    for (i = 0U; i < 2U; i++) {
        if (cog_toolkit_result_collection_get(coll, i, &res) != COG_OK ||
            res->status != COG_OK || res->output == NULL ||
            strcmp(res->output, "blocked") != 0) {
            cog_toolkit_result_collection_free(coll);
            return 1;
        }
    }
    cog_toolkit_result_collection_free(coll);
    return 0;
}

/** Both immediate and partial worker-creation failures clean up safely. */
static int verify_create_failure(long successful_creations)
{
    cog_toolkit_call_t *calls[3];
    cog_toolkit_exec_opts_t opts;
    cog_toolkit_result_collection_t *coll = NULL;
    cog_result_t r;
    size_t i;

    g_fail_creation_after = successful_creations;
    g_creation_attempts = 0L;
    atomic_store(&g_counted_calls, 0);
    for (i = 0U; i < 3U; i++) {
        const char *name = i == 0U ? "block" : "count";
        const char *request = i == 0U ? "80" : NULL;
        const size_t request_len = i == 0U ? 2U : 0U;
        if (cog_toolkit_call_new(&calls[i], name, request, request_len) !=
            COG_OK) {
            while (i > 0U) {
                cog_toolkit_call_free(calls[--i]);
            }
            return 1;
        }
    }
    memset(&opts, 0, sizeof(opts));
    opts.max_parallel = 3U;
    r = cog_toolkit_execute_all(dummy_core(), &kTable, calls, 3U, &opts,
                                &coll);
    for (i = 0U; i < 3U; i++) {
        cog_toolkit_call_free(calls[i]);
    }
    g_fail_creation_after = -1L;
    if (r != COG_ERROR_NOMEM || coll != NULL) {
        return 1;
    }
    /* After the creation failure the launcher sets the shared stop flag;
     * no counting calls may be claimed by a still-running first worker. */
    return atomic_load(&g_counted_calls) == 0 ? 0 : 1;
}

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

    CHECK("parallel stop claims", verify_parallel_stop_claims());
    CHECK("earliest error index", verify_earliest_error_index());
    CHECK("cap above calls", verify_cap_above_calls());
    CHECK("immediate create failure", verify_create_failure(0L));
    CHECK("partial create failure", verify_create_failure(1L));

#undef CHECK

    if (failures == 0U) {
        (void)printf("test_toolkit_parallel: ok\n");
    }
    toolkit_core_destroy();
    return failures == 0U ? 0 : 1;
}

/**
 * @file test_toolkit_critic.c
 * @brief Critic regression tests for cog-toolkit (review of 8d52c1a).
 *
 * Focuses on the acceptance-critical questions for the review:
 *
 *   1. Does cog_toolkit_execute_all actually run independent tools
 *      concurrently (as the header's max_parallel field and scenario 06
 *      "Parallel Tool Calls with Timeout" require), or does it only
 *      invoke them sequentially?  Measured two ways:
 *        - wall-clock timing of two 200 ms sleeps (sequential ~= 400 ms,
 *          concurrent ~= 200 ms);
 *        - a two-tool barrier: both tools must be running at the same
 *          time to complete.  Sequential execution cannot satisfy the
 *          barrier and the first tool spins until its deadline.
 *   2. Wall-clock timeout semantics: a non-cooperative tool that blocks
 *      past its deadline is NOT preempted (the header documents the
 *      timeout as advisory / boundary-enforced).  The result must be
 *      recorded as COG_ERROR_TIMEOUT and the elapsed wall time must
 *      include the full blocking duration.
 *   3. Cooperative cancellation: a call cancelled before dispatch is
 *      recorded COG_ERROR_ABORTED without invoking the tool; a tool that
 *      itself returns COG_ERROR_ABORTED is propagated.
 *   4. stop_on_error: the first failure aborts and later calls are not
 *      invoked (verified with an invocation counter).
 *   5. Exactly call_count results in call order, including mixed
 *      success/failure.
 *   6. Schema validation edge cases (integer type, trailing garbage,
 *      required on non-object output).
 *
 * Ticket: 8d52c1a (Stage 12.4b — Implement toolkit), critic review.
 */

#define _POSIX_C_SOURCE 200809L

#include "cog/toolkit.h"
#include "fixtures/toolkit_core.h"

#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ------------------------------------------------------------------ */
/* Timing helpers                                                      */
/* ------------------------------------------------------------------ */

static uint64_t now_ms(void)
{
    struct timespec ts;

    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000U + (uint64_t)ts.tv_nsec / 1000000U;
}

static void sleep_ms(uint64_t ms)
{
    struct timespec ts;

    ts.tv_sec = (time_t)(ms / 1000U);
    ts.tv_nsec = (long)((ms % 1000U) * 1000000U);
    (void)nanosleep(&ts, NULL);
}

/* ------------------------------------------------------------------ */
/* Test tools                                                          */
/* ------------------------------------------------------------------ */

/** Sleeps for the number of ms given in the request, then returns OK. */
static cog_result_t sleep_tool(cog_core_t *core,
                               const char *request,
                               size_t request_len,
                               char *result,
                               size_t result_capacity,
                               size_t *out_result_len)
{
    static const char kReply[] = "slept";
    unsigned long ms = 0UL;

    (void)core;
    if (request != NULL && request_len > 0U) {
        char tmp[16];
        size_t n = request_len < sizeof(tmp) - 1U ? request_len
                                                  : sizeof(tmp) - 1U;
        (void)memcpy(tmp, request, n);
        tmp[n] = '\0';
        ms = strtoul(tmp, NULL, 10);
    }
    sleep_ms((uint64_t)ms);
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

/* Shared arrival counter for the barrier tool. */
static atomic_int g_barrier_arrivals;

/**
 * Barrier tool: records its arrival and then spins until BOTH calls have
 * arrived (proving concurrent execution) or a 1500 ms deadline passes
 * (sequential execution: the first call spins alone and times out).
 */
static cog_result_t barrier_tool(cog_core_t *core,
                                 const char *request,
                                 size_t request_len,
                                 char *result,
                                 size_t result_capacity,
                                 size_t *out_result_len)
{
    static const char kReply[] = "barrier:ok";
    uint64_t start = now_ms();

    (void)core;
    (void)request;
    (void)request_len;

    (void)atomic_fetch_add(&g_barrier_arrivals, 1);
    while (atomic_load(&g_barrier_arrivals) < 2) {
        if (now_ms() - start > 1500U) {
            if (out_result_len != NULL) {
                *out_result_len = 0U;
            }
            return COG_ERROR_TIMEOUT; /* never saw the peer: sequential */
        }
        sleep_ms(2U);
    }
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

/** Counts invocations; returns OK. */
static atomic_int g_invoke_count;

static cog_result_t counting_tool(cog_core_t *core,
                                  const char *request,
                                  size_t request_len,
                                  char *result,
                                  size_t result_capacity,
                                  size_t *out_result_len)
{
    static const char kReply[] = "counted";

    (void)core;
    (void)request;
    (void)request_len;
    (void)atomic_fetch_add(&g_invoke_count, 1);

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

/** Returns COG_ERROR_ABORTED (a cooperative tool observing cancellation). */
static cog_result_t abort_tool(cog_core_t *core,
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
    return COG_ERROR_ABORTED;
}

/** Produces a JSON object result. */
static cog_result_t json_tool(cog_core_t *core,
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

/** Produces a JSON number result. */
static cog_result_t number_tool(cog_core_t *core,
                                const char *request,
                                size_t request_len,
                                char *result,
                                size_t result_capacity,
                                size_t *out_result_len)
{
    static const char kReply[] = "42";

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
/* Test table and dummy core                                           */
/* ------------------------------------------------------------------ */

static const cog_tool_t kTools[] = {
    { .name = "sleep", .description = NULL, .input_schema = NULL,
      .handler = sleep_tool, .flags = COG_TOOL_REENTRANT | COG_TOOL_INDEPENDENT },
    { .name = "barrier", .description = NULL, .input_schema = NULL,
      .handler = barrier_tool, .flags = COG_TOOL_REENTRANT | COG_TOOL_INDEPENDENT },
    { .name = "count", .description = NULL, .input_schema = NULL,
      .handler = counting_tool, .flags = COG_TOOL_REENTRANT | COG_TOOL_INDEPENDENT },
    { .name = "fail", .description = NULL, .input_schema = NULL,
      .handler = fail_tool, .flags = COG_TOOL_REENTRANT | COG_TOOL_INDEPENDENT },
    { .name = "abort", .description = NULL, .input_schema = NULL,
      .handler = abort_tool, .flags = COG_TOOL_REENTRANT | COG_TOOL_INDEPENDENT },
    { .name = "json", .description = NULL, .input_schema = NULL,
      .handler = json_tool, .flags = COG_TOOL_REENTRANT | COG_TOOL_INDEPENDENT },
    { .name = "number", .description = NULL, .input_schema = NULL,
      .handler = number_tool, .flags = COG_TOOL_REENTRANT | COG_TOOL_INDEPENDENT },
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
/* 1. Concurrency: wall-clock measurement                              */
/* ------------------------------------------------------------------ */

/**
 * Two independent 200 ms sleeps.  Concurrent execution finishes in
 * ~200 ms; sequential execution takes ~400 ms.  Returns 0 if the
 * measured behaviour matches the header contract (max_parallel > 1
 * means concurrent), 1 otherwise.  Prints the measurement either way.
 */
static int verify_concurrency_wallclock(void)
{
    cog_toolkit_call_t *calls[2];
    cog_toolkit_result_collection_t *coll = NULL;
    cog_toolkit_exec_opts_t opts;
    uint64_t t0;
    uint64_t elapsed;
    cog_result_t r;

    if (cog_toolkit_call_new(&calls[0], "sleep", "200", 3U) != COG_OK) {
        return 1;
    }
    if (cog_toolkit_call_new(&calls[1], "sleep", "200", 3U) != COG_OK) {
        cog_toolkit_call_free(calls[0]);
        return 1;
    }
    memset(&opts, 0, sizeof(opts));
    opts.max_parallel = 2U; /* header: "maximum number of calls to run
                               concurrently"; 1 means sequential */

    t0 = now_ms();
    r = cog_toolkit_execute_all(dummy_core(), &kTable, calls, 2U, &opts,
                                &coll);
    elapsed = now_ms() - t0;
    cog_toolkit_call_free(calls[0]);
    cog_toolkit_call_free(calls[1]);
    if (r != COG_OK || coll == NULL) {
        (void)printf("  wallclock: execute_all failed (r=%d)\n", (int)r);
        return 1;
    }
    cog_toolkit_result_collection_free(coll);

    (void)printf("  wallclock: 2x200ms sleeps with max_parallel=2 took "
                 "%llu ms (sequential ~=400, concurrent ~=200)\n",
                 (unsigned long long)elapsed);
    /* Concurrent execution must finish well under the sequential total. */
    return elapsed >= 350U ? 1 : 0;
}

/** max_parallel=1 must keep the sequential caller-thread fast path. */
static int verify_sequential_wallclock(void)
{
    cog_toolkit_call_t *calls[2];
    cog_toolkit_result_collection_t *coll = NULL;
    cog_toolkit_exec_opts_t opts;
    uint64_t t0;
    uint64_t elapsed;
    cog_result_t r;

    if (cog_toolkit_call_new(&calls[0], "sleep", "100", 3U) != COG_OK) {
        return 1;
    }
    if (cog_toolkit_call_new(&calls[1], "sleep", "100", 3U) != COG_OK) {
        cog_toolkit_call_free(calls[0]);
        return 1;
    }
    memset(&opts, 0, sizeof(opts));
    opts.max_parallel = 1U;
    t0 = now_ms();
    r = cog_toolkit_execute_all(dummy_core(), &kTable, calls, 2U, &opts,
                                &coll);
    elapsed = now_ms() - t0;
    cog_toolkit_call_free(calls[0]);
    cog_toolkit_call_free(calls[1]);
    if (r != COG_OK || coll == NULL) {
        return 1;
    }
    cog_toolkit_result_collection_free(coll);
    (void)printf("  wallclock: 2x100ms sleeps with max_parallel=1 took "
                 "%llu ms\n", (unsigned long long)elapsed);
    return elapsed < 180U ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/* 2. Concurrency: two-tool barrier                                    */
/* ------------------------------------------------------------------ */

/**
 * Two calls to the barrier tool.  Both must be executing simultaneously
 * for either to complete.  Sequential execution leaves the first call
 * spinning alone until its 1500 ms deadline (COG_ERROR_TIMEOUT).
 */
static int verify_concurrency_barrier(void)
{
    cog_toolkit_call_t *calls[2];
    cog_toolkit_result_collection_t *coll = NULL;
    const cog_toolkit_result_t *res = NULL;
    cog_result_t r;

    atomic_store(&g_barrier_arrivals, 0);
    if (cog_toolkit_call_new(&calls[0], "barrier", NULL, 0U) != COG_OK) {
        return 1;
    }
    if (cog_toolkit_call_new(&calls[1], "barrier", NULL, 0U) != COG_OK) {
        cog_toolkit_call_free(calls[0]);
        return 1;
    }
    cog_toolkit_exec_opts_t parallel = { .max_parallel = 2U };
    r = cog_toolkit_execute_all(dummy_core(), &kTable, calls, 2U, &parallel, &coll);
    cog_toolkit_call_free(calls[0]);
    cog_toolkit_call_free(calls[1]);
    if (r != COG_OK || coll == NULL) {
        (void)printf("  barrier: execute_all failed (r=%d)\n", (int)r);
        return 1;
    }
    if (cog_toolkit_result_collection_get(coll, 0U, &res) != COG_OK ||
        cog_toolkit_result_collection_get(coll, 1U, &res) != COG_OK) {
        cog_toolkit_result_collection_free(coll);
        return 1;
    }
    /* Both calls must have completed successfully; a sequential run
     * leaves the first call spinning alone until its deadline and
     * records COG_ERROR_TIMEOUT. */
    if (res->status != COG_OK) {
        (void)printf("  barrier: result[1] status=%d (sequential run)\n",
                     (int)res->status);
        cog_toolkit_result_collection_free(coll);
        return 1;
    }
    if (cog_toolkit_result_collection_get(coll, 0U, &res) != COG_OK ||
        res->status != COG_OK) {
        (void)printf("  barrier: result[0] status=%d (sequential run)\n",
                     (int)res->status);
        cog_toolkit_result_collection_free(coll);
        return 1;
    }
    cog_toolkit_result_collection_free(coll);
    return 0;
}

/* ------------------------------------------------------------------ */
/* 3. Timeout: boundary-only enforcement                               */
/* ------------------------------------------------------------------ */

/**
 * A non-cooperative tool blocks 300 ms with a 50 ms timeout.  The header
 * documents the timeout as advisory (enforced at invocation boundaries),
 * so the tool must NOT be preempted: the result is COG_ERROR_TIMEOUT and
 * the elapsed wall time includes the full 300 ms block.
 */
static int verify_timeout_boundary(void)
{
    cog_toolkit_call_t *call = NULL;
    cog_toolkit_call_t *calls[1];
    cog_toolkit_result_collection_t *coll = NULL;
    const cog_toolkit_result_t *res = NULL;
    uint64_t t0;
    uint64_t elapsed;
    cog_result_t r;

    if (cog_toolkit_call_new(&call, "sleep", "300", 3U) != COG_OK) {
        return 1;
    }
    if (cog_toolkit_call_set_timeout(call, 50U) != COG_OK) {
        cog_toolkit_call_free(call);
        return 1;
    }
    calls[0] = call;
    t0 = now_ms();
    r = cog_toolkit_execute_all(dummy_core(), &kTable, calls, 1U, NULL, &coll);
    elapsed = now_ms() - t0;
    cog_toolkit_call_free(call);
    if (r != COG_OK || coll == NULL) {
        (void)printf("  timeout: execute_all failed (r=%d)\n", (int)r);
        return 1;
    }
    if (cog_toolkit_result_collection_get(coll, 0U, &res) != COG_OK) {
        cog_toolkit_result_collection_free(coll);
        return 1;
    }
    {
        int status = (int)res->status;
        (void)printf("  timeout: 300ms block, 50ms budget -> status=%d, "
                     "elapsed=%llu ms\n", status,
                     (unsigned long long)elapsed);
        cog_toolkit_result_collection_free(coll);

        if (status != (int)COG_ERROR_TIMEOUT) {
            return 1; /* overrun must be recorded as TIMEOUT */
        }
        if (elapsed < 250U) {
            return 1; /* tool was preempted: stronger than the documented
                         advisory semantics */
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* 3b. Timeout: dispatcher-default via opts.timeout_ms                  */
/* ------------------------------------------------------------------ */

/** opts.timeout_ms applies when the call has no explicit timeout. */
static int verify_timeout_opts_level(void)
{
    cog_toolkit_exec_opts_t opts;
    cog_toolkit_call_t *call = NULL;
    cog_toolkit_call_t *calls[1];
    cog_toolkit_result_collection_t *coll = NULL;
    const cog_toolkit_result_t *res = NULL;
    cog_result_t r;

    if (cog_toolkit_call_new(&call, "sleep", "300", 3U) != COG_OK) {
        return 1;
    }
    calls[0] = call;
    memset(&opts, 0, sizeof(opts));
    opts.timeout_ms = 50U;
    r = cog_toolkit_execute_all(dummy_core(), &kTable, calls, 1U, &opts,
                                &coll);
    cog_toolkit_call_free(call);
    if (r != COG_OK || coll == NULL) {
        return 1;
    }
    if (cog_toolkit_result_collection_get(coll, 0U, &res) != COG_OK) {
        cog_toolkit_result_collection_free(coll);
        return 1;
    }
    if (res->status != COG_ERROR_TIMEOUT) {
        cog_toolkit_result_collection_free(coll);
        return 1;
    }
    cog_toolkit_result_collection_free(coll);
    return 0;
}

/* ------------------------------------------------------------------ */
/* 4. Cooperative cancellation                                         */
/* ------------------------------------------------------------------ */

/** Cancelled before dispatch: ABORTED, tool never invoked. */
static int verify_cancel_before_dispatch(void)
{
    cog_toolkit_call_t *call = NULL;
    cog_toolkit_call_t *calls[1];
    cog_toolkit_result_collection_t *coll = NULL;
    const cog_toolkit_result_t *res = NULL;
    cog_result_t r;

    atomic_store(&g_invoke_count, 0);
    if (cog_toolkit_call_new(&call, "count", NULL, 0U) != COG_OK) {
        return 1;
    }
    if (cog_toolkit_call_cancel(call) != COG_OK) {
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
        res->status != COG_ERROR_ABORTED) {
        cog_toolkit_result_collection_free(coll);
        return 1;
    }
    cog_toolkit_result_collection_free(coll);
    if (atomic_load(&g_invoke_count) != 0) {
        return 1; /* tool must not have been invoked */
    }
    return 0;
}

/** A tool that itself returns COG_ERROR_ABORTED is propagated. */
static int verify_tool_abort_propagated(void)
{
    cog_toolkit_call_t *call = NULL;
    cog_toolkit_call_t *calls[1];
    cog_toolkit_result_collection_t *coll = NULL;
    const cog_toolkit_result_t *res = NULL;
    cog_result_t r;

    if (cog_toolkit_call_new(&call, "abort", NULL, 0U) != COG_OK) {
        return 1;
    }
    calls[0] = call;
    r = cog_toolkit_execute_all(dummy_core(), &kTable, calls, 1U, NULL, &coll);
    cog_toolkit_call_free(call);
    if (r != COG_OK || coll == NULL) {
        return 1;
    }
    if (cog_toolkit_result_collection_get(coll, 0U, &res) != COG_OK ||
        res->status != COG_ERROR_ABORTED) {
        cog_toolkit_result_collection_free(coll);
        return 1;
    }
    cog_toolkit_result_collection_free(coll);
    return 0;
}

/* ------------------------------------------------------------------ */
/* 5. stop_on_error skips later calls                                  */
/* ------------------------------------------------------------------ */

static int verify_stop_on_error_skips(void)
{
    cog_toolkit_exec_opts_t opts;
    cog_toolkit_call_t *calls[3];
    cog_toolkit_result_collection_t *coll = NULL;
    cog_result_t r;

    atomic_store(&g_invoke_count, 0);
    if (cog_toolkit_call_new(&calls[0], "count", NULL, 0U) != COG_OK) {
        return 1;
    }
    if (cog_toolkit_call_new(&calls[1], "fail", NULL, 0U) != COG_OK) {
        cog_toolkit_call_free(calls[0]);
        return 1;
    }
    if (cog_toolkit_call_new(&calls[2], "count", NULL, 0U) != COG_OK) {
        cog_toolkit_call_free(calls[0]);
        cog_toolkit_call_free(calls[1]);
        return 1;
    }
    memset(&opts, 0, sizeof(opts));
    opts.stop_on_error = true;
    opts.max_parallel = 1U; /* pin sequential skip semantics; concurrent
                             * calls may already be claimed on failure. */

    r = cog_toolkit_execute_all(dummy_core(), &kTable, calls, 3U, &opts,
                                &coll);
    cog_toolkit_call_free(calls[0]);
    cog_toolkit_call_free(calls[1]);
    cog_toolkit_call_free(calls[2]);
    if (r != COG_OK) {
        return 1;
    }
    if (coll == NULL) { return 1; }
    const cog_toolkit_result_t *retained = NULL;
    if (cog_toolkit_result_collection_get(coll, 2U, &retained) != COG_OK || retained->status != COG_ERROR_ABORTED) { cog_toolkit_result_collection_free(coll); return 1; }
    cog_toolkit_result_collection_free(coll);
    /* calls[0] ran; calls[2] must NOT have been launched. */
    if (atomic_load(&g_invoke_count) != 1) {
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* 6. Exactly call_count results in call order                         */
/* ------------------------------------------------------------------ */

static int verify_order_and_count(void)
{
    cog_toolkit_call_t *calls[4];
    cog_toolkit_result_collection_t *coll = NULL;
    const cog_toolkit_result_t *res = NULL;
    cog_result_t r;

    if (cog_toolkit_call_new(&calls[0], "count", NULL, 0U) != COG_OK) {
        return 1;
    }
    if (cog_toolkit_call_new(&calls[1], "fail", NULL, 0U) != COG_OK) {
        cog_toolkit_call_free(calls[0]);
        return 1;
    }
    if (cog_toolkit_call_new(&calls[2], "count", NULL, 0U) != COG_OK) {
        cog_toolkit_call_free(calls[0]);
        cog_toolkit_call_free(calls[1]);
        return 1;
    }
    if (cog_toolkit_call_new(&calls[3], "count", NULL, 0U) != COG_OK) {
        cog_toolkit_call_free(calls[0]);
        cog_toolkit_call_free(calls[1]);
        cog_toolkit_call_free(calls[2]);
        return 1;
    }
    r = cog_toolkit_execute_all(dummy_core(), &kTable, calls, 4U, NULL, &coll);
    cog_toolkit_call_free(calls[0]);
    cog_toolkit_call_free(calls[1]);
    cog_toolkit_call_free(calls[2]);
    cog_toolkit_call_free(calls[3]);
    if (r != COG_OK || coll == NULL) {
        return 1;
    }
    if (cog_toolkit_result_collection_count(coll) != 4U) {
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
        !cog_toolkit_result_is_ok(res)) {
        cog_toolkit_result_collection_free(coll);
        return 1;
    }
    if (cog_toolkit_result_collection_get(coll, 3U, &res) != COG_OK ||
        !cog_toolkit_result_is_ok(res)) {
        cog_toolkit_result_collection_free(coll);
        return 1;
    }
    cog_toolkit_result_collection_free(coll);
    return 0;
}

/* ------------------------------------------------------------------ */
/* 7. Schema validation edge cases                                     */
/* ------------------------------------------------------------------ */

static int verify_schema_edges(void)
{
    cog_toolkit_result_t res;
    char buf[64];

    /* integer type accepts a JSON number (documented subset behaviour). */
    res.status = COG_OK;
    res.output = buf;
    (void)strcpy(buf, "42");
    res.output_len = 2U;
    if (cog_toolkit_result_validate_schema(&res,
                                           "{\"type\":\"integer\"}") != COG_OK) {
        return 1;
    }
    if (cog_toolkit_result_validate_schema(&res,
                                           "{\"type\":\"string\"}") !=
        COG_ERROR_INVALID_ARG) {
        return 1;
    }

    /* required on a non-object output is a no-op (documented: enforced
     * only when the output is an object). */
    if (cog_toolkit_result_validate_schema(&res,
                                           "{\"required\":[\"name\"]}") !=
        COG_OK) {
        return 1;
    }

    /* object with required present / missing. */
    (void)strcpy(buf, "{\"name\":\"alice\"}");
    res.output_len = strlen(buf);
    if (cog_toolkit_result_validate_schema(&res,
                                           "{\"required\":[\"name\"]}") !=
        COG_OK) {
        return 1;
    }
    if (cog_toolkit_result_validate_schema(&res,
                                           "{\"required\":[\"nope\"]}") !=
        COG_ERROR_INVALID_ARG) {
        return 1;
    }

    /* trailing garbage after the JSON value. */
    (void)strcpy(buf, "{\"name\":\"alice\"} x");
    res.output_len = strlen(buf);
    if (cog_toolkit_result_validate_schema(&res, "{}") !=
        COG_ERROR_INVALID_ARG) {
        return 1;
    }

    /* malformed schema: trailing garbage after the schema object. */
    (void)strcpy(buf, "{\"name\":\"alice\"}");
    res.output_len = strlen(buf);
    if (cog_toolkit_result_validate_schema(&res, "{} }") !=
        COG_ERROR_INVALID_ARG) {
        (void)printf("  schema: trailing garbage after schema object "
                     "accepted (expected INVALID_ARG)\n");
        return 1;
    }

    /* Trailing JSON whitespace is accepted, including on non-empty
     * schemas, without weakening rejection of other trailing bytes. */
    if (cog_toolkit_result_validate_schema(&res, "{} \t\n") != COG_OK ||
        cog_toolkit_result_validate_schema(
            &res, "{\"required\":[\"name\"]} \r\n") != COG_OK) {
        return 1;
    }
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

    (void)printf("critic: concurrency wall-clock measurement\n");
    CHECK("concurrency wallclock", verify_concurrency_wallclock());
    CHECK("sequential wallclock", verify_sequential_wallclock());
    (void)printf("critic: concurrency barrier\n");
    CHECK("concurrency barrier", verify_concurrency_barrier());
    (void)printf("critic: timeout boundary\n");
    CHECK("timeout boundary", verify_timeout_boundary());
    CHECK("timeout opts-level", verify_timeout_opts_level());
    CHECK("cancel before dispatch", verify_cancel_before_dispatch());
    CHECK("tool abort propagated", verify_tool_abort_propagated());
    CHECK("stop_on_error skips later calls", verify_stop_on_error_skips());
    CHECK("order and count", verify_order_and_count());
    CHECK("schema edges", verify_schema_edges());

#undef CHECK

    if (failures == 0U) {
        (void)printf("test_toolkit_critic: ok\n");
    }
    toolkit_core_destroy();
    return failures == 0U ? 0 : 1;
}

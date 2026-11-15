/**
 * @file test_06_parallel_tools.c
 * @brief Scenario 06: Parallel tool calls with timeout.
 *
 * Drives the real cog-core agent (with a static const tool table) through
 * the real cog-toolkit dispatcher (cog_toolkit_execute_all) and proves
 * the scenario-06 contract from docs/scenarios/06-parallel-tools.md:
 *
 *   1. independent tools actually overlap: two tools that each sleep are
 *      run with max_parallel > 1 and complete in less wall-clock time
 *      than the same two calls run sequentially;
 *   2. exactly call_count ordered results: execute_all returns a
 *      collection with exactly call_count entries, in call order, and the
 *      results are collected into the session tree as tool-result nodes
 *      in that same order;
 *   3. timeout semantics: a tool that outlives its per-call timeout is
 *      reported as COG_ERROR_TIMEOUT with an error message;
 *   4. cancellation semantics: a call cancelled before dispatch is
 *      reported as COG_ERROR_ABORTED with an error message;
 *   5. the COG_EVENT_TOOL_RESULT event kind flows through the real state
 *      machine: a handler receives the event with a cog_toolkit_result_t
 *      payload and records it.
 *
 * The toolkit's worker threads write only per-call result slots and are
 * joined before the collection is assembled on the caller thread, so the
 * test is data-race free (it also runs clean under TSan).
 *
 * Ticket: 0d8ff50 (Stage 12.6c — Scenario tests 06-10).
 */
#define _POSIX_C_SOURCE 200809L

#include "cog/core.h"
#include "cog/toolkit.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ------------------------------------------------------------------ */
/* Test application vocabulary.                                        */
/* ------------------------------------------------------------------ */

enum {
    ST_IDLE = 0,
};

enum {
    EV_USER_INPUT = COG_EVENT_USER_INPUT,
    EV_TOOL_RESULT = COG_EVENT_TOOL_RESULT,
};

/* ------------------------------------------------------------------ */
/* Time helpers                                                        */
/* ------------------------------------------------------------------ */

static uint64_t now_ms(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0U;
    }
    return (uint64_t)ts.tv_sec * 1000U + (uint64_t)ts.tv_nsec / 1000000U;
}

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

/* ------------------------------------------------------------------ */
/* Tools                                                               */
/* ------------------------------------------------------------------ */

/** Sleep for the request's millisecond value, then echo the request. */
static cog_result_t sleep_echo_tool(cog_core_t *core,
                                    const char *request,
                                    size_t request_len,
                                    char *result,
                                    size_t result_capacity,
                                    size_t *out_result_len)
{
    (void)core;
    sleep_ms(request_ms(request, request_len));
    if (request == NULL || request_len == 0U) {
        if (result_capacity > 0U) {
            result[0] = '\0';
        }
        if (out_result_len != NULL) {
            *out_result_len = 0U;
        }
        return COG_OK;
    }
    if (result_capacity < request_len + 1U) {
        if (out_result_len != NULL) {
            *out_result_len = request_len;
        }
        return COG_ERROR_OVERFLOW;
    }
    (void)memcpy(result, request, request_len);
    result[request_len] = '\0';
    if (out_result_len != NULL) {
        *out_result_len = request_len;
    }
    return COG_OK;
}

/** Sleeps longer than any test timeout; used to trigger COG_ERROR_TIMEOUT. */
static cog_result_t slow_tool(cog_core_t *core,
                              const char *request,
                              size_t request_len,
                              char *result,
                              size_t result_capacity,
                              size_t *out_result_len)
{
    static const char reply[] = "slow";

    (void)core;
    (void)request;
    (void)request_len;
    sleep_ms(500UL);
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
    { .name = "sleep_echo", .description = NULL, .input_schema = NULL,
      .handler = sleep_echo_tool, .flags = COG_TOOL_REENTRANT | COG_TOOL_INDEPENDENT },
    { .name = "slow", .description = NULL, .input_schema = NULL,
      .handler = slow_tool, .flags = COG_TOOL_REENTRANT | COG_TOOL_INDEPENDENT },
};

static const cog_tool_table_t kTable = {
    .tools = kTools,
    .count = sizeof(kTools) / sizeof(kTools[0]),
};

/* ------------------------------------------------------------------ */
/* Handlers                                                            */
/* ------------------------------------------------------------------ */

/** Records the last COG_EVENT_TOOL_RESULT payload (test-owned state). */
static cog_result_t g_tool_result_status = COG_OK;
static int g_tool_result_seen = 0;

static cog_handler_result_t on_user_input(cog_core_t *core,
                                          const cog_event_t *event)
{
    (void)core;
    (void)event;
    return (cog_handler_result_t){ST_IDLE, COG_OK};
}

static cog_handler_result_t on_tool_result(cog_core_t *core,
                                           const cog_event_t *event)
{
    const cog_toolkit_result_t *res =
        (const cog_toolkit_result_t *)event->payload;

    (void)core;
    if (res != NULL) {
        g_tool_result_status = res->status;
        g_tool_result_seen = 1;
    }
    return (cog_handler_result_t){ST_IDLE, COG_OK};
}

static const cog_state_t idle_transitions[] = {
    ST_IDLE,
    COG_TRANSITION_SELF,
    COG_TRANSITION_END,
};

static const cog_dispatch_entry_t dispatch_table[] = {
    COG_DISPATCH_ENTRY(ST_IDLE, EV_USER_INPUT, on_user_input,
                       idle_transitions),
    COG_DISPATCH_ENTRY(ST_IDLE, EV_TOOL_RESULT, on_tool_result,
                       idle_transitions),
};

/* ------------------------------------------------------------------ */
/* Assertion helpers.                                                  */
/* ------------------------------------------------------------------ */

static int failures = 0;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,     \
                    #cond);                                             \
            ++failures;                                                 \
        }                                                               \
    } while (0)

#define CHECK_EQ_INT(a, b)                                              \
    do {                                                                \
        long long _a = (long long)(a);                                  \
        long long _b = (long long)(b);                                  \
        if (_a != _b) {                                                 \
            fprintf(stderr, "FAIL %s:%d: %s == %s (%lld != %lld)\n",    \
                    __FILE__, __LINE__, #a, #b, _a, _b);                \
            ++failures;                                                 \
        }                                                               \
    } while (0)

/* ------------------------------------------------------------------ */
/* Core + session fixture                                              */
/* ------------------------------------------------------------------ */

typedef struct fixture {
    cog_session_t *session;
    cog_core_t *core;
} fixture_t;

static int fixture_new(fixture_t *fx)
{
    cog_core_config_t cfg;

    memset(fx, 0, sizeof(*fx));
    if (cog_session_new(&fx->session, NULL) != COG_OK) {
        return 1;
    }
    memset(&cfg, 0, sizeof(cfg));
    cfg.session = fx->session;
    cfg.dispatch_table = dispatch_table;
    cfg.dispatch_table_size = sizeof(dispatch_table) / sizeof(dispatch_table[0]);
    cfg.tool_table = &kTable;
    if (cog_core_new(&fx->core, &cfg) != COG_OK) {
        cog_session_free(fx->session);
        fx->session = NULL;
        return 1;
    }
    return 0;
}

static void fixture_free(fixture_t *fx)
{
    cog_core_free(fx->core);
    cog_session_free(fx->session);
}

/* ------------------------------------------------------------------ */
/* 1. Independent tools actually overlap                               */
/* ------------------------------------------------------------------ */

static void scenario_overlap(void)
{
    fixture_t fx;
    cog_toolkit_call_t *calls[2];
    cog_toolkit_exec_opts_t opts;
    cog_toolkit_result_collection_t *coll = NULL;
    uint64_t t0, t_seq, t_par;
    cog_result_t r;

    if (fixture_new(&fx) != 0) {
        CHECK(0);
        return;
    }

    /* Two tools that each sleep 150 ms. */
    CHECK(cog_toolkit_call_new(&calls[0], "sleep_echo", "150", 3U) == COG_OK);
    CHECK(cog_toolkit_call_new(&calls[1], "sleep_echo", "150", 3U) == COG_OK);

    /* Sequential baseline (max_parallel = 1). */
    memset(&opts, 0, sizeof(opts));
    opts.max_parallel = 1U;
    t0 = now_ms();
    r = cog_toolkit_execute_all(fx.core, &kTable, calls, 2U, &opts, &coll);
    t_seq = now_ms() - t0;
    CHECK(r == COG_OK);
    cog_toolkit_result_collection_free(coll);
    coll = NULL;

    /* A call context is at-most-once: fresh contexts are required to measure
     * actual parallel work rather than the cached sequential outcomes. */
    cog_toolkit_call_free(calls[0]);
    cog_toolkit_call_free(calls[1]);
    CHECK(cog_toolkit_call_new(&calls[0], "sleep_echo", "150", 3U) == COG_OK);
    CHECK(cog_toolkit_call_new(&calls[1], "sleep_echo", "150", 3U) == COG_OK);

    /* Parallel (max_parallel = 2). */
    memset(&opts, 0, sizeof(opts));
    opts.max_parallel = 2U;
    t0 = now_ms();
    r = cog_toolkit_execute_all(fx.core, &kTable, calls, 2U, &opts, &coll);
    t_par = now_ms() - t0;
    CHECK(r == COG_OK);
    cog_toolkit_result_collection_free(coll);
    coll = NULL;

    /* Parallel must be strictly faster than sequential, proving the two
     * tools ran concurrently (real overlap, not serialised dispatch). */
    CHECK(t_par < t_seq);

    cog_toolkit_call_free(calls[0]);
    cog_toolkit_call_free(calls[1]);
    fixture_free(&fx);
}

/* ------------------------------------------------------------------ */
/* 2. Exactly call_count ordered results                               */
/* ------------------------------------------------------------------ */

/* Depth-first walk that counts tool-result nodes and records their
 * contents in traversal order.  Test-owned scratch state. */
static char g_results[3][16];
static size_t g_result_count;

static void collect_results_rec(const cog_session_node_t *node)
{
    const cog_session_node_t *child;
    const cog_node_data_t *d = cog_node_data(node);

    if (d != NULL && d->role == COG_NODE_TOOL_RESULT &&
        d->tool_result != NULL && g_result_count < 3U) {
        (void)snprintf(g_results[g_result_count], 16, "%s", d->tool_result);
        g_result_count++;
    }
    for (child = cog_node_first_child(node); child != NULL;
         child = cog_node_next_sibling(child)) {
        collect_results_rec(child);
    }
}

static void scenario_ordered_results(void)
{
    fixture_t fx;
    cog_toolkit_call_t *calls[3];
    cog_toolkit_exec_opts_t opts;
    cog_toolkit_result_collection_t *coll = NULL;
    const cog_toolkit_result_t *res = NULL;
    cog_session_node_t *user_node = NULL;
    cog_session_node_t *call_nodes[3];
    cog_message_t user_msg = {0};
    size_t i;
    size_t req_len;
    const char *req;
    cog_result_t r;

    if (fixture_new(&fx) != 0) {
        CHECK(0);
        return;
    }

    CHECK(cog_toolkit_call_new(&calls[0], "sleep_echo", "alpha", 5U) == COG_OK);
    CHECK(cog_toolkit_call_new(&calls[1], "sleep_echo", "beta", 4U) == COG_OK);
    CHECK(cog_toolkit_call_new(&calls[2], "sleep_echo", "gamma", 5U) == COG_OK);

    /* A user turn roots the conversation. */
    user_msg.role = COG_MESSAGE_ROLE_USER;
    user_msg.content = "run tools";
    user_msg.content_len = 9U;
    user_msg.name = NULL;
    CHECK(cog_session_append(fx.session, &user_msg, &user_node) == COG_OK);

    /* One tool-call node per call, under the user turn. */
    for (i = 0U; i < 3U; ++i) {
        CHECK(cog_session_add_tool_call(fx.session, user_node,
                                        cog_toolkit_call_name(calls[i]),
                                        "{}", &call_nodes[i]) == COG_OK);
    }

    memset(&opts, 0, sizeof(opts));
    opts.max_parallel = 3U;
    r = cog_toolkit_execute_all(fx.core, &kTable, calls, 3U, &opts, &coll);
    CHECK(r == COG_OK);
    CHECK(coll != NULL);
    if (coll == NULL) { fixture_free(&fx); return; }

    /* Exactly call_count results, in call order, echoing each request. */
    CHECK_EQ_INT((long long)cog_toolkit_result_collection_count(coll), 3LL);
    for (i = 0U; i < 3U; ++i) {
        req = cog_toolkit_call_request(calls[i], &req_len);
        CHECK(cog_toolkit_result_collection_get(coll, i, &res) == COG_OK);
        if (res == NULL) { CHECK(0); break; }
        CHECK(cog_toolkit_result_is_ok(res));
        CHECK(res->output != NULL);
        CHECK(res->output_len == req_len);
        CHECK(memcmp(res->output, req, req_len) == 0);
    }

    /* Attach each result under its tool-call node. */
    for (i = 0U; i < 3U; ++i) {
        CHECK(cog_toolkit_result_collection_get(coll, i, &res) == COG_OK);
        if (res == NULL) { CHECK(0); break; }
        CHECK(cog_session_add_tool_result(fx.session, call_nodes[i],
                                          res->output, NULL) == COG_OK);
    }

    /* Walk the whole tree: exactly 3 tool-result nodes in call order. */
    g_result_count = 0U;
    collect_results_rec(cog_session_root(fx.session));
    CHECK_EQ_INT((long long)g_result_count, 3LL);
    for (i = 0U; i < 3U; ++i) {
        req = cog_toolkit_call_request(calls[i], &req_len);
        CHECK(strlen(g_results[i]) == req_len);
        CHECK(memcmp(g_results[i], req, req_len) == 0);
    }

    cog_toolkit_result_collection_free(coll);
    cog_toolkit_call_free(calls[0]);
    cog_toolkit_call_free(calls[1]);
    cog_toolkit_call_free(calls[2]);
    fixture_free(&fx);
}

/* ------------------------------------------------------------------ */
/* 3. Timeout semantics                                                */
/* ------------------------------------------------------------------ */

static void scenario_timeout(void)
{
    fixture_t fx;
    cog_toolkit_call_t *calls[1];
    cog_toolkit_exec_opts_t opts;
    cog_toolkit_result_collection_t *coll = NULL;
    const cog_toolkit_result_t *res = NULL;
    cog_result_t r;

    if (fixture_new(&fx) != 0) {
        CHECK(0);
        return;
    }

    /* The slow tool sleeps 500 ms; a 50 ms timeout must be reported. */
    CHECK(cog_toolkit_call_new(&calls[0], "slow", NULL, 0U) == COG_OK);
    CHECK(cog_toolkit_call_set_timeout(calls[0], 50U) == COG_OK);

    memset(&opts, 0, sizeof(opts));
    opts.max_parallel = 1U;
    r = cog_toolkit_execute_all(fx.core, &kTable, calls, 1U, &opts, &coll);
    CHECK(r == COG_OK);
    CHECK(coll != NULL);
    if (coll == NULL) { fixture_free(&fx); return; }
    CHECK(cog_toolkit_result_collection_get(coll, 0U, &res) == COG_OK);
    if (res == NULL) { CHECK(0); cog_toolkit_result_collection_free(coll); fixture_free(&fx); return; }
    CHECK(res->status == COG_ERROR_TIMEOUT);
    CHECK(res->error_message != NULL);

    cog_toolkit_result_collection_free(coll);
    cog_toolkit_call_free(calls[0]);
    fixture_free(&fx);
}

/* ------------------------------------------------------------------ */
/* 4. Cancellation semantics                                           */
/* ------------------------------------------------------------------ */

static void scenario_cancel(void)
{
    fixture_t fx;
    cog_toolkit_call_t *calls[1];
    cog_toolkit_exec_opts_t opts;
    cog_toolkit_result_collection_t *coll = NULL;
    const cog_toolkit_result_t *res = NULL;
    cog_result_t r;

    if (fixture_new(&fx) != 0) {
        CHECK(0);
        return;
    }

    CHECK(cog_toolkit_call_new(&calls[0], "sleep_echo", "10", 2U) == COG_OK);
    CHECK(cog_toolkit_call_cancel(calls[0]) == COG_OK);
    CHECK(cog_toolkit_call_is_cancelled(calls[0]));

    memset(&opts, 0, sizeof(opts));
    opts.max_parallel = 1U;
    r = cog_toolkit_execute_all(fx.core, &kTable, calls, 1U, &opts, &coll);
    CHECK(r == COG_OK);
    CHECK(coll != NULL);
    if (coll == NULL) { fixture_free(&fx); return; }
    CHECK(cog_toolkit_result_collection_get(coll, 0U, &res) == COG_OK);
    if (res == NULL) { CHECK(0); cog_toolkit_result_collection_free(coll); fixture_free(&fx); return; }
    CHECK(res->status == COG_ERROR_ABORTED);
    CHECK(res->error_message != NULL);

    cog_toolkit_result_collection_free(coll);
    cog_toolkit_call_free(calls[0]);
    fixture_free(&fx);
}

/* ------------------------------------------------------------------ */
/* 5. COG_EVENT_TOOL_RESULT flows through the state machine            */
/* ------------------------------------------------------------------ */

/** Wait hook: stop the loop after the tool-result event is dispatched. */
typedef struct wait_ctx {
    cog_core_t *core;
    int calls;
} wait_ctx_t;

static void tool_result_wait_fn(void *arg)
{
    wait_ctx_t *ctx = (wait_ctx_t *)arg;

    ctx->calls++;
    if (g_tool_result_seen || ctx->calls > 100) {
        cog_core_stop(ctx->core);
    }
}

static void scenario_tool_result_event(void)
{
    cog_session_t *session = NULL;
    cog_core_t *core = NULL;
    cog_core_config_t cfg;
    cog_toolkit_call_t *calls[1];
    cog_toolkit_exec_opts_t opts;
    cog_toolkit_result_collection_t *coll = NULL;
    const cog_toolkit_result_t *res = NULL;
    cog_event_t ev;
    wait_ctx_t wctx;
    cog_result_t r;

    /* Core with a wait hook so the loop can stop after dispatch. */
    CHECK(cog_session_new(&session, NULL) == COG_OK);
    memset(&cfg, 0, sizeof(cfg));
    cfg.session = session;
    cfg.dispatch_table = dispatch_table;
    cfg.dispatch_table_size = sizeof(dispatch_table) / sizeof(dispatch_table[0]);
    cfg.tool_table = &kTable;
    cfg.wait_fn = tool_result_wait_fn;
    cfg.wait_arg = &wctx;
    wctx.core = NULL;
    wctx.calls = 0;
    CHECK(cog_core_new(&core, &cfg) == COG_OK);
    wctx.core = core;

    /* Produce a real result through the toolkit. */
    CHECK(cog_toolkit_call_new(&calls[0], "sleep_echo", "ok", 2U) == COG_OK);
    memset(&opts, 0, sizeof(opts));
    opts.max_parallel = 1U;
    r = cog_toolkit_execute_all(core, &kTable, calls, 1U, &opts, &coll);
    CHECK(r == COG_OK);
    CHECK(cog_toolkit_result_collection_get(coll, 0U, &res) == COG_OK);
    CHECK(cog_toolkit_result_is_ok(res));

    /* Feed the result back to the core as a COG_EVENT_TOOL_RESULT. */
    g_tool_result_seen = 0;
    g_tool_result_status = COG_ERROR_IO;
    ev.id = (cog_event_id_t)EV_TOOL_RESULT;
    ev.kind = COG_EVENT_TOOL_RESULT;
    ev.payload = res;
    ev.payload_len = sizeof(*res);
    ev.source_id = 0U;
    CHECK(cog_core_enqueue(core, &ev) == COG_OK);
    CHECK(cog_core_run(core) == COG_OK);

    /* The handler observed the real result status. */
    CHECK(g_tool_result_seen);
    CHECK(g_tool_result_status == COG_OK);

    cog_toolkit_result_collection_free(coll);
    cog_toolkit_call_free(calls[0]);
    cog_core_free(core);
    cog_session_free(session);
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int main(void)
{
    (void)printf("Scenario 06: parallel tool calls with timeout\n");
    scenario_overlap();
    scenario_ordered_results();
    scenario_timeout();
    scenario_cancel();
    scenario_tool_result_event();

    if (failures != 0) {
        (void)fprintf(stderr, "test_06_parallel_tools: %d failure(s)\n",
                      failures);
        return 1;
    }
    (void)printf("test_06_parallel_tools: all checks passed\n");
    return 0;
}

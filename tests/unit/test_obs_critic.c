/**
 * @file test_obs_critic.c
 * @brief Critic regression tests for cog-obs (ticket 84edd97, Writer 7f5b166).
 *
 * These tests probe edge cases and error paths that the Writer's
 * test_obs.c / test_obs_types.c do not cover directly:
 *
 *   - diagnostic slot reuse after clear, and update-in-place preserving
 *     the slot position in the enumeration order;
 *   - trace_begin / trace_end leaving the span stack unchanged when the
 *     clock fails (not just when the sink rejects);
 *   - trace_end retry idempotence: after a rejected end and a successful
 *     retry, the span is fully closed (depth 0, a further end is
 *     NOT_FOUND) and is emitted exactly once on success;
 *   - log_to log-only mode (NULL out_buf, zero capacity) still delivers
 *     to sinks and reports the formatted length;
 *   - a NULL deliver callback among working sinks is skipped without
 *     suppressing the other sinks;
 *   - filtered-out records still undergo field validation (documented
 *     precedence: INVALID_ARG wins over the filter);
 *   - connection events with conn_id == 0 are not rejected (the header
 *     only validates kind), pinning that behaviour.
 *
 * Ticket: 84edd97 (Critic review of 7f5b166).
 */

#include "cog/obs.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Deterministic / fault-injecting clock                               */
/* ------------------------------------------------------------------ */

static uint64_t g_now_ns = 1000000U;
static int g_clock_fail_on = -1; /* Fail on the Nth call; -1 = never. */
static int g_clock_calls = 0;

static cog_result_t test_clock(uint64_t *out_now_ns)
{
    ++g_clock_calls;
    if (g_clock_calls == g_clock_fail_on) {
        return COG_ERROR_IO;
    }
    *out_now_ns = g_now_ns;
    return COG_OK;
}

/* ------------------------------------------------------------------ */
/* Capture sinks                                                       */
/* ------------------------------------------------------------------ */

typedef struct sink_capture {
    char buf[2048];
    size_t len;
    size_t calls;
    cog_result_t result;
} sink_capture_t;

static cog_result_t capture_sink(void *user, const char *record,
                                 size_t record_len)
{
    sink_capture_t *c = (sink_capture_t *)user;

    ++c->calls;
    if (record_len < sizeof(c->buf)) {
        memcpy(c->buf, record, record_len);
        c->buf[record_len] = '\0';
        c->len = record_len;
    }
    return c->result;
}

static cog_obs_span_record_t g_last_span;
static size_t g_span_calls;
static cog_result_t g_span_result = COG_OK;

static cog_result_t capture_span(void *user, const cog_obs_span_record_t *span)
{
    (void)user;
    ++g_span_calls;
    g_last_span = *span;
    return g_span_result;
}

static cog_obs_conn_event_t g_last_event;
static size_t g_event_calls;
static cog_result_t g_event_result = COG_OK;

static cog_result_t capture_event(void *user, const cog_obs_conn_event_t *event)
{
    (void)user;
    ++g_event_calls;
    g_last_event = *event;
    return g_event_result;
}

/* ------------------------------------------------------------------ */
/* Fixtures                                                            */
/* ------------------------------------------------------------------ */

static cog_obs_config_t make_config(void)
{
    cog_obs_config_t cfg;

    memset(&cfg, 0, sizeof(cfg));
    cfg.min_severity = COG_OBS_TRACE;
    cfg.clock_ns = test_clock;
    return cfg;
}

static cog_obs_conn_snapshot_t make_snapshot(uint64_t conn_id,
                                             cog_obs_conn_state_t state)
{
    cog_obs_conn_snapshot_t snap;

    memset(&snap, 0, sizeof(snap));
    snap.conn_id = conn_id;
    snap.state = state;
    snap.endpoint = "ep";
    snap.provider = "prov";
    snap.started_ns = 1000000000U;
    snap.last_progress_ns = 2000000000U;
    snap.bytes_in = 1U;
    snap.bytes_out = 2U;
    snap.retries = 0U;
    snap.last_status = COG_OK;
    return snap;
}

/* ------------------------------------------------------------------ */
/* Diagnostics: slot reuse and update-in-place                         */
/* ------------------------------------------------------------------ */

static int test_diag_slot_reuse_after_clear(void)
{
    cog_obs_t *obs = NULL;
    cog_obs_config_t cfg = make_config();
    cog_obs_conn_snapshot_t snap;
    cog_obs_conn_snapshot_t out;
    size_t count = 0U;

    if (cog_obs_new(&obs, &cfg) != COG_OK) {
        return 1;
    }
    snap = make_snapshot(1U, COG_OBS_CONN_ACTIVE);
    if (cog_obs_diag_publish(obs, &snap) != COG_OK) {
        cog_obs_free(obs);
        return 1;
    }
    snap = make_snapshot(2U, COG_OBS_CONN_ACTIVE);
    if (cog_obs_diag_publish(obs, &snap) != COG_OK) {
        cog_obs_free(obs);
        return 1;
    }
    /* Remove slot 0; republishing a new id must reuse it. */
    if (cog_obs_diag_clear(obs, 1U) != COG_OK) {
        cog_obs_free(obs);
        return 1;
    }
    snap = make_snapshot(3U, COG_OBS_CONN_ACTIVE);
    if (cog_obs_diag_publish(obs, &snap) != COG_OK) {
        cog_obs_free(obs);
        return 1;
    }
    if (cog_obs_diag_count(obs, &count) != COG_OK || count != 2U) {
        cog_obs_free(obs);
        return 1;
    }
    /* Enumeration order is slot order: 3 (reused slot 0) then 2. */
    if (cog_obs_diag_at(obs, 0U, &out) != COG_OK || out.conn_id != 3U) {
        cog_obs_free(obs);
        return 1;
    }
    if (cog_obs_diag_at(obs, 1U, &out) != COG_OK || out.conn_id != 2U) {
        cog_obs_free(obs);
        return 1;
    }
    cog_obs_free(obs);
    return 0;
}

static int test_diag_update_preserves_slot(void)
{
    cog_obs_t *obs = NULL;
    cog_obs_config_t cfg = make_config();
    cog_obs_conn_snapshot_t snap;
    cog_obs_conn_snapshot_t out;

    if (cog_obs_new(&obs, &cfg) != COG_OK) {
        return 1;
    }
    snap = make_snapshot(1U, COG_OBS_CONN_ACTIVE);
    if (cog_obs_diag_publish(obs, &snap) != COG_OK) {
        cog_obs_free(obs);
        return 1;
    }
    snap = make_snapshot(2U, COG_OBS_CONN_ACTIVE);
    if (cog_obs_diag_publish(obs, &snap) != COG_OK) {
        cog_obs_free(obs);
        return 1;
    }
    /* Updating id 1 must keep it at slot 0, not move it. */
    snap = make_snapshot(1U, COG_OBS_CONN_STALLED);
    snap.bytes_in = 999U;
    if (cog_obs_diag_publish(obs, &snap) != COG_OK) {
        cog_obs_free(obs);
        return 1;
    }
    if (cog_obs_diag_at(obs, 0U, &out) != COG_OK || out.conn_id != 1U ||
        out.state != COG_OBS_CONN_STALLED || out.bytes_in != 999U) {
        cog_obs_free(obs);
        return 1;
    }
    if (cog_obs_diag_at(obs, 1U, &out) != COG_OK || out.conn_id != 2U) {
        cog_obs_free(obs);
        return 1;
    }
    cog_obs_free(obs);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Trace spans: clock-failure leaves the stack unchanged               */
/* ------------------------------------------------------------------ */

static int test_trace_begin_clock_failure_unchanged(void)
{
    cog_obs_t *obs = NULL;
    cog_obs_config_t cfg = make_config();
    cog_obs_span_id_t id = 77U;
    size_t depth = 99U;

    g_clock_fail_on = 1; /* First clock call fails. */
    g_clock_calls = 0;
    if (cog_obs_new(&obs, &cfg) != COG_OK) {
        g_clock_fail_on = -1;
        return 1;
    }
    if (cog_obs_trace_begin(obs, "a", &id) != COG_ERROR_IO) {
        cog_obs_free(obs);
        g_clock_fail_on = -1;
        return 1;
    }
    if (id != 77U) {
        cog_obs_free(obs);
        g_clock_fail_on = -1;
        return 1; /* Out parameter must be unchanged on failure. */
    }
    if (cog_obs_trace_depth(obs, &depth) != COG_OK || depth != 0U) {
        cog_obs_free(obs);
        g_clock_fail_on = -1;
        return 1; /* Stack must be unchanged. */
    }
    g_clock_fail_on = -1;
    cog_obs_free(obs);
    return 0;
}

static int test_trace_end_clock_failure_unchanged(void)
{
    cog_obs_t *obs = NULL;
    cog_obs_config_t cfg = make_config();
    cog_obs_span_id_t id = COG_OBS_SPAN_ID_NONE;
    size_t depth = 99U;

    cfg.trace_sink = capture_span;
    g_span_calls = 0U;
    g_span_result = COG_OK;
    g_clock_fail_on = -1;
    g_clock_calls = 0;
    if (cog_obs_new(&obs, &cfg) != COG_OK) {
        return 1;
    }
    g_now_ns = 1000U;
    if (cog_obs_trace_begin(obs, "a", &id) != COG_OK) {
        cog_obs_free(obs);
        return 1;
    }
    /* Fail the clock on the end's timestamp read. */
    g_clock_fail_on = 2;
    if (cog_obs_trace_end(obs, id, COG_OK) != COG_ERROR_IO) {
        cog_obs_free(obs);
        g_clock_fail_on = -1;
        return 1;
    }
    if (cog_obs_trace_depth(obs, &depth) != COG_OK || depth != 1U) {
        cog_obs_free(obs);
        g_clock_fail_on = -1;
        return 1; /* Stack must be unchanged on clock failure. */
    }
    if (g_span_calls != 0U) {
        cog_obs_free(obs);
        g_clock_fail_on = -1;
        return 1; /* No emission on clock failure. */
    }
    /* A retry with a working clock succeeds and emits exactly once. */
    g_clock_fail_on = -1;
    if (cog_obs_trace_end(obs, id, COG_OK) != COG_OK) {
        cog_obs_free(obs);
        return 1;
    }
    if (g_span_calls != 1U) {
        cog_obs_free(obs);
        return 1;
    }
    if (cog_obs_trace_depth(obs, &depth) != COG_OK || depth != 0U) {
        cog_obs_free(obs);
        return 1;
    }
    /* The span is fully closed: a further end is NOT_FOUND. */
    if (cog_obs_trace_end(obs, id, COG_OK) != COG_ERROR_NOT_FOUND) {
        cog_obs_free(obs);
        return 1;
    }
    cog_obs_free(obs);
    return 0;
}

static int test_trace_end_retry_no_duplicate(void)
{
    cog_obs_t *obs = NULL;
    cog_obs_config_t cfg = make_config();
    cog_obs_span_id_t id = COG_OBS_SPAN_ID_NONE;
    size_t depth = 99U;

    cfg.trace_sink = capture_span;
    g_span_calls = 0U;
    g_span_result = COG_ERROR_IO; /* First end is rejected. */
    if (cog_obs_new(&obs, &cfg) != COG_OK) {
        return 1;
    }
    g_now_ns = 1000U;
    if (cog_obs_trace_begin(obs, "a", &id) != COG_OK) {
        cog_obs_free(obs);
        return 1;
    }
    if (cog_obs_trace_end(obs, id, COG_OK) != COG_ERROR_IO) {
        cog_obs_free(obs);
        return 1;
    }
    if (cog_obs_trace_depth(obs, &depth) != COG_OK || depth != 1U) {
        cog_obs_free(obs);
        return 1;
    }
    /* Retry succeeds: exactly one successful emission, span closed. */
    g_span_result = COG_OK;
    if (cog_obs_trace_end(obs, id, COG_OK) != COG_OK) {
        cog_obs_free(obs);
        return 1;
    }
    if (g_span_calls != 2U) {
        cog_obs_free(obs);
        return 1; /* One rejected + one accepted = 2 sink calls. */
    }
    if (cog_obs_trace_depth(obs, &depth) != COG_OK || depth != 0U) {
        cog_obs_free(obs);
        return 1;
    }
    if (cog_obs_trace_end(obs, id, COG_OK) != COG_ERROR_NOT_FOUND) {
        cog_obs_free(obs);
        return 1; /* No duplicate emission possible after close. */
    }
    cog_obs_free(obs);
    return 0;
}

/* ------------------------------------------------------------------ */
/* log_to log-only mode                                                */
/* ------------------------------------------------------------------ */

static int test_log_to_log_only_delivers(void)
{
    cog_obs_t *obs = NULL;
    cog_obs_config_t cfg = make_config();
    sink_capture_t cap;
    size_t len = 0U;

    memset(&cap, 0, sizeof(cap));
    cfg.sinks = &(cog_obs_sink_table_t){ &(cog_obs_sink_t){
        "cap", capture_sink, &cap, COG_OBS_TRACE }, 1U };
    if (cog_obs_new(&obs, &cfg) != COG_OK) {
        return 1;
    }
    g_now_ns = 1000000U;
    /* NULL out_buf with zero capacity is log-only but still delivers. */
    if (cog_obs_log_to(obs, COG_OBS_INFO, "m", NULL, 0U, NULL, 0U,
                       &len) != COG_OK) {
        cog_obs_free(obs);
        return 1;
    }
    if (cap.calls != 1U) {
        cog_obs_free(obs);
        return 1;
    }
    if (len != strlen("1000000 info m")) {
        cog_obs_free(obs);
        return 1;
    }
    cog_obs_free(obs);
    return 0;
}

/* ------------------------------------------------------------------ */
/* NULL deliver callback among working sinks                           */
/* ------------------------------------------------------------------ */

static int test_log_null_deliver_among_working(void)
{
    cog_obs_t *obs = NULL;
    cog_obs_config_t cfg = make_config();
    sink_capture_t cap;
    cog_obs_sink_t sinks[2];

    memset(&cap, 0, sizeof(cap));
    sinks[0].name = "null-deliver";
    sinks[0].deliver = NULL;
    sinks[0].user = NULL;
    sinks[0].min_severity = COG_OBS_TRACE;
    sinks[1].name = "working";
    sinks[1].deliver = capture_sink;
    sinks[1].user = &cap;
    sinks[1].min_severity = COG_OBS_TRACE;
    cfg.sinks = &(cog_obs_sink_table_t){ sinks, 2U };
    if (cog_obs_new(&obs, &cfg) != COG_OK) {
        return 1;
    }
    if (cog_obs_log(obs, COG_OBS_INFO, "m", NULL, 0U) != COG_OK) {
        cog_obs_free(obs);
        return 1;
    }
    if (cap.calls != 1U) {
        cog_obs_free(obs);
        return 1; /* The working sink must still receive the record. */
    }
    cog_obs_free(obs);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Filtered-out records still undergo field validation                 */
/* ------------------------------------------------------------------ */

static int test_log_filtered_invalid_fields_precedence(void)
{
    cog_obs_t *obs = NULL;
    cog_obs_config_t cfg = make_config();
    cog_obs_field_t f;

    cfg.min_severity = COG_OBS_ERROR; /* INFO would be filtered out. */
    if (cog_obs_new(&obs, &cfg) != COG_OK) {
        return 1;
    }
    memset(&f, 0, sizeof(f));
    f.index = 99U; /* Out of range for the (NULL) field table. */
    f.kind = COG_OBS_FIELD_UINT;
    f.value.unsigned_integer = 1U;
    /* Validation runs before the filter: INVALID_ARG wins. */
    if (cog_obs_log(obs, COG_OBS_INFO, "m", &f, 1U) != COG_ERROR_INVALID_ARG) {
        cog_obs_free(obs);
        return 1;
    }
    cog_obs_free(obs);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Connection events: conn_id == 0 is not rejected                     */
/* ------------------------------------------------------------------ */

static int test_conn_event_conn_id_zero(void)
{
    cog_obs_t *obs = NULL;
    cog_obs_config_t cfg = make_config();
    cog_obs_conn_event_t ev;

    memset(&ev, 0, sizeof(ev));
    ev.kind = COG_OBS_CONN_EVENT_OPEN;
    ev.conn_id = 0U; /* Header only validates kind, not conn_id. */
    cfg.conn_event_sink = capture_event;
    g_event_calls = 0U;
    g_event_result = COG_OK;
    if (cog_obs_new(&obs, &cfg) != COG_OK) {
        return 1;
    }
    if (cog_obs_conn_event_emit(obs, &ev) != COG_OK) {
        cog_obs_free(obs);
        return 1;
    }
    if (g_event_calls != 1U || g_last_event.conn_id != 0U) {
        cog_obs_free(obs);
        return 1;
    }
    cog_obs_free(obs);
    return 0;
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

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

    CHECK("diag slot reuse after clear", test_diag_slot_reuse_after_clear());
    CHECK("diag update preserves slot", test_diag_update_preserves_slot());
    CHECK("trace begin clock failure unchanged",
          test_trace_begin_clock_failure_unchanged());
    CHECK("trace end clock failure unchanged",
          test_trace_end_clock_failure_unchanged());
    CHECK("trace end retry no duplicate", test_trace_end_retry_no_duplicate());
    CHECK("log_to log-only delivers", test_log_to_log_only_delivers());
    CHECK("log NULL deliver among working",
          test_log_null_deliver_among_working());
    CHECK("log filtered invalid fields precedence",
          test_log_filtered_invalid_fields_precedence());
    CHECK("conn event conn_id zero", test_conn_event_conn_id_zero());

#undef CHECK

    if (failures == 0U) {
        (void)printf("test_obs_critic: ok\n");
    }
    return failures == 0U ? 0 : 1;
}

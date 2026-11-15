/**
 * @file test_obs.c
 * @brief Contract test for cog/obs.h: structured logging, trace spans,
 *        connection diagnostics, and the connection event stream.
 *
 * This test pins the public contract of libraries/cog-obs/include/cog/
 * obs.h as implemented by libraries/cog-obs/src/obs.c:
 *
 *   - lifecycle: NULL/argument validation, defaults, custom capacities,
 *     allocation-failure reporting, free(NULL);
 *   - logging: NULL/severity/field validation, context filtering,
 *     exact record formatting (all field kinds, string escaping),
 *     sink threshold filtering, sink failure propagation with remaining
 *     sinks still offered the record, scratch overflow;
 *   - log_to: caller-buffer copy, overflow with required-capacity
 *     reporting, filtered-out empty output, text matches the sinks;
 *   - enabled: threshold query;
 *   - trace spans: begin/end round-trip, nesting/parent linkage,
 *     current/depth, capacity overflow, NOT_FOUND and out-of-order
 *     INVALID_ARG, NULL sink, sink rejection with stack restore,
 *     error status records;
 *   - diagnostics: publish insert/update, validation, table overflow,
 *     clear, enumeration skipping gaps, counts, state counts, and the
 *     exact text line produced by diag_format;
 *   - connection events: all seven kinds delivered with every field
 *     intact, no-sink no-op, validation, sink failure propagation.
 *
 * A deterministic clock makes every timestamp reproducible.
 *
 * Nothing is tested here that is not either stated in the header or
 * guaranteed by C11 semantics.
 *
 * Ticket: 7f5b166 (Stage 12.3b — Implement obs.c)
 */

#include "cog/obs.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef COG_OBS_H
#error "COG_OBS_H include guard missing"
#endif

/* ------------------------------------------------------------------ */
/* Compile-time checks: enum values and struct field types.            */
/* ------------------------------------------------------------------ */

_Static_assert(COG_OBS_CONN_IDLE == 0, "COG_OBS_CONN_IDLE must be 0");
_Static_assert(COG_OBS_CONN_CONNECTING == 1,
               "COG_OBS_CONN_CONNECTING must be 1");
_Static_assert(COG_OBS_CONN_ACTIVE == 2, "COG_OBS_CONN_ACTIVE must be 2");
_Static_assert(COG_OBS_CONN_STALLED == 3, "COG_OBS_CONN_STALLED must be 3");
_Static_assert(COG_OBS_CONN_CLOSED == 4, "COG_OBS_CONN_CLOSED must be 4");
_Static_assert(COG_OBS_CONN_FAILED == 5, "COG_OBS_CONN_FAILED must be 5");

_Static_assert(COG_OBS_CONN_EVENT_OPEN == 0,
               "COG_OBS_CONN_EVENT_OPEN must be 0");
_Static_assert(COG_OBS_CONN_EVENT_CONNECTED == 1,
               "COG_OBS_CONN_EVENT_CONNECTED must be 1");
_Static_assert(COG_OBS_CONN_EVENT_PROGRESS == 2,
               "COG_OBS_CONN_EVENT_PROGRESS must be 2");
_Static_assert(COG_OBS_CONN_EVENT_STALLED == 3,
               "COG_OBS_CONN_EVENT_STALLED must be 3");
_Static_assert(COG_OBS_CONN_EVENT_RETRY == 4,
               "COG_OBS_CONN_EVENT_RETRY must be 4");
_Static_assert(COG_OBS_CONN_EVENT_CLOSED == 5,
               "COG_OBS_CONN_EVENT_CLOSED must be 5");
_Static_assert(COG_OBS_CONN_EVENT_FAILED == 6,
               "COG_OBS_CONN_EVENT_FAILED must be 6");

#define FIELD_TYPE_ASSERT(strukt, field, type)                                 \
    _Static_assert(_Generic(((strukt){0}).field, type : 1, default : 0),        \
                   #strukt "." #field " must have type " #type)

FIELD_TYPE_ASSERT(cog_obs_sink_t, name, const char *);
FIELD_TYPE_ASSERT(cog_obs_sink_t, user, void *);
FIELD_TYPE_ASSERT(cog_obs_sink_t, min_severity, cog_obs_severity_t);
FIELD_TYPE_ASSERT(cog_obs_sink_table_t, sinks, const cog_obs_sink_t *);
FIELD_TYPE_ASSERT(cog_obs_sink_table_t, count, size_t);
FIELD_TYPE_ASSERT(cog_obs_conn_snapshot_t, conn_id, uint64_t);
FIELD_TYPE_ASSERT(cog_obs_conn_snapshot_t, state, cog_obs_conn_state_t);
FIELD_TYPE_ASSERT(cog_obs_conn_snapshot_t, endpoint, const char *);
FIELD_TYPE_ASSERT(cog_obs_conn_snapshot_t, provider, const char *);
FIELD_TYPE_ASSERT(cog_obs_conn_snapshot_t, started_ns, uint64_t);
FIELD_TYPE_ASSERT(cog_obs_conn_snapshot_t, last_progress_ns, uint64_t);
FIELD_TYPE_ASSERT(cog_obs_conn_snapshot_t, bytes_in, uint64_t);
FIELD_TYPE_ASSERT(cog_obs_conn_snapshot_t, bytes_out, uint64_t);
FIELD_TYPE_ASSERT(cog_obs_conn_snapshot_t, retries, uint32_t);
FIELD_TYPE_ASSERT(cog_obs_conn_snapshot_t, last_status, cog_result_t);
FIELD_TYPE_ASSERT(cog_obs_conn_event_t, kind, cog_obs_conn_event_kind_t);
FIELD_TYPE_ASSERT(cog_obs_conn_event_t, conn_id, uint64_t);
FIELD_TYPE_ASSERT(cog_obs_conn_event_t, timestamp_ns, uint64_t);
FIELD_TYPE_ASSERT(cog_obs_conn_event_t, session_id, const char *);
FIELD_TYPE_ASSERT(cog_obs_conn_event_t, endpoint, const char *);
FIELD_TYPE_ASSERT(cog_obs_conn_event_t, provider, const char *);
FIELD_TYPE_ASSERT(cog_obs_conn_event_t, model, const char *);
FIELD_TYPE_ASSERT(cog_obs_conn_event_t, bytes_in_delta, uint64_t);
FIELD_TYPE_ASSERT(cog_obs_conn_event_t, bytes_out_delta, uint64_t);
FIELD_TYPE_ASSERT(cog_obs_conn_event_t, bytes_in_total, uint64_t);
FIELD_TYPE_ASSERT(cog_obs_conn_event_t, bytes_out_total, uint64_t);
FIELD_TYPE_ASSERT(cog_obs_conn_event_t, retry_attempt, uint32_t);
FIELD_TYPE_ASSERT(cog_obs_conn_event_t, status, cog_result_t);
FIELD_TYPE_ASSERT(cog_obs_conn_event_t, http_status, uint16_t);

#undef FIELD_TYPE_ASSERT

/* ------------------------------------------------------------------ */
/* Deterministic clock and capture sinks                               */
/* ------------------------------------------------------------------ */

/** Current test-clock reading; tests set this before each operation. */
static uint64_t g_now_ns = 1000000U;

static cog_result_t test_clock(uint64_t *out_now_ns)
{
    *out_now_ns = g_now_ns;
    return COG_OK;
}

/** Capture buffer for one sink. */
typedef struct sink_capture {
    char buf[2048];      /**< Last record received. */
    size_t len;          /**< Length of the last record. */
    size_t calls;        /**< Number of deliveries. */
    cog_result_t result; /**< Value the sink returns. */
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

/** Last span record received by the trace sink. */
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

/** Last connection event received by the event sink. */
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
/* Shared fixtures                                                     */
/* ------------------------------------------------------------------ */

static const cog_obs_field_desc_t test_fields[] = {
    { "conn_id",  COG_OBS_FIELD_UINT   },
    { "endpoint", COG_OBS_FIELD_STRING },
    { "retries",  COG_OBS_FIELD_UINT   },
    { "delta",    COG_OBS_FIELD_INT    },
    { "ok",       COG_OBS_FIELD_BOOL   },
    { "ratio",    COG_OBS_FIELD_DOUBLE },
};
static const cog_obs_field_table_t test_field_table = { test_fields, 6U };

/** Build a zero-initialised config with the test clock and field table. */
static cog_obs_config_t make_config(void)
{
    cog_obs_config_t cfg;

    memset(&cfg, 0, sizeof(cfg));
    cfg.min_severity = COG_OBS_TRACE;
    cfg.fields = &test_field_table;
    cfg.clock_ns = test_clock;
    return cfg;
}

/** Build a snapshot with deterministic, easily checked values. */
static cog_obs_conn_snapshot_t make_snapshot(uint64_t conn_id,
                                             cog_obs_conn_state_t state)
{
    cog_obs_conn_snapshot_t snap;

    memset(&snap, 0, sizeof(snap));
    snap.conn_id = conn_id;
    snap.state = state;
    snap.endpoint = "api.openai.com";
    snap.provider = "openai";
    snap.started_ns = 1000000000U;
    snap.last_progress_ns = 2000000000U;
    snap.bytes_in = 512U;
    snap.bytes_out = 1024U;
    snap.retries = 2U;
    snap.last_status = COG_OK;
    return snap;
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

static int test_new_null_out(void)
{
    cog_obs_config_t cfg = make_config();

    if (cog_obs_new(NULL, &cfg) != COG_ERROR_NULL_PTR) {
        return 1;
    }
    return 0;
}

static int test_new_null_config(void)
{
    cog_obs_t *obs = (cog_obs_t *)(uintptr_t)1; /* Sentinel. */

    if (cog_obs_new(&obs, NULL) != COG_ERROR_NULL_PTR) {
        return 1;
    }
    if (obs != NULL) {
        return 1; /* *out must be NULL on failure. */
    }
    return 0;
}

static int test_new_invalid_severity(void)
{
    cog_obs_t *obs = NULL;
    cog_obs_config_t cfg = make_config();

    cfg.min_severity = (cog_obs_severity_t)99;
    if (cog_obs_new(&obs, &cfg) != COG_ERROR_INVALID_ARG || obs != NULL) {
        return 1;
    }
    cfg.min_severity = (cog_obs_severity_t)-1;
    if (cog_obs_new(&obs, &cfg) != COG_ERROR_INVALID_ARG || obs != NULL) {
        return 1;
    }
    return 0;
}

static int test_new_bad_field_table(void)
{
    cog_obs_t *obs = NULL;
    cog_obs_config_t cfg = make_config();
    cog_obs_field_table_t bad = { NULL, 3U };

    cfg.fields = &bad;
    if (cog_obs_new(&obs, &cfg) != COG_ERROR_INVALID_ARG || obs != NULL) {
        return 1;
    }
    return 0;
}

static int test_new_bad_sink_table(void)
{
    cog_obs_t *obs = NULL;
    cog_obs_config_t cfg = make_config();
    cog_obs_sink_table_t bad = { NULL, 2U };

    cfg.sinks = &bad;
    if (cog_obs_new(&obs, &cfg) != COG_ERROR_INVALID_ARG || obs != NULL) {
        return 1;
    }
    return 0;
}

static int test_new_nomem(void)
{
    cog_obs_t *obs = NULL;
    cog_obs_config_t cfg = make_config();

    cfg.diag_capacity = SIZE_MAX;
    if (cog_obs_new(&obs, &cfg) != COG_ERROR_NOMEM || obs != NULL) {
        return 1;
    }
    cfg.diag_capacity = 0U;
    cfg.span_capacity = SIZE_MAX;
    if (cog_obs_new(&obs, &cfg) != COG_ERROR_NOMEM || obs != NULL) {
        return 1;
    }
    return 0;
}

static int test_new_default_capacities(void)
{
    cog_obs_t *obs = NULL;
    cog_obs_config_t cfg = make_config();
    cog_obs_conn_snapshot_t snap;
    cog_obs_span_id_t id = COG_OBS_SPAN_ID_NONE;
    size_t i;

    if (cog_obs_new(&obs, &cfg) != COG_OK || obs == NULL) {
        return 1;
    }
    /* Default diag capacity is 16: 16 inserts fit, the 17th overflows. */
    for (i = 0U; i < 16U; ++i) {
        snap = make_snapshot((uint64_t)i + 1U, COG_OBS_CONN_ACTIVE);
        if (cog_obs_diag_publish(obs, &snap) != COG_OK) {
            cog_obs_free(obs);
            return 1;
        }
    }
    snap = make_snapshot(999U, COG_OBS_CONN_ACTIVE);
    if (cog_obs_diag_publish(obs, &snap) != COG_ERROR_OVERFLOW) {
        cog_obs_free(obs);
        return 1;
    }
    /* Default span capacity is 32: 32 begins fit, the 33rd overflows. */
    for (i = 0U; i < 32U; ++i) {
        if (cog_obs_trace_begin(obs, "s", &id) != COG_OK) {
            cog_obs_free(obs);
            return 1;
        }
    }
    if (cog_obs_trace_begin(obs, "s", &id) != COG_ERROR_OVERFLOW) {
        cog_obs_free(obs);
        return 1;
    }
    cog_obs_free(obs);
    return 0;
}

static int test_new_custom_capacities(void)
{
    cog_obs_t *obs = NULL;
    cog_obs_config_t cfg = make_config();
    cog_obs_conn_snapshot_t snap;
    cog_obs_span_id_t id = COG_OBS_SPAN_ID_NONE;

    cfg.diag_capacity = 2U;
    cfg.span_capacity = 3U;
    if (cog_obs_new(&obs, &cfg) != COG_OK || obs == NULL) {
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
    snap = make_snapshot(3U, COG_OBS_CONN_ACTIVE);
    if (cog_obs_diag_publish(obs, &snap) != COG_ERROR_OVERFLOW) {
        cog_obs_free(obs);
        return 1;
    }
    if (cog_obs_trace_begin(obs, "a", &id) != COG_OK ||
        cog_obs_trace_begin(obs, "b", &id) != COG_OK ||
        cog_obs_trace_begin(obs, "c", &id) != COG_OK) {
        cog_obs_free(obs);
        return 1;
    }
    if (cog_obs_trace_begin(obs, "d", &id) != COG_ERROR_OVERFLOW) {
        cog_obs_free(obs);
        return 1;
    }
    cog_obs_free(obs);
    return 0;
}

static int test_free_null(void)
{
    cog_obs_free(NULL);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Structured logging                                                  */
/* ------------------------------------------------------------------ */

static int test_log_null_args(void)
{
    cog_obs_t *obs = NULL;
    cog_obs_config_t cfg = make_config();

    if (cog_obs_new(&obs, &cfg) != COG_OK) {
        return 1;
    }
    if (cog_obs_log(NULL, COG_OBS_INFO, "m", NULL, 0U) != COG_ERROR_NULL_PTR) {
        cog_obs_free(obs);
        return 1;
    }
    if (cog_obs_log(obs, COG_OBS_INFO, NULL, NULL, 0U) != COG_ERROR_NULL_PTR) {
        cog_obs_free(obs);
        return 1;
    }
    cog_obs_free(obs);
    return 0;
}

static int test_log_invalid_severity(void)
{
    cog_obs_t *obs = NULL;
    cog_obs_config_t cfg = make_config();

    if (cog_obs_new(&obs, &cfg) != COG_OK) {
        return 1;
    }
    if (cog_obs_log(obs, (cog_obs_severity_t)99, "m", NULL, 0U) !=
        COG_ERROR_INVALID_ARG) {
        cog_obs_free(obs);
        return 1;
    }
    if (cog_obs_log(obs, (cog_obs_severity_t)-1, "m", NULL, 0U) !=
        COG_ERROR_INVALID_ARG) {
        cog_obs_free(obs);
        return 1;
    }
    cog_obs_free(obs);
    return 0;
}

static int test_log_field_validation(void)
{
    cog_obs_t *obs = NULL;
    cog_obs_config_t cfg = make_config();
    cog_obs_field_t f;

    if (cog_obs_new(&obs, &cfg) != COG_OK) {
        return 1;
    }
    /* fields NULL with non-zero count. */
    if (cog_obs_log(obs, COG_OBS_INFO, "m", NULL, 1U) != COG_ERROR_INVALID_ARG) {
        cog_obs_free(obs);
        return 1;
    }
    /* Index out of range for the field table. */
    memset(&f, 0, sizeof(f));
    f.index = 6U;
    f.kind = COG_OBS_FIELD_UINT;
    f.value.unsigned_integer = 1U;
    if (cog_obs_log(obs, COG_OBS_INFO, "m", &f, 1U) != COG_ERROR_INVALID_ARG) {
        cog_obs_free(obs);
        return 1;
    }
    /* Kind disagrees with the descriptor. */
    memset(&f, 0, sizeof(f));
    f.index = 0U; /* conn_id is declared UINT. */
    f.kind = COG_OBS_FIELD_INT;
    f.value.integer = 1;
    if (cog_obs_log(obs, COG_OBS_INFO, "m", &f, 1U) != COG_ERROR_INVALID_ARG) {
        cog_obs_free(obs);
        return 1;
    }
    /* NULL string value. */
    memset(&f, 0, sizeof(f));
    f.index = 1U; /* endpoint is declared STRING. */
    f.kind = COG_OBS_FIELD_STRING;
    f.value.string = NULL;
    if (cog_obs_log(obs, COG_OBS_INFO, "m", &f, 1U) != COG_ERROR_INVALID_ARG) {
        cog_obs_free(obs);
        return 1;
    }
    cog_obs_free(obs);
    return 0;
}

static int test_log_filtered(void)
{
    cog_obs_t *obs = NULL;
    cog_obs_config_t cfg = make_config();
    sink_capture_t cap;

    memset(&cap, 0, sizeof(cap));
    cfg.min_severity = COG_OBS_WARN;
    cfg.sinks = &(cog_obs_sink_table_t){ &(cog_obs_sink_t){
        "cap", capture_sink, &cap, COG_OBS_TRACE }, 1U };
    if (cog_obs_new(&obs, &cfg) != COG_OK) {
        return 1;
    }
    if (cog_obs_log(obs, COG_OBS_INFO, "dropped", NULL, 0U) != COG_OK) {
        cog_obs_free(obs);
        return 1;
    }
    if (cap.calls != 0U) {
        cog_obs_free(obs);
        return 1; /* Filtered record must not reach any sink. */
    }
    if (cog_obs_log(obs, COG_OBS_ERROR, "kept", NULL, 0U) != COG_OK) {
        cog_obs_free(obs);
        return 1;
    }
    if (cap.calls != 1U) {
        cog_obs_free(obs);
        return 1;
    }
    cog_obs_free(obs);
    return 0;
}

static int test_log_no_sinks(void)
{
    cog_obs_t *obs = NULL;
    cog_obs_config_t cfg = make_config();

    cfg.sinks = NULL;
    if (cog_obs_new(&obs, &cfg) != COG_OK) {
        return 1;
    }
    if (cog_obs_log(obs, COG_OBS_INFO, "m", NULL, 0U) != COG_OK) {
        cog_obs_free(obs);
        return 1;
    }
    cog_obs_free(obs);
    return 0;
}

static int test_log_format_all_kinds(void)
{
    cog_obs_t *obs = NULL;
    cog_obs_config_t cfg = make_config();
    sink_capture_t cap;
    cog_obs_field_t fields[6];
    char out[512];
    size_t len = 0U;
    const char *expected =
        "1000000 info cog hello conn_id=42 endpoint=\"api.openai.com\" "
        "retries=2 delta=-5 ok=true ratio=1.500000";

    memset(&cap, 0, sizeof(cap));
    cfg.service = "cog";
    cfg.sinks = &(cog_obs_sink_table_t){ &(cog_obs_sink_t){
        "cap", capture_sink, &cap, COG_OBS_TRACE }, 1U };
    if (cog_obs_new(&obs, &cfg) != COG_OK) {
        return 1;
    }

    memset(fields, 0, sizeof(fields));
    fields[0].index = 0U;
    fields[0].kind = COG_OBS_FIELD_UINT;
    fields[0].value.unsigned_integer = 42U;
    fields[1].index = 1U;
    fields[1].kind = COG_OBS_FIELD_STRING;
    fields[1].value.string = "api.openai.com";
    fields[2].index = 2U;
    fields[2].kind = COG_OBS_FIELD_UINT;
    fields[2].value.unsigned_integer = 2U;
    fields[3].index = 3U;
    fields[3].kind = COG_OBS_FIELD_INT;
    fields[3].value.integer = -5;
    fields[4].index = 4U;
    fields[4].kind = COG_OBS_FIELD_BOOL;
    fields[4].value.boolean = 1;
    fields[5].index = 5U;
    fields[5].kind = COG_OBS_FIELD_DOUBLE;
    fields[5].value.real = 1.5;

    if (cog_obs_log_to(obs, COG_OBS_INFO, "hello", fields, 6U, out,
                       sizeof(out), &len) != COG_OK) {
        cog_obs_free(obs);
        return 1;
    }
    if (strcmp(out, expected) != 0) {
        (void)fprintf(stderr, "record:\n  \"%s\"\nexpected:\n  \"%s\"\n",
                      out, expected);
        cog_obs_free(obs);
        return 1;
    }
    if (len != strlen(expected)) {
        cog_obs_free(obs);
        return 1;
    }
    if (cap.calls != 1U || cap.len != len || strcmp(cap.buf, out) != 0) {
        cog_obs_free(obs);
        return 1; /* Sink must receive exactly the formatted text. */
    }
    cog_obs_free(obs);
    return 0;
}

static int test_log_format_no_service(void)
{
    cog_obs_t *obs = NULL;
    cog_obs_config_t cfg = make_config();
    char out[128];
    size_t len = 0U;
    const char *expected = "2000000 warn plain";

    cfg.service = NULL;
    if (cog_obs_new(&obs, &cfg) != COG_OK) {
        return 1;
    }
    g_now_ns = 2000000U;
    if (cog_obs_log_to(obs, COG_OBS_WARN, "plain", NULL, 0U, out, sizeof(out),
                       &len) != COG_OK) {
        cog_obs_free(obs);
        return 1;
    }
    if (strcmp(out, expected) != 0) {
        (void)fprintf(stderr, "record \"%s\", expected \"%s\"\n", out,
                      expected);
        cog_obs_free(obs);
        return 1;
    }
    cog_obs_free(obs);
    return 0;
}

static int test_log_string_escaping(void)
{
    cog_obs_t *obs = NULL;
    cog_obs_config_t cfg = make_config();
    cog_obs_field_t f;
    char out[256];
    size_t len = 0U;
    const char *expected = "1000000 info esc endpoint=\"a\\\"b\\\\c\\nd\\te\\rf\"";

    if (cog_obs_new(&obs, &cfg) != COG_OK) {
        return 1;
    }
    g_now_ns = 1000000U;
    memset(&f, 0, sizeof(f));
    f.index = 1U;
    f.kind = COG_OBS_FIELD_STRING;
    f.value.string = "a\"b\\c\nd\te\rf";
    if (cog_obs_log_to(obs, COG_OBS_INFO, "esc", &f, 1U, out, sizeof(out),
                       &len) != COG_OK) {
        cog_obs_free(obs);
        return 1;
    }
    if (strcmp(out, expected) != 0) {
        (void)fprintf(stderr, "record \"%s\", expected \"%s\"\n", out,
                      expected);
        cog_obs_free(obs);
        return 1;
    }
    cog_obs_free(obs);
    return 0;
}

static int test_log_bool_false(void)
{
    cog_obs_t *obs = NULL;
    cog_obs_config_t cfg = make_config();
    cog_obs_field_t f;
    char out[128];
    size_t len = 0U;
    const char *expected = "1000000 info b ok=false";

    if (cog_obs_new(&obs, &cfg) != COG_OK) {
        return 1;
    }
    g_now_ns = 1000000U;
    memset(&f, 0, sizeof(f));
    f.index = 4U;
    f.kind = COG_OBS_FIELD_BOOL;
    f.value.boolean = 0;
    if (cog_obs_log_to(obs, COG_OBS_INFO, "b", &f, 1U, out, sizeof(out),
                       &len) != COG_OK) {
        cog_obs_free(obs);
        return 1;
    }
    if (strcmp(out, expected) != 0) {
        cog_obs_free(obs);
        return 1;
    }
    cog_obs_free(obs);
    return 0;
}

static int test_log_sink_thresholds(void)
{
    cog_obs_t *obs = NULL;
    cog_obs_config_t cfg = make_config();
    sink_capture_t verbose;
    sink_capture_t serious;
    cog_obs_sink_t sinks[2];

    memset(&verbose, 0, sizeof(verbose));
    memset(&serious, 0, sizeof(serious));
    sinks[0].name = "verbose";
    sinks[0].deliver = capture_sink;
    sinks[0].user = &verbose;
    sinks[0].min_severity = COG_OBS_TRACE;
    sinks[1].name = "serious";
    sinks[1].deliver = capture_sink;
    sinks[1].user = &serious;
    sinks[1].min_severity = COG_OBS_ERROR;
    cfg.sinks = &(cog_obs_sink_table_t){ sinks, 2U };

    if (cog_obs_new(&obs, &cfg) != COG_OK) {
        return 1;
    }
    /* INFO passes the context filter and the verbose sink, not serious. */
    if (cog_obs_log(obs, COG_OBS_INFO, "m", NULL, 0U) != COG_OK) {
        cog_obs_free(obs);
        return 1;
    }
    if (verbose.calls != 1U || serious.calls != 0U) {
        cog_obs_free(obs);
        return 1;
    }
    /* ERROR reaches both. */
    if (cog_obs_log(obs, COG_OBS_ERROR, "m", NULL, 0U) != COG_OK) {
        cog_obs_free(obs);
        return 1;
    }
    if (verbose.calls != 2U || serious.calls != 1U) {
        cog_obs_free(obs);
        return 1;
    }
    cog_obs_free(obs);
    return 0;
}

static int test_log_sink_failure(void)
{
    cog_obs_t *obs = NULL;
    cog_obs_config_t cfg = make_config();
    sink_capture_t first;
    sink_capture_t second;
    cog_obs_sink_t sinks[2];

    memset(&first, 0, sizeof(first));
    memset(&second, 0, sizeof(second));
    first.result = COG_ERROR_IO; /* This sink rejects the record. */
    sinks[0].name = "failing";
    sinks[0].deliver = capture_sink;
    sinks[0].user = &first;
    sinks[0].min_severity = COG_OBS_TRACE;
    sinks[1].name = "later";
    sinks[1].deliver = capture_sink;
    sinks[1].user = &second;
    sinks[1].min_severity = COG_OBS_TRACE;
    cfg.sinks = &(cog_obs_sink_table_t){ sinks, 2U };

    if (cog_obs_new(&obs, &cfg) != COG_OK) {
        return 1;
    }
    if (cog_obs_log(obs, COG_OBS_INFO, "m", NULL, 0U) != COG_ERROR_IO) {
        cog_obs_free(obs);
        return 1;
    }
    if (first.calls != 1U || second.calls != 1U) {
        cog_obs_free(obs);
        return 1; /* Remaining sinks must still be offered the record. */
    }
    cog_obs_free(obs);
    return 0;
}

static int test_log_scratch_overflow(void)
{
    cog_obs_t *obs = NULL;
    cog_obs_config_t cfg = make_config();
    sink_capture_t cap;
    char *long_msg;
    size_t i;

    memset(&cap, 0, sizeof(cap));
    cfg.sinks = &(cog_obs_sink_table_t){ &(cog_obs_sink_t){
        "cap", capture_sink, &cap, COG_OBS_TRACE }, 1U };
    if (cog_obs_new(&obs, &cfg) != COG_OK) {
        return 1;
    }
    long_msg = (char *)malloc(2048U);
    if (long_msg == NULL) {
        cog_obs_free(obs);
        return 1;
    }
    for (i = 0U; i < 2047U; ++i) {
        long_msg[i] = 'x';
    }
    long_msg[2047] = '\0';

    if (cog_obs_log(obs, COG_OBS_INFO, long_msg, NULL, 0U) !=
        COG_ERROR_OVERFLOW) {
        free(long_msg);
        cog_obs_free(obs);
        return 1;
    }
    if (cap.calls != 0U) {
        free(long_msg);
        cog_obs_free(obs);
        return 1; /* Over-long records must not be delivered. */
    }
    free(long_msg);
    cog_obs_free(obs);
    return 0;
}

/* ------------------------------------------------------------------ */
/* cog_obs_log_to                                                      */
/* ------------------------------------------------------------------ */

static int test_log_to_null_buf_with_capacity(void)
{
    cog_obs_t *obs = NULL;
    cog_obs_config_t cfg = make_config();

    if (cog_obs_new(&obs, &cfg) != COG_OK) {
        return 1;
    }
    if (cog_obs_log_to(obs, COG_OBS_INFO, "m", NULL, 0U, NULL, 16U, NULL) !=
        COG_ERROR_NULL_PTR) {
        cog_obs_free(obs);
        return 1;
    }
    /* NULL buffer with zero capacity is legal (log-only). */
    if (cog_obs_log_to(obs, COG_OBS_INFO, "m", NULL, 0U, NULL, 0U, NULL) !=
        COG_OK) {
        cog_obs_free(obs);
        return 1;
    }
    cog_obs_free(obs);
    return 0;
}

static int test_log_to_overflow_reports_required(void)
{
    cog_obs_t *obs = NULL;
    cog_obs_config_t cfg = make_config();
    char small[8];
    char big[512];
    size_t required = 0U;
    size_t len = 0U;
    const char *msg = "a message that is longer than eight bytes";
    const char *expected = "1000000 info a message that is longer than eight bytes";

    if (cog_obs_new(&obs, &cfg) != COG_OK) {
        return 1;
    }
    g_now_ns = 1000000U;
    if (cog_obs_log_to(obs, COG_OBS_INFO, msg, NULL, 0U, small,
                       sizeof(small), &required) != COG_ERROR_OVERFLOW) {
        cog_obs_free(obs);
        return 1;
    }
    if (required == 0U) {
        cog_obs_free(obs);
        return 1;
    }
    /* Retry with the reported capacity: must succeed. */
    if (cog_obs_log_to(obs, COG_OBS_INFO, msg, NULL, 0U, big, sizeof(big),
                       &len) != COG_OK) {
        cog_obs_free(obs);
        return 1;
    }
    if (len != required || strcmp(big, expected) != 0) {
        cog_obs_free(obs);
        return 1;
    }
    cog_obs_free(obs);
    return 0;
}

static int test_log_to_exact_fit(void)
{
    cog_obs_t *obs = NULL;
    cog_obs_config_t cfg = make_config();
    char out[64];
    size_t len = 0U;
    const char *expected = "1000000 info fit";

    if (cog_obs_new(&obs, &cfg) != COG_OK) {
        return 1;
    }
    g_now_ns = 1000000U;
    /* Buffer sized exactly to the record plus NUL. */
    if (cog_obs_log_to(obs, COG_OBS_INFO, "fit", NULL, 0U, out,
                       strlen(expected) + 1U, &len) != COG_OK) {
        cog_obs_free(obs);
        return 1;
    }
    if (len != strlen(expected) || strcmp(out, expected) != 0) {
        cog_obs_free(obs);
        return 1;
    }
    /* One byte short: overflow. */
    if (cog_obs_log_to(obs, COG_OBS_INFO, "fit", NULL, 0U, out,
                       strlen(expected), &len) != COG_ERROR_OVERFLOW) {
        cog_obs_free(obs);
        return 1;
    }
    if (len != strlen(expected)) {
        cog_obs_free(obs);
        return 1;
    }
    cog_obs_free(obs);
    return 0;
}

static int test_log_to_filtered_empty(void)
{
    cog_obs_t *obs = NULL;
    cog_obs_config_t cfg = make_config();
    char out[64];
    size_t len = 99U;

    cfg.min_severity = COG_OBS_ERROR;
    if (cog_obs_new(&obs, &cfg) != COG_OK) {
        return 1;
    }
    if (cog_obs_log_to(obs, COG_OBS_INFO, "m", NULL, 0U, out, sizeof(out),
                       &len) != COG_OK) {
        cog_obs_free(obs);
        return 1;
    }
    if (len != 0U || out[0] != '\0') {
        cog_obs_free(obs);
        return 1;
    }
    cog_obs_free(obs);
    return 0;
}

static int test_log_to_out_len_null(void)
{
    cog_obs_t *obs = NULL;
    cog_obs_config_t cfg = make_config();
    char out[64];

    if (cog_obs_new(&obs, &cfg) != COG_OK) {
        return 1;
    }
    if (cog_obs_log_to(obs, COG_OBS_INFO, "m", NULL, 0U, out, sizeof(out),
                       NULL) != COG_OK) {
        cog_obs_free(obs);
        return 1;
    }
    cog_obs_free(obs);
    return 0;
}

static int test_log_to_zero_capacity_buffer(void)
{
    cog_obs_t *obs = NULL;
    cog_obs_config_t cfg = make_config();
    char out[1];
    size_t required = 0U;

    if (cog_obs_new(&obs, &cfg) != COG_OK) {
        return 1;
    }
    /* A non-NULL buffer with zero capacity cannot hold the record. */
    if (cog_obs_log_to(obs, COG_OBS_INFO, "m", NULL, 0U, out, 0U,
                       &required) != COG_ERROR_OVERFLOW) {
        cog_obs_free(obs);
        return 1;
    }
    if (required == 0U) {
        cog_obs_free(obs);
        return 1;
    }
    cog_obs_free(obs);
    return 0;
}

static int test_log_null_deliver_callback(void)
{
    cog_obs_t *obs = NULL;
    cog_obs_config_t cfg = make_config();
    cog_obs_sink_t sink;

    /* A sink with a NULL deliver callback accepts nothing (defensive). */
    sink.name = "null-deliver";
    sink.deliver = NULL;
    sink.user = NULL;
    sink.min_severity = COG_OBS_TRACE;
    cfg.sinks = &(cog_obs_sink_table_t){ &sink, 1U };
    if (cog_obs_new(&obs, &cfg) != COG_OK) {
        return 1;
    }
    if (cog_obs_log(obs, COG_OBS_INFO, "m", NULL, 0U) != COG_OK) {
        cog_obs_free(obs);
        return 1;
    }
    cog_obs_free(obs);
    return 0;
}

/* ------------------------------------------------------------------ */
/* cog_obs_enabled                                                     */
/* ------------------------------------------------------------------ */

static int test_enabled_null_args(void)
{
    cog_obs_t *obs = NULL;
    cog_obs_config_t cfg = make_config();
    int enabled = 0;

    if (cog_obs_new(&obs, &cfg) != COG_OK) {
        return 1;
    }
    if (cog_obs_enabled(NULL, COG_OBS_INFO, &enabled) != COG_ERROR_NULL_PTR) {
        cog_obs_free(obs);
        return 1;
    }
    if (cog_obs_enabled(obs, COG_OBS_INFO, NULL) != COG_ERROR_NULL_PTR) {
        cog_obs_free(obs);
        return 1;
    }
    cog_obs_free(obs);
    return 0;
}

static int test_enabled_invalid_severity(void)
{
    cog_obs_t *obs = NULL;
    cog_obs_config_t cfg = make_config();
    int enabled = 0;

    if (cog_obs_new(&obs, &cfg) != COG_OK) {
        return 1;
    }
    if (cog_obs_enabled(obs, (cog_obs_severity_t)99, &enabled) !=
        COG_ERROR_INVALID_ARG) {
        cog_obs_free(obs);
        return 1;
    }
    cog_obs_free(obs);
    return 0;
}

static int test_enabled_threshold(void)
{
    cog_obs_t *obs = NULL;
    cog_obs_config_t cfg = make_config();
    int enabled = 0;
    int i;

    cfg.min_severity = COG_OBS_WARN;
    if (cog_obs_new(&obs, &cfg) != COG_OK) {
        return 1;
    }
    for (i = COG_OBS_TRACE; i <= COG_OBS_FATAL; ++i) {
        if (cog_obs_enabled(obs, (cog_obs_severity_t)i, &enabled) != COG_OK) {
            cog_obs_free(obs);
            return 1;
        }
        if (i >= COG_OBS_WARN) {
            if (enabled == 0) {
                cog_obs_free(obs);
                return 1;
            }
        } else if (enabled != 0) {
            cog_obs_free(obs);
            return 1;
        }
    }
    cog_obs_free(obs);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Trace spans                                                         */
/* ------------------------------------------------------------------ */

static int test_trace_begin_null_args(void)
{
    cog_obs_t *obs = NULL;
    cog_obs_config_t cfg = make_config();
    cog_obs_span_id_t id = COG_OBS_SPAN_ID_NONE;

    if (cog_obs_new(&obs, &cfg) != COG_OK) {
        return 1;
    }
    if (cog_obs_trace_begin(NULL, "n", &id) != COG_ERROR_NULL_PTR) {
        cog_obs_free(obs);
        return 1;
    }
    if (cog_obs_trace_begin(obs, NULL, &id) != COG_ERROR_NULL_PTR) {
        cog_obs_free(obs);
        return 1;
    }
    if (cog_obs_trace_begin(obs, "n", NULL) != COG_ERROR_NULL_PTR) {
        cog_obs_free(obs);
        return 1;
    }
    cog_obs_free(obs);
    return 0;
}

static int test_trace_begin_end_roundtrip(void)
{
    cog_obs_t *obs = NULL;
    cog_obs_config_t cfg = make_config();
    cog_obs_span_id_t id = COG_OBS_SPAN_ID_NONE;

    cfg.trace_sink = capture_span;
    cfg.trace_user = NULL;
    g_span_calls = 0U;
    g_span_result = COG_OK;
    if (cog_obs_new(&obs, &cfg) != COG_OK) {
        return 1;
    }
    g_now_ns = 5000U;
    if (cog_obs_trace_begin(obs, "root", &id) != COG_OK) {
        cog_obs_free(obs);
        return 1;
    }
    if (id != 1U) {
        cog_obs_free(obs);
        return 1; /* First span id must be 1, not the reserved zero. */
    }
    g_now_ns = 9000U;
    if (cog_obs_trace_end(obs, id, COG_OK) != COG_OK) {
        cog_obs_free(obs);
        return 1;
    }
    if (g_span_calls != 1U) {
        cog_obs_free(obs);
        return 1;
    }
    if (g_last_span.id != 1U || g_last_span.parent != COG_OBS_SPAN_ID_NONE ||
        strcmp(g_last_span.name, "root") != 0 ||
        g_last_span.start_ns != 5000U || g_last_span.duration_ns != 4000U ||
        g_last_span.status != COG_OK || g_last_span.ok != 1) {
        cog_obs_free(obs);
        return 1;
    }
    cog_obs_free(obs);
    return 0;
}

static int test_trace_nesting(void)
{
    cog_obs_t *obs = NULL;
    cog_obs_config_t cfg = make_config();
    cog_obs_span_id_t outer = COG_OBS_SPAN_ID_NONE;
    cog_obs_span_id_t inner = COG_OBS_SPAN_ID_NONE;

    cfg.trace_sink = capture_span;
    g_span_calls = 0U;
    g_span_result = COG_OK;
    if (cog_obs_new(&obs, &cfg) != COG_OK) {
        return 1;
    }
    g_now_ns = 1000U;
    if (cog_obs_trace_begin(obs, "outer", &outer) != COG_OK) {
        cog_obs_free(obs);
        return 1;
    }
    g_now_ns = 2000U;
    if (cog_obs_trace_begin(obs, "inner", &inner) != COG_OK) {
        cog_obs_free(obs);
        return 1;
    }
    if (inner != outer + 1U) {
        cog_obs_free(obs);
        return 1;
    }
    g_now_ns = 3000U;
    if (cog_obs_trace_end(obs, inner, COG_OK) != COG_OK) {
        cog_obs_free(obs);
        return 1;
    }
    if (g_last_span.parent != outer) {
        cog_obs_free(obs);
        return 1; /* Inner span's parent must be the outer span. */
    }
    g_now_ns = 4000U;
    if (cog_obs_trace_end(obs, outer, COG_OK) != COG_OK) {
        cog_obs_free(obs);
        return 1;
    }
    if (g_last_span.parent != COG_OBS_SPAN_ID_NONE) {
        cog_obs_free(obs);
        return 1; /* Outer span is a root. */
    }
    if (g_span_calls != 2U) {
        cog_obs_free(obs);
        return 1;
    }
    cog_obs_free(obs);
    return 0;
}

static int test_trace_current_depth(void)
{
    cog_obs_t *obs = NULL;
    cog_obs_config_t cfg = make_config();
    cog_obs_span_id_t id = COG_OBS_SPAN_ID_NONE;
    cog_obs_span_id_t current = 99U;
    size_t depth = 99U;

    if (cog_obs_new(&obs, &cfg) != COG_OK) {
        return 1;
    }
    if (cog_obs_trace_current(obs, &current) != COG_OK ||
        current != COG_OBS_SPAN_ID_NONE) {
        cog_obs_free(obs);
        return 1;
    }
    if (cog_obs_trace_depth(obs, &depth) != COG_OK || depth != 0U) {
        cog_obs_free(obs);
        return 1;
    }
    if (cog_obs_trace_begin(obs, "a", &id) != COG_OK) {
        cog_obs_free(obs);
        return 1;
    }
    if (cog_obs_trace_current(obs, &current) != COG_OK || current != id) {
        cog_obs_free(obs);
        return 1;
    }
    if (cog_obs_trace_depth(obs, &depth) != COG_OK || depth != 1U) {
        cog_obs_free(obs);
        return 1;
    }
    if (cog_obs_trace_end(obs, id, COG_OK) != COG_OK) {
        cog_obs_free(obs);
        return 1;
    }
    if (cog_obs_trace_depth(obs, &depth) != COG_OK || depth != 0U) {
        cog_obs_free(obs);
        return 1;
    }
    cog_obs_free(obs);
    return 0;
}

static int test_trace_current_depth_null(void)
{
    cog_obs_t *obs = NULL;
    cog_obs_config_t cfg = make_config();
    cog_obs_span_id_t id = COG_OBS_SPAN_ID_NONE;
    size_t depth = 0U;

    if (cog_obs_new(&obs, &cfg) != COG_OK) {
        return 1;
    }
    if (cog_obs_trace_current(NULL, &id) != COG_ERROR_NULL_PTR ||
        cog_obs_trace_current(obs, NULL) != COG_ERROR_NULL_PTR ||
        cog_obs_trace_depth(NULL, &depth) != COG_ERROR_NULL_PTR ||
        cog_obs_trace_depth(obs, NULL) != COG_ERROR_NULL_PTR) {
        cog_obs_free(obs);
        return 1;
    }
    cog_obs_free(obs);
    return 0;
}

static int test_trace_end_not_found(void)
{
    cog_obs_t *obs = NULL;
    cog_obs_config_t cfg = make_config();
    cog_obs_span_id_t id = COG_OBS_SPAN_ID_NONE;

    if (cog_obs_new(&obs, &cfg) != COG_OK) {
        return 1;
    }
    /* Empty stack. */
    if (cog_obs_trace_end(obs, 1U, COG_OK) != COG_ERROR_NOT_FOUND) {
        cog_obs_free(obs);
        return 1;
    }
    /* Unknown id on a non-empty stack. */
    if (cog_obs_trace_begin(obs, "a", &id) != COG_OK) {
        cog_obs_free(obs);
        return 1;
    }
    if (cog_obs_trace_end(obs, 42U, COG_OK) != COG_ERROR_NOT_FOUND) {
        cog_obs_free(obs);
        return 1;
    }
    /* A second unknown id, distinct from the open span's id. */
    if (cog_obs_trace_end(obs, 43U, COG_OK) != COG_ERROR_NOT_FOUND) {
        cog_obs_free(obs);
        return 1;
    }
    cog_obs_free(obs);
    return 0;
}

static int test_trace_end_out_of_order(void)
{
    cog_obs_t *obs = NULL;
    cog_obs_config_t cfg = make_config();
    cog_obs_span_id_t a = COG_OBS_SPAN_ID_NONE;
    cog_obs_span_id_t b = COG_OBS_SPAN_ID_NONE;
    size_t depth = 0U;

    if (cog_obs_new(&obs, &cfg) != COG_OK) {
        return 1;
    }
    if (cog_obs_trace_begin(obs, "a", &a) != COG_OK ||
        cog_obs_trace_begin(obs, "b", &b) != COG_OK) {
        cog_obs_free(obs);
        return 1;
    }
    /* Closing the outer span while the inner is open is invalid. */
    if (cog_obs_trace_end(obs, a, COG_OK) != COG_ERROR_INVALID_ARG) {
        cog_obs_free(obs);
        return 1;
    }
    if (cog_obs_trace_depth(obs, &depth) != COG_OK || depth != 2U) {
        cog_obs_free(obs);
        return 1; /* Stack must be unchanged on failure. */
    }
    /* Closing in order succeeds. */
    if (cog_obs_trace_end(obs, b, COG_OK) != COG_OK ||
        cog_obs_trace_end(obs, a, COG_OK) != COG_OK) {
        cog_obs_free(obs);
        return 1;
    }
    cog_obs_free(obs);
    return 0;
}

static int test_trace_null_sink(void)
{
    cog_obs_t *obs = NULL;
    cog_obs_config_t cfg = make_config();
    cog_obs_span_id_t id = COG_OBS_SPAN_ID_NONE;
    size_t depth = 0U;

    cfg.trace_sink = NULL; /* Spans tracked but records discarded. */
    if (cog_obs_new(&obs, &cfg) != COG_OK) {
        return 1;
    }
    if (cog_obs_trace_begin(obs, "a", &id) != COG_OK) {
        cog_obs_free(obs);
        return 1;
    }
    if (cog_obs_trace_depth(obs, &depth) != COG_OK || depth != 1U) {
        cog_obs_free(obs);
        return 1;
    }
    if (cog_obs_trace_end(obs, id, COG_OK) != COG_OK) {
        cog_obs_free(obs);
        return 1;
    }
    if (cog_obs_trace_depth(obs, &depth) != COG_OK || depth != 0U) {
        cog_obs_free(obs);
        return 1;
    }
    cog_obs_free(obs);
    return 0;
}

static int test_trace_sink_failure_restores(void)
{
    cog_obs_t *obs = NULL;
    cog_obs_config_t cfg = make_config();
    cog_obs_span_id_t id = COG_OBS_SPAN_ID_NONE;
    size_t depth = 0U;

    cfg.trace_sink = capture_span;
    g_span_calls = 0U;
    g_span_result = COG_ERROR_IO; /* Sink rejects the record. */
    if (cog_obs_new(&obs, &cfg) != COG_OK) {
        return 1;
    }
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
        return 1; /* Stack must be unchanged on failure. */
    }
    /* A retry with a working sink succeeds and emits the record. */
    g_span_result = COG_OK;
    if (cog_obs_trace_end(obs, id, COG_OK) != COG_OK) {
        cog_obs_free(obs);
        return 1;
    }
    if (g_span_calls != 2U || g_last_span.id != id) {
        cog_obs_free(obs);
        return 1;
    }
    cog_obs_free(obs);
    return 0;
}

static int test_trace_error_status(void)
{
    cog_obs_t *obs = NULL;
    cog_obs_config_t cfg = make_config();
    cog_obs_span_id_t id = COG_OBS_SPAN_ID_NONE;

    cfg.trace_sink = capture_span;
    g_span_calls = 0U;
    g_span_result = COG_OK;
    if (cog_obs_new(&obs, &cfg) != COG_OK) {
        return 1;
    }
    if (cog_obs_trace_begin(obs, "a", &id) != COG_OK) {
        cog_obs_free(obs);
        return 1;
    }
    if (cog_obs_trace_end(obs, id, COG_ERROR_TIMEOUT) != COG_OK) {
        cog_obs_free(obs);
        return 1;
    }
    if (g_last_span.status != COG_ERROR_TIMEOUT || g_last_span.ok != 0) {
        cog_obs_free(obs);
        return 1;
    }
    cog_obs_free(obs);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Connection diagnostics                                              */
/* ------------------------------------------------------------------ */

static int test_diag_publish_null_args(void)
{
    cog_obs_t *obs = NULL;
    cog_obs_config_t cfg = make_config();
    cog_obs_conn_snapshot_t snap = make_snapshot(1U, COG_OBS_CONN_ACTIVE);

    if (cog_obs_new(&obs, &cfg) != COG_OK) {
        return 1;
    }
    if (cog_obs_diag_publish(NULL, &snap) != COG_ERROR_NULL_PTR) {
        cog_obs_free(obs);
        return 1;
    }
    if (cog_obs_diag_publish(obs, NULL) != COG_ERROR_NULL_PTR) {
        cog_obs_free(obs);
        return 1;
    }
    cog_obs_free(obs);
    return 0;
}

static int test_diag_publish_validation(void)
{
    cog_obs_t *obs = NULL;
    cog_obs_config_t cfg = make_config();
    cog_obs_conn_snapshot_t snap;

    if (cog_obs_new(&obs, &cfg) != COG_OK) {
        return 1;
    }
    /* Zero conn_id is rejected. */
    snap = make_snapshot(0U, COG_OBS_CONN_ACTIVE);
    if (cog_obs_diag_publish(obs, &snap) != COG_ERROR_INVALID_ARG) {
        cog_obs_free(obs);
        return 1;
    }
    /* Out-of-range state is rejected. */
    snap = make_snapshot(1U, (cog_obs_conn_state_t)99);
    if (cog_obs_diag_publish(obs, &snap) != COG_ERROR_INVALID_ARG) {
        cog_obs_free(obs);
        return 1;
    }
    snap = make_snapshot(1U, (cog_obs_conn_state_t)-1);
    if (cog_obs_diag_publish(obs, &snap) != COG_ERROR_INVALID_ARG) {
        cog_obs_free(obs);
        return 1;
    }
    cog_obs_free(obs);
    return 0;
}

static int test_diag_publish_insert_update(void)
{
    cog_obs_t *obs = NULL;
    cog_obs_config_t cfg = make_config();
    cog_obs_conn_snapshot_t snap;
    cog_obs_conn_snapshot_t out;
    size_t count = 0U;

    if (cog_obs_new(&obs, &cfg) != COG_OK) {
        return 1;
    }
    snap = make_snapshot(42U, COG_OBS_CONN_CONNECTING);
    if (cog_obs_diag_publish(obs, &snap) != COG_OK) {
        cog_obs_free(obs);
        return 1;
    }
    /* Update the same id: count stays one, slot is overwritten. */
    snap = make_snapshot(42U, COG_OBS_CONN_ACTIVE);
    snap.bytes_in = 999U;
    if (cog_obs_diag_publish(obs, &snap) != COG_OK) {
        cog_obs_free(obs);
        return 1;
    }
    if (cog_obs_diag_count(obs, &count) != COG_OK || count != 1U) {
        cog_obs_free(obs);
        return 1;
    }
    if (cog_obs_diag_at(obs, 0U, &out) != COG_OK) {
        cog_obs_free(obs);
        return 1;
    }
    if (out.conn_id != 42U || out.state != COG_OBS_CONN_ACTIVE ||
        out.bytes_in != 999U) {
        cog_obs_free(obs);
        return 1;
    }
    cog_obs_free(obs);
    return 0;
}

static int test_diag_publish_overflow(void)
{
    cog_obs_t *obs = NULL;
    cog_obs_config_t cfg = make_config();
    cog_obs_conn_snapshot_t snap;

    cfg.diag_capacity = 2U;
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
    snap = make_snapshot(3U, COG_OBS_CONN_ACTIVE);
    if (cog_obs_diag_publish(obs, &snap) != COG_ERROR_OVERFLOW) {
        cog_obs_free(obs);
        return 1;
    }
    /* Updating an existing id still works on a full table. */
    snap = make_snapshot(1U, COG_OBS_CONN_STALLED);
    if (cog_obs_diag_publish(obs, &snap) != COG_OK) {
        cog_obs_free(obs);
        return 1;
    }
    cog_obs_free(obs);
    return 0;
}

static int test_diag_clear(void)
{
    cog_obs_t *obs = NULL;
    cog_obs_config_t cfg = make_config();
    cog_obs_conn_snapshot_t snap;
    size_t count = 0U;

    if (cog_obs_new(&obs, &cfg) != COG_OK) {
        return 1;
    }
    snap = make_snapshot(7U, COG_OBS_CONN_ACTIVE);
    if (cog_obs_diag_publish(obs, &snap) != COG_OK) {
        cog_obs_free(obs);
        return 1;
    }
    if (cog_obs_diag_clear(obs, 7U) != COG_OK) {
        cog_obs_free(obs);
        return 1;
    }
    if (cog_obs_diag_count(obs, &count) != COG_OK || count != 0U) {
        cog_obs_free(obs);
        return 1;
    }
    /* Clearing again reports NOT_FOUND. */
    if (cog_obs_diag_clear(obs, 7U) != COG_ERROR_NOT_FOUND) {
        cog_obs_free(obs);
        return 1;
    }
    /* Zero id is invalid. */
    if (cog_obs_diag_clear(obs, 0U) != COG_ERROR_INVALID_ARG) {
        cog_obs_free(obs);
        return 1;
    }
    /* NULL context. */
    if (cog_obs_diag_clear(NULL, 7U) != COG_ERROR_NULL_PTR) {
        cog_obs_free(obs);
        return 1;
    }
    cog_obs_free(obs);
    return 0;
}

static int test_diag_at_skips_gaps(void)
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
    snap = make_snapshot(3U, COG_OBS_CONN_ACTIVE);
    if (cog_obs_diag_publish(obs, &snap) != COG_OK) {
        cog_obs_free(obs);
        return 1;
    }
    /* Remove the middle slot, leaving a gap. */
    if (cog_obs_diag_clear(obs, 2U) != COG_OK) {
        cog_obs_free(obs);
        return 1;
    }
    if (cog_obs_diag_count(obs, &count) != COG_OK || count != 2U) {
        cog_obs_free(obs);
        return 1;
    }
    if (cog_obs_diag_at(obs, 0U, &out) != COG_OK || out.conn_id != 1U) {
        cog_obs_free(obs);
        return 1;
    }
    if (cog_obs_diag_at(obs, 1U, &out) != COG_OK || out.conn_id != 3U) {
        cog_obs_free(obs);
        return 1;
    }
    /* Index beyond the occupied count is invalid. */
    if (cog_obs_diag_at(obs, 2U, &out) != COG_ERROR_INVALID_ARG) {
        cog_obs_free(obs);
        return 1;
    }
    /* NULL arguments. */
    if (cog_obs_diag_at(NULL, 0U, &out) != COG_ERROR_NULL_PTR ||
        cog_obs_diag_at(obs, 0U, NULL) != COG_ERROR_NULL_PTR) {
        cog_obs_free(obs);
        return 1;
    }
    cog_obs_free(obs);
    return 0;
}

static int test_diag_count_null(void)
{
    cog_obs_t *obs = NULL;
    cog_obs_config_t cfg = make_config();
    size_t count = 0U;

    if (cog_obs_new(&obs, &cfg) != COG_OK) {
        return 1;
    }
    if (cog_obs_diag_count(NULL, &count) != COG_ERROR_NULL_PTR ||
        cog_obs_diag_count(obs, NULL) != COG_ERROR_NULL_PTR) {
        cog_obs_free(obs);
        return 1;
    }
    cog_obs_free(obs);
    return 0;
}

static int test_diag_count_state(void)
{
    cog_obs_t *obs = NULL;
    cog_obs_config_t cfg = make_config();
    cog_obs_conn_snapshot_t snap;
    size_t stalled = 0U;
    size_t active = 0U;

    if (cog_obs_new(&obs, &cfg) != COG_OK) {
        return 1;
    }
    snap = make_snapshot(1U, COG_OBS_CONN_STALLED);
    if (cog_obs_diag_publish(obs, &snap) != COG_OK) {
        cog_obs_free(obs);
        return 1;
    }
    snap = make_snapshot(2U, COG_OBS_CONN_ACTIVE);
    if (cog_obs_diag_publish(obs, &snap) != COG_OK) {
        cog_obs_free(obs);
        return 1;
    }
    snap = make_snapshot(3U, COG_OBS_CONN_STALLED);
    if (cog_obs_diag_publish(obs, &snap) != COG_OK) {
        cog_obs_free(obs);
        return 1;
    }
    if (cog_obs_diag_count_state(obs, COG_OBS_CONN_STALLED, &stalled) !=
            COG_OK ||
        stalled != 2U) {
        cog_obs_free(obs);
        return 1;
    }
    if (cog_obs_diag_count_state(obs, COG_OBS_CONN_ACTIVE, &active) != COG_OK ||
        active != 1U) {
        cog_obs_free(obs);
        return 1;
    }
    /* Invalid state. */
    if (cog_obs_diag_count_state(obs, (cog_obs_conn_state_t)99, &stalled) !=
        COG_ERROR_INVALID_ARG) {
        cog_obs_free(obs);
        return 1;
    }
    /* NULL arguments. */
    if (cog_obs_diag_count_state(NULL, COG_OBS_CONN_ACTIVE, &stalled) !=
            COG_ERROR_NULL_PTR ||
        cog_obs_diag_count_state(obs, COG_OBS_CONN_ACTIVE, NULL) !=
            COG_ERROR_NULL_PTR) {
        cog_obs_free(obs);
        return 1;
    }
    cog_obs_free(obs);
    return 0;
}

static int test_diag_format_full(void)
{
    cog_obs_t *obs = NULL;
    cog_obs_config_t cfg = make_config();
    cog_obs_conn_snapshot_t snap;
    char out[512];
    size_t len = 0U;
    const char *expected =
        "conn=42 state=active endpoint=api.openai.com provider=openai "
        "in=512 out=1024 retries=2 age_ms=4000 idle_ms=3000 status=0";

    if (cog_obs_new(&obs, &cfg) != COG_OK) {
        return 1;
    }
    g_now_ns = 5000000000U; /* 5000 ms; started 1000 ms, progress 2000 ms. */
    snap = make_snapshot(42U, COG_OBS_CONN_ACTIVE);
    if (cog_obs_diag_format(obs, &snap, out, sizeof(out), &len) != COG_OK) {
        cog_obs_free(obs);
        return 1;
    }
    if (strcmp(out, expected) != 0) {
        (void)fprintf(stderr, "diag line:\n  \"%s\"\nexpected:\n  \"%s\"\n",
                      out, expected);
        cog_obs_free(obs);
        return 1;
    }
    if (len != strlen(expected)) {
        cog_obs_free(obs);
        return 1;
    }
    cog_obs_free(obs);
    return 0;
}

static int test_diag_format_omits_null(void)
{
    cog_obs_t *obs = NULL;
    cog_obs_config_t cfg = make_config();
    cog_obs_conn_snapshot_t snap;
    char out[512];
    size_t len = 0U;
    const char *expected =
        "conn=7 state=failed in=512 out=1024 retries=3 age_ms=1000 idle_ms=0 "
        "status=-4";

    if (cog_obs_new(&obs, &cfg) != COG_OK) {
        return 1;
    }
    g_now_ns = 2000000000U;
    snap = make_snapshot(7U, COG_OBS_CONN_FAILED);
    snap.endpoint = NULL;
    snap.provider = NULL;
    snap.retries = 3U;
    snap.last_status = COG_ERROR_NOT_FOUND;
    if (cog_obs_diag_format(obs, &snap, out, sizeof(out), &len) != COG_OK) {
        cog_obs_free(obs);
        return 1;
    }
    if (strcmp(out, expected) != 0) {
        (void)fprintf(stderr, "diag line \"%s\", expected \"%s\"\n", out,
                      expected);
        cog_obs_free(obs);
        return 1;
    }
    cog_obs_free(obs);
    return 0;
}

static int test_diag_format_errors(void)
{
    cog_obs_t *obs = NULL;
    cog_obs_config_t cfg = make_config();
    cog_obs_conn_snapshot_t snap;
    char out[512];
    size_t len = 0U;

    if (cog_obs_new(&obs, &cfg) != COG_OK) {
        return 1;
    }
    snap = make_snapshot(1U, COG_OBS_CONN_ACTIVE);
    /* NULL arguments. */
    if (cog_obs_diag_format(NULL, &snap, out, sizeof(out), &len) !=
            COG_ERROR_NULL_PTR ||
        cog_obs_diag_format(obs, NULL, out, sizeof(out), &len) !=
            COG_ERROR_NULL_PTR ||
        cog_obs_diag_format(obs, &snap, NULL, sizeof(out), &len) !=
            COG_ERROR_NULL_PTR) {
        cog_obs_free(obs);
        return 1;
    }
    /* Invalid state. */
    snap.state = (cog_obs_conn_state_t)99;
    if (cog_obs_diag_format(obs, &snap, out, sizeof(out), &len) !=
        COG_ERROR_INVALID_ARG) {
        cog_obs_free(obs);
        return 1;
    }
    /* Overflow reports the required capacity. */
    snap.state = COG_OBS_CONN_ACTIVE;
    if (cog_obs_diag_format(obs, &snap, out, 8U, &len) !=
        COG_ERROR_OVERFLOW) {
        cog_obs_free(obs);
        return 1;
    }
    if (len == 0U) {
        cog_obs_free(obs);
        return 1;
    }
    cog_obs_free(obs);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Connection event stream                                             */
/* ------------------------------------------------------------------ */

static int test_conn_event_null_args(void)
{
    cog_obs_t *obs = NULL;
    cog_obs_config_t cfg = make_config();
    cog_obs_conn_event_t ev;

    memset(&ev, 0, sizeof(ev));
    ev.kind = COG_OBS_CONN_EVENT_OPEN;
    if (cog_obs_new(&obs, &cfg) != COG_OK) {
        return 1;
    }
    if (cog_obs_conn_event_emit(NULL, &ev) != COG_ERROR_NULL_PTR) {
        cog_obs_free(obs);
        return 1;
    }
    if (cog_obs_conn_event_emit(obs, NULL) != COG_ERROR_NULL_PTR) {
        cog_obs_free(obs);
        return 1;
    }
    cog_obs_free(obs);
    return 0;
}

static int test_conn_event_invalid_kind(void)
{
    cog_obs_t *obs = NULL;
    cog_obs_config_t cfg = make_config();
    cog_obs_conn_event_t ev;

    memset(&ev, 0, sizeof(ev));
    ev.kind = (cog_obs_conn_event_kind_t)99;
    if (cog_obs_new(&obs, &cfg) != COG_OK) {
        return 1;
    }
    if (cog_obs_conn_event_emit(obs, &ev) != COG_ERROR_INVALID_ARG) {
        cog_obs_free(obs);
        return 1;
    }
    ev.kind = (cog_obs_conn_event_kind_t)-1;
    if (cog_obs_conn_event_emit(obs, &ev) != COG_ERROR_INVALID_ARG) {
        cog_obs_free(obs);
        return 1;
    }
    cog_obs_free(obs);
    return 0;
}

static int test_conn_event_no_sink(void)
{
    cog_obs_t *obs = NULL;
    cog_obs_config_t cfg = make_config();
    cog_obs_conn_event_t ev;

    memset(&ev, 0, sizeof(ev));
    ev.kind = COG_OBS_CONN_EVENT_OPEN;
    cfg.conn_event_sink = NULL;
    if (cog_obs_new(&obs, &cfg) != COG_OK) {
        return 1;
    }
    if (cog_obs_conn_event_emit(obs, &ev) != COG_OK) {
        cog_obs_free(obs);
        return 1;
    }
    cog_obs_free(obs);
    return 0;
}

static int test_conn_event_all_kinds(void)
{
    cog_obs_t *obs = NULL;
    cog_obs_config_t cfg = make_config();
    cog_obs_conn_event_t ev;
    int kind;

    cfg.conn_event_sink = capture_event;
    g_event_calls = 0U;
    g_event_result = COG_OK;
    if (cog_obs_new(&obs, &cfg) != COG_OK) {
        return 1;
    }
    for (kind = COG_OBS_CONN_EVENT_OPEN; kind <= COG_OBS_CONN_EVENT_FAILED;
         ++kind) {
        memset(&ev, 0, sizeof(ev));
        ev.kind = (cog_obs_conn_event_kind_t)kind;
        ev.conn_id = 42U;
        ev.timestamp_ns = 1234U;
        ev.session_id = "sess-abc";
        ev.endpoint = "api.openai.com";
        ev.provider = "openai";
        ev.model = "gpt-4";
        ev.bytes_in_delta = 512U;
        ev.bytes_out_delta = 1024U;
        ev.bytes_in_total = 4096U;
        ev.bytes_out_total = 8192U;
        ev.retry_attempt = 1U;
        ev.status = COG_OK;
        ev.http_status = 200U;
        if (cog_obs_conn_event_emit(obs, &ev) != COG_OK) {
            cog_obs_free(obs);
            return 1;
        }
        if (g_event_calls != (size_t)kind + 1U) {
            cog_obs_free(obs);
            return 1;
        }
        if (g_last_event.kind != ev.kind || g_last_event.conn_id != 42U ||
            g_last_event.timestamp_ns != 1234U ||
            strcmp(g_last_event.session_id, "sess-abc") != 0 ||
            strcmp(g_last_event.endpoint, "api.openai.com") != 0 ||
            strcmp(g_last_event.provider, "openai") != 0 ||
            strcmp(g_last_event.model, "gpt-4") != 0 ||
            g_last_event.bytes_in_delta != 512U ||
            g_last_event.bytes_out_delta != 1024U ||
            g_last_event.bytes_in_total != 4096U ||
            g_last_event.bytes_out_total != 8192U ||
            g_last_event.retry_attempt != 1U ||
            g_last_event.status != COG_OK ||
            g_last_event.http_status != 200U) {
            cog_obs_free(obs);
            return 1;
        }
    }
    cog_obs_free(obs);
    return 0;
}

static int test_conn_event_sink_failure(void)
{
    cog_obs_t *obs = NULL;
    cog_obs_config_t cfg = make_config();
    cog_obs_conn_event_t ev;

    memset(&ev, 0, sizeof(ev));
    ev.kind = COG_OBS_CONN_EVENT_OPEN;
    cfg.conn_event_sink = capture_event;
    g_event_calls = 0U;
    g_event_result = COG_ERROR_IO;
    if (cog_obs_new(&obs, &cfg) != COG_OK) {
        return 1;
    }
    if (cog_obs_conn_event_emit(obs, &ev) != COG_ERROR_IO) {
        cog_obs_free(obs);
        return 1;
    }
    if (g_event_calls != 1U) {
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

    /* Lifecycle. */
    CHECK("new NULL out", test_new_null_out());
    CHECK("new NULL config", test_new_null_config());
    CHECK("new invalid severity", test_new_invalid_severity());
    CHECK("new bad field table", test_new_bad_field_table());
    CHECK("new bad sink table", test_new_bad_sink_table());
    CHECK("new NOMEM", test_new_nomem());
    CHECK("new default capacities", test_new_default_capacities());
    CHECK("new custom capacities", test_new_custom_capacities());
    CHECK("free NULL", test_free_null());

    /* Logging. */
    CHECK("log NULL args", test_log_null_args());
    CHECK("log invalid severity", test_log_invalid_severity());
    CHECK("log field validation", test_log_field_validation());
    CHECK("log filtered", test_log_filtered());
    CHECK("log no sinks", test_log_no_sinks());
    CHECK("log format all kinds", test_log_format_all_kinds());
    CHECK("log format no service", test_log_format_no_service());
    CHECK("log string escaping", test_log_string_escaping());
    CHECK("log bool false", test_log_bool_false());
    CHECK("log sink thresholds", test_log_sink_thresholds());
    CHECK("log sink failure", test_log_sink_failure());
    CHECK("log scratch overflow", test_log_scratch_overflow());

    /* log_to. */
    CHECK("log_to NULL buf with capacity", test_log_to_null_buf_with_capacity());
    CHECK("log_to overflow reports required", test_log_to_overflow_reports_required());
    CHECK("log_to exact fit", test_log_to_exact_fit());
    CHECK("log_to filtered empty", test_log_to_filtered_empty());
    CHECK("log_to out_len NULL", test_log_to_out_len_null());
    CHECK("log_to zero capacity buffer", test_log_to_zero_capacity_buffer());
    CHECK("log NULL deliver callback", test_log_null_deliver_callback());

    /* enabled. */
    CHECK("enabled NULL args", test_enabled_null_args());
    CHECK("enabled invalid severity", test_enabled_invalid_severity());
    CHECK("enabled threshold", test_enabled_threshold());

    /* Trace spans. */
    CHECK("trace begin NULL args", test_trace_begin_null_args());
    CHECK("trace begin/end roundtrip", test_trace_begin_end_roundtrip());
    CHECK("trace nesting", test_trace_nesting());
    CHECK("trace current/depth", test_trace_current_depth());
    CHECK("trace current/depth NULL", test_trace_current_depth_null());
    CHECK("trace end not found", test_trace_end_not_found());
    CHECK("trace end out of order", test_trace_end_out_of_order());
    CHECK("trace NULL sink", test_trace_null_sink());
    CHECK("trace sink failure restores", test_trace_sink_failure_restores());
    CHECK("trace error status", test_trace_error_status());

    /* Diagnostics. */
    CHECK("diag publish NULL args", test_diag_publish_null_args());
    CHECK("diag publish validation", test_diag_publish_validation());
    CHECK("diag publish insert/update", test_diag_publish_insert_update());
    CHECK("diag publish overflow", test_diag_publish_overflow());
    CHECK("diag clear", test_diag_clear());
    CHECK("diag at skips gaps", test_diag_at_skips_gaps());
    CHECK("diag count NULL", test_diag_count_null());
    CHECK("diag count state", test_diag_count_state());
    CHECK("diag format full", test_diag_format_full());
    CHECK("diag format omits null", test_diag_format_omits_null());
    CHECK("diag format errors", test_diag_format_errors());

    /* Connection events. */
    CHECK("conn event NULL args", test_conn_event_null_args());
    CHECK("conn event invalid kind", test_conn_event_invalid_kind());
    CHECK("conn event no sink", test_conn_event_no_sink());
    CHECK("conn event all kinds", test_conn_event_all_kinds());
    CHECK("conn event sink failure", test_conn_event_sink_failure());

#undef CHECK

    if (failures == 0U) {
        (void)printf("test_obs: ok\n");
    }
    return failures == 0U ? 0 : 1;
}

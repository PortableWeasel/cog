/**
 * @file test_19_connection_events.c
 * @brief Scenario 19: connection event stream rows for a GUI table.
 *
 * Uses the real cog-obs connection-event API (cog_obs_conn_event_emit)
 * with a real conn_event_sink, plus the diagnostic table
 * (cog_obs_diag_publish / cog_obs_diag_at) that backs the GUI rows.
 *
 * The test drives one logical upstream connection through its lifecycle:
 * OPEN -> CONNECTED -> PROGRESS (byte deltas + totals) -> STALLED ->
 * RETRY -> CLOSED.  A GUI-style accumulator keyed by conn_id records each
 * row with session / endpoint / model, byte deltas and totals, retry
 * count, and state/status.  Both the event stream and the diagnostic
 * table are asserted.
 */
#define _POSIX_C_SOURCE 200809L

#include <cog/obs.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            (void)fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__,         \
                          __LINE__, #cond);                             \
            ++failures;                                                 \
        }                                                               \
    } while (0)

#define CHECK_EQ_INT(a, b)                                              \
    do {                                                                \
        long long _a = (long long)(a);                                  \
        long long _b = (long long)(b);                                  \
        if (_a != _b) {                                                 \
            (void)fprintf(stderr, "FAIL %s:%d: %s == %s (%lld != %lld)\n", \
                          __FILE__, __LINE__, #a, #b, _a, _b);          \
            ++failures;                                                 \
        }                                                               \
    } while (0)

/* ------------------------------------------------------------------ */
/* GUI-style accumulator: one row per conn_id                          */
/* ------------------------------------------------------------------ */

#define ROW_CAP 8U

typedef struct gui_row {
    uint64_t conn_id;
    const char *session_id;
    const char *endpoint;
    const char *provider;
    const char *model;
    uint64_t bytes_in_total;
    uint64_t bytes_out_total;
    uint32_t retries;
    cog_obs_conn_event_kind_t last_kind;
    cog_result_t status;
    uint16_t http_status;
    int seen;
} gui_row_t;

static gui_row_t g_rows[ROW_CAP];
static size_t g_row_count;

static gui_row_t *row_for(uint64_t conn_id)
{
    size_t i;

    for (i = 0U; i < g_row_count; ++i) {
        if (g_rows[i].conn_id == conn_id) {
            return &g_rows[i];
        }
    }
    if (g_row_count < ROW_CAP) {
        gui_row_t *r = &g_rows[g_row_count];
        (void)memset(r, 0, sizeof(*r));
        r->conn_id = conn_id;
        ++g_row_count;
        return r;
    }
    return NULL;
}

/** Connection event sink: accumulate the event into the GUI table. */
static cog_result_t conn_sink(void *user, const cog_obs_conn_event_t *ev)
{
    gui_row_t *r;
    (void)user;

    r = row_for(ev->conn_id);
    if (r == NULL) {
        return COG_ERROR_OVERFLOW;
    }
    r->seen = 1;
    r->session_id = ev->session_id;
    r->endpoint = ev->endpoint;
    r->provider = ev->provider;
    r->model = ev->model;
    r->bytes_in_total = ev->bytes_in_total;
    r->bytes_out_total = ev->bytes_out_total;
    r->retries = ev->retry_attempt;
    r->last_kind = ev->kind;
    r->status = ev->status;
    r->http_status = ev->http_status;
    return COG_OK;
}

static void test_connection_events(void)
{
    cog_obs_config_t cfg;
    cog_obs_t *obs = NULL;
    cog_obs_conn_event_t ev;
    cog_obs_conn_snapshot_t snap;
    cog_obs_conn_snapshot_t out;
    size_t count;

    (void)memset(&cfg, 0, sizeof(cfg));
    cfg.min_severity = COG_OBS_TRACE;
    cfg.conn_event_sink = conn_sink;
    cfg.diag_capacity = 8U;
    CHECK(cog_obs_new(&obs, &cfg) == COG_OK);
    CHECK(obs != NULL);

    /* OPEN: add row 42. */
    (void)memset(&ev, 0, sizeof(ev));
    ev.kind = COG_OBS_CONN_EVENT_OPEN;
    ev.conn_id = 42U;
    ev.session_id = "sess-abc";
    ev.endpoint = "api.openai.com";
    ev.provider = "openai";
    ev.model = "gpt-4";
    CHECK(cog_obs_conn_event_emit(obs, &ev) == COG_OK);

    /* CONNECTED: row -> ACTIVE. */
    ev.kind = COG_OBS_CONN_EVENT_CONNECTED;
    CHECK(cog_obs_conn_event_emit(obs, &ev) == COG_OK);

    /* PROGRESS: byte deltas + totals. */
    ev.kind = COG_OBS_CONN_EVENT_PROGRESS;
    ev.bytes_in_delta = 512U;
    ev.bytes_out_delta = 1024U;
    ev.bytes_in_total = 512U;
    ev.bytes_out_total = 1024U;
    CHECK(cog_obs_conn_event_emit(obs, &ev) == COG_OK);

    /* STALLED. */
    ev.kind = COG_OBS_CONN_EVENT_STALLED;
    CHECK(cog_obs_conn_event_emit(obs, &ev) == COG_OK);

    /* RETRY: attempt 1. */
    ev.kind = COG_OBS_CONN_EVENT_RETRY;
    ev.retry_attempt = 1U;
    CHECK(cog_obs_conn_event_emit(obs, &ev) == COG_OK);

    /* CLOSED: status OK. */
    ev.kind = COG_OBS_CONN_EVENT_CLOSED;
    ev.status = COG_OK;
    CHECK(cog_obs_conn_event_emit(obs, &ev) == COG_OK);

    /* --- Assert the GUI table accumulated the row. --- */
    CHECK_EQ_INT((long long)g_row_count, 1LL);
    CHECK(g_rows[0].seen);
    CHECK_EQ_INT((long long)g_rows[0].conn_id, 42LL);
    CHECK(strcmp(g_rows[0].session_id, "sess-abc") == 0);
    CHECK(strcmp(g_rows[0].endpoint, "api.openai.com") == 0);
    CHECK(strcmp(g_rows[0].provider, "openai") == 0);
    CHECK(strcmp(g_rows[0].model, "gpt-4") == 0);
    CHECK_EQ_INT((long long)g_rows[0].bytes_in_total, 512LL);
    CHECK_EQ_INT((long long)g_rows[0].bytes_out_total, 1024LL);
    CHECK_EQ_INT((long long)g_rows[0].retries, 1LL);
    CHECK(g_rows[0].last_kind == COG_OBS_CONN_EVENT_CLOSED);
    CHECK(g_rows[0].status == COG_OK);

    /* --- Also publish the same connection to the diagnostic table and
     * assert the state/status row is queryable. --- */
    (void)memset(&snap, 0, sizeof(snap));
    snap.conn_id = 42U;
    snap.state = COG_OBS_CONN_ACTIVE;
    snap.endpoint = "api.openai.com";
    snap.provider = "openai";
    snap.bytes_in = 512U;
    snap.bytes_out = 1024U;
    snap.retries = 1U;
    snap.last_status = COG_OK;
    CHECK(cog_obs_diag_publish(obs, &snap) == COG_OK);

    CHECK(cog_obs_diag_count(obs, &count) == COG_OK);
    CHECK_EQ_INT((long long)count, 1LL);
    CHECK(cog_obs_diag_at(obs, 0U, &out) == COG_OK);
    CHECK_EQ_INT((long long)out.conn_id, 42LL);
    CHECK(out.state == COG_OBS_CONN_ACTIVE);
    CHECK_EQ_INT((long long)out.bytes_in, 512LL);
    CHECK_EQ_INT((long long)out.bytes_out, 1024LL);
    CHECK_EQ_INT((long long)out.retries, 1LL);
    CHECK(out.last_status == COG_OK);

    cog_obs_free(obs);
}

int main(void)
{
    test_connection_events();

    if (failures != 0) {
        (void)fprintf(stderr, "test_19_connection_events: %d failure(s)\n",
                      failures);
        return 1;
    }
    (void)printf("test_19_connection_events: all checks passed\n");
    return 0;
}
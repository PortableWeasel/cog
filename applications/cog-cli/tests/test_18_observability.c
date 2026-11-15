/**
 * @file test_18_observability.c
 * @brief Scenario 18: logs and trace spans capture provider/tool timing
 *        and session context through real sinks.
 *
 * Uses the real cog-obs API (cog_obs_new / cog_obs_log / cog_obs_trace_begin
 * / cog_obs_trace_end) with a real buffer sink and a real trace sink.
 * The test emits:
 *
 *  - structured log records for a provider call and a tool call, each
 *    carrying timing fields (duration_ms, ttft_ms) and session context
 *    (session_id, provider, model, tool);
 *  - nested trace spans bracketing the provider call and the tool call,
 *    with a parent/child relationship and a status.
 *
 * The buffer sink receives the formatted records; the trace sink receives
 * the completed span records.  Both are asserted so the observability
 * layer demonstrably captures timing and session context end-to-end.
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
/* Sink state                                                          */
/* ------------------------------------------------------------------ */

#define SINK_CAP 8192U
#define SPAN_CAP 16U

static char g_log_buf[SINK_CAP];
static size_t g_log_len;
static cog_obs_span_record_t g_spans[SPAN_CAP];
static size_t g_span_count;

/** Buffer sink: append the formatted record to g_log_buf. */
static cog_result_t buffer_sink(void *user, const char *record,
                                size_t record_len)
{
    (void)user;
    if (g_log_len + record_len + 1U > SINK_CAP) {
        return COG_ERROR_OVERFLOW;
    }
    (void)memcpy(g_log_buf + g_log_len, record, record_len);
    g_log_len += record_len;
    g_log_buf[g_log_len] = '\0';
    return COG_OK;
}

/** Trace sink: record the completed span. */
static cog_result_t trace_sink(void *user, const cog_obs_span_record_t *span)
{
    (void)user;
    if (g_span_count < SPAN_CAP) {
        g_spans[g_span_count] = *span;
        ++g_span_count;
    }
    return COG_OK;
}

/* ------------------------------------------------------------------ */
/* Field table                                                         */
/* ------------------------------------------------------------------ */

static const cog_obs_field_desc_t field_desc[] = {
    {"session_id", COG_OBS_FIELD_STRING},
    {"provider", COG_OBS_FIELD_STRING},
    {"model", COG_OBS_FIELD_STRING},
    {"tool", COG_OBS_FIELD_STRING},
    {"duration_ms", COG_OBS_FIELD_UINT},
    {"ttft_ms", COG_OBS_FIELD_UINT},
    {"tokens", COG_OBS_FIELD_UINT},
};

static const cog_obs_field_table_t field_table = {
    field_desc,
    sizeof(field_desc) / sizeof(field_desc[0]),
};

enum {
    F_SESSION = 0,
    F_PROVIDER,
    F_MODEL,
    F_TOOL,
    F_DURATION,
    F_TTFT,
    F_TOKENS,
};

static void test_observability(void)
{
    static const cog_obs_sink_t sink = {
        "buffer", buffer_sink, NULL, COG_OBS_TRACE,
    };
    static const cog_obs_sink_table_t sink_table = {
        &sink, 1U,
    };
    cog_obs_config_t cfg;
    cog_obs_t *obs = NULL;
    cog_obs_span_id_t provider_span;
    cog_obs_span_id_t tool_span;
    cog_obs_field_t fields[4];
    size_t depth;

    (void)memset(&cfg, 0, sizeof(cfg));
    cfg.min_severity = COG_OBS_TRACE;
    cfg.fields = &field_table;
    cfg.sinks = &sink_table;
    cfg.trace_sink = trace_sink;
    cfg.service = "cog-test";
    CHECK(cog_obs_new(&obs, &cfg) == COG_OK);
    CHECK(obs != NULL);

    /* --- Provider call: log timing + session context, bracketed by a
     * trace span.  The tool call is nested inside the provider span so
     * the trace tree shows a parent/child relationship. --- */
    CHECK(cog_obs_trace_begin(obs, "provider.call", &provider_span) == COG_OK);
    fields[0].index = F_SESSION;
    fields[0].kind = COG_OBS_FIELD_STRING;
    fields[0].value.string = "sess-abc";
    fields[1].index = F_PROVIDER;
    fields[1].kind = COG_OBS_FIELD_STRING;
    fields[1].value.string = "mock";
    fields[2].index = F_MODEL;
    fields[2].kind = COG_OBS_FIELD_STRING;
    fields[2].value.string = "mock-model";
    fields[3].index = F_DURATION;
    fields[3].kind = COG_OBS_FIELD_UINT;
    fields[3].value.unsigned_integer = 42U;
    CHECK(cog_obs_log(obs, COG_OBS_INFO, "provider.call",
                      fields, 4U) == COG_OK);

    /* --- Tool call: log timing + session context, nested under the
     * provider span. --- */
    CHECK(cog_obs_trace_begin(obs, "tool.exec", &tool_span) == COG_OK);
    fields[0].index = F_SESSION;
    fields[0].kind = COG_OBS_FIELD_STRING;
    fields[0].value.string = "sess-abc";
    fields[1].index = F_TOOL;
    fields[1].kind = COG_OBS_FIELD_STRING;
    fields[1].value.string = "read_file";
    fields[2].index = F_DURATION;
    fields[2].kind = COG_OBS_FIELD_UINT;
    fields[2].value.unsigned_integer = 7U;
    fields[3].index = F_TOKENS;
    fields[3].kind = COG_OBS_FIELD_UINT;
    fields[3].value.unsigned_integer = 128U;
    CHECK(cog_obs_log(obs, COG_OBS_DEBUG, "tool.exec",
                      fields, 4U) == COG_OK);
    /* Close the tool span first (LIFO), then the provider span. */
    CHECK(cog_obs_trace_end(obs, tool_span, COG_OK) == COG_OK);
    CHECK(cog_obs_trace_end(obs, provider_span, COG_OK) == COG_OK);

    /* Span stack is empty again. */
    CHECK(cog_obs_trace_depth(obs, &depth) == COG_OK);
    CHECK_EQ_INT((long long)depth, 0LL);

    /* --- Assert the buffer sink captured the records with timing and
     * session context. --- */
    CHECK(g_log_len > 0U);
    CHECK(strstr(g_log_buf, "provider.call") != NULL);
    CHECK(strstr(g_log_buf, "tool.exec") != NULL);
    CHECK(strstr(g_log_buf, "sess-abc") != NULL);
    CHECK(strstr(g_log_buf, "mock-model") != NULL);
    CHECK(strstr(g_log_buf, "read_file") != NULL);
    CHECK(strstr(g_log_buf, "duration_ms=42") != NULL);
    CHECK(strstr(g_log_buf, "duration_ms=7") != NULL);

    /* --- Assert the trace sink captured both spans with timing and
     * parent/child relationship.  tool.exec is closed first (LIFO), so
     * it is g_spans[0]; provider.call is g_spans[1]. --- */
    CHECK_EQ_INT((long long)g_span_count, 2LL);
    CHECK(strcmp(g_spans[0].name, "tool.exec") == 0);
    CHECK(g_spans[0].ok);
    CHECK(g_spans[0].status == COG_OK);
    CHECK(g_spans[0].duration_ns > 0U);
    CHECK(strcmp(g_spans[1].name, "provider.call") == 0);
    CHECK(g_spans[1].ok);
    CHECK(g_spans[1].status == COG_OK);
    CHECK(g_spans[1].parent == COG_OBS_SPAN_ID_NONE); /* root span */
    CHECK(g_spans[0].parent == g_spans[1].id); /* nested under provider */

    cog_obs_free(obs);
}

int main(void)
{
    test_observability();

    if (failures != 0) {
        (void)fprintf(stderr, "test_18_observability: %d failure(s)\n",
                      failures);
        return 1;
    }
    (void)printf("test_18_observability: all checks passed\n");
    return 0;
}

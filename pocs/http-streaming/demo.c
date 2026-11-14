/**
 * @file demo.c
 * @brief Demo harness for the HTTP streaming PoC.
 *
 * Exercises all four acceptance criteria of ticket 34df374 against the mock
 * server, with either transport (or both):
 *
 *   --scenario stream  stream SSE chunks end to end (streaming proof)
 *   --scenario stall   prove stall detection (silence > threshold)
 *   --scenario flaky   prove retry on a transient 503
 *
 * Exit status is 0 only when every run client met its scenario's
 * expectation, so the CTest wrapper can rely on it.
 */

#define _POSIX_C_SOURCE 200809L

#include "conn.h"
#include "http_common.h"
#include "sse.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if COG_POC_HAVE_CURL
#include <curl/curl.h>
#endif

/** Default host the mock server listens on. */
#define DEFAULT_HOST "127.0.0.1"

/** Default mock server port. */
#define DEFAULT_PORT 18765U

/** Default stall threshold in milliseconds. */
#define DEFAULT_STALL_MS 700U

/** Default retry budget. */
#define DEFAULT_MAX_RETRIES 3U

/** Overall budget per request in milliseconds. */
#define OVERALL_TIMEOUT_MS 15000U

/** SSE parser event cap (mock events are tiny). */
#define SSE_MAX_EVENT 4096U

/* ------------------------------------------------------------------ */
/* Scenario table                                                      */
/* ------------------------------------------------------------------ */

struct scenario {
    const char *name;   /**< Scenario identifier (matches --scenario). */
    const char *path;   /**< URL path to request. */
    size_t min_events;  /**< Minimum events for the stream scenario. */
};

static const struct scenario k_scenarios[] = {
    {"stream", "/stream", 5U},
    {"stall", "/stall?events=3&stall=2000&delay=100", 0U},
    {"flaky", "/flaky", 0U},
};

#define SCENARIO_COUNT (sizeof k_scenarios / sizeof k_scenarios[0])

/* ------------------------------------------------------------------ */
/* Demo state and callbacks                                            */
/* ------------------------------------------------------------------ */

/** Per-run state shared with the transport callbacks. */
struct demo_ctx {
    const char *label;       /**< "hand" or "curl", for output prefixes. */
    sse_parser_t *parser;   /**< Decodes SSE events from chunk bytes. */
    conn_tracker_t *tracker; /**< Tracks this run. */
    size_t events;           /**< SSE events seen so far. */
};

/**
 * @brief SSE event callback: count and print each complete event.
 */
static void demo_on_event(const char *data, size_t len, void *ud)
{
    struct demo_ctx *dc = ud;
    dc->events++;
    (void)printf("[%s] +event %zu (%zu bytes): %.*s\n", dc->label,
                 dc->events, len, (int)len, data);
}

/**
 * @brief Chunk callback: feed raw body bytes into the SSE parser.
 */
static void demo_on_chunk(const uint8_t *data, size_t len, void *ud)
{
    struct demo_ctx *dc = ud;
    sse_parser_feed(dc->parser, data, len, demo_on_event, dc);
}

/**
 * @brief Live status callback: print the connection-tracker snapshot.
 */
static void demo_on_status(const conn_tracker_t *t, void *ud)
{
    struct demo_ctx *dc = ud;
    char snap[160];
    conn_tracker_snapshot(t, snap, sizeof snap);
    (void)printf("[%s] status: %s\n", dc->label, snap);
}

/* ------------------------------------------------------------------ */
/* Argument parsing                                                    */
/* ------------------------------------------------------------------ */

static const char *g_client = "both";      /**< hand | curl | both. */
static const char *g_host = DEFAULT_HOST;
static unsigned g_port = DEFAULT_PORT;
static uint64_t g_stall_ms = DEFAULT_STALL_MS;
static unsigned g_max_retries = DEFAULT_MAX_RETRIES;
static const struct scenario *g_scenario = &k_scenarios[0];

/**
 * @brief Parse command-line options; exits 2 on usage errors.
 */
static void parse_args(int argc, char **argv)
{
    int i;
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--client") == 0 && i + 1 < argc) {
            g_client = argv[++i];
            if (strcmp(g_client, "hand") != 0 &&
                strcmp(g_client, "curl") != 0 &&
                strcmp(g_client, "both") != 0) {
                (void)fprintf(stderr, "--client must be hand|curl|both\n");
                exit(2);
            }
        } else if (strcmp(argv[i], "--host") == 0 && i + 1 < argc) {
            g_host = argv[++i];
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            char *end = NULL;
            long v = strtol(argv[i + 1], &end, 10);
            if (end == argv[i + 1] || v <= 0L || v > 65535L) {
                (void)fprintf(stderr, "invalid --port\n");
                exit(2);
            }
            g_port = (unsigned)v;
            i++;
        } else if (strcmp(argv[i], "--stall-ms") == 0 && i + 1 < argc) {
            char *end = NULL;
            unsigned long long v = strtoull(argv[i + 1], &end, 10);
            if (end == argv[i + 1] || v > 60000ULL) {
                (void)fprintf(stderr, "invalid --stall-ms\n");
                exit(2);
            }
            g_stall_ms = (uint64_t)v;
            i++;
        } else if (strcmp(argv[i], "--max-retries") == 0 && i + 1 < argc) {
            char *end = NULL;
            unsigned long v = strtoul(argv[i + 1], &end, 10);
            if (end == argv[i + 1] || v > 100UL) {
                (void)fprintf(stderr, "invalid --max-retries\n");
                exit(2);
            }
            g_max_retries = (unsigned)v;
            i++;
        } else if (strcmp(argv[i], "--scenario") == 0 && i + 1 < argc) {
            const char *name = argv[++i];
            size_t s;
            bool found = false;
            for (s = 0U; s < SCENARIO_COUNT; s++) {
                if (strcmp(k_scenarios[s].name, name) == 0) {
                    g_scenario = &k_scenarios[s];
                    found = true;
                    break;
                }
            }
            if (!found) {
                (void)fprintf(stderr, "unknown --scenario %s\n", name);
                exit(2);
            }
        } else {
            (void)fprintf(stderr,
                          "usage: %s [--client hand|curl|both] "
                          "[--scenario stream|stall|flaky]\n"
                          "       [--host H] [--port P] [--stall-ms N] "
                          "[--max-retries N]\n",
                          argv[0]);
            exit(2);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Per-client run                                                      */
/* ------------------------------------------------------------------ */

/**
 * @brief Run one transport against one scenario.
 *
 * @param[in] label    Output prefix ("hand" / "curl").
 * @param[in] use_curl Select libcurl transport.
 * @param[in] url      Full URL.
 * @return 0 when the scenario expectation was met, 1 otherwise.
 */
static int run_client(const char *label, bool use_curl, const char *url)
{
    struct demo_ctx dc;
    (void)memset(&dc, 0, sizeof dc);
    dc.label = label;
    dc.parser = sse_parser_create(SSE_MAX_EVENT);
    dc.tracker = conn_tracker_create(1U);
    if (dc.parser == NULL || dc.tracker == NULL) {
        (void)printf("[%s] allocation failure\n", label);
        sse_parser_destroy(dc.parser);
        conn_tracker_destroy(dc.tracker);
        return 1;
    }

    http_client_cfg_t cfg;
    cfg.stall_ms = g_stall_ms;
    cfg.max_retries = g_max_retries;
    cfg.overall_timeout_ms = OVERALL_TIMEOUT_MS;

    http_request_t req;
    (void)memset(&req, 0, sizeof req);
    req.url = url;
    req.cfg = &cfg;
    req.tracker = dc.tracker;
    req.on_chunk = demo_on_chunk;
    req.on_status = demo_on_status;
    req.user_data = &dc;

    (void)printf("[demo] client=%s scenario=%s url=%s stall_ms=%llu "
                 "max_retries=%u\n",
                 label, g_scenario->name, url,
                 (unsigned long long)g_stall_ms, g_max_retries);

    http_result_t res;
    if (use_curl) {
        res = curl_request(&req); /* requires COG_POC_HAVE_CURL */
    } else {
        res = hand_request(&req);
    }
    sse_parser_finish(dc.parser, demo_on_event, &dc);

    char snap[160];
    conn_tracker_snapshot(dc.tracker, snap, sizeof snap);
    (void)printf("[%s] RESULT err=%s status=%d retries=%u bytes=%zu "
                 "chunks=%zu state=%s | %s\n",
                 label, http_err_name(res.err), res.status, res.retries,
                 res.bytes, res.chunks, conn_state_name(res.state), snap);

    bool pass;
    if (strcmp(g_scenario->name, "stall") == 0) {
        pass = (res.state == CONN_STATE_STALLED && res.err == HTTP_ERR_STALL);
    } else if (strcmp(g_scenario->name, "flaky") == 0) {
        pass = (res.retries >= 1U && res.state == CONN_STATE_DONE &&
                res.err == HTTP_ERR_NONE);
    } else {
        pass = (res.state == CONN_STATE_DONE && res.err == HTTP_ERR_NONE &&
                dc.events >= g_scenario->min_events);
    }
    (void)printf("[%s] scenario %s: %s\n", label, g_scenario->name,
                 pass ? "PASS" : "FAIL");

    sse_parser_destroy(dc.parser);
    conn_tracker_destroy(dc.tracker);
    return pass ? 0 : 1;
}

int main(int argc, char **argv)
{
    parse_args(argc, argv);

    char url[512];
    int n = snprintf(url, sizeof url, "http://%s:%u%s", g_host,
                     (unsigned)g_port, g_scenario->path);
    if (n < 0 || (size_t)n >= sizeof url) {
        (void)fprintf(stderr, "URL too long\n");
        return 1;
    }

    int rc = 0;
    bool deliver_curl = (strcmp(g_client, "curl") == 0 ||
                         strcmp(g_client, "both") == 0);

#if COG_POC_HAVE_CURL
    CURLcode grc = curl_global_init(CURL_GLOBAL_DEFAULT);
    if (grc != CURLE_OK) {
        (void)fprintf(stderr, "curl_global_init failed: %d\n", (int)grc);
        return 1;
    }
#else
    if (deliver_curl) {
        (void)printf("[demo] libcurl not available; skipping curl client\n");
    }
#endif

    if (strcmp(g_client, "hand") == 0 || strcmp(g_client, "both") == 0) {
        rc |= run_client("hand", false, url);
    }
#if COG_POC_HAVE_CURL
    if (deliver_curl) {
        rc |= run_client("curl", true, url);
    }
#endif

#if COG_POC_HAVE_CURL
    curl_global_cleanup();
#endif
    return rc;
}

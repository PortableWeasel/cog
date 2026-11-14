/**
 * @file unit-test.c
 * @brief Unit tests for the transport-independent layers of the HTTP
 *        streaming PoC: conn.c, sse.c, http_common.c.
 *
 * The demo (demo.c) proves the end-to-end scenarios; this test hammers
 * the pure-logic modules at their boundaries: NULL handling, empty
 * inputs, truncation, state transitions, and URL edge cases. It needs no
 * network and no server.
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

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        g_checks++;                                                          \
        if (!(cond)) {                                                       \
            g_failures++;                                                    \
            (void)fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,    \
                          #cond);                                            \
        }                                                                    \
    } while (0)

/* ------------------------------------------------------------------ */
/* conn.c                                                              */
/* ------------------------------------------------------------------ */

static void test_conn_lifecycle(void)
{
    conn_tracker_t *t = conn_tracker_create(42U);
    CHECK(t != NULL);
    CHECK(conn_tracker_conn_id(t) == 42U);
    CHECK(conn_tracker_state(t) == CONN_STATE_CONNECTING);
    CHECK(conn_tracker_bytes(t) == 0U);
    CHECK(conn_tracker_chunks(t) == 0U);

    /* A chunk promotes CONNECTING -> STREAMING and updates counters. */
    conn_tracker_note_chunk(t, 10U);
    CHECK(conn_tracker_state(t) == CONN_STATE_STREAMING);
    CHECK(conn_tracker_bytes(t) == 10U);
    CHECK(conn_tracker_chunks(t) == 1U);
    conn_tracker_note_chunk(t, 5U);
    CHECK(conn_tracker_bytes(t) == 15U);
    CHECK(conn_tracker_chunks(t) == 2U);

    /* Stall detection: threshold 0 with any elapsed time stalls. */
    uint64_t now = conn_tracker_now_ms();
    CHECK(conn_tracker_check_stall(t, now + 1U, 0U) == true);
    CHECK(conn_tracker_state(t) == CONN_STATE_STALLED);
    /* Already-stalled connections never re-report. */
    CHECK(conn_tracker_check_stall(t, now + 1000U, 0U) == false);

    /* A chunk after a stall resumes streaming. */
    conn_tracker_note_chunk(t, 1U);
    CHECK(conn_tracker_state(t) == CONN_STATE_STREAMING);

    /* DONE is terminal: chunks do not promote it. */
    conn_tracker_set_state(t, CONN_STATE_DONE);
    conn_tracker_note_chunk(t, 7U);
    CHECK(conn_tracker_state(t) == CONN_STATE_DONE);
    CHECK(conn_tracker_check_stall(t, now + 5000U, 0U) == false);

    /* Reset clears counters and returns to CONNECTING. */
    conn_tracker_reset(t);
    CHECK(conn_tracker_state(t) == CONN_STATE_CONNECTING);
    CHECK(conn_tracker_bytes(t) == 0U);
    CHECK(conn_tracker_chunks(t) == 0U);
    CHECK(conn_tracker_conn_id(t) == 42U); /* ID survives a reset */

    conn_tracker_destroy(t);
}

static void test_conn_null_and_bounds(void)
{
    conn_tracker_destroy(NULL); /* must not crash */
    conn_tracker_note_chunk(NULL, 5U);
    conn_tracker_set_state(NULL, CONN_STATE_DONE);
    conn_tracker_reset(NULL);
    CHECK(conn_tracker_check_stall(NULL, 0U, 0U) == false);
    CHECK(conn_tracker_age_ms(NULL, 100U) == 0U);
    CHECK(conn_tracker_last_chunk_age_ms(NULL, 100U) == 0U);
    CHECK(conn_tracker_conn_id(NULL) == 0U);
    CHECK(conn_tracker_bytes(NULL) == 0U);
    CHECK(conn_tracker_chunks(NULL) == 0U);
    CHECK(conn_tracker_state(NULL) == CONN_STATE_IDLE);

    /* Age helpers saturate rather than underflow. */
    conn_tracker_t *t = conn_tracker_create(1U);
    CHECK(t != NULL);
    uint64_t start = conn_tracker_start_ms(t);
    CHECK(conn_tracker_age_ms(t, start) == 0U);
    CHECK(conn_tracker_age_ms(t, start - 1000U) == 0U); /* clock skew */
    CHECK(conn_tracker_last_chunk_age_ms(t, start - 1U) == 0U);
    CHECK(conn_tracker_age_ms(t, start + 250U) == 250U);

    /* Snapshot: NULL buffer / zero cap are no-ops; otherwise the output
     * is always NUL-terminated and contains the key fields. */
    char buf[160];
    conn_tracker_snapshot(t, NULL, sizeof buf);
    conn_tracker_snapshot(t, buf, 0U);
    conn_tracker_snapshot(NULL, buf, sizeof buf);
    conn_tracker_snapshot(t, buf, sizeof buf);
    CHECK(strstr(buf, "id=1") != NULL);
    CHECK(strstr(buf, "state=connecting") != NULL);
    CHECK(strstr(buf, "bytes=0") != NULL);
    CHECK(strstr(buf, "chunks=0") != NULL);

    /* Tiny buffer: must truncate but still NUL-terminate. */
    char tiny[8];
    conn_tracker_snapshot(t, tiny, sizeof tiny);
    CHECK(tiny[sizeof tiny - 1U] == '\0');

    /* State names: every enum value has a name; garbage maps to
     * "unknown". */
    CHECK(strcmp(conn_state_name(CONN_STATE_IDLE), "idle") == 0);
    CHECK(strcmp(conn_state_name(CONN_STATE_CONNECTING), "connecting") == 0);
    CHECK(strcmp(conn_state_name(CONN_STATE_STREAMING), "streaming") == 0);
    CHECK(strcmp(conn_state_name(CONN_STATE_STALLED), "stalled") == 0);
    CHECK(strcmp(conn_state_name(CONN_STATE_DONE), "done") == 0);
    CHECK(strcmp(conn_state_name(CONN_STATE_ERROR), "error") == 0);
    CHECK(strcmp(conn_state_name((conn_state_t)99), "unknown") == 0);

    conn_tracker_destroy(t);
}

/* ------------------------------------------------------------------ */
/* sse.c                                                               */
/* ------------------------------------------------------------------ */

struct sse_sink {
    char events[8][64];
    size_t lens[8];
    size_t count;
};

static void sse_collect(const char *data, size_t len, void *ud)
{
    struct sse_sink *s = ud;
    if (s->count < 8U) {
        size_t n = len < 63U ? len : 63U;
        (void)memcpy(s->events[s->count], data, n);
        s->events[s->count][n] = '\0';
        s->lens[s->count] = len;
        s->count++;
    }
}

static void test_sse_basic(void)
{
    sse_parser_t *p = sse_parser_create(0U); /* default cap */
    CHECK(p != NULL);
    struct sse_sink s;
    (void)memset(&s, 0, sizeof s);

    static const char wire[] = "data: hello\n\n"
                               "data: world\n\n";
    sse_parser_feed(p, (const uint8_t *)wire, sizeof wire - 1U,
                    sse_collect, &s);
    CHECK(s.count == 2U);
    CHECK(strcmp(s.events[0], "hello") == 0);
    CHECK(s.lens[0] == 5U);
    CHECK(strcmp(s.events[1], "world") == 0);

    sse_parser_destroy(p);
}

static void test_sse_split_feeds(void)
{
    sse_parser_t *p = sse_parser_create(0U);
    CHECK(p != NULL);
    struct sse_sink s;
    (void)memset(&s, 0, sizeof s);

    /* Feed one byte at a time: the parser must reassemble the event. */
    static const char wire[] = "data: split\n\n";
    size_t i;
    for (i = 0U; i < sizeof wire - 1U; i++) {
        sse_parser_feed(p, (const uint8_t *)&wire[i], 1U, sse_collect, &s);
    }
    CHECK(s.count == 1U);
    CHECK(strcmp(s.events[0], "split") == 0);

    /* A second event split mid-line across two feeds. */
    sse_parser_feed(p, (const uint8_t *)"data: par", 9U, sse_collect, &s);
    sse_parser_feed(p, (const uint8_t *)"t2\n\n", 4U, sse_collect, &s);
    CHECK(s.count == 2U);
    CHECK(strcmp(s.events[1], "part2") == 0);

    sse_parser_destroy(p);
}

static void test_sse_crlf_and_ignored_lines(void)
{
    sse_parser_t *p = sse_parser_create(0U);
    CHECK(p != NULL);
    struct sse_sink s;
    (void)memset(&s, 0, sizeof s);

    /* CRLF line endings. */
    static const char crlf[] = "data: crlf\r\n\r\n";
    sse_parser_feed(p, (const uint8_t *)crlf, sizeof crlf - 1U,
                    sse_collect, &s);
    CHECK(s.count == 1U);
    CHECK(strcmp(s.events[0], "crlf") == 0);

    /* Comments and non-data fields are ignored. */
    static const char mixed[] = ": keepalive\n"
                                "event: custom\n"
                                "id: 7\n"
                                "retry: 1000\n"
                                "data: payload\n\n";
    sse_parser_feed(p, (const uint8_t *)mixed, sizeof mixed - 1U,
                    sse_collect, &s);
    CHECK(s.count == 2U);
    CHECK(strcmp(s.events[1], "payload") == 0);

    /* Multiple data: lines join into one event (no separator). */
    static const char multi[] = "data: a\ndata: b\n\n";
    sse_parser_feed(p, (const uint8_t *)multi, sizeof multi - 1U,
                    sse_collect, &s);
    CHECK(s.count == 3U);
    CHECK(strcmp(s.events[2], "ab") == 0);

    /* Empty data value: the parser suppresses events with no payload
     * (sse_emit only fires when event_len > 0). This is a documented
     * deviation from RFC 9126, which would emit an empty event. */
    static const char empty[] = "data:\n\n";
    sse_parser_feed(p, (const uint8_t *)empty, sizeof empty - 1U,
                    sse_collect, &s);
    CHECK(s.count == 3U); /* no new event for an empty payload */

    /* Bare "data" with no colon is treated as an empty value too. */
    static const char bare[] = "data\n\n";
    sse_parser_feed(p, (const uint8_t *)bare, sizeof bare - 1U,
                    sse_collect, &s);
    CHECK(s.count == 3U); /* still suppressed */

    sse_parser_destroy(p);
}

static void test_sse_finish_and_truncation(void)
{
    sse_parser_t *p = sse_parser_create(0U);
    CHECK(p != NULL);
    struct sse_sink s;
    (void)memset(&s, 0, sizeof s);

    /* Body ends without a blank line: finish() emits the pending event. */
    sse_parser_feed(p, (const uint8_t *)"data: tail", 10U, sse_collect, &s);
    CHECK(s.count == 0U);
    sse_parser_finish(p, sse_collect, &s);
    CHECK(s.count == 1U);
    CHECK(strcmp(s.events[0], "tail") == 0);

    /* finish() with nothing pending emits nothing. */
    sse_parser_finish(p, sse_collect, &s);
    CHECK(s.count == 1U);

    /* NULL data feed is a no-op. */
    sse_parser_feed(p, NULL, 5U, sse_collect, &s);
    sse_parser_feed(NULL, (const uint8_t *)"x", 1U, sse_collect, &s);
    CHECK(s.count == 1U);

    sse_parser_destroy(p);

    /* Truncation: the line buffer is capped at max_event, so a single
     * data line's payload is truncated to max_event minus the "data: "
     * prefix (6 bytes). The event is always a prefix of the input and
     * never exceeds max_event. */
    sse_parser_t *q = sse_parser_create(8U);
    CHECK(q != NULL);
    (void)memset(&s, 0, sizeof s);
    static const char big[] = "data: 1234567890\n\n";
    sse_parser_feed(q, (const uint8_t *)big, sizeof big - 1U,
                    sse_collect, &s);
    CHECK(s.count == 1U);
    CHECK(s.lens[0] <= 8U);
    CHECK(strncmp(s.events[0], "1234567890", s.lens[0]) == 0);

    /* A single line longer than the cap is dropped without overflow. */
    (void)memset(&s, 0, sizeof s);
    char longline[64];
    (void)snprintf(longline, sizeof longline, "data: %s\n\n",
                   "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx");
    sse_parser_feed(q, (const uint8_t *)longline, strlen(longline),
                    sse_collect, &s);
    CHECK(s.count == 1U);
    CHECK(s.lens[0] <= 8U);

    sse_parser_destroy(q);
}

/* ------------------------------------------------------------------ */
/* http_common.c                                                      */
/* ------------------------------------------------------------------ */

static void test_split_url(void)
{
    char host[64];
    uint16_t port = 0U;
    const char *path = NULL;

    CHECK(http_split_url("http://example.com/path", host, sizeof host,
                         &port, &path) == 0);
    CHECK(strcmp(host, "example.com") == 0);
    CHECK(port == 80U);
    CHECK(strcmp(path, "/path") == 0);

    CHECK(http_split_url("http://example.com:8080/x", host, sizeof host,
                         &port, &path) == 0);
    CHECK(strcmp(host, "example.com") == 0);
    CHECK(port == 8080U);
    CHECK(strcmp(path, "/x") == 0);

    /* No path -> "/". */
    CHECK(http_split_url("http://example.com", host, sizeof host, &port,
                         &path) == 0);
    CHECK(strcmp(path, "/") == 0);

    /* No port -> 80. */
    CHECK(http_split_url("http://example.com/", host, sizeof host, &port,
                         &path) == 0);
    CHECK(port == 80U);
    CHECK(strcmp(path, "/") == 0);

    /* NULL / invalid arguments. */
    CHECK(http_split_url(NULL, host, sizeof host, &port, &path) == -1);
    CHECK(http_split_url("http://example.com/", NULL, sizeof host, &port,
                         &path) == -1);
    CHECK(http_split_url("http://example.com/", host, 0U, &port, &path) ==
          -1);
    CHECK(http_split_url("http://example.com/", host, sizeof host, NULL,
                         &path) == -1);
    CHECK(http_split_url("http://example.com/", host, sizeof host, &port,
                         NULL) == -1);

    /* Wrong scheme / empty host / oversized host. */
    CHECK(http_split_url("https://example.com/", host, sizeof host, &port,
                         &path) == -1);
    CHECK(http_split_url("ftp://example.com/", host, sizeof host, &port,
                         &path) == -1);
    CHECK(http_split_url("http:///path", host, sizeof host, &port, &path) ==
          -1);

    char small[4];
    CHECK(http_split_url("http://toolonghost.example/", small, sizeof small,
                         &port, &path) == -1);

    /* Bad ports. */
    CHECK(http_split_url("http://h:0/", host, sizeof host, &port, &path) ==
          -1);
    CHECK(http_split_url("http://h:70000/", host, sizeof host, &port,
                         &path) == -1);
    CHECK(http_split_url("http://h:abc/", host, sizeof host, &port, &path) ==
          -1);
    CHECK(http_split_url("http://h:-1/", host, sizeof host, &port, &path) ==
          -1);
}

static void test_err_names(void)
{
    CHECK(strcmp(http_err_name(HTTP_ERR_NONE), "none") == 0);
    CHECK(strcmp(http_err_name(HTTP_ERR_RETRYABLE), "retryable") == 0);
    CHECK(strcmp(http_err_name(HTTP_ERR_CLIENT), "client") == 0);
    CHECK(strcmp(http_err_name(HTTP_ERR_STALL), "stall") == 0);
    CHECK(strcmp(http_err_name(HTTP_ERR_NET), "net") == 0);
    CHECK(strcmp(http_err_name(HTTP_ERR_PARSE), "parse") == 0);
    CHECK(strcmp(http_err_name(HTTP_ERR_TIMEOUT), "timeout") == 0);
    CHECK(strcmp(http_err_name((http_err_t)99), "unknown") == 0);
}

int main(void)
{
    test_conn_lifecycle();
    test_conn_null_and_bounds();
    test_sse_basic();
    test_sse_split_feeds();
    test_sse_crlf_and_ignored_lines();
    test_sse_finish_and_truncation();
    test_split_url();
    test_err_names();

    (void)printf("unit-test: %d checks, %d failures\n", g_checks,
                 g_failures);
    return g_failures == 0 ? 0 : 1;
}

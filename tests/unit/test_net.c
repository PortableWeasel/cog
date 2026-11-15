/**
 * @file test_net.c
 * @brief Contract test for cog/net.h (libraries/cog-net).
 *
 * Drives the public cog-net API against a local loopback test server
 * (fork-per-connection, no external network dependency).  Covers:
 *
 *   - lifecycle and request ids
 *   - request/stream completion and response headers/status
 *   - fragmented headers and chunks (partial socket I/O)
 *   - cancellation and bulk active-id queries
 *   - stall detection and recovery (retry/backoff)
 *   - 429 rate-limit metadata (Retry-After)
 *   - malformed responses
 *   - error/ownership semantics (NULL args, overflow, cleanup)
 *
 * Ticket: 92e883f (Stage 12.3d — Implement net.c)
 */

#define _POSIX_C_SOURCE 200809L

#include "cog/net.h"

#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

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
/* Time helper                                                         */
/* ------------------------------------------------------------------ */

static uint64_t now_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0U;
    }
    return (uint64_t)ts.tv_sec * 1000U + (uint64_t)ts.tv_nsec / 1000000U;
}

static void sleep_ms(long ms)
{
    if (ms <= 0L) {
        return;
    }
    struct timespec ts;
    ts.tv_sec = ms / 1000L;
    ts.tv_nsec = (ms % 1000L) * 1000000L;
    (void)nanosleep(&ts, NULL);
}

/* ------------------------------------------------------------------ */
/* Loopback test server                                                */
/* ------------------------------------------------------------------ */

#define REQUEST_CAP 4096U
#define PATH_CAP 512U
#define LISTEN_BACKLOG 16

/** Per-path request counters for /flaky and /stallonce (mmap'd). */
typedef struct server_counters {
    long flaky;
    long stallonce;
} server_counters_t;

static server_counters_t *g_counters = NULL;

static int write_all(int fd, const void *buf, size_t len)
{
    const uint8_t *p = buf;
    size_t off = 0U;
    while (off < len) {
        ssize_t w = send(fd, p + off, len - off, 0);
        if (w < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        off += (size_t)w;
    }
    return 0;
}

static int send_simple(int fd, int status, const char *reason)
{
    char head[256];
    int n = snprintf(head, sizeof head,
                     "HTTP/1.1 %d %s\r\n"
                     "Content-Length: 0\r\n"
                     "Connection: close\r\n"
                     "\r\n",
                     status, reason);
    if (n < 0 || (size_t)n >= sizeof head) {
        return -1;
    }
    return write_all(fd, head, (size_t)n);
}

static int send_stream_head(int fd)
{
    static const char head[] =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/event-stream\r\n"
        "Transfer-Encoding: chunked\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: close\r\n"
        "\r\n";
    return write_all(fd, head, sizeof head - 1U);
}

static int write_chunk(int fd, const char *payload)
{
    size_t plen = strlen(payload);
    char hdr[32];
    int n = snprintf(hdr, sizeof hdr, "%zx\r\n", plen);
    if (n < 0 || (size_t)n >= sizeof hdr) {
        return -1;
    }
    if (write_all(fd, hdr, (size_t)n) != 0) {
        return -1;
    }
    if (write_all(fd, payload, plen) != 0) {
        return -1;
    }
    return write_all(fd, "\r\n", 2U);
}

static int write_last_chunk(int fd)
{
    return write_all(fd, "0\r\n\r\n", 5U);
}

static int send_event(int fd, long idx)
{
    char ev[128];
    int n = snprintf(ev, sizeof ev, "data: {\"chunk\":%ld}\n\n", idx);
    if (n < 0 || (size_t)n >= sizeof ev) {
        return -1;
    }
    return write_chunk(fd, ev);
}

static int handle_stream(int fd, long events, long delay_ms)
{
    long i;
    for (i = 0L; i < events; i++) {
        if (send_event(fd, i) != 0) {
            return -1;
        }
        sleep_ms(delay_ms);
    }
    return write_last_chunk(fd);
}

static int handle_stall(int fd, long events, long stall_ms, long delay_ms)
{
    long i;
    for (i = 0L; i < events; i++) {
        if (send_event(fd, i) != 0) {
            return -1;
        }
        sleep_ms(delay_ms);
    }
    sleep_ms(stall_ms);
    if (send_event(fd, events) != 0) {
        return -1;
    }
    return write_last_chunk(fd);
}

static int handle_fragmented(int fd)
{
    if (send_stream_head(fd) != 0) {
        return -1;
    }
    /* Send the body in tiny pieces with delays to force fragmented reads. */
    static const char *parts[] = { "hello", " world", "!", " done" };
    size_t i;
    for (i = 0U; i < sizeof parts / sizeof parts[0]; i++) {
        if (write_chunk(fd, parts[i]) != 0) {
            return -1;
        }
        sleep_ms(30L);
    }
    return write_last_chunk(fd);
}

static int handle_echo(int fd, const char *body, size_t body_len)
{
    char head[256];
    int n = snprintf(head, sizeof head,
                     "HTTP/1.1 200 OK\r\n"
                     "Content-Length: %zu\r\n"
                     "Connection: close\r\n"
                     "\r\n",
                     body_len);
    if (n < 0 || (size_t)n >= sizeof head) {
        return -1;
    }
    if (write_all(fd, head, (size_t)n) != 0) {
        return -1;
    }
    if (body_len > 0U) {
        if (write_all(fd, body, body_len) != 0) {
            return -1;
        }
    }
    return 0;
}

static int handle_rate(int fd)
{
    static const char head[] =
        "HTTP/1.1 429 Too Many Requests\r\n"
        "Retry-After: 3\r\n"
        "Content-Length: 0\r\n"
        "Connection: close\r\n"
        "\r\n";
    return write_all(fd, head, sizeof head - 1U);
}

static int handle_malformed(int fd)
{
    static const char garbage[] = "GARBAGE NOT HTTP\r\n\r\n";
    return write_all(fd, garbage, sizeof garbage - 1U);
}

static int handle_plain_close(int fd)
{
    static const char head[] =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/plain\r\n"
        "Connection: close\r\n"
        "\r\n";
    if (write_all(fd, head, sizeof head - 1U) != 0) {
        return -1;
    }
    return write_all(fd, "plain-body", 10U);
}

static int read_request(int fd, uint8_t *buf, size_t cap, size_t *len_out)
{
    size_t got = 0U;
    while (got < cap) {
        ssize_t n = recv(fd, buf + got, cap - got, 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (n == 0) {
            return -1;
        }
        got += (size_t)n;
        /* Scan every position: the head may be followed by body bytes in
         * the same recv, so CRLFCRLF need not be at the tail. */
        size_t i;
        for (i = 0U; i + 4U <= got; i++) {
            if (memcmp(buf + i, "\r\n\r\n", 4U) == 0) {
                *len_out = got;
                return 0;
            }
        }
    }
    return -1;
}

static int parse_target(const uint8_t *buf, size_t len, char *path,
                        size_t path_cap)
{
    if (len < 6U || (memcmp(buf, "GET ", 4U) != 0 &&
                     memcmp(buf, "POST", 4U) != 0)) {
        return -1;
    }
    size_t line_end = 0U;
    while (line_end < len && buf[line_end] != '\n') {
        line_end++;
    }
    if (line_end >= len) {
        return -1;
    }
    /* Find the first space (end of method) and the second (end of target). */
    size_t sp1 = 0U;
    while (sp1 < line_end && buf[sp1] != ' ') {
        sp1++;
    }
    size_t sp2 = sp1 + 1U;
    while (sp2 < line_end && buf[sp2] != ' ') {
        sp2++;
    }
    if (sp1 >= line_end || sp2 >= line_end || sp2 <= sp1 + 1U) {
        return -1;
    }
    size_t plen = sp2 - (sp1 + 1U);
    if (plen == 0U || plen >= path_cap) {
        return -1;
    }
    (void)memcpy(path, buf + sp1 + 1U, plen);
    path[plen] = '\0';
    return 0;
}

/** @brief Case-insensitive prefix match of @p buf against a literal. */
static bool ci_prefix(const uint8_t *buf, size_t len, const char *lit)
{
    size_t i;
    for (i = 0U; i < len; i++) {
        if (lit[i] == '\0') {
            return false;
        }
        if ((char)(buf[i] | 0x20U) != lit[i]) {
            return false;
        }
    }
    return lit[i] == '\0';
}

static long request_content_length(const uint8_t *buf, size_t len)
{
    size_t pos = 0U;
    while (pos < len && buf[pos] != '\n') {
        pos++;
    }
    pos++;
    while (pos < len) {
        size_t eol = pos;
        while (eol < len && buf[eol] != '\n') {
            eol++;
        }
        size_t hlen = eol - pos;
        if (hlen > 0U && buf[pos + hlen - 1U] == '\r') {
            hlen--;
        }
        if (hlen == 0U) {
            break;
        }
        if (hlen >= 15U && ci_prefix(buf + pos, 15U, "content-length:")) {
            char vbuf[32];
            size_t vn = hlen - 15U;
            if (vn > sizeof vbuf - 1U) {
                vn = sizeof vbuf - 1U;
            }
            (void)memcpy(vbuf, buf + pos + 15U, vn);
            vbuf[vn] = '\0';
            char *end = NULL;
            long v = strtol(vbuf, &end, 10);
            if (end != vbuf) {
                return v;
            }
            return 0;
        }
        pos = eol + 1U;
    }
    return 0;
}

static void handle_connection(int fd)
{
    uint8_t req_buf[REQUEST_CAP];
    size_t req_len = 0U;
    if (read_request(fd, req_buf, sizeof req_buf, &req_len) != 0) {
        (void)close(fd);
        return;
    }
    char target[PATH_CAP];
    if (parse_target(req_buf, req_len, target, sizeof target) != 0) {
        (void)send_simple(fd, 400, "Bad Request");
        (void)close(fd);
        return;
    }
    char *query = strchr(target, '?');
    if (query != NULL) {
        *query = '\0';
        query++;
    } else {
        query = (char *)"";
    }

    int rc;
    if (strcmp(target, "/stream") == 0) {
        (void)send_stream_head(fd);
        rc = handle_stream(fd, 5L, 25L);
    } else if (strcmp(target, "/stall") == 0) {
        (void)send_stream_head(fd);
        rc = handle_stall(fd, 2L, 600L, 20L);
    } else if (strcmp(target, "/stallonce") == 0) {
        long n = 0L;
        if (g_counters != NULL) {
            n = g_counters->stallonce++;
        }
        if (n % 2L == 0L) {
            /* First connection: head, then a long silence, then finish. */
            (void)send_stream_head(fd);
            sleep_ms(400L);
            (void)send_event(fd, 0L);
            rc = write_last_chunk(fd);
        } else {
            (void)send_stream_head(fd);
            rc = handle_stream(fd, 3L, 20L);
        }
    } else if (strcmp(target, "/flaky") == 0) {
        long n = 0L;
        if (g_counters != NULL) {
            n = g_counters->flaky++;
        }
        if (n % 2L == 0L) {
            rc = send_simple(fd, 503, "Service Unavailable");
        } else {
            (void)send_stream_head(fd);
            rc = handle_stream(fd, 3L, 20L);
        }
    } else if (strcmp(target, "/fragmented") == 0) {
        rc = handle_fragmented(fd);
    } else if (strcmp(target, "/echo") == 0) {
        long cl = request_content_length(req_buf, req_len);
        uint8_t body[512];
        size_t body_len = 0U;
        if (cl > 0L) {
            size_t want = (size_t)cl;
            if (want > sizeof body) {
                want = sizeof body;
            }
            /* The head read may have over-read body bytes into req_buf. */
            size_t head_end = 0U;
            while (head_end + 4U <= req_len &&
                   memcmp(req_buf + head_end, "\r\n\r\n", 4U) != 0) {
                head_end++;
            }
            head_end += 4U;
            if (head_end < req_len) {
                size_t avail = req_len - head_end;
                size_t take = avail < want ? avail : want;
                (void)memcpy(body, req_buf + head_end, take);
                body_len += take;
            }
            while (body_len < want) {
                ssize_t r = recv(fd, body + body_len, want - body_len, 0);
                if (r <= 0) {
                    break;
                }
                body_len += (size_t)r;
            }
        }
        rc = handle_echo(fd, (const char *)body, body_len);
    } else if (strcmp(target, "/rate") == 0) {
        rc = handle_rate(fd);
    } else if (strcmp(target, "/malformed") == 0) {
        rc = handle_malformed(fd);
    } else if (strcmp(target, "/plain") == 0) {
        rc = handle_plain_close(fd);
    } else {
        rc = send_simple(fd, 404, "Not Found");
    }
    (void)rc;
    (void)close(fd);
}

static int bind_listener(uint16_t *port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    int one = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one,
                     (socklen_t)sizeof one);
    struct sockaddr_in addr;
    (void)memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0U;
    if (bind(fd, (struct sockaddr *)&addr, (socklen_t)sizeof addr) != 0) {
        (void)close(fd);
        return -1;
    }
    socklen_t alen = (socklen_t)sizeof addr;
    if (getsockname(fd, (struct sockaddr *)&addr, &alen) != 0) {
        (void)close(fd);
        return -1;
    }
    *port = ntohs(addr.sin_port);
    if (listen(fd, LISTEN_BACKLOG) != 0) {
        (void)close(fd);
        return -1;
    }
    return fd;
}

static void server_loop(int listen_fd)
{
    (void)signal(SIGPIPE, SIG_IGN);
    (void)signal(SIGCHLD, SIG_IGN);
    for (;;) {
        int c = accept(listen_fd, NULL, NULL);
        if (c < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        pid_t pid = fork();
        if (pid < 0) {
            (void)close(c);
            continue;
        }
        if (pid == 0) {
            (void)close(listen_fd);
            handle_connection(c);
            (void)close(c);
            _exit(0);
        }
        (void)close(c);
    }
}

/**
 * @brief Spawn the loopback test server.
 *
 * @param[out] port  Bound port.
 * @param[out] pid   Server process id (kill to stop).
 * @return 0 on success, -1 on failure.
 */
static int spawn_server(uint16_t *port, pid_t *pid)
{
    int fd = bind_listener(port);
    if (fd < 0) {
        return -1;
    }
    pid_t p = fork();
    if (p < 0) {
        (void)close(fd);
        return -1;
    }
    if (p == 0) {
        server_loop(fd);
        _exit(0);
    }
    (void)close(fd);
    *pid = p;
    return 0;
}

static void stop_server(pid_t pid)
{
    if (pid > 0) {
        (void)kill(pid, SIGTERM);
        (void)waitpid(pid, NULL, 0);
    }
}

/* ------------------------------------------------------------------ */
/* Client helpers                                                      */
/* ------------------------------------------------------------------ */

typedef struct collect {
    char data[4096];
    size_t len;
    int status;
    bool got_headers;
    char headers[1024];
    int chunks;
} collect_t;

/**
 * @brief Drain a request to completion (or a terminal error).
 *
 * Loops cog_net_poll until is_complete, an error, or @p timeout_ms elapses.
 */
static cog_result_t drain(cog_net_ctx_t *ctx, collect_t *c,
                          uint64_t timeout_ms)
{
    uint64_t start = now_ms();
    while (now_ms() - start < timeout_ms) {
        cog_net_response_t resp;
        cog_result_t r = cog_net_poll(ctx, &resp);
        if (r == COG_OK) {
            if (resp.headers != NULL && !c->got_headers) {
                c->got_headers = true;
                c->status = resp.http_status;
                (void)snprintf(c->headers, sizeof c->headers, "%s",
                               resp.headers);
            }
            if (resp.chunk != NULL && resp.chunk_len > 0U) {
                if (c->len + resp.chunk_len < sizeof c->data) {
                    (void)memcpy(c->data + c->len, resp.chunk,
                                 resp.chunk_len);
                    c->len += resp.chunk_len;
                }
                c->chunks++;
            }
            if (resp.is_complete) {
                return COG_OK;
            }
        } else if (r != COG_ERROR_AGAIN) {
            return r;
        }
        sleep_ms(1);
    }
    return COG_ERROR_TIMEOUT;
}

static void collect_init(collect_t *c)
{
    (void)memset(c, 0, sizeof *c);
}

static cog_net_ctx_t *make_ctx(const cog_net_config_t *cfg)
{
    cog_net_ctx_t *ctx = NULL;
    cog_result_t r = cog_net_new(&ctx, cfg);
    if (r != COG_OK) {
        return NULL;
    }
    return ctx;
}

/* ------------------------------------------------------------------ */
/* Tests                                                               */
/* ------------------------------------------------------------------ */

static void test_lifecycle_and_validation(void)
{
    /* NULL out pointer. */
    cog_result_t r = cog_net_new(NULL, NULL);
    CHECK(r == COG_ERROR_NULL_PTR);

    /* Default config. */
    cog_net_ctx_t *ctx = NULL;
    r = cog_net_new(&ctx, NULL);
    CHECK(r == COG_OK);
    CHECK(ctx != NULL);
    CHECK(cog_net_fd(ctx) >= 0);
    CHECK(cog_net_active_count(ctx) == 0U);
    CHECK(cog_net_active_count(NULL) == 0U);
    CHECK(cog_net_fd(NULL) == -1);

    /* NULL-arg validation on the request surface. */
    cog_request_id_t id = 0U;
    CHECK(cog_net_request(NULL, "GET", "http://x/", NULL, NULL, 0U, &id) ==
          COG_ERROR_NULL_PTR);
    CHECK(cog_net_request(ctx, NULL, "http://x/", NULL, NULL, 0U, &id) ==
          COG_ERROR_NULL_PTR);
    CHECK(cog_net_request(ctx, "GET", NULL, NULL, NULL, 0U, &id) ==
          COG_ERROR_NULL_PTR);
    CHECK(cog_net_request(ctx, "GET", "http://x/", NULL, NULL, 0U, NULL) ==
          COG_ERROR_NULL_PTR);

    /* NULL-arg validation on cancel/poll/status/ids. */
    CHECK(cog_net_cancel(NULL, 1U) == COG_ERROR_NULL_PTR);
    CHECK(cog_net_cancel(ctx, 999U) == COG_ERROR_NOT_FOUND);
    cog_net_response_t resp;
    CHECK(cog_net_poll(NULL, &resp) == COG_ERROR_NULL_PTR);
    CHECK(cog_net_poll(ctx, NULL) == COG_ERROR_NULL_PTR);
    bool active = false;
    CHECK(cog_net_request_status(NULL, 1U, &active) == COG_ERROR_NULL_PTR);
    CHECK(cog_net_request_status(ctx, 1U, NULL) == COG_ERROR_NULL_PTR);
    CHECK(cog_net_request_status(ctx, 1U, &active) == COG_ERROR_NOT_FOUND);
    size_t count = 0U;
    cog_request_id_t ids[4];
    CHECK(cog_net_active_ids(NULL, ids, 4U, &count) == COG_ERROR_NULL_PTR);
    CHECK(cog_net_active_ids(ctx, NULL, 4U, &count) == COG_ERROR_NULL_PTR);
    CHECK(cog_net_active_ids(ctx, ids, 4U, NULL) == COG_ERROR_NULL_PTR);

    /* Empty poll returns COG_ERROR_AGAIN. */
    CHECK(cog_net_poll(ctx, &resp) == COG_ERROR_AGAIN);

    cog_net_free(ctx);
    cog_net_free(NULL); /* must not crash */
}

static void test_overflow(void)
{
    cog_net_config_t cfg;
    (void)memset(&cfg, 0, sizeof cfg);
    cfg.max_connections = 1U;
    cfg.connect_timeout_ms = 500U;
    cfg.stall_timeout_ms = 500U;
    cfg.max_retries = 0U;
    cfg.retry_backoff_ms = 0U;
    cog_net_ctx_t *ctx = make_ctx(&cfg);
    CHECK(ctx != NULL);

    cog_request_id_t id1 = 0U, id2 = 0U;
    CHECK(cog_net_request(ctx, "GET", "http://127.0.0.1:1/x", NULL, NULL, 0U,
                          &id1) == COG_OK);
    CHECK(id1 != 0U);
    CHECK(cog_net_active_count(ctx) == 1U);
    /* Second request overflows the single connection slot. */
    CHECK(cog_net_request(ctx, "GET", "http://127.0.0.1:1/x", NULL, NULL, 0U,
                          &id2) == COG_ERROR_OVERFLOW);

    /* active_ids with a too-small buffer overflows. */
    size_t count = 0U;
    cog_request_id_t ids[1];
    CHECK(cog_net_active_ids(ctx, ids, 0U, &count) == COG_ERROR_OVERFLOW);

    cog_net_free(ctx);
}

static void test_streaming_completion(uint16_t port)
{
    cog_net_config_t cfg;
    (void)memset(&cfg, 0, sizeof cfg);
    cfg.max_connections = 4U;
    cfg.connect_timeout_ms = 2000U;
    cfg.stall_timeout_ms = 2000U;
    cfg.max_retries = 0U;
    cfg.retry_backoff_ms = 0U;
    cog_net_ctx_t *ctx = make_ctx(&cfg);
    CHECK(ctx != NULL);

    char url[128];
    (void)snprintf(url, sizeof url, "http://127.0.0.1:%u/stream",
                   (unsigned)port);
    cog_request_id_t id = 0U;
    CHECK(cog_net_request(ctx, "GET", url, NULL, NULL, 0U, &id) == COG_OK);
    CHECK(id != 0U);

    collect_t c;
    collect_init(&c);
    cog_result_t r = drain(ctx, &c, 5000U);
    CHECK(r == COG_OK);
    CHECK(c.got_headers);
    CHECK(c.status == 200);
    CHECK(strstr(c.headers, "Content-Type: text/event-stream") != NULL);
    CHECK(strstr(c.headers, "Transfer-Encoding: chunked") != NULL);
    CHECK(c.chunks >= 5);
    CHECK(strstr(c.data, "\"chunk\":0") != NULL);
    CHECK(strstr(c.data, "\"chunk\":4") != NULL);

    /* Request is no longer active after completion. */
    bool active = true;
    CHECK(cog_net_request_status(ctx, id, &active) == COG_ERROR_NOT_FOUND);
    CHECK(cog_net_active_count(ctx) == 0U);

    cog_net_free(ctx);
}

static void test_fragmented(uint16_t port)
{
    cog_net_config_t cfg;
    (void)memset(&cfg, 0, sizeof cfg);
    cfg.max_connections = 4U;
    cfg.connect_timeout_ms = 2000U;
    cfg.stall_timeout_ms = 2000U;
    cfg.max_retries = 0U;
    cfg.retry_backoff_ms = 0U;
    cog_net_ctx_t *ctx = make_ctx(&cfg);
    CHECK(ctx != NULL);

    char url[128];
    (void)snprintf(url, sizeof url, "http://127.0.0.1:%u/fragmented",
                   (unsigned)port);
    cog_request_id_t id = 0U;
    CHECK(cog_net_request(ctx, "GET", url, NULL, NULL, 0U, &id) == COG_OK);

    collect_t c;
    collect_init(&c);
    cog_result_t r = drain(ctx, &c, 5000U);
    CHECK(r == COG_OK);
    CHECK(c.status == 200);
    CHECK(c.len == strlen("hello world! done"));
    CHECK(memcmp(c.data, "hello world! done", c.len) == 0);

    cog_net_free(ctx);
}

static void test_echo_body(uint16_t port)
{
    cog_net_config_t cfg;
    (void)memset(&cfg, 0, sizeof cfg);
    cfg.max_connections = 4U;
    cfg.connect_timeout_ms = 2000U;
    cfg.stall_timeout_ms = 2000U;
    cfg.max_retries = 0U;
    cfg.retry_backoff_ms = 0U;
    cog_net_ctx_t *ctx = make_ctx(&cfg);
    CHECK(ctx != NULL);

    char url[128];
    (void)snprintf(url, sizeof url, "http://127.0.0.1:%u/echo",
                   (unsigned)port);
    static const char payload[] = "hello-echo-body";
    cog_request_id_t id = 0U;
    CHECK(cog_net_request(ctx, "POST", url, NULL, payload,
                          sizeof payload - 1U, &id) == COG_OK);

    collect_t c;
    collect_init(&c);
    cog_result_t r = drain(ctx, &c, 5000U);
    CHECK(r == COG_OK);
    CHECK(c.status == 200);
    CHECK(c.len == sizeof payload - 1U);
    CHECK(memcmp(c.data, payload, sizeof payload - 1U) == 0);

    cog_net_free(ctx);
}

static void test_plain_until_close(uint16_t port)
{
    cog_net_config_t cfg;
    (void)memset(&cfg, 0, sizeof cfg);
    cfg.max_connections = 4U;
    cfg.connect_timeout_ms = 2000U;
    cfg.stall_timeout_ms = 2000U;
    cfg.max_retries = 0U;
    cfg.retry_backoff_ms = 0U;
    cog_net_ctx_t *ctx = make_ctx(&cfg);
    CHECK(ctx != NULL);

    char url[128];
    (void)snprintf(url, sizeof url, "http://127.0.0.1:%u/plain",
                   (unsigned)port);
    cog_request_id_t id = 0U;
    CHECK(cog_net_request(ctx, "GET", url, NULL, NULL, 0U, &id) == COG_OK);

    collect_t c;
    collect_init(&c);
    cog_result_t r = drain(ctx, &c, 5000U);
    CHECK(r == COG_OK);
    CHECK(c.status == 200);
    CHECK(c.len == 10U);
    CHECK(memcmp(c.data, "plain-body", 10U) == 0);

    cog_net_free(ctx);
}

static void test_cancel(uint16_t port)
{
    cog_net_config_t cfg;
    (void)memset(&cfg, 0, sizeof cfg);
    cfg.max_connections = 4U;
    cfg.connect_timeout_ms = 2000U;
    cfg.stall_timeout_ms = 2000U;
    cfg.max_retries = 0U;
    cfg.retry_backoff_ms = 0U;
    cog_net_ctx_t *ctx = make_ctx(&cfg);
    CHECK(ctx != NULL);

    char url[128];
    (void)snprintf(url, sizeof url, "http://127.0.0.1:%u/stream",
                   (unsigned)port);
    cog_request_id_t id = 0U;
    CHECK(cog_net_request(ctx, "GET", url, NULL, NULL, 0U, &id) == COG_OK);
    CHECK(cog_net_active_count(ctx) == 1U);

    CHECK(cog_net_cancel(ctx, id) == COG_OK);
    /* The request is removed on cancel, so a second cancel is NOT_FOUND. */
    CHECK(cog_net_cancel(ctx, id) == COG_ERROR_NOT_FOUND);
    CHECK(cog_net_active_count(ctx) == 0U);

    /* Poll surfaces the abort with the request id. */
    cog_net_response_t resp;
    cog_result_t r = cog_net_poll(ctx, &resp);
    CHECK(r == COG_ERROR_ABORTED);
    CHECK(resp.request_id == id);
    CHECK(resp.is_complete);

    cog_net_free(ctx);
}

static void test_stall_timeout(uint16_t port)
{
    cog_net_config_t cfg;
    (void)memset(&cfg, 0, sizeof cfg);
    cfg.max_connections = 4U;
    cfg.connect_timeout_ms = 2000U;
    cfg.stall_timeout_ms = 150U;
    cfg.max_retries = 0U;
    cfg.retry_backoff_ms = 0U;
    cog_net_ctx_t *ctx = make_ctx(&cfg);
    CHECK(ctx != NULL);

    char url[128];
    (void)snprintf(url, sizeof url, "http://127.0.0.1:%u/stall",
                   (unsigned)port);
    cog_request_id_t id = 0U;
    CHECK(cog_net_request(ctx, "GET", url, NULL, NULL, 0U, &id) == COG_OK);

    collect_t c;
    collect_init(&c);
    cog_result_t r = drain(ctx, &c, 3000U);
    CHECK(r == COG_ERROR_TIMEOUT);
    CHECK(cog_net_active_count(ctx) == 0U);

    cog_net_free(ctx);
}

static void test_stall_recovery(uint16_t port)
{
    cog_net_config_t cfg;
    (void)memset(&cfg, 0, sizeof cfg);
    cfg.max_connections = 4U;
    cfg.connect_timeout_ms = 2000U;
    cfg.stall_timeout_ms = 150U;
    cfg.max_retries = 1U;
    cfg.retry_backoff_ms = 20U;
    cog_net_ctx_t *ctx = make_ctx(&cfg);
    CHECK(ctx != NULL);

    char url[128];
    (void)snprintf(url, sizeof url, "http://127.0.0.1:%u/stallonce",
                   (unsigned)port);
    cog_request_id_t id = 0U;
    CHECK(cog_net_request(ctx, "GET", url, NULL, NULL, 0U, &id) == COG_OK);

    collect_t c;
    collect_init(&c);
    cog_result_t r = drain(ctx, &c, 5000U);
    CHECK(r == COG_OK);
    CHECK(c.status == 200);
    CHECK(c.chunks >= 3);

    cog_net_free(ctx);
}

static void test_retry_backoff(uint16_t port)
{
    cog_net_config_t cfg;
    (void)memset(&cfg, 0, sizeof cfg);
    cfg.max_connections = 4U;
    cfg.connect_timeout_ms = 2000U;
    cfg.stall_timeout_ms = 2000U;
    cfg.max_retries = 1U;
    cfg.retry_backoff_ms = 20U;
    cog_net_ctx_t *ctx = make_ctx(&cfg);
    CHECK(ctx != NULL);

    char url[128];
    (void)snprintf(url, sizeof url, "http://127.0.0.1:%u/flaky",
                   (unsigned)port);
    cog_request_id_t id = 0U;
    CHECK(cog_net_request(ctx, "GET", url, NULL, NULL, 0U, &id) == COG_OK);

    collect_t c;
    collect_init(&c);
    cog_result_t r = drain(ctx, &c, 5000U);
    CHECK(r == COG_OK);
    CHECK(c.status == 200);
    CHECK(c.chunks >= 3);

    cog_net_free(ctx);
}

static void test_retry_exhausted(uint16_t port)
{
    /* max_retries=0: the first 503 is surfaced as the final response. */
    cog_net_config_t cfg;
    (void)memset(&cfg, 0, sizeof cfg);
    cfg.max_connections = 4U;
    cfg.connect_timeout_ms = 2000U;
    cfg.stall_timeout_ms = 2000U;
    cfg.max_retries = 0U;
    cfg.retry_backoff_ms = 0U;
    cog_net_ctx_t *ctx = make_ctx(&cfg);
    CHECK(ctx != NULL);

    char url[128];
    (void)snprintf(url, sizeof url, "http://127.0.0.1:%u/flaky",
                   (unsigned)port);
    cog_request_id_t id = 0U;
    CHECK(cog_net_request(ctx, "GET", url, NULL, NULL, 0U, &id) == COG_OK);

    collect_t c;
    collect_init(&c);
    cog_result_t r = drain(ctx, &c, 5000U);
    CHECK(r == COG_OK);
    CHECK(c.status == 503);

    cog_net_free(ctx);
}

static void test_rate_limit(uint16_t port)
{
    cog_net_config_t cfg;
    (void)memset(&cfg, 0, sizeof cfg);
    cfg.max_connections = 4U;
    cfg.connect_timeout_ms = 2000U;
    cfg.stall_timeout_ms = 2000U;
    cfg.max_retries = 0U;
    cfg.retry_backoff_ms = 0U;
    cog_net_ctx_t *ctx = make_ctx(&cfg);
    CHECK(ctx != NULL);

    char url[128];
    (void)snprintf(url, sizeof url, "http://127.0.0.1:%u/rate",
                   (unsigned)port);
    cog_request_id_t id = 0U;
    CHECK(cog_net_request(ctx, "GET", url, NULL, NULL, 0U, &id) == COG_OK);

    collect_t c;
    collect_init(&c);
    cog_result_t r = drain(ctx, &c, 5000U);
    CHECK(r == COG_OK);
    CHECK(c.status == 429);
    CHECK(strstr(c.headers, "Retry-After: 3") != NULL);

    cog_net_free(ctx);
}

static void test_malformed(uint16_t port)
{
    cog_net_config_t cfg;
    (void)memset(&cfg, 0, sizeof cfg);
    cfg.max_connections = 4U;
    cfg.connect_timeout_ms = 2000U;
    cfg.stall_timeout_ms = 2000U;
    cfg.max_retries = 0U;
    cfg.retry_backoff_ms = 0U;
    cog_net_ctx_t *ctx = make_ctx(&cfg);
    CHECK(ctx != NULL);

    char url[128];
    (void)snprintf(url, sizeof url, "http://127.0.0.1:%u/malformed",
                   (unsigned)port);
    cog_request_id_t id = 0U;
    CHECK(cog_net_request(ctx, "GET", url, NULL, NULL, 0U, &id) == COG_OK);

    collect_t c;
    collect_init(&c);
    cog_result_t r = drain(ctx, &c, 3000U);
    CHECK(r == COG_ERROR_IO);
    CHECK(cog_net_active_count(ctx) == 0U);

    cog_net_free(ctx);
}

static void test_connection_refused(void)
{
    /* Bind an ephemeral port, close it, then connect: refused. */
    uint16_t port = 0U;
    int fd = bind_listener(&port);
    CHECK(fd >= 0);
    (void)close(fd);

    cog_net_config_t cfg;
    (void)memset(&cfg, 0, sizeof cfg);
    cfg.max_connections = 4U;
    cfg.connect_timeout_ms = 500U;
    cfg.stall_timeout_ms = 500U;
    cfg.max_retries = 0U;
    cfg.retry_backoff_ms = 0U;
    cog_net_ctx_t *ctx = make_ctx(&cfg);
    CHECK(ctx != NULL);

    char url[128];
    (void)snprintf(url, sizeof url, "http://127.0.0.1:%u/x", (unsigned)port);
    cog_request_id_t id = 0U;
    CHECK(cog_net_request(ctx, "GET", url, NULL, NULL, 0U, &id) == COG_OK);

    collect_t c;
    collect_init(&c);
    cog_result_t r = drain(ctx, &c, 3000U);
    CHECK(r == COG_ERROR_IO);
    CHECK(cog_net_active_count(ctx) == 0U);

    cog_net_free(ctx);
}

static void test_bulk_and_status(uint16_t port)
{
    cog_net_config_t cfg;
    (void)memset(&cfg, 0, sizeof cfg);
    cfg.max_connections = 4U;
    cfg.connect_timeout_ms = 2000U;
    cfg.stall_timeout_ms = 2000U;
    cfg.max_retries = 0U;
    cfg.retry_backoff_ms = 0U;
    cog_net_ctx_t *ctx = make_ctx(&cfg);
    CHECK(ctx != NULL);

    char url[128];
    (void)snprintf(url, sizeof url, "http://127.0.0.1:%u/stream",
                   (unsigned)port);
    cog_request_id_t id1 = 0U, id2 = 0U;
    CHECK(cog_net_request(ctx, "GET", url, NULL, NULL, 0U, &id1) == COG_OK);
    CHECK(cog_net_request(ctx, "GET", url, NULL, NULL, 0U, &id2) == COG_OK);
    CHECK(id1 != id2);

    CHECK(cog_net_active_count(ctx) == 2U);

    cog_request_id_t ids[4];
    size_t count = 0U;
    CHECK(cog_net_active_ids(ctx, ids, 4U, &count) == COG_OK);
    CHECK(count == 2U);
    bool found1 = false, found2 = false;
    size_t i;
    for (i = 0U; i < count; i++) {
        if (ids[i] == id1) {
            found1 = true;
        }
        if (ids[i] == id2) {
            found2 = true;
        }
    }
    CHECK(found1 && found2);

    bool active = false;
    CHECK(cog_net_request_status(ctx, id1, &active) == COG_OK);
    CHECK(active);
    CHECK(cog_net_request_status(ctx, id2, &active) == COG_OK);
    CHECK(active);

    /* Bulk cancellation during shutdown. */
    CHECK(cog_net_cancel(ctx, id1) == COG_OK);
    CHECK(cog_net_cancel(ctx, id2) == COG_OK);
    CHECK(cog_net_active_count(ctx) == 0U);

    cog_net_free(ctx);
}

static void test_cleanup_cancels_inflight(uint16_t port)
{
    cog_net_config_t cfg;
    (void)memset(&cfg, 0, sizeof cfg);
    cfg.max_connections = 4U;
    cfg.connect_timeout_ms = 2000U;
    cfg.stall_timeout_ms = 2000U;
    cfg.max_retries = 0U;
    cfg.retry_backoff_ms = 0U;
    cog_net_ctx_t *ctx = make_ctx(&cfg);
    CHECK(ctx != NULL);

    char url[128];
    (void)snprintf(url, sizeof url, "http://127.0.0.1:%u/stream",
                   (unsigned)port);
    cog_request_id_t id = 0U;
    CHECK(cog_net_request(ctx, "GET", url, NULL, NULL, 0U, &id) == COG_OK);
    CHECK(cog_net_active_count(ctx) == 1U);

    /* Freeing the context must cancel in-flight work without leaking. */
    cog_net_free(ctx);
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int main(void)
{
    /* Per-path counters for /flaky and /stallonce across forked children. */
    g_counters = mmap(NULL, sizeof *g_counters, PROT_READ | PROT_WRITE,
                      MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (g_counters == MAP_FAILED) {
        (void)fprintf(stderr, "mmap failed\n");
        return 1;
    }
    g_counters->flaky = 0L;
    g_counters->stallonce = 0L;

    uint16_t port = 0U;
    pid_t server = 0;
    if (spawn_server(&port, &server) != 0) {
        (void)fprintf(stderr, "failed to spawn test server\n");
        return 1;
    }

    test_lifecycle_and_validation();
    test_overflow();
    test_streaming_completion(port);
    test_fragmented(port);
    test_echo_body(port);
    test_plain_until_close(port);
    test_cancel(port);
    test_stall_timeout(port);
    test_stall_recovery(port);
    test_retry_backoff(port);
    test_retry_exhausted(port);
    test_rate_limit(port);
    test_malformed(port);
    test_connection_refused();
    test_bulk_and_status(port);
    test_cleanup_cancels_inflight(port);

    stop_server(server);
    (void)munmap(g_counters, sizeof *g_counters);

    (void)printf("test_net: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}

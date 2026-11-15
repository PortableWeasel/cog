/**
 * @file test_net_adversarial.c
 * @brief Adversarial contract tests for cog/net.h (review ffb1ae0).
 *
 * Pins edge cases and error paths the primary contract test (test_net.c)
 * does not cover, all against a local loopback fork-per-connection server
 * (no external network):
 *
 *   - EOF mid-frame (head, Content-Length body, chunked body)
 *   - malformed chunk size, chunk size over the payload cap, oversized head
 *   - chunk extensions and trailer lines
 *   - slow-head trickle (stall must be based on "no data", not "no parse")
 *   - trickled until-close body must not false-timeout
 *   - peer FIN without reading the request (send-after-FIN survival)
 *   - interleaved multi-request streaming with correct request ids
 *   - slot reuse across sequential requests
 *   - 404 status with a body surfaced alongside headers
 *   - 429 surfaced without retry even when max_retries > 0
 *   - cancel pins ABORTED (is_complete, request_id) then NOT_FOUND
 *
 * All assertions encode the documented contract in net.h / CONTRACT.md;
 * failures here are implementation defects, not test flakiness.
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
/* Time helpers                                                         */
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
/* Loopback server (fork-per-connection)                                */
/* ------------------------------------------------------------------ */

#define REQUEST_CAP 4096U
#define PATH_CAP 512U
#define LISTEN_BACKLOG 16
#define FILLER_LEN 5120U

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

static int send_head(int fd, const char *head)
{
    return write_all(fd, head, strlen(head));
}

/**
 * @brief Blocking read of the client's request head (until CRLFCRLF).
 *
 * Called exactly once per connection by handle_connection; handlers only
 * write.
 */
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

static void handle_eofhead(int fd)
{
    (void)send_head(fd, "HTTP/1.1 200 O"); /* partial status line, then EOF */
}

static void handle_eofcl(int fd)
{
    static const char head[] =
        "HTTP/1.1 200 OK\r\nContent-Length: 10\r\nConnection: close\r\n\r\n";
    (void)write_all(fd, head, sizeof head - 1U);
    (void)write_all(fd, "abc", 3U); /* fewer than Content-Length, then EOF */
}

static void handle_eofchunk(int fd)
{
    static const char head[] =
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n";
    (void)write_all(fd, head, sizeof head - 1U);
    (void)write_all(fd, "5\r\nhel", 6U); /* mid-chunk, then EOF */
}

static void handle_badchunk(int fd)
{
    static const char head[] =
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n";
    (void)write_all(fd, head, sizeof head - 1U);
    (void)write_all(fd, "ZZ\r\n", 4U); /* not a hex chunk size */
}

static void handle_bigchunk(int fd)
{
    static const char head[] =
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n";
    (void)write_all(fd, head, sizeof head - 1U);
    (void)write_all(fd, "200000\r\n", 8U); /* 2 MB > 1 MB payload cap */
}

static void handle_bighead(int fd)
{
    static const char first[] = "HTTP/1.1 200 OK\r\nX-Filler: ";
    (void)write_all(fd, first, sizeof first - 1U);
    char filler[FILLER_LEN + 4U];
    (void)memset(filler, 'y', FILLER_LEN);
    (void)memcpy(filler + FILLER_LEN, "\r\n\r\n", 4U);
    (void)write_all(fd, filler, FILLER_LEN + 4U); /* head >> HEAD_CAP */
}

static void handle_trailers(int fd)
{
    static const char head[] =
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n";
    static const char body[] =
        "5;ext=a\r\nhello\r\n0;done=1\r\nX-Trail: yes\r\n\r\n";
    (void)write_all(fd, head, sizeof head - 1U);
    (void)write_all(fd, body, sizeof body - 1U);
}

static void handle_slowhead(int fd)
{
    static const char head[] = "HTTP/1.1 200 OK\r\n\r\n";
    size_t i;
    for (i = 0U; i < sizeof head - 1U; i++) {
        (void)write_all(fd, head + i, 1U);
        sleep_ms(50L); /* one byte per 50 ms: data IS arriving */
    }
}

static void handle_trickle(int fd)
{
    static const char head[] =
        "HTTP/1.1 200 OK\r\nContent-Length: 10\r\nConnection: close\r\n\r\n";
    (void)write_all(fd, head, sizeof head - 1U);
    size_t i;
    for (i = 0U; i < 10U; i++) {
        (void)write_all(fd, "t", 1U);
        sleep_ms(30L);
    }
}

/** Peer closes (FIN) without reading a single request byte. */
static void handle_finclose(int fd)
{
    (void)fd; /* close only */
}

static void handle_stream2(int fd)
{
    static const char head[] =
        "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
        "Transfer-Encoding: chunked\r\n\r\n";
    (void)write_all(fd, head, sizeof head - 1U);
    long i;
    for (i = 0L; i < 4L; i++) {
        char ev[64];
        int m = snprintf(ev, sizeof ev, "data: %ld\n\n", i);
        if (m > 0 && (size_t)m < sizeof ev) {
            char hdr[24];
            int hn = snprintf(hdr, sizeof hdr, "%zx\r\n", (size_t)m);
            (void)write_all(fd, hdr, (size_t)hn);
            (void)write_all(fd, ev, (size_t)m);
            (void)write_all(fd, "\r\n", 2U);
        }
        sleep_ms(10L);
    }
    (void)write_all(fd, "0\r\n\r\n", 5U);
}

static void handle_echo2(int fd, const uint8_t *req, size_t req_len)
{
    size_t head_end = 0U;
    while (head_end + 4U <= req_len &&
           memcmp(req + head_end, "\r\n\r\n", 4U) != 0) {
        head_end++;
    }
    if (head_end + 4U > req_len) {
        head_end = 0U;
    } else {
        head_end += 4U;
    }
    size_t body_len = req_len - head_end;
    char hd[128];
    int m = snprintf(hd, sizeof hd,
                     "HTTP/1.1 200 OK\r\nContent-Length: %zu\r\n\r\n",
                     body_len);
    (void)write_all(fd, hd, (size_t)m);
    if (body_len > 0U) {
        (void)write_all(fd, req + head_end, body_len);
    }
}

static void handle_404(int fd)
{
    static const char head[] =
        "HTTP/1.1 404 Not Found\r\nContent-Length: 9\r\n\r\n";
    (void)write_all(fd, head, sizeof head - 1U);
    (void)write_all(fd, "not found", 9U);
}

static void handle_429_retry(int fd)
{
    static const char head[] =
        "HTTP/1.1 429 Too Many Requests\r\nRetry-After: 7\r\n"
        "Content-Length: 0\r\n\r\n";
    (void)write_all(fd, head, sizeof head - 1U);
}

static void handle_connection(int fd)
{
    uint8_t req_buf[REQUEST_CAP];
    size_t req_len = 0U;
    if (read_request(fd, req_buf, sizeof req_buf, &req_len) != 0) {
        return;
    }
    char target[PATH_CAP];
    size_t line_end = 0U;
    while (line_end < req_len && req_buf[line_end] != '\n') {
        line_end++;
    }
    if (line_end >= req_len) {
        return;
    }
    size_t sp1 = 0U;
    while (sp1 < line_end && req_buf[sp1] != ' ') {
        sp1++;
    }
    size_t sp2 = sp1 + 1U;
    while (sp2 < line_end && req_buf[sp2] != ' ') {
        sp2++;
    }
    if (sp2 > sp1 + 1U && sp2 - (sp1 + 1U) < PATH_CAP) {
        size_t plen = sp2 - (sp1 + 1U);
        (void)memcpy(target, req_buf + sp1 + 1U, plen);
        target[plen] = '\0';
    } else {
        (void)snprintf(target, sizeof target, "/");
    }
    char *q = strchr(target, '?');
    if (q != NULL) {
        *q = '\0';
    }

    if (strcmp(target, "/eofhead") == 0) {
        handle_eofhead(fd);
    } else if (strcmp(target, "/eofcl") == 0) {
        handle_eofcl(fd);
    } else if (strcmp(target, "/eofchunk") == 0) {
        handle_eofchunk(fd);
    } else if (strcmp(target, "/badchunk") == 0) {
        handle_badchunk(fd);
    } else if (strcmp(target, "/bigchunk") == 0) {
        handle_bigchunk(fd);
    } else if (strcmp(target, "/bighead") == 0) {
        handle_bighead(fd);
    } else if (strcmp(target, "/trailers") == 0) {
        handle_trailers(fd);
    } else if (strcmp(target, "/slowhead") == 0) {
        handle_slowhead(fd);
    } else if (strcmp(target, "/trickle") == 0) {
        handle_trickle(fd);
    } else if (strcmp(target, "/finclose") == 0) {
        handle_finclose(fd);
    } else if (strcmp(target, "/stream2") == 0) {
        handle_stream2(fd);
    } else if (strcmp(target, "/echo2") == 0) {
        handle_echo2(fd, req_buf, req_len);
    } else if (strcmp(target, "/404") == 0) {
        handle_404(fd);
    } else if (strcmp(target, "/429") == 0) {
        handle_429_retry(fd);
    }
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

static int spawn_server(uint16_t *port, pid_t *pid)
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
    char data[8192];
    size_t len;
    char headers[2048];
    bool got_headers;
    int status;
    bool done;
    cog_result_t term; /* terminal error, or COG_OK if completed cleanly */
    int chunks;
} collect_t;

static void collect_init(collect_t *c)
{
    (void)memset(c, 0, sizeof *c);
}

/**
 * @brief Poll the context, dispatching response nodes by request id.
 *
 * @param ctx       Network context.
 * @param cols      Array of collectors indexed parallel to @p ids.
 * @param ids       Request ids being tracked.
 * @param n         Number of tracked requests.
 * @param timeout_ms Overall deadline for all requests to finish.
 * @return COG_OK once every request has a terminal node, otherwise the
 *         first terminal error seen, or COG_ERROR_TIMEOUT on deadline.
 */
static cog_result_t poll_all(cog_net_ctx_t *ctx, collect_t *cols,
                             const cog_request_id_t *ids, size_t n,
                             uint64_t timeout_ms)
{
    uint64_t start = now_ms();
    while (now_ms() - start < timeout_ms) {
        cog_net_response_t resp;
        cog_result_t r = cog_net_poll(ctx, &resp);
        if (r == COG_OK) {
            size_t k;
            for (k = 0U; k < n; k++) {
                if (ids[k] == resp.request_id) {
                    collect_t *c = &cols[k];
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
                        c->done = true;
                        c->term = COG_OK;
                    }
                    break;
                }
            }
        } else if (r != COG_ERROR_AGAIN) {
            size_t k;
            for (k = 0U; k < n; k++) {
                if (ids[k] == resp.request_id) {
                    cols[k].done = true;
                    cols[k].term = r;
                    break;
                }
            }
            if (k == n) {
                return r; /* terminal error for an untracked request */
            }
        }
        bool all_done = true;
        size_t k;
        for (k = 0U; k < n; k++) {
            if (!cols[k].done) {
                all_done = false;
                break;
            }
        }
        if (all_done && n > 0U) {
            return COG_OK;
        }
        sleep_ms(1);
    }
    return COG_ERROR_TIMEOUT;
}

/** Single-request drain: returns the terminal result (COG_OK or error). */
static cog_result_t drain_one(cog_net_ctx_t *ctx, collect_t *c,
                              cog_request_id_t id, uint64_t timeout_ms)
{
    cog_result_t r = poll_all(ctx, c, &id, 1U, timeout_ms);
    if (r == COG_OK) {
        return c->term;
    }
    return r;
}

static cog_net_ctx_t *make_ctx(uint32_t stall_ms, size_t retries)
{
    cog_net_config_t cfg;
    (void)memset(&cfg, 0, sizeof cfg);
    cfg.max_connections = 8U;
    cfg.connect_timeout_ms = 2000U;
    cfg.stall_timeout_ms = stall_ms;
    cfg.max_retries = retries;
    cfg.retry_backoff_ms = 0U;
    cog_net_ctx_t *ctx = NULL;
    if (cog_net_new(&ctx, &cfg) != COG_OK) {
        return NULL;
    }
    return ctx;
}

static void make_url(char *url, size_t cap, uint16_t port, const char *path)
{
    (void)snprintf(url, cap, "http://127.0.0.1:%u%s", (unsigned)port, path);
}

/* ------------------------------------------------------------------ */
/* Tests                                                               */
/* ------------------------------------------------------------------ */

static void test_eof_mid_head(uint16_t port)
{
    cog_net_ctx_t *ctx = make_ctx(1000U, 0U);
    CHECK(ctx != NULL);
    char url[128];
    make_url(url, sizeof url, port, "/eofhead");
    cog_request_id_t id = 0U;
    CHECK(cog_net_request(ctx, "GET", url, NULL, NULL, 0U, &id) == COG_OK);
    /* Manual drain: the terminal IO node must carry the request id and
     * is_complete=true, and nothing may follow it. */
    uint64_t start = now_ms();
    cog_result_t r = COG_ERROR_AGAIN;
    cog_net_response_t resp;
    while (r == COG_ERROR_AGAIN && now_ms() - start < 3000U) {
        r = cog_net_poll(ctx, &resp);
    }
    CHECK(r == COG_ERROR_IO); /* EOF mid-head, nothing to retry */
    CHECK(resp.request_id == id);
    CHECK(resp.is_complete);
    CHECK(cog_net_poll(ctx, &resp) == COG_ERROR_AGAIN);
    CHECK(cog_net_active_count(ctx) == 0U);
    cog_net_free(ctx);
}

static void test_eof_mid_cl(uint16_t port)
{
    cog_net_ctx_t *ctx = make_ctx(1000U, 0U);
    CHECK(ctx != NULL);
    char url[128];
    make_url(url, sizeof url, port, "/eofcl");
    cog_request_id_t id = 0U;
    CHECK(cog_net_request(ctx, "GET", url, NULL, NULL, 0U, &id) == COG_OK);
    collect_t c;
    collect_init(&c);
    cog_result_t r = drain_one(ctx, &c, id, 3000U);
    CHECK(r == COG_ERROR_IO); /* EOF before Content-Length satisfied */
    CHECK(cog_net_active_count(ctx) == 0U);
    cog_net_free(ctx);
}

static void test_eof_mid_chunk(uint16_t port)
{
    cog_net_ctx_t *ctx = make_ctx(1000U, 0U);
    CHECK(ctx != NULL);
    char url[128];
    make_url(url, sizeof url, port, "/eofchunk");
    cog_request_id_t id = 0U;
    CHECK(cog_net_request(ctx, "GET", url, NULL, NULL, 0U, &id) == COG_OK);
    collect_t c;
    collect_init(&c);
    cog_result_t r = drain_one(ctx, &c, id, 3000U);
    CHECK(r == COG_ERROR_IO); /* EOF mid-chunk */
    CHECK(cog_net_active_count(ctx) == 0U);
    cog_net_free(ctx);
}

static void test_bad_chunk_size(uint16_t port)
{
    cog_net_ctx_t *ctx = make_ctx(1000U, 0U);
    CHECK(ctx != NULL);
    char url[128];
    make_url(url, sizeof url, port, "/badchunk");
    cog_request_id_t id = 0U;
    CHECK(cog_net_request(ctx, "GET", url, NULL, NULL, 0U, &id) == COG_OK);
    collect_t c;
    collect_init(&c);
    cog_result_t r = drain_one(ctx, &c, id, 3000U);
    CHECK(r == COG_ERROR_IO); /* non-hex chunk size is a protocol violation */
    cog_net_free(ctx);
}

static void test_chunk_over_cap(uint16_t port)
{
    cog_net_ctx_t *ctx = make_ctx(1000U, 0U);
    CHECK(ctx != NULL);
    char url[128];
    make_url(url, sizeof url, port, "/bigchunk");
    cog_request_id_t id = 0U;
    CHECK(cog_net_request(ctx, "GET", url, NULL, NULL, 0U, &id) == COG_OK);
    collect_t c;
    collect_init(&c);
    cog_result_t r = drain_one(ctx, &c, id, 3000U);
    CHECK(r == COG_ERROR_IO); /* declared chunk exceeds the 1 MB cap */
    cog_net_free(ctx);
}

static void test_head_over_cap(uint16_t port)
{
    cog_net_ctx_t *ctx = make_ctx(1000U, 0U);
    CHECK(ctx != NULL);
    char url[128];
    make_url(url, sizeof url, port, "/bighead");
    cog_request_id_t id = 0U;
    CHECK(cog_net_request(ctx, "GET", url, NULL, NULL, 0U, &id) == COG_OK);
    collect_t c;
    collect_init(&c);
    cog_result_t r = drain_one(ctx, &c, id, 3000U);
    CHECK(r == COG_ERROR_IO); /* head exceeds the 4 KB cap */
    CHECK(cog_net_active_count(ctx) == 0U);
    cog_net_free(ctx);
}

static void test_chunk_ext_and_trailers(uint16_t port)
{
    cog_net_ctx_t *ctx = make_ctx(1000U, 0U);
    CHECK(ctx != NULL);
    char url[128];
    make_url(url, sizeof url, port, "/trailers");
    cog_request_id_t id = 0U;
    CHECK(cog_net_request(ctx, "GET", url, NULL, NULL, 0U, &id) == COG_OK);
    collect_t c;
    collect_init(&c);
    cog_result_t r = drain_one(ctx, &c, id, 3000U);
    CHECK(r == COG_OK);
    CHECK(c.status == 200);
    CHECK(c.len == 5U);
    CHECK(memcmp(c.data, "hello", 5U) == 0); /* chunk extension + trailing
                                                headers consumed cleanly */
    cog_net_free(ctx);
}

static void test_slow_head_no_false_stall(uint16_t port)
{
    /* Contract: stall = "no data received for stall_timeout_ms". A server
     * trickling the head one byte per 50 ms is sending data; the client
     * must not report a stall. */
    cog_net_ctx_t *ctx = make_ctx(200U, 0U);
    CHECK(ctx != NULL);
    char url[128];
    make_url(url, sizeof url, port, "/slowhead");
    cog_request_id_t id = 0U;
    CHECK(cog_net_request(ctx, "GET", url, NULL, NULL, 0U, &id) == COG_OK);
    collect_t c;
    collect_init(&c);
    cog_result_t r = drain_one(ctx, &c, id, 4000U);
    CHECK(r == COG_OK);
    CHECK(c.status == 200);
    cog_net_free(ctx);
}

static void test_trickled_body_no_false_stall(uint16_t port)
{
    cog_net_ctx_t *ctx = make_ctx(200U, 0U);
    CHECK(ctx != NULL);
    char url[128];
    make_url(url, sizeof url, port, "/trickle");
    cog_request_id_t id = 0U;
    CHECK(cog_net_request(ctx, "GET", url, NULL, NULL, 0U, &id) == COG_OK);
    collect_t c;
    collect_init(&c);
    cog_result_t r = drain_one(ctx, &c, id, 4000U);
    CHECK(r == COG_OK);
    CHECK(c.len == 10U);
    cog_net_free(ctx);
}

static void test_finclose_survival(uint16_t port)
{
    /* The server FINs without reading a single request byte. The client
     * must surface an error — never crash (SIGPIPE) — and must be able
     * to reuse the slot afterwards. Repeated to widen the FIN/RST race. */
    int i;
    for (i = 0; i < 12; i++) {
        cog_net_ctx_t *ctx = make_ctx(1000U, 0U);
        CHECK(ctx != NULL);
        char url[128];
        make_url(url, sizeof url, port, "/finclose");
        cog_request_id_t id = 0U;
        CHECK(cog_net_request(ctx, "GET", url, NULL, NULL, 0U, &id) ==
              COG_OK);
        collect_t c;
        collect_init(&c);
        cog_result_t r = drain_one(ctx, &c, id, 3000U);
        CHECK(r == COG_ERROR_IO || r == COG_ERROR_TIMEOUT);
        CHECK(cog_net_active_count(ctx) == 0U);
        cog_net_free(ctx);
    }
}

static void test_interleaved_streams(uint16_t port)
{
    cog_net_ctx_t *ctx = make_ctx(2000U, 0U);
    CHECK(ctx != NULL);
    char url[128];
    make_url(url, sizeof url, port, "/stream2");
    cog_request_id_t id1 = 0U, id2 = 0U;
    CHECK(cog_net_request(ctx, "GET", url, NULL, NULL, 0U, &id1) == COG_OK);
    CHECK(cog_net_request(ctx, "GET", url, NULL, NULL, 0U, &id2) == COG_OK);
    CHECK(id1 != id2);
    CHECK(cog_net_active_count(ctx) == 2U);

    cog_request_id_t ids[2];
    ids[0] = id1;
    ids[1] = id2;
    collect_t cols[2];
    collect_init(&cols[0]);
    collect_init(&cols[1]);
    cog_result_t r = poll_all(ctx, cols, ids, 2U, 5000U);
    CHECK(r == COG_OK);
    CHECK(cols[0].done && cols[1].done);
    CHECK(cols[0].term == COG_OK && cols[1].term == COG_OK);
    CHECK(cols[0].got_headers && cols[1].got_headers);
    CHECK(cols[0].status == 200 && cols[1].status == 200);
    CHECK(strstr(cols[0].headers, "Transfer-Encoding: chunked") != NULL);
    CHECK(cols[0].chunks >= 4 && cols[1].chunks >= 4);
    CHECK(strstr(cols[0].data, "data: 3") != NULL);
    CHECK(strstr(cols[1].data, "data: 3") != NULL);
    CHECK(cog_net_active_count(ctx) == 0U);
    cog_net_free(ctx);
}

static void test_slot_reuse(uint16_t port)
{
    cog_net_ctx_t *ctx = make_ctx(2000U, 0U);
    CHECK(ctx != NULL);
    static const char payload[] = "reuse-me-42";
    cog_request_id_t prev = 0U;
    int i;
    for (i = 0; i < 5; i++) {
        char url[128];
        make_url(url, sizeof url, port, "/echo2");
        cog_request_id_t id = 0U;
        CHECK(cog_net_request(ctx, "POST", url, NULL, payload,
                              sizeof payload - 1U, &id) == COG_OK);
        CHECK(id != prev);
        prev = id;
        collect_t c;
        collect_init(&c);
        cog_result_t r = drain_one(ctx, &c, id, 3000U);
        CHECK(r == COG_OK);
        CHECK(c.len == sizeof payload - 1U);
        CHECK(memcmp(c.data, payload, sizeof payload - 1U) == 0);
        /* Terminal node fully drained: nothing must be left over. */
        cog_net_response_t resp;
        CHECK(cog_net_poll(ctx, &resp) == COG_ERROR_AGAIN);
        bool active = true;
        CHECK(cog_net_request_status(ctx, id, &active) ==
              COG_ERROR_NOT_FOUND);
    }
    cog_net_free(ctx);
}

static void test_404_surface(uint16_t port)
{
    cog_net_ctx_t *ctx = make_ctx(2000U, 1U); /* retries must NOT apply */
    CHECK(ctx != NULL);
    char url[128];
    make_url(url, sizeof url, port, "/404");
    cog_request_id_t id = 0U;
    CHECK(cog_net_request(ctx, "GET", url, NULL, NULL, 0U, &id) == COG_OK);
    collect_t c;
    collect_init(&c);
    cog_result_t r = drain_one(ctx, &c, id, 3000U);
    CHECK(r == COG_OK);
    CHECK(c.status == 404);
    CHECK(c.len == 9U);
    CHECK(memcmp(c.data, "not found", 9U) == 0);
    CHECK(c.done);
    cog_net_free(ctx);
}

static void test_429_no_retry(uint16_t port)
{
    cog_net_ctx_t *ctx = make_ctx(2000U, 3U); /* 429 must not be retried */
    CHECK(ctx != NULL);
    char url[128];
    make_url(url, sizeof url, port, "/429");
    cog_request_id_t id = 0U;
    CHECK(cog_net_request(ctx, "GET", url, NULL, NULL, 0U, &id) == COG_OK);
    collect_t c;
    collect_init(&c);
    cog_result_t r = drain_one(ctx, &c, id, 3000U);
    CHECK(r == COG_OK);
    CHECK(c.status == 429);
    CHECK(strstr(c.headers, "Retry-After: 7") != NULL);
    CHECK(c.done);
    CHECK(cog_net_active_count(ctx) == 0U);
    cog_net_free(ctx);
}

static void test_cancel_no_stray_nodes(uint16_t port)
{
    cog_net_ctx_t *ctx = make_ctx(2000U, 0U);
    CHECK(ctx != NULL);
    char url[128];
    make_url(url, sizeof url, port, "/stream2");
    cog_request_id_t id = 0U;
    CHECK(cog_net_request(ctx, "GET", url, NULL, NULL, 0U, &id) == COG_OK);
    CHECK(cog_net_cancel(ctx, id) == COG_OK);
    CHECK(cog_net_active_count(ctx) == 0U);

    cog_net_response_t resp;
    cog_result_t r = cog_net_poll(ctx, &resp);
    CHECK(r == COG_ERROR_ABORTED);
    CHECK(resp.request_id == id);
    CHECK(resp.is_complete);
    /* Nothing may follow the ABORTED terminal node. */
    CHECK(cog_net_poll(ctx, &resp) == COG_ERROR_AGAIN);
    /* No stray response nodes or late socket activity may re-enqueue. */
    sleep_ms(100L);
    CHECK(cog_net_poll(ctx, &resp) == COG_ERROR_AGAIN);
    bool active = true;
    CHECK(cog_net_request_status(ctx, id, &active) == COG_ERROR_NOT_FOUND);
    cog_net_free(ctx);
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int main(void)
{
    uint16_t port = 0U;
    pid_t server = 0;
    if (spawn_server(&port, &server) != 0) {
        (void)fprintf(stderr, "failed to spawn test server\n");
        return 1;
    }

    test_eof_mid_head(port);
    test_eof_mid_cl(port);
    test_eof_mid_chunk(port);
    test_bad_chunk_size(port);
    test_chunk_over_cap(port);
    test_head_over_cap(port);
    test_chunk_ext_and_trailers(port);
    test_slow_head_no_false_stall(port);
    test_trickled_body_no_false_stall(port);
    test_finclose_survival(port);
    test_interleaved_streams(port);
    test_slot_reuse(port);
    test_404_surface(port);
    test_429_no_retry(port);
    test_cancel_no_stray_nodes(port);

    stop_server(server);
    (void)printf("test_net_adversarial: %d checks, %d failures\n",
                 g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}

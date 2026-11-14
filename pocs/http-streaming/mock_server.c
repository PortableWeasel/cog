/**
 * @file mock_server.c
 * @brief Mock HTTP/1.1 SSE streaming server for the HTTP streaming PoC.
 *
 * A loopback-only server that proves the client-side acceptance criteria
 * without any external provider. The parent process only accepts
 * connections and forks a child to serve each one, so a handler that
 * deliberately sleeps (the /stall scenario) never blocks new clients.
 * Each request is answered based on its path:
 *
 *   /stream                - 8 SSE events, one per 120 ms chunk.
 *   /stall?events=&stall=  - N events, then a deliberate silence of
 *                            `stall` ms, then a final event.
 *   /flaky                 - Alternating 503 / stream responses so every
 *                            client's FIRST request fails transiently.
 *
 * Responses use Transfer-Encoding: chunked with a text/event-stream body,
 * matching how real LLM providers deliver streaming deltas.
 */

#define _GNU_SOURCE

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

/** Default listen port. */
#define DEFAULT_PORT 18765U

/** Cap on a single request head. */
#define REQUEST_CAP 4096U

/** Cap on one parsed request target (path + query). */
#define PATH_CAP 512U

/** Listen backlog. */
#define LISTEN_BACKLOG 16

/** Maximum number of tracked forked children. */
#define MAX_CHILDREN 64

/** Child pids for deterministic shutdown; reaped by SIGCHLD=SIG_IGN. */
static pid_t g_children[MAX_CHILDREN];
static size_t g_child_count = 0U;

/** Set by SIGTERM to stop the accept loop. */
static volatile sig_atomic_t g_stop = 0;

/**
 * @brief SIGTERM handler: ask the accept loop to finish.
 *
 * @param[in] sig Signal number (unused).
 */
static void on_sigterm(int sig)
{
    (void)sig;
    g_stop = 1;
}

/**
 * @brief Sleep for a number of milliseconds.
 *
 * @param[in] ms Milliseconds (clamped to 0).
 */
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

/**
 * @brief Write all bytes to a socket.
 *
 * @param[in] fd  Socket.
 * @param[in] buf Data.
 * @param[in] len Byte count.
 * @return 0 on success, -1 on error (including EPIPE).
 */
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

/**
 * @brief Send a complete small response (no body streaming).
 *
 * @param[in] fd     Socket.
 * @param[in] status HTTP status code.
 * @param[in] reason Reason phrase.
 * @return 0 on success, -1 on error.
 */
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

/**
 * @brief Send a chunked streaming response head.
 *
 * @param[in] fd Socket.
 * @return 0 on success, -1 on error.
 */
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

/**
 * @brief Write one HTTP chunk (hex size + payload + CRLF).
 *
 * @param[in] fd      Socket.
 * @param[in] payload Chunk payload.
 * @return 0 on success, -1 on error.
 */
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

/**
 * @brief Write the terminal 0-length chunk.
 *
 * @param[in] fd Socket.
 * @return 0 on success, -1 on error.
 */
static int write_last_chunk(int fd)
{
    return write_all(fd, "0\r\n\r\n", 5U);
}

/**
 * @brief Send one SSE data event as a single chunk.
 *
 * @param[in] fd  Socket.
 * @param[in] idx Event index (put in the payload for verification).
 * @return 0 on success, -1 on error.
 */
static int send_event(int fd, long idx)
{
    char ev[128];
    int n = snprintf(ev, sizeof ev, "data: {\"chunk\":%ld}\n\n", idx);
    if (n < 0 || (size_t)n >= sizeof ev) {
        return -1;
    }
    return write_chunk(fd, ev);
}

/**
 * @brief Stream @p events SSE events with @p delay_ms between them.
 *
 * @param[in] fd       Socket.
 * @param[in] events   Event count.
 * @param[in] delay_ms Inter-event delay in milliseconds.
 * @return 0 on success, -1 on error.
 */
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

/**
 * @brief Stream events, then go silent for a while, then finish.
 *
 * Used to prove stall detection: the client's threshold is much smaller
 * than @p stall_ms, so it must classify the connection as stalled even
 * though the server eventually finishes.
 *
 * @param[in] fd       Socket.
 * @param[in] events   Events before the silence.
 * @param[in] stall_ms Silence duration in milliseconds.
 * @param[in] delay_ms Inter-event delay in milliseconds.
 * @return 0 on success, -1 on error.
 */
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

/**
 * @brief Answer /flaky: every even-numbered request (0-based) fails with
 *        503, every odd one streams. This guarantees that each client's
 *        FIRST request fails transiently — matching how a real provider
 *        might shed load — while a retry on the same client succeeds.
 *
 * @param[in] fd    Socket.
 * @param[in,out] count Running request counter for this process.
 * @return 0 on success, -1 on error.
 */
static int handle_flaky(int fd, long *count)
{
    long n = *count;
    (*count)++;
    if (n % 2L == 0L) {
        return send_simple(fd, 503, "Service Unavailable");
    }
    (void)send_stream_head(fd);
    return handle_stream(fd, 4L, 120L);
}

/**
 * @brief Read the request head up to the terminating CRLFCRLF.
 *
 * @param[in]  fd      Socket.
 * @param[out] buf     Buffer.
 * @param[in]  cap     Buffer capacity.
 * @param[out] len_out Bytes read.
 * @return 0 on success, -1 on error/EOF/oversize.
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
        if (got >= 4U) {
            size_t i = got - 4U;
            size_t j = 0U;
            while (j < 4U && buf[i + j] == "\r\n\r\n"[j]) {
                j++;
            }
            if (j == 4U) {
                *len_out = got;
                return 0;
            }
        }
    }
    return -1;
}

/**
 * @brief Extract the request target (path + optional query) from line 1.
 *
 * @param[in]  buf      Request head.
 * @param[in]  len      Head length.
 * @param[out] path     Request-target buffer (NUL-terminated).
 * @param[in]  path_cap Buffer capacity.
 * @return 0 on success, -1 on malformed input.
 */
static int parse_target(const uint8_t *buf, size_t len, char *path,
                        size_t path_cap)
{
    if (len < 6U || memcmp(buf, "GET ", 4U) != 0) {
        return -1;
    }
    size_t line_end = 0U;
    while (line_end < len && buf[line_end] != '\n') {
        line_end++;
    }
    if (line_end >= len) {
        return -1;
    }
    size_t sp1 = 4U;
    size_t sp2 = sp1;
    while (sp2 < line_end && buf[sp2] != ' ') {
        sp2++;
    }
    if (sp2 <= sp1 || sp2 >= line_end) {
        return -1;
    }
    size_t plen = sp2 - sp1;
    if (plen == 0U || plen >= path_cap) {
        return -1;
    }
    (void)memcpy(path, buf + sp1, plen);
    path[plen] = '\0';
    return 0;
}

/**
 * @brief Look up a numeric query parameter.
 *
 * @param[in] q    Query string (may be NULL).
 * @param[in] key  Parameter name.
 * @param[in] dflt Default when missing or unparseable.
 * @return Parsed value clamped by callers.
 */
static long query_long(const char *q, const char *key, long dflt)
{
    if (q == NULL) {
        return dflt;
    }
    size_t klen = strlen(key);
    const char *p = q;
    while (*p != '\0') {
        const char *amp = strchr(p, '&');
        size_t tok_len = (amp != NULL) ? (size_t)(amp - p) : strlen(p);
        if (tok_len > klen && strncmp(p, key, klen) == 0 && p[klen] == '=') {
            char *end = NULL;
            long v = strtol(p + klen + 1U, &end, 10);
            if (end != p + klen + 1U) {
                return v;
            }
            return dflt;
        }
        if (amp == NULL) {
            break;
        }
        p = amp + 1U;
    }
    return dflt;
}

/**
 * @brief Clamp a query-derived long into [lo, hi].
 */
static long clamp_long(long v, long lo, long hi)
{
    if (v < lo) {
        return lo;
    }
    if (v > hi) {
        return hi;
    }
    return v;
}

/**
 * @brief Dispatch one connection: parse the request and answer it.
 *
 * @param[in]     fd          Accepted socket.
 * @param[in,out] flaky_count /flaky request counter.
 */
static void handle_connection(int fd, long *flaky_count)
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
        rc = handle_stream(fd, 8L, 120L);
    } else if (strcmp(target, "/stall") == 0) {
        long events = clamp_long(query_long(query, "events", 3L), 0L, 100L);
        long stall_ms = clamp_long(query_long(query, "stall", 2000L), 0L,
                                   30000L);
        long delay_ms = clamp_long(query_long(query, "delay", 100L), 0L,
                                   5000L);
        (void)send_stream_head(fd);
        rc = handle_stall(fd, events, stall_ms, delay_ms);
    } else if (strcmp(target, "/flaky") == 0) {
        rc = handle_flaky(fd, flaky_count);
    } else {
        rc = send_simple(fd, 404, "Not Found");
    }
    (void)rc;
    (void)close(fd);
}

/**
 * @brief Bind the loopback listener.
 *
 * @param[in]  port     Port to bind.
 * @param[out] err_out  errno on failure.
 * @return Listening socket fd, or -1.
 */
static int bind_listener(uint16_t port, int *err_out)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        *err_out = errno;
        return -1;
    }
    int one = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one,
                     (socklen_t)sizeof one);
    struct sockaddr_in addr;
    (void)memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    if (bind(fd, (struct sockaddr *)&addr, (socklen_t)sizeof addr) != 0) {
        *err_out = errno;
        (void)close(fd);
        return -1;
    }
    if (listen(fd, LISTEN_BACKLOG) != 0) {
        *err_out = errno;
        (void)close(fd);
        return -1;
    }
    *err_out = 0;
    return fd;
}

int main(int argc, char **argv)
{
    uint16_t port = DEFAULT_PORT;
    int i;
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            char *end = NULL;
            long v = strtol(argv[i + 1], &end, 10);
            if (end != argv[i + 1] && v > 0L && v <= 65535L) {
                port = (uint16_t)v;
            } else {
                (void)fprintf(stderr, "invalid --port value\n");
                return 2;
            }
            i++;
        } else {
            (void)fprintf(stderr, "usage: %s [--port N]\n", argv[0]);
            return 2;
        }
    }

    (void)signal(SIGPIPE, SIG_IGN); /* client may close mid-write */
    (void)signal(SIGCHLD, SIG_IGN); /* auto-reap children, no zombies */

    /* Install SIGTERM with sigaction and NO SA_RESTART. With _GNU_SOURCE,
     * signal() sets SA_RESTART, so a blocking accept() is transparently
     * restarted after the handler runs and the while (!g_stop) loop never
     * re-evaluates — the server would be impossible to stop. Without
     * SA_RESTART, accept() returns EINTR and the loop re-checks g_stop. */
    struct sigaction sa;
    (void)memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sigterm;
    (void)sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    (void)sigaction(SIGTERM, &sa, NULL);

    int err = 0;
    int fd = bind_listener(port, &err);
    if (fd < 0) {
        (void)fprintf(stderr, "bind failed: %s\n", strerror(err));
        return 1;
    }

    /* Share the /flaky counter across all forked children so the
     * alternating 503 behaviour holds process-wide. */
    long *flaky_count = mmap(NULL, sizeof *flaky_count,
                             PROT_READ | PROT_WRITE,
                             MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (flaky_count == MAP_FAILED) {
        (void)fprintf(stderr, "mmap failed: %s\n", strerror(errno));
        (void)close(fd);
        return 1;
    }
    *flaky_count = 0L;

    (void)printf("mock server listening on 127.0.0.1:%u\n", (unsigned)port);
    (void)fflush(stdout);

    /* Fork per connection: a handler that sleeps (stall scenario) must
     * not block new connections. The parent only accepts and reaps. */
    while (!g_stop) {
        struct sockaddr_in peer;
        socklen_t plen = (socklen_t)sizeof peer;
        int c = accept(fd, (struct sockaddr *)&peer, &plen);
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
            /* Child: serve exactly one connection, then exit. */
            (void)close(fd);
            handle_connection(c, flaky_count);
            (void)close(c);
            _exit(0);
        }
        (void)close(c);
        if (g_child_count < MAX_CHILDREN) {
            g_children[g_child_count++] = pid;
        }
    }

    /* Shutdown: terminate any children still sleeping (e.g. a stall
     * handler whose client already gave up). */
    size_t ci;
    for (ci = 0U; ci < g_child_count; ci++) {
        (void)kill(g_children[ci], SIGTERM);
    }
    (void)close(fd);
    (void)munmap(flaky_count, sizeof *flaky_count);
    return 0;
}

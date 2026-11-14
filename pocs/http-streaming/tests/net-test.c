/**
 * @file net-test.c
 * @brief Network error-path tests for the hand-rolled HTTP client.
 *
 * The demo covers the happy paths (stream, stall, flaky). This test
 * drives the documented error taxonomy of http_common.h against a real
 * socket:
 *
 *   - 4xx response            -> HTTP_ERR_CLIENT, state ERROR
 *   - connection refused      -> HTTP_ERR_NET,   state ERROR
 *   - malformed response head -> HTTP_ERR_PARSE, state ERROR
 *   - chunked body with a size-line extension and trailer headers
 *                             -> HTTP_ERR_NONE,  state DONE
 *   - NULL request context    -> HTTP_ERR_PARSE
 *
 * A tiny fork-per-test raw server sends canned bytes; the mock server is
 * used for the 404 case (its /nope endpoint answers 404).
 */

#define _POSIX_C_SOURCE 200809L

#include "conn.h"
#include "http_common.h"

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

/**
 * @brief Bind an ephemeral loopback port.
 *
 * @param[out] port Bound port.
 * @return Listening socket fd, or -1.
 */
static int bind_ephemeral(uint16_t *port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    struct sockaddr_in addr;
    (void)memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0U; /* ephemeral */
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
    if (listen(fd, 4) != 0) {
        (void)close(fd);
        return -1;
    }
    return fd;
}

/**
 * @brief Fork a raw server that answers one connection with @p bytes.
 *
 * @param[in]  bytes Canned response bytes.
 * @param[in]  len   Byte count.
 * @param[out] port  Port the server listens on.
 * @return 0 on success, -1 on failure.
 */
static int spawn_raw_server(const char *bytes, size_t len, uint16_t *port)
{
    int fd = bind_ephemeral(port);
    if (fd < 0) {
        return -1;
    }
    pid_t pid = fork();
    if (pid < 0) {
        (void)close(fd);
        return -1;
    }
    if (pid == 0) {
        (void)signal(SIGPIPE, SIG_IGN);
        int c = accept(fd, NULL, NULL);
        if (c >= 0) {
            size_t off = 0U;
            while (off < len) {
                ssize_t w = send(c, bytes + off, len - off, 0);
                if (w < 0) {
                    break;
                }
                off += (size_t)w;
            }
            /* Give the client a moment to read before closing. */
            struct timespec pause;
            pause.tv_sec = 0;
            pause.tv_nsec = 200000000L;
            (void)nanosleep(&pause, NULL);
            (void)close(c);
        }
        (void)close(fd);
        _exit(0);
    }
    (void)close(fd);
    return 0;
}

/**
 * @brief Run the hand client against a URL and return its result.
 */
static http_result_t run_hand(const char *url, uint64_t stall_ms)
{
    conn_tracker_t *t = conn_tracker_create(1U);
    http_client_cfg_t cfg;
    cfg.stall_ms = stall_ms;
    cfg.max_retries = 0U;
    cfg.overall_timeout_ms = 5000U;
    http_request_t req;
    (void)memset(&req, 0, sizeof req);
    req.url = url;
    req.cfg = &cfg;
    req.tracker = t;
    http_result_t res = hand_request(&req);
    conn_tracker_destroy(t);
    return res;
}

static void test_client_error(void)
{
    /* The mock server answers unknown paths with 404. */
    const char *port_str = getenv("COG_POC_MOCK_PORT");
    if (port_str == NULL) {
        (void)fprintf(stderr, "SKIP client-error: COG_POC_MOCK_PORT unset\n");
        return;
    }
    char url[128];
    (void)snprintf(url, sizeof url, "http://127.0.0.1:%s/nope", port_str);
    http_result_t res = run_hand(url, 700U);
    CHECK(res.err == HTTP_ERR_CLIENT);
    CHECK(res.state == CONN_STATE_ERROR);
    CHECK(res.status == 404);
    CHECK(res.retries == 0U);
}

static void test_connection_refused(void)
{
    /* Bind an ephemeral port, close it, then connect: refused. */
    uint16_t port = 0U;
    int fd = bind_ephemeral(&port);
    CHECK(fd >= 0);
    (void)close(fd); /* now nothing listens */

    char url[128];
    (void)snprintf(url, sizeof url, "http://127.0.0.1:%u/x", (unsigned)port);
    http_result_t res = run_hand(url, 700U);
    CHECK(res.err == HTTP_ERR_NET);
    CHECK(res.state == CONN_STATE_ERROR);
}

static void test_malformed_head(void)
{
    static const char garbage[] = "GARBAGE NOT HTTP\r\n\r\n";
    uint16_t port = 0U;
    CHECK(spawn_raw_server(garbage, sizeof garbage - 1U, &port) == 0);

    char url[128];
    (void)snprintf(url, sizeof url, "http://127.0.0.1:%u/x", (unsigned)port);
    http_result_t res = run_hand(url, 700U);
    CHECK(res.err == HTTP_ERR_PARSE);
    CHECK(res.state == CONN_STATE_ERROR);
    (void)wait(NULL);
}

static void test_chunked_extensions_and_trailers(void)
{
    /* 0x1a = 26 bytes of payload, size line with an extension, then a
     * trailer header before the terminal blank line. */
    static const char body[] = "0123456789abcdefghijklmnop"; /* 26 bytes */
    char resp[512];
    int n = snprintf(resp, sizeof resp,
                     "HTTP/1.1 200 OK\r\n"
                     "Transfer-Encoding: chunked\r\n"
                     "\r\n"
                     "1a;ext=ignored\r\n"
                     "%s\r\n"
                     "0\r\n"
                     "X-Trailer: yes\r\n"
                     "\r\n",
                     body);
    CHECK(n > 0 && (size_t)n < sizeof resp);

    uint16_t port = 0U;
    CHECK(spawn_raw_server(resp, (size_t)n, &port) == 0);

    char url[128];
    (void)snprintf(url, sizeof url, "http://127.0.0.1:%u/x", (unsigned)port);
    http_result_t res = run_hand(url, 700U);
    CHECK(res.err == HTTP_ERR_NONE);
    CHECK(res.state == CONN_STATE_DONE);
    CHECK(res.bytes == sizeof body - 1U);
    (void)wait(NULL);
}

static void test_null_request(void)
{
    http_result_t res = hand_request(NULL);
    CHECK(res.err == HTTP_ERR_PARSE);
    CHECK(res.state == CONN_STATE_ERROR);
}

int main(void)
{
    test_client_error();
    test_connection_refused();
    test_malformed_head();
    test_chunked_extensions_and_trailers();
    test_null_request();

    (void)printf("net-test: %d checks, %d failures\n", g_checks,
                 g_failures);
    return g_failures == 0 ? 0 : 1;
}

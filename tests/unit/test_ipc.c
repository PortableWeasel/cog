/**
 * @file test_ipc.c
 * @brief Contract test for cog/ipc.h implemented by libraries/cog-ipc/src/ipc.c.
 *
 * Covers the documented public contract:
 *   - message buffer allocation / release
 *   - endpoint lifecycle (new / listen / connect / accept / disconnect / free)
 *   - full-duplex framed send / recv, including partial reads and writes
 *   - framing correctness (multiple frames, empty frames, large frames)
 *   - length bounds and malformed-frame handling (overflow closes the link)
 *   - poll readiness and timeouts
 *   - peer ids
 *   - broadcast
 *   - session registration / routing / unregistration and table bounds
 *   - disconnect / reconnect and crash-like (peer closes without handshake)
 *   - invalid inputs and documented error codes
 *
 * Uses local temporary socket paths and pthreads for concurrency where the
 * blocking transport requires it.  All socket paths are cleaned up.
 *
 * Ticket: 4de26fd (Stage 12.3a — Implement ipc.c)
 */

#define _POSIX_C_SOURCE 200809L

#include "cog/ipc.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* Test harness                                                        */
/* ------------------------------------------------------------------ */

static unsigned g_failures = 0U;

#define CHECK(label, expr)                                                   \
    do {                                                                     \
        if (!(expr)) {                                                       \
            (void)fprintf(stderr, "FAIL: %s (line %d)\n", label, __LINE__);  \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

#define CHECK_RESULT(label, expected, actual)                                \
    do {                                                                     \
        cog_result_t _e = (expected);                                        \
        cog_result_t _a = (actual);                                          \
        if (_e != _a) {                                                      \
            (void)fprintf(stderr,                                            \
                          "FAIL: %s (line %d): expected %d, got %d\n",       \
                          label, __LINE__, (int)_e, (int)_a);                \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

/** Create a unique temporary socket path; caller unlinks it when done. */
static int make_socket_path(char *buf, size_t bufsize)
{
    char tmpl[] = "/tmp/cog-ipc-test-XXXXXX";
    int fd = mkstemp(tmpl);

    if (fd < 0) {
        return -1;
    }
    (void)close(fd);
    (void)unlink(tmpl);
    (void)snprintf(buf, bufsize, "%s", tmpl);
    return 0;
}

/** Open a raw connected socket to a listener path (for malformed frames). */
static int raw_connect(const char *path)
{
    struct sockaddr_un addr;
    int fd;
    size_t path_len;

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    (void)memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    path_len = strlen(path);
    if (path_len >= sizeof(addr.sun_path)) {
        (void)close(fd);
        return -1;
    }
    (void)memcpy(addr.sun_path, path, path_len);
    if (connect(fd, (const struct sockaddr *)&addr, sizeof(addr)) < 0) {
        (void)close(fd);
        return -1;
    }
    return fd;
}

/** Write a raw 4-byte big-endian length header to a socket. */
static int raw_write_header(int fd, uint32_t len)
{
    uint8_t header[4];

    header[0] = (uint8_t)((len >> 24) & 0xFFU);
    header[1] = (uint8_t)((len >> 16) & 0xFFU);
    header[2] = (uint8_t)((len >> 8) & 0xFFU);
    header[3] = (uint8_t)(len & 0xFFU);

    if (write(fd, header, sizeof(header)) != (ssize_t)sizeof(header)) {
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Message buffer tests                                                */
/* ------------------------------------------------------------------ */

static void test_message_new_free(void)
{
    cog_ipc_message_t msg;
    cog_ipc_message_t *p = NULL;

    CHECK_RESULT("msg_new NULL", COG_ERROR_NULL_PTR,
                 cog_ipc_message_new(NULL, 16U));

    CHECK_RESULT("msg_new zero cap", COG_ERROR_INVALID_ARG,
                 cog_ipc_message_new(&msg, 0U));
    CHECK("msg_new zero cap zeroed", msg.data == NULL && msg.capacity == 0U);

    CHECK_RESULT("msg_new ok", COG_OK, cog_ipc_message_new(&msg, 64U));
    CHECK("msg_new data", msg.data != NULL);
    CHECK("msg_new len", msg.len == 0U);
    CHECK("msg_new cap", msg.capacity == 64U);

    CHECK_RESULT("msg_free ok", COG_OK, cog_ipc_message_free(&msg));
    CHECK("msg_free zeroed", msg.data == NULL && msg.capacity == 0U);

    CHECK_RESULT("msg_free NULL", COG_OK, cog_ipc_message_free(NULL));

    /* free of a zeroed message is a no-op */
    CHECK_RESULT("msg_free zeroed", COG_OK, cog_ipc_message_free(&msg));

    (void)p;
}

/* ------------------------------------------------------------------ */
/* Endpoint lifecycle tests                                            */
/* ------------------------------------------------------------------ */

static void test_endpoint_new_free(void)
{
    cog_ipc_endpoint_t *e = NULL;

    CHECK_RESULT("ep_new NULL", COG_ERROR_NULL_PTR,
                 cog_ipc_endpoint_new(NULL));
    CHECK_RESULT("ep_new ok", COG_OK, cog_ipc_endpoint_new(&e));
    CHECK("ep_new non-null", e != NULL);
    CHECK_RESULT("ep_free ok", COG_OK, cog_ipc_endpoint_free(e));
    CHECK_RESULT("ep_free NULL", COG_ERROR_NULL_PTR,
                 cog_ipc_endpoint_free(NULL));
}

static void test_listen_connect_accept(void)
{
    char path[128];
    cog_ipc_endpoint_t *listener = NULL;
    cog_ipc_endpoint_t *client = NULL;
    cog_ipc_endpoint_t *server = NULL;
    cog_peer_id_t pid = COG_PEER_ID_INVALID;

    CHECK("make path", make_socket_path(path, sizeof(path)) == 0);

    CHECK_RESULT("listen NULL ep", COG_ERROR_NULL_PTR,
                 cog_ipc_endpoint_listen(NULL, path));
    CHECK_RESULT("listen NULL path", COG_ERROR_NULL_PTR,
                 cog_ipc_endpoint_listen(listener, NULL));

    CHECK_RESULT("ep_new listener", COG_OK, cog_ipc_endpoint_new(&listener));
    CHECK_RESULT("listen ok", COG_OK, cog_ipc_endpoint_listen(listener, path));

    /* listen again on the same endpoint -> INVALID_STATE */
    CHECK_RESULT("listen twice", COG_ERROR_INVALID_STATE,
                 cog_ipc_endpoint_listen(listener, path));

    /* connect */
    CHECK_RESULT("ep_new client", COG_OK, cog_ipc_endpoint_new(&client));
    CHECK_RESULT("connect ok", COG_OK,
                 cog_ipc_endpoint_connect(client, path, &pid));
    CHECK("connect peer id", pid != COG_PEER_ID_INVALID);

    /* connect again -> INVALID_STATE */
    CHECK_RESULT("connect twice", COG_ERROR_INVALID_STATE,
                 cog_ipc_endpoint_connect(client, path, NULL));

    /* accept */
    CHECK_RESULT("accept ok", COG_OK,
                 cog_ipc_endpoint_accept(listener, &server, &pid));
    CHECK("accept peer id", pid != COG_PEER_ID_INVALID);

    /* accept on a non-listener -> INVALID_STATE */
    {
        cog_ipc_endpoint_t *x = NULL;
        CHECK_RESULT("accept non-listener", COG_ERROR_INVALID_STATE,
                     cog_ipc_endpoint_accept(client, &x, NULL));
    }

    /* connect to a non-existent path -> IO */
    {
        cog_ipc_endpoint_t *c2 = NULL;
        const char *bad = "/tmp/cog-ipc-nonexistent-xyz.sock";
        CHECK_RESULT("ep_new c2", COG_OK, cog_ipc_endpoint_new(&c2));
        CHECK_RESULT("connect bad path", COG_ERROR_IO,
                     cog_ipc_endpoint_connect(c2, bad, NULL));
        CHECK_RESULT("free c2", COG_OK, cog_ipc_endpoint_free(c2));
    }

    /* NULL args */
    CHECK_RESULT("accept NULL listener", COG_ERROR_NULL_PTR,
                 cog_ipc_endpoint_accept(NULL, &server, NULL));
    CHECK_RESULT("accept NULL client", COG_ERROR_NULL_PTR,
                 cog_ipc_endpoint_accept(listener, NULL, NULL));

    CHECK_RESULT("free server", COG_OK, cog_ipc_endpoint_free(server));
    CHECK_RESULT("free client", COG_OK, cog_ipc_endpoint_free(client));
    CHECK_RESULT("free listener", COG_OK, cog_ipc_endpoint_free(listener));
    (void)unlink(path);
}

/* ------------------------------------------------------------------ */
/* Send / receive round-trip and framing                               */
/* ------------------------------------------------------------------ */

static void test_roundtrip(void)
{
    char path[128];
    cog_ipc_endpoint_t *listener = NULL;
    cog_ipc_endpoint_t *client = NULL;
    cog_ipc_endpoint_t *server = NULL;
    cog_ipc_message_t out;
    cog_ipc_message_t in;
    const char *payload = "hello from client";

    CHECK("make path", make_socket_path(path, sizeof(path)) == 0);
    CHECK_RESULT("new listener", COG_OK, cog_ipc_endpoint_new(&listener));
    CHECK_RESULT("listen", COG_OK, cog_ipc_endpoint_listen(listener, path));
    CHECK_RESULT("new client", COG_OK, cog_ipc_endpoint_new(&client));
    CHECK_RESULT("connect", COG_OK, cog_ipc_endpoint_connect(client, path, NULL));
    CHECK_RESULT("accept", COG_OK, cog_ipc_endpoint_accept(listener, &server, NULL));

    CHECK_RESULT("msg_new out", COG_OK, cog_ipc_message_new(&out, 64U));
    CHECK_RESULT("msg_new in", COG_OK, cog_ipc_message_new(&in, 64U));

    (void)memcpy(out.data, payload, strlen(payload));
    out.len = strlen(payload);

    CHECK_RESULT("send", COG_OK, cog_ipc_endpoint_send(client, &out));
    CHECK_RESULT("recv", COG_OK, cog_ipc_endpoint_recv(server, &in));
    CHECK("recv len", in.len == strlen(payload));
    CHECK("recv data", memcmp(in.data, payload, in.len) == 0);

    /* reply back */
    (void)memcpy(out.data, "pong", 4U);
    out.len = 4U;
    CHECK_RESULT("send reply", COG_OK, cog_ipc_endpoint_send(server, &out));
    CHECK_RESULT("recv reply", COG_OK, cog_ipc_endpoint_recv(client, &in));
    CHECK("reply len", in.len == 4U);
    CHECK("reply data", memcmp(in.data, "pong", 4U) == 0);

    CHECK_RESULT("free msg out", COG_OK, cog_ipc_message_free(&out));
    CHECK_RESULT("free msg in", COG_OK, cog_ipc_message_free(&in));
    CHECK_RESULT("free server", COG_OK, cog_ipc_endpoint_free(server));
    CHECK_RESULT("free client", COG_OK, cog_ipc_endpoint_free(client));
    CHECK_RESULT("free listener", COG_OK, cog_ipc_endpoint_free(listener));
    (void)unlink(path);
}

static void test_framing_multiple(void)
{
    char path[128];
    cog_ipc_endpoint_t *listener = NULL;
    cog_ipc_endpoint_t *client = NULL;
    cog_ipc_endpoint_t *server = NULL;
    cog_ipc_message_t out;
    cog_ipc_message_t in;
    static const char *msgs[] = {"one", "", "three", "four"};
    size_t i;

    CHECK("make path", make_socket_path(path, sizeof(path)) == 0);
    CHECK_RESULT("new listener", COG_OK, cog_ipc_endpoint_new(&listener));
    CHECK_RESULT("listen", COG_OK, cog_ipc_endpoint_listen(listener, path));
    CHECK_RESULT("new client", COG_OK, cog_ipc_endpoint_new(&client));
    CHECK_RESULT("connect", COG_OK, cog_ipc_endpoint_connect(client, path, NULL));
    CHECK_RESULT("accept", COG_OK, cog_ipc_endpoint_accept(listener, &server, NULL));
    CHECK_RESULT("msg_new out", COG_OK, cog_ipc_message_new(&out, 64U));
    CHECK_RESULT("msg_new in", COG_OK, cog_ipc_message_new(&in, 64U));

    /* Send all four frames back-to-back; they must arrive as separate frames. */
    for (i = 0U; i < sizeof(msgs) / sizeof(msgs[0]); i++) {
        size_t len = strlen(msgs[i]);
        (void)memcpy(out.data, msgs[i], len);
        out.len = len;
        CHECK_RESULT("send burst", COG_OK, cog_ipc_endpoint_send(client, &out));
    }

    for (i = 0U; i < sizeof(msgs) / sizeof(msgs[0]); i++) {
        size_t len = strlen(msgs[i]);
        CHECK_RESULT("recv burst", COG_OK, cog_ipc_endpoint_recv(server, &in));
        CHECK("burst len", in.len == len);
        CHECK("burst data", memcmp(in.data, msgs[i], len) == 0);
    }

    CHECK_RESULT("free msg out", COG_OK, cog_ipc_message_free(&out));
    CHECK_RESULT("free msg in", COG_OK, cog_ipc_message_free(&in));
    CHECK_RESULT("free server", COG_OK, cog_ipc_endpoint_free(server));
    CHECK_RESULT("free client", COG_OK, cog_ipc_endpoint_free(client));
    CHECK_RESULT("free listener", COG_OK, cog_ipc_endpoint_free(listener));
    (void)unlink(path);
}

static void test_large_message(void)
{
    char path[128];
    cog_ipc_endpoint_t *listener = NULL;
    cog_ipc_endpoint_t *client = NULL;
    cog_ipc_endpoint_t *server = NULL;
    cog_ipc_message_t out;
    cog_ipc_message_t in;
    const size_t len = 60000U; /* near the 65535 max, exercises partial I/O */
    size_t i;

    CHECK("make path", make_socket_path(path, sizeof(path)) == 0);
    CHECK_RESULT("new listener", COG_OK, cog_ipc_endpoint_new(&listener));
    CHECK_RESULT("listen", COG_OK, cog_ipc_endpoint_listen(listener, path));
    CHECK_RESULT("new client", COG_OK, cog_ipc_endpoint_new(&client));
    CHECK_RESULT("connect", COG_OK, cog_ipc_endpoint_connect(client, path, NULL));
    CHECK_RESULT("accept", COG_OK, cog_ipc_endpoint_accept(listener, &server, NULL));
    CHECK_RESULT("msg_new out", COG_OK, cog_ipc_message_new(&out, len));
    CHECK_RESULT("msg_new in", COG_OK, cog_ipc_message_new(&in, len));

    for (i = 0U; i < len; i++) {
        out.data[i] = (uint8_t)('A' + (i % 26U));
    }
    out.len = len;

    CHECK_RESULT("send large", COG_OK, cog_ipc_endpoint_send(client, &out));
    CHECK_RESULT("recv large", COG_OK, cog_ipc_endpoint_recv(server, &in));
    CHECK("large len", in.len == len);
    CHECK("large data", memcmp(in.data, out.data, len) == 0);

    CHECK_RESULT("free msg out", COG_OK, cog_ipc_message_free(&out));
    CHECK_RESULT("free msg in", COG_OK, cog_ipc_message_free(&in));
    CHECK_RESULT("free server", COG_OK, cog_ipc_endpoint_free(server));
    CHECK_RESULT("free client", COG_OK, cog_ipc_endpoint_free(client));
    CHECK_RESULT("free listener", COG_OK, cog_ipc_endpoint_free(listener));
    (void)unlink(path);
}

/* ------------------------------------------------------------------ */
/* Full-duplex test (concurrent send/recv on both endpoints)           */
/* ------------------------------------------------------------------ */

struct duplex_arg {
    cog_ipc_endpoint_t *server;
    cog_ipc_message_t *out;
    cog_ipc_message_t *in;
    int result;
};

static void *duplex_thread(void *arg)
{
    struct duplex_arg *a = arg;

    /* server sends first, then recvs; both directions are independent. */
    a->result = 0;
    if (cog_ipc_endpoint_send(a->server, a->out) != COG_OK) {
        a->result = 1;
        return NULL;
    }
    if (cog_ipc_endpoint_recv(a->server, a->in) != COG_OK) {
        a->result = 2;
        return NULL;
    }
    return NULL;
}

static void test_full_duplex(void)
{
    char path[128];
    cog_ipc_endpoint_t *listener = NULL;
    cog_ipc_endpoint_t *client = NULL;
    cog_ipc_endpoint_t *server = NULL;
    cog_ipc_message_t cout;
    cog_ipc_message_t cin;
    cog_ipc_message_t sout;
    cog_ipc_message_t sin;
    struct duplex_arg arg;
    pthread_t th;
    const char *cmsg = "from-client";
    const char *smsg = "from-server";

    CHECK("make path", make_socket_path(path, sizeof(path)) == 0);
    CHECK_RESULT("new listener", COG_OK, cog_ipc_endpoint_new(&listener));
    CHECK_RESULT("listen", COG_OK, cog_ipc_endpoint_listen(listener, path));
    CHECK_RESULT("new client", COG_OK, cog_ipc_endpoint_new(&client));
    CHECK_RESULT("connect", COG_OK, cog_ipc_endpoint_connect(client, path, NULL));
    CHECK_RESULT("accept", COG_OK, cog_ipc_endpoint_accept(listener, &server, NULL));

    CHECK_RESULT("msg_new cout", COG_OK, cog_ipc_message_new(&cout, 64U));
    CHECK_RESULT("msg_new cin", COG_OK, cog_ipc_message_new(&cin, 64U));
    CHECK_RESULT("msg_new sout", COG_OK, cog_ipc_message_new(&sout, 64U));
    CHECK_RESULT("msg_new sin", COG_OK, cog_ipc_message_new(&sin, 64U));

    (void)memcpy(cout.data, cmsg, strlen(cmsg));
    cout.len = strlen(cmsg);
    (void)memcpy(sout.data, smsg, strlen(smsg));
    sout.len = strlen(smsg);

    arg.server = server;
    arg.out = &sout;
    arg.in = &sin;
    arg.result = -1;

    CHECK("pthread create", pthread_create(&th, NULL, duplex_thread, &arg) == 0);

    /* client sends and recvs concurrently with the server thread. */
    CHECK_RESULT("client send", COG_OK, cog_ipc_endpoint_send(client, &cout));
    CHECK_RESULT("client recv", COG_OK, cog_ipc_endpoint_recv(client, &cin));

    CHECK("pthread join", pthread_join(th, NULL) == 0);
    CHECK("duplex thread ok", arg.result == 0);

    CHECK("duplex cin len", cin.len == strlen(smsg));
    CHECK("duplex cin data", memcmp(cin.data, smsg, strlen(smsg)) == 0);
    CHECK("duplex sin len", sin.len == strlen(cmsg));
    CHECK("duplex sin data", memcmp(sin.data, cmsg, strlen(cmsg)) == 0);

    CHECK_RESULT("free msg cout", COG_OK, cog_ipc_message_free(&cout));
    CHECK_RESULT("free msg cin", COG_OK, cog_ipc_message_free(&cin));
    CHECK_RESULT("free msg sout", COG_OK, cog_ipc_message_free(&sout));
    CHECK_RESULT("free msg sin", COG_OK, cog_ipc_message_free(&sin));
    CHECK_RESULT("free server", COG_OK, cog_ipc_endpoint_free(server));
    CHECK_RESULT("free client", COG_OK, cog_ipc_endpoint_free(client));
    CHECK_RESULT("free listener", COG_OK, cog_ipc_endpoint_free(listener));
    (void)unlink(path);
}

/* ------------------------------------------------------------------ */
/* Partial-write test: a peer writes a frame in pieces; recv must      */
/* assemble it.                                                        */
/* ------------------------------------------------------------------ */

struct partial_arg {
    const char *path;
    int result;
};

static void *partial_thread(void *arg)
{
    struct partial_arg *a = arg;
    int fd = raw_connect(a->path);
    const char *payload = "partial-frame-payload";
    size_t len = strlen(payload);
    uint8_t header[4];

    a->result = 1;
    if (fd < 0) {
        return NULL;
    }

    header[0] = (uint8_t)((len >> 24) & 0xFFU);
    header[1] = (uint8_t)((len >> 16) & 0xFFU);
    header[2] = (uint8_t)((len >> 8) & 0xFFU);
    header[3] = (uint8_t)(len & 0xFFU);

    /* header */
    if (write(fd, header, sizeof(header)) != (ssize_t)sizeof(header)) {
        (void)close(fd);
        return NULL;
    }
    (void)nanosleep(&(struct timespec){0, 20000000L}, NULL);
    /* first half of payload */
    if (write(fd, payload, len / 2U) != (ssize_t)(len / 2U)) {
        (void)close(fd);
        return NULL;
    }
    (void)nanosleep(&(struct timespec){0, 20000000L}, NULL);
    /* second half */
    if (write(fd, payload + len / 2U, len - len / 2U) !=
        (ssize_t)(len - len / 2U)) {
        (void)close(fd);
        return NULL;
    }

    (void)close(fd);
    a->result = 0;
    return NULL;
}

static void test_partial_read(void)
{
    char path[128];
    cog_ipc_endpoint_t *listener = NULL;
    cog_ipc_endpoint_t *server = NULL;
    cog_ipc_message_t in;
    struct partial_arg arg;
    pthread_t th;
    const char *payload = "partial-frame-payload";
    size_t len = strlen(payload);

    CHECK("make path", make_socket_path(path, sizeof(path)) == 0);
    CHECK_RESULT("new listener", COG_OK, cog_ipc_endpoint_new(&listener));
    CHECK_RESULT("listen", COG_OK, cog_ipc_endpoint_listen(listener, path));
    CHECK_RESULT("msg_new in", COG_OK, cog_ipc_message_new(&in, 64U));

    arg.path = path;
    arg.result = -1;
    CHECK("pthread create", pthread_create(&th, NULL, partial_thread, &arg) == 0);

    CHECK_RESULT("accept", COG_OK, cog_ipc_endpoint_accept(listener, &server, NULL));
    CHECK_RESULT("recv partial", COG_OK, cog_ipc_endpoint_recv(server, &in));
    CHECK("partial len", in.len == len);
    CHECK("partial data", memcmp(in.data, payload, len) == 0);

    CHECK("pthread join", pthread_join(th, NULL) == 0);
    CHECK("partial thread ok", arg.result == 0);

    CHECK_RESULT("free msg in", COG_OK, cog_ipc_message_free(&in));
    CHECK_RESULT("free server", COG_OK, cog_ipc_endpoint_free(server));
    CHECK_RESULT("free listener", COG_OK, cog_ipc_endpoint_free(listener));
    (void)unlink(path);
}

/* ------------------------------------------------------------------ */
/* Overflow / malformed-frame test                                     */
/* ------------------------------------------------------------------ */

static void test_overflow(void)
{
    char path[128];
    cog_ipc_endpoint_t *listener = NULL;
    cog_ipc_endpoint_t *server = NULL;
    cog_ipc_message_t in;
    int fd;

    CHECK("make path", make_socket_path(path, sizeof(path)) == 0);
    CHECK_RESULT("new listener", COG_OK, cog_ipc_endpoint_new(&listener));
    CHECK_RESULT("listen", COG_OK, cog_ipc_endpoint_listen(listener, path));
    CHECK_RESULT("msg_new in", COG_OK, cog_ipc_message_new(&in, 16U));

    fd = raw_connect(path);
    CHECK("raw connect", fd >= 0);
    if (fd >= 0) {
        /* Declare a frame far larger than the 16-byte buffer. */
        CHECK("write huge header", raw_write_header(fd, 0x00010000U) == 0);
        (void)close(fd);
    }

    CHECK_RESULT("accept", COG_OK, cog_ipc_endpoint_accept(listener, &server, NULL));
    CHECK_RESULT("recv overflow", COG_ERROR_OVERFLOW,
                 cog_ipc_endpoint_recv(server, &in));

    /* The connection is closed after an overflow; recv now fails. */
    CHECK_RESULT("recv after overflow", COG_ERROR_INVALID_STATE,
                 cog_ipc_endpoint_recv(server, &in));

    CHECK_RESULT("free msg in", COG_OK, cog_ipc_message_free(&in));
    CHECK_RESULT("free server", COG_OK, cog_ipc_endpoint_free(server));
    CHECK_RESULT("free listener", COG_OK, cog_ipc_endpoint_free(listener));
    (void)unlink(path);
}

/* ------------------------------------------------------------------ */
/* Invalid-input tests for send / recv                                 */
/* ------------------------------------------------------------------ */

static void test_send_recv_invalid(void)
{
    cog_ipc_endpoint_t *e = NULL;
    cog_ipc_message_t msg;

    CHECK_RESULT("ep_new", COG_OK, cog_ipc_endpoint_new(&e));
    CHECK_RESULT("msg_new", COG_OK, cog_ipc_message_new(&msg, 16U));

    /* unbound endpoint -> INVALID_STATE */
    CHECK_RESULT("send unbound", COG_ERROR_INVALID_STATE,
                 cog_ipc_endpoint_send(e, &msg));
    CHECK_RESULT("recv unbound", COG_ERROR_INVALID_STATE,
                 cog_ipc_endpoint_recv(e, &msg));

    /* NULL args */
    CHECK_RESULT("send NULL ep", COG_ERROR_NULL_PTR,
                 cog_ipc_endpoint_send(NULL, &msg));
    CHECK_RESULT("send NULL msg", COG_ERROR_NULL_PTR,
                 cog_ipc_endpoint_send(e, NULL));
    CHECK_RESULT("recv NULL ep", COG_ERROR_NULL_PTR,
                 cog_ipc_endpoint_recv(NULL, &msg));
    CHECK_RESULT("recv NULL msg", COG_ERROR_NULL_PTR,
                 cog_ipc_endpoint_recv(e, NULL));

    /* msg->data NULL -> INVALID_ARG */
    {
        cog_ipc_message_t bad;
        (void)memset(&bad, 0, sizeof(bad));
        CHECK_RESULT("send data NULL", COG_ERROR_INVALID_ARG,
                     cog_ipc_endpoint_send(e, &bad));
        CHECK_RESULT("recv data NULL", COG_ERROR_INVALID_ARG,
                     cog_ipc_endpoint_recv(e, &bad));
    }

    /* len > capacity -> INVALID_ARG */
    msg.len = 17U; /* capacity is 16 */
    CHECK_RESULT("send len>cap", COG_ERROR_INVALID_ARG,
                 cog_ipc_endpoint_send(e, &msg));

    /* len > max payload -> INVALID_ARG */
    {
        cog_ipc_message_t big;
        CHECK_RESULT("msg_new big", COG_OK, cog_ipc_message_new(&big, 70000U));
        big.len = 70000U;
        CHECK_RESULT("send len>max", COG_ERROR_INVALID_ARG,
                     cog_ipc_endpoint_send(e, &big));
        CHECK_RESULT("free big", COG_OK, cog_ipc_message_free(&big));
    }

    CHECK_RESULT("free msg", COG_OK, cog_ipc_message_free(&msg));
    CHECK_RESULT("free ep", COG_OK, cog_ipc_endpoint_free(e));
}

/* ------------------------------------------------------------------ */
/* Poll tests                                                          */
/* ------------------------------------------------------------------ */

static void test_poll(void)
{
    char path[128];
    cog_ipc_endpoint_t *listener = NULL;
    cog_ipc_endpoint_t *client = NULL;
    cog_ipc_endpoint_t *server = NULL;
    cog_ipc_message_t out;
    cog_ipc_message_t in;
    int revents = 0;

    CHECK("make path", make_socket_path(path, sizeof(path)) == 0);
    CHECK_RESULT("new listener", COG_OK, cog_ipc_endpoint_new(&listener));
    CHECK_RESULT("listen", COG_OK, cog_ipc_endpoint_listen(listener, path));
    CHECK_RESULT("new client", COG_OK, cog_ipc_endpoint_new(&client));
    CHECK_RESULT("connect", COG_OK, cog_ipc_endpoint_connect(client, path, NULL));
    CHECK_RESULT("accept", COG_OK, cog_ipc_endpoint_accept(listener, &server, NULL));
    CHECK_RESULT("msg_new out", COG_OK, cog_ipc_message_new(&out, 16U));
    CHECK_RESULT("msg_new in", COG_OK, cog_ipc_message_new(&in, 16U));

    /* NULL endpoint */
    CHECK_RESULT("poll NULL", COG_ERROR_NULL_PTR,
                 cog_ipc_endpoint_poll(NULL, COG_IPC_POLL_IN, 0, &revents));
    /* zero events */
    CHECK_RESULT("poll zero events", COG_ERROR_INVALID_ARG,
                 cog_ipc_endpoint_poll(client, 0, 0, &revents));

    /* timeout on a quiet connected endpoint -> AGAIN */
    CHECK_RESULT("poll timeout", COG_ERROR_AGAIN,
                 cog_ipc_endpoint_poll(client, COG_IPC_POLL_IN, 10, &revents));

    /* client is writable */
    CHECK_RESULT("poll out", COG_OK,
                 cog_ipc_endpoint_poll(client, COG_IPC_POLL_OUT, 100, &revents));
    CHECK("poll out bit", (revents & COG_IPC_POLL_OUT) != 0);

    /* send data, then server is readable */
    (void)memcpy(out.data, "x", 1U);
    out.len = 1U;
    CHECK_RESULT("send", COG_OK, cog_ipc_endpoint_send(client, &out));
    CHECK_RESULT("poll in", COG_OK,
                 cog_ipc_endpoint_poll(server, COG_IPC_POLL_IN, 100, &revents));
    CHECK("poll in bit", (revents & COG_IPC_POLL_IN) != 0);

    /* listener is readable when a connection is pending */
    {
        cog_ipc_endpoint_t *c2 = NULL;
        CHECK_RESULT("new c2", COG_OK, cog_ipc_endpoint_new(&c2));
        CHECK_RESULT("connect c2", COG_OK,
                     cog_ipc_endpoint_connect(c2, path, NULL));
        CHECK_RESULT("poll listener", COG_OK,
                     cog_ipc_endpoint_poll(listener, COG_IPC_POLL_IN, 100,
                                           &revents));
        CHECK("poll listener in", (revents & COG_IPC_POLL_IN) != 0);
        CHECK_RESULT("free c2", COG_OK, cog_ipc_endpoint_free(c2));
    }

    CHECK_RESULT("free msg out", COG_OK, cog_ipc_message_free(&out));
    CHECK_RESULT("free msg in", COG_OK, cog_ipc_message_free(&in));
    CHECK_RESULT("free server", COG_OK, cog_ipc_endpoint_free(server));
    CHECK_RESULT("free client", COG_OK, cog_ipc_endpoint_free(client));
    CHECK_RESULT("free listener", COG_OK, cog_ipc_endpoint_free(listener));
    (void)unlink(path);
}

/* ------------------------------------------------------------------ */
/* Peer id tests                                                       */
/* ------------------------------------------------------------------ */

static void test_peer_id(void)
{
    char path[128];
    cog_ipc_endpoint_t *listener = NULL;
    cog_ipc_endpoint_t *client = NULL;
    cog_ipc_endpoint_t *server = NULL;
    cog_peer_id_t pid = COG_PEER_ID_INVALID;

    CHECK("make path", make_socket_path(path, sizeof(path)) == 0);
    CHECK_RESULT("new listener", COG_OK, cog_ipc_endpoint_new(&listener));
    CHECK_RESULT("new client", COG_OK, cog_ipc_endpoint_new(&client));

    /* unbound -> INVALID_STATE */
    CHECK_RESULT("peer id unbound", COG_ERROR_INVALID_STATE,
                 cog_ipc_endpoint_get_peer_id(client, &pid));
    /* NULL args */
    CHECK_RESULT("peer id NULL ep", COG_ERROR_NULL_PTR,
                 cog_ipc_endpoint_get_peer_id(NULL, &pid));
    CHECK_RESULT("peer id NULL out", COG_ERROR_NULL_PTR,
                 cog_ipc_endpoint_get_peer_id(client, NULL));

    CHECK_RESULT("listen", COG_OK, cog_ipc_endpoint_listen(listener, path));
    /* listener has no peer id -> INVALID_STATE */
    CHECK_RESULT("peer id listener", COG_ERROR_INVALID_STATE,
                 cog_ipc_endpoint_get_peer_id(listener, &pid));

    CHECK_RESULT("connect", COG_OK, cog_ipc_endpoint_connect(client, path, &pid));
    CHECK("connect pid", pid != COG_PEER_ID_INVALID);
    CHECK_RESULT("peer id client", COG_OK,
                 cog_ipc_endpoint_get_peer_id(client, &pid));
    CHECK("client pid non-invalid", pid != COG_PEER_ID_INVALID);

    CHECK_RESULT("accept", COG_OK, cog_ipc_endpoint_accept(listener, &server, &pid));
    CHECK("accept pid", pid != COG_PEER_ID_INVALID);
    CHECK_RESULT("peer id server", COG_OK,
                 cog_ipc_endpoint_get_peer_id(server, &pid));
    CHECK("server pid non-invalid", pid != COG_PEER_ID_INVALID);

    CHECK_RESULT("free server", COG_OK, cog_ipc_endpoint_free(server));
    CHECK_RESULT("free client", COG_OK, cog_ipc_endpoint_free(client));
    CHECK_RESULT("free listener", COG_OK, cog_ipc_endpoint_free(listener));
    (void)unlink(path);
}

/* ------------------------------------------------------------------ */
/* Broadcast test                                                      */
/* ------------------------------------------------------------------ */

static void test_broadcast(void)
{
    char path[128];
    cog_ipc_endpoint_t *listener = NULL;
    cog_ipc_endpoint_t *client1 = NULL;
    cog_ipc_endpoint_t *client2 = NULL;
    cog_ipc_endpoint_t *server1 = NULL;
    cog_ipc_endpoint_t *server2 = NULL;
    cog_ipc_message_t out;
    cog_ipc_message_t in;
    const char *payload = "broadcast!";

    CHECK("make path", make_socket_path(path, sizeof(path)) == 0);
    CHECK_RESULT("new listener", COG_OK, cog_ipc_endpoint_new(&listener));
    CHECK_RESULT("listen", COG_OK, cog_ipc_endpoint_listen(listener, path));

    CHECK_RESULT("new client1", COG_OK, cog_ipc_endpoint_new(&client1));
    CHECK_RESULT("connect1", COG_OK, cog_ipc_endpoint_connect(client1, path, NULL));
    CHECK_RESULT("accept1", COG_OK, cog_ipc_endpoint_accept(listener, &server1, NULL));
    CHECK_RESULT("new client2", COG_OK, cog_ipc_endpoint_new(&client2));
    CHECK_RESULT("connect2", COG_OK, cog_ipc_endpoint_connect(client2, path, NULL));
    CHECK_RESULT("accept2", COG_OK, cog_ipc_endpoint_accept(listener, &server2, NULL));

    CHECK_RESULT("msg_new out", COG_OK, cog_ipc_message_new(&out, 64U));
    CHECK_RESULT("msg_new in", COG_OK, cog_ipc_message_new(&in, 64U));
    (void)memcpy(out.data, payload, strlen(payload));
    out.len = strlen(payload);

    /* broadcast on a non-listener -> INVALID_STATE */
    CHECK_RESULT("broadcast non-listener", COG_ERROR_INVALID_STATE,
                 cog_ipc_broadcast(client1, &out));
    /* NULL args */
    CHECK_RESULT("broadcast NULL ep", COG_ERROR_NULL_PTR,
                 cog_ipc_broadcast(NULL, &out));
    CHECK_RESULT("broadcast NULL msg", COG_ERROR_NULL_PTR,
                 cog_ipc_broadcast(listener, NULL));

    CHECK_RESULT("broadcast", COG_OK, cog_ipc_broadcast(listener, &out));

    /* The listener sends to each accepted endpoint, so the data arrives
     * on the client side of each connection. */
    CHECK_RESULT("recv1", COG_OK, cog_ipc_endpoint_recv(client1, &in));
    CHECK("recv1 len", in.len == strlen(payload));
    CHECK("recv1 data", memcmp(in.data, payload, strlen(payload)) == 0);
    CHECK_RESULT("recv2", COG_OK, cog_ipc_endpoint_recv(client2, &in));
    CHECK("recv2 len", in.len == strlen(payload));
    CHECK("recv2 data", memcmp(in.data, payload, strlen(payload)) == 0);

    CHECK_RESULT("free msg out", COG_OK, cog_ipc_message_free(&out));
    CHECK_RESULT("free msg in", COG_OK, cog_ipc_message_free(&in));
    CHECK_RESULT("free server1", COG_OK, cog_ipc_endpoint_free(server1));
    CHECK_RESULT("free server2", COG_OK, cog_ipc_endpoint_free(server2));
    CHECK_RESULT("free client1", COG_OK, cog_ipc_endpoint_free(client1));
    CHECK_RESULT("free client2", COG_OK, cog_ipc_endpoint_free(client2));
    CHECK_RESULT("free listener", COG_OK, cog_ipc_endpoint_free(listener));
    (void)unlink(path);
}

/* ------------------------------------------------------------------ */
/* Session routing tests                                               */
/* ------------------------------------------------------------------ */

static void test_session_routing(void)
{
    char path[128];
    cog_ipc_endpoint_t *listener = NULL;
    cog_ipc_endpoint_t *client1 = NULL;
    cog_ipc_endpoint_t *client2 = NULL;
    cog_ipc_endpoint_t *server1 = NULL;
    cog_ipc_endpoint_t *server2 = NULL;
    cog_peer_id_t pid1 = COG_PEER_ID_INVALID;
    cog_peer_id_t pid2 = COG_PEER_ID_INVALID;
    cog_ipc_message_t out;
    cog_ipc_message_t in;
    const char *payload = "to-session";

    CHECK("make path", make_socket_path(path, sizeof(path)) == 0);
    CHECK_RESULT("new listener", COG_OK, cog_ipc_endpoint_new(&listener));
    CHECK_RESULT("listen", COG_OK, cog_ipc_endpoint_listen(listener, path));

    CHECK_RESULT("new client1", COG_OK, cog_ipc_endpoint_new(&client1));
    CHECK_RESULT("connect1", COG_OK, cog_ipc_endpoint_connect(client1, path, &pid1));
    CHECK_RESULT("accept1", COG_OK, cog_ipc_endpoint_accept(listener, &server1, &pid1));
    CHECK_RESULT("new client2", COG_OK, cog_ipc_endpoint_new(&client2));
    CHECK_RESULT("connect2", COG_OK, cog_ipc_endpoint_connect(client2, path, &pid2));
    CHECK_RESULT("accept2", COG_OK, cog_ipc_endpoint_accept(listener, &server2, &pid2));

    CHECK_RESULT("msg_new out", COG_OK, cog_ipc_message_new(&out, 64U));
    CHECK_RESULT("msg_new in", COG_OK, cog_ipc_message_new(&in, 64U));
    (void)memcpy(out.data, payload, strlen(payload));
    out.len = strlen(payload);

    /* register on a non-listener -> INVALID_STATE */
    CHECK_RESULT("register non-listener", COG_ERROR_INVALID_STATE,
                 cog_ipc_register_session(client1, "sess-a", pid1));
    /* NULL args */
    CHECK_RESULT("register NULL ep", COG_ERROR_NULL_PTR,
                 cog_ipc_register_session(NULL, "sess-a", pid1));
    CHECK_RESULT("register NULL id", COG_ERROR_NULL_PTR,
                 cog_ipc_register_session(listener, NULL, pid1));

    /* register two sessions */
    CHECK_RESULT("register a", COG_OK,
                 cog_ipc_register_session(listener, "sess-a", pid1));
    CHECK_RESULT("register b", COG_OK,
                 cog_ipc_register_session(listener, "sess-b", pid2));

    /* re-register updates the mapping (idempotent) */
    CHECK_RESULT("re-register a", COG_OK,
                 cog_ipc_register_session(listener, "sess-a", pid1));

    /* send to session a -> the peer hosting it; data arrives on client1. */
    CHECK_RESULT("send to a", COG_OK,
                 cog_ipc_send_to_session(listener, "sess-a", &out));
    CHECK_RESULT("recv1", COG_OK, cog_ipc_endpoint_recv(client1, &in));
    CHECK("recv1 len", in.len == strlen(payload));
    CHECK("recv1 data", memcmp(in.data, payload, strlen(payload)) == 0);

    /* send to session b -> the peer hosting it; data arrives on client2. */
    CHECK_RESULT("send to b", COG_OK,
                 cog_ipc_send_to_session(listener, "sess-b", &out));
    CHECK_RESULT("recv2", COG_OK, cog_ipc_endpoint_recv(client2, &in));
    CHECK("recv2 len", in.len == strlen(payload));

    /* unknown session -> NOT_FOUND */
    CHECK_RESULT("send unknown", COG_ERROR_NOT_FOUND,
                 cog_ipc_send_to_session(listener, "nope", &out));

    /* NULL args for send_to_session */
    CHECK_RESULT("send_to NULL ep", COG_ERROR_NULL_PTR,
                 cog_ipc_send_to_session(NULL, "sess-a", &out));
    CHECK_RESULT("send_to NULL id", COG_ERROR_NULL_PTR,
                 cog_ipc_send_to_session(listener, NULL, &out));
    CHECK_RESULT("send_to NULL msg", COG_ERROR_NULL_PTR,
                 cog_ipc_send_to_session(listener, "sess-a", NULL));

    /* unregister */
    CHECK_RESULT("unregister a", COG_OK,
                 cog_ipc_unregister_session(listener, "sess-a"));
    CHECK_RESULT("unregister a again", COG_ERROR_NOT_FOUND,
                 cog_ipc_unregister_session(listener, "sess-a"));
    CHECK_RESULT("unregister NULL ep", COG_ERROR_NULL_PTR,
                 cog_ipc_unregister_session(NULL, "sess-a"));
    CHECK_RESULT("unregister NULL id", COG_ERROR_NULL_PTR,
                 cog_ipc_unregister_session(listener, NULL));

    /* after unregister, send to a -> NOT_FOUND */
    CHECK_RESULT("send after unregister", COG_ERROR_NOT_FOUND,
                 cog_ipc_send_to_session(listener, "sess-a", &out));

    CHECK_RESULT("free msg out", COG_OK, cog_ipc_message_free(&out));
    CHECK_RESULT("free msg in", COG_OK, cog_ipc_message_free(&in));
    CHECK_RESULT("free server1", COG_OK, cog_ipc_endpoint_free(server1));
    CHECK_RESULT("free server2", COG_OK, cog_ipc_endpoint_free(server2));
    CHECK_RESULT("free client1", COG_OK, cog_ipc_endpoint_free(client1));
    CHECK_RESULT("free client2", COG_OK, cog_ipc_endpoint_free(client2));
    CHECK_RESULT("free listener", COG_OK, cog_ipc_endpoint_free(listener));
    (void)unlink(path);
}

static void test_session_overflow(void)
{
    char path[128];
    cog_ipc_endpoint_t *listener = NULL;
    cog_ipc_endpoint_t *client = NULL;
    cog_ipc_endpoint_t *server = NULL;
    cog_peer_id_t pid = COG_PEER_ID_INVALID;
    char id[64];
    int i;

    CHECK("make path", make_socket_path(path, sizeof(path)) == 0);
    CHECK_RESULT("new listener", COG_OK, cog_ipc_endpoint_new(&listener));
    CHECK_RESULT("listen", COG_OK, cog_ipc_endpoint_listen(listener, path));
    CHECK_RESULT("new client", COG_OK, cog_ipc_endpoint_new(&client));
    CHECK_RESULT("connect", COG_OK, cog_ipc_endpoint_connect(client, path, &pid));
    CHECK_RESULT("accept", COG_OK, cog_ipc_endpoint_accept(listener, &server, &pid));

    /* Fill the session table (64 entries). */
    for (i = 0; i < 64; i++) {
        (void)snprintf(id, sizeof(id), "sess-%d", i);
        CHECK_RESULT("register fill", COG_OK,
                     cog_ipc_register_session(listener, id, pid));
    }

    /* The 65th must overflow. */
    CHECK_RESULT("register overflow", COG_ERROR_OVERFLOW,
                 cog_ipc_register_session(listener, "overflow", pid));

    /* Re-registering an existing id still succeeds (updates, not insert). */
    CHECK_RESULT("re-register existing", COG_OK,
                 cog_ipc_register_session(listener, "sess-0", pid));

    CHECK_RESULT("free server", COG_OK, cog_ipc_endpoint_free(server));
    CHECK_RESULT("free client", COG_OK, cog_ipc_endpoint_free(client));
    CHECK_RESULT("free listener", COG_OK, cog_ipc_endpoint_free(listener));
    (void)unlink(path);
}

/* ------------------------------------------------------------------ */
/* Disconnect / reconnect / crash-like tests                           */
/* ------------------------------------------------------------------ */

static void test_disconnect_reconnect(void)
{
    char path[128];
    cog_ipc_endpoint_t *listener = NULL;
    cog_ipc_endpoint_t *client = NULL;
    cog_ipc_endpoint_t *server = NULL;
    cog_ipc_message_t out;
    cog_ipc_message_t in;
    const char *payload = "again";

    CHECK("make path", make_socket_path(path, sizeof(path)) == 0);
    CHECK_RESULT("new listener", COG_OK, cog_ipc_endpoint_new(&listener));
    CHECK_RESULT("listen", COG_OK, cog_ipc_endpoint_listen(listener, path));
    CHECK_RESULT("msg_new out", COG_OK, cog_ipc_message_new(&out, 64U));
    CHECK_RESULT("msg_new in", COG_OK, cog_ipc_message_new(&in, 64U));
    (void)memcpy(out.data, payload, strlen(payload));
    out.len = strlen(payload);

    /* First connection. */
    CHECK_RESULT("new client", COG_OK, cog_ipc_endpoint_new(&client));
    CHECK_RESULT("connect", COG_OK, cog_ipc_endpoint_connect(client, path, NULL));
    CHECK_RESULT("accept", COG_OK, cog_ipc_endpoint_accept(listener, &server, NULL));

    /* disconnect the client; it returns to unbound state. */
    CHECK_RESULT("disconnect client", COG_OK, cog_ipc_endpoint_disconnect(client));
    /* send on a disconnected endpoint -> INVALID_STATE */
    CHECK_RESULT("send disconnected", COG_ERROR_INVALID_STATE,
                 cog_ipc_endpoint_send(client, &out));
    /* disconnect again -> INVALID_STATE */
    CHECK_RESULT("disconnect twice", COG_ERROR_INVALID_STATE,
                 cog_ipc_endpoint_disconnect(client));

    /* The server side sees the peer close (recv returns IO). */
    CHECK_RESULT("recv closed", COG_ERROR_IO, cog_ipc_endpoint_recv(server, &in));
    CHECK_RESULT("free old server", COG_OK, cog_ipc_endpoint_free(server));

    /* Reconnect on the same client endpoint. */
    CHECK_RESULT("reconnect", COG_OK, cog_ipc_endpoint_connect(client, path, NULL));
    CHECK_RESULT("accept again", COG_OK, cog_ipc_endpoint_accept(listener, &server, NULL));
    CHECK_RESULT("send after reconnect", COG_OK, cog_ipc_endpoint_send(client, &out));
    CHECK_RESULT("recv after reconnect", COG_OK, cog_ipc_endpoint_recv(server, &in));
    CHECK("reconnect len", in.len == strlen(payload));

    CHECK_RESULT("free msg out", COG_OK, cog_ipc_message_free(&out));
    CHECK_RESULT("free msg in", COG_OK, cog_ipc_message_free(&in));
    CHECK_RESULT("free server", COG_OK, cog_ipc_endpoint_free(server));
    CHECK_RESULT("free client", COG_OK, cog_ipc_endpoint_free(client));
    CHECK_RESULT("free listener", COG_OK, cog_ipc_endpoint_free(listener));
    (void)unlink(path);
}

static void test_crash_like(void)
{
    char path[128];
    cog_ipc_endpoint_t *listener = NULL;
    cog_ipc_endpoint_t *server = NULL;
    cog_ipc_message_t in;
    int fd;

    CHECK("make path", make_socket_path(path, sizeof(path)) == 0);
    CHECK_RESULT("new listener", COG_OK, cog_ipc_endpoint_new(&listener));
    CHECK_RESULT("listen", COG_OK, cog_ipc_endpoint_listen(listener, path));
    CHECK_RESULT("msg_new in", COG_OK, cog_ipc_message_new(&in, 64U));

    /* A peer connects and then closes without any handshake. */
    fd = raw_connect(path);
    CHECK("raw connect", fd >= 0);
    if (fd >= 0) {
        (void)close(fd);
    }

    CHECK_RESULT("accept", COG_OK, cog_ipc_endpoint_accept(listener, &server, NULL));
    /* recv on the accepted endpoint sees the peer gone -> IO. */
    CHECK_RESULT("recv crash", COG_ERROR_IO, cog_ipc_endpoint_recv(server, &in));

    CHECK_RESULT("free msg in", COG_OK, cog_ipc_message_free(&in));
    CHECK_RESULT("free server", COG_OK, cog_ipc_endpoint_free(server));
    CHECK_RESULT("free listener", COG_OK, cog_ipc_endpoint_free(listener));
    (void)unlink(path);
}

/* ------------------------------------------------------------------ */
/* Listener disconnect detaches accepted endpoints                     */
/* ------------------------------------------------------------------ */

static void test_listener_disconnect_detaches(void)
{
    char path[128];
    cog_ipc_endpoint_t *listener = NULL;
    cog_ipc_endpoint_t *client = NULL;
    cog_ipc_endpoint_t *server = NULL;
    cog_ipc_message_t out;
    cog_ipc_message_t in;
    const char *payload = "detached";

    CHECK("make path", make_socket_path(path, sizeof(path)) == 0);
    CHECK_RESULT("new listener", COG_OK, cog_ipc_endpoint_new(&listener));
    CHECK_RESULT("listen", COG_OK, cog_ipc_endpoint_listen(listener, path));
    CHECK_RESULT("new client", COG_OK, cog_ipc_endpoint_new(&client));
    CHECK_RESULT("connect", COG_OK, cog_ipc_endpoint_connect(client, path, NULL));
    CHECK_RESULT("accept", COG_OK, cog_ipc_endpoint_accept(listener, &server, NULL));
    CHECK_RESULT("msg_new out", COG_OK, cog_ipc_message_new(&out, 64U));
    CHECK_RESULT("msg_new in", COG_OK, cog_ipc_message_new(&in, 64U));
    (void)memcpy(out.data, payload, strlen(payload));
    out.len = strlen(payload);

    /* Disconnecting the listener detaches the accepted endpoint. */
    CHECK_RESULT("disconnect listener", COG_OK, cog_ipc_endpoint_disconnect(listener));

    /* The accepted endpoint is now independent: it can still send/recv. */
    CHECK_RESULT("send detached", COG_OK, cog_ipc_endpoint_send(server, &out));
    CHECK_RESULT("recv detached", COG_OK, cog_ipc_endpoint_recv(client, &in));
    CHECK("detached len", in.len == strlen(payload));

    /* The listener can be reused. */
    CHECK_RESULT("listen again", COG_OK, cog_ipc_endpoint_listen(listener, path));

    CHECK_RESULT("free msg out", COG_OK, cog_ipc_message_free(&out));
    CHECK_RESULT("free msg in", COG_OK, cog_ipc_message_free(&in));
    CHECK_RESULT("free server", COG_OK, cog_ipc_endpoint_free(server));
    CHECK_RESULT("free client", COG_OK, cog_ipc_endpoint_free(client));
    CHECK_RESULT("free listener", COG_OK, cog_ipc_endpoint_free(listener));
    (void)unlink(path);
}

/* ------------------------------------------------------------------ */
/* main                                                               */
/* ------------------------------------------------------------------ */

int main(void)
{
    test_message_new_free();
    test_endpoint_new_free();
    test_listen_connect_accept();
    test_roundtrip();
    test_framing_multiple();
    test_large_message();
    test_full_duplex();
    test_partial_read();
    test_overflow();
    test_send_recv_invalid();
    test_poll();
    test_peer_id();
    test_broadcast();
    test_session_routing();
    test_session_overflow();
    test_disconnect_reconnect();
    test_crash_like();
    test_listener_disconnect_detaches();

    if (g_failures == 0U) {
        (void)printf("test_ipc: ok\n");
        return 0;
    }
    (void)fprintf(stderr, "test_ipc: %u failure(s)\n", g_failures);
    return 1;
}

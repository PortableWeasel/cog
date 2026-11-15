/**
 * @file test_ipc_critic.c
 * @brief Critic adversarial tests for cog/ipc.h (ticket 7856a39, writer 4de26fd).
 *
 * Targets gaps in tests/unit/test_ipc.c:
 *   - SIGPIPE safety: sending/broadcasting to a peer whose remote end closed
 *     must NOT kill the process (scenarios 3/7/9/10/11: crash / disconnect /
 *     reconnect).  The library uses write(2) with the default SIGPIPE
 *     disposition, which terminates the process on EPIPE.  Critical.
 *   - broadcast must drop dead peers and keep delivering to live ones.
 *   - frame boundary 65535 exactly (inclusive max) and 65536 rejected.
 *   - recv capacity overflow (payload <= MAX but > caller capacity) closes
 *     the connection with COG_ERROR_OVERFLOW.
 *   - freeing an accepted endpoint removes it from the listener's peer
 *     table (no dangling pointer on later broadcast).
 *   - send_to_session after the hosting peer is freed -> COG_ERROR_IO.
 *   - listener free (not just disconnect) while accepted endpoints are live:
 *     accepted endpoints stay usable; no use-after-free / double-close.
 *   - partial frame header then EOF -> COG_ERROR_IO, endpoint disconnected.
 *   - session id length boundary: 127 chars ok, 128 -> INVALID_ARG.
 *   - peer ids strictly increment per accept (1, 2, ...), no reuse after
 *     disconnect of an earlier peer.
 *
 * The SIGPIPE assertions run in a forked child with the default signal
 * disposition (what a real embedded app gets: the library installs no
 * handler), so a buggy transport crashes the child, not the test runner.
 *
 * Not wired into tests/unit/CMakeLists.txt: the shared tests CMake is owned
 * by the integration ticket (dd2f55e), so this file is compiled directly:
 *
 *   gcc -std=c11 -Wall -Wextra -Werror -I libraries/cog-ipc/include \
 *       -I libraries/cog-core/include tests/unit/test_ipc_critic.c \
 *       libraries/cog-ipc/src/ipc.c -lpthread -o test_ipc_critic
 *
 * Ticket: 7856a39 (Stage 12.3a review of 4de26fd).
 */

#define _POSIX_C_SOURCE 200809L

#include "cog/ipc.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

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

static int make_socket_path(char *buf, size_t bufsize)
{
    char tmpl[] = "/tmp/cog-ipc-critic-XXXXXX";
    int fd = mkstemp(tmpl);

    if (fd < 0) {
        return -1;
    }
    (void)close(fd);
    (void)unlink(tmpl);
    (void)snprintf(buf, bufsize, "%s", tmpl);
    return 0;
}

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

/* ------------------------------------------------------------------ */
/* SIGPIPE safety                                                      */
/* ------------------------------------------------------------------ */

/**
 * Child with the DEFAULT SIGPIPE disposition: broadcast to an accepted
 * endpoint whose remote end closed.  Returns 0 only if the process survives
 * every send (a correct transport must not die on peer churn).  Dies with
 * SIGPIPE when the bug is present.
 */
static int sigpipe_default_child(const char *path)
{
    cog_ipc_endpoint_t *listener = NULL;
    cog_ipc_endpoint_t *server = NULL;
    cog_ipc_message_t msg;
    int i;

    if (cog_ipc_endpoint_new(&listener) != COG_OK) {
        return 2;
    }
    if (cog_ipc_endpoint_listen(listener, path) != COG_OK) {
        return 3;
    }
    if (cog_ipc_message_new(&msg, 70000U) != COG_OK) {
        return 4;
    }
    (void)memset(msg.data, 'x', 60000U);
    msg.len = 60000U;

    {
        int fd = raw_connect(path);

        if (fd < 0) {
            return 5;
        }
        /* A byte the server never reads, then close: guarantees RST. */
        if (write(fd, "z", 1) != 1) {
            (void)close(fd);
            return 6;
        }
        (void)close(fd);
    }

    if (cog_ipc_endpoint_accept(listener, &server, NULL) != COG_OK) {
        return 7;
    }
    (void)nanosleep(&(struct timespec){0, 100000000L}, NULL);

    /* Drive many large frames at the dead peer.  Any EPIPE with the default
     * disposition kills the process; a correct transport returns an error. */
    for (i = 0; i < 32; i++) {
        (void)cog_ipc_broadcast(listener, &msg);
    }

    (void)cog_ipc_message_free(&msg);
    (void)cog_ipc_endpoint_free(server);
    (void)cog_ipc_endpoint_free(listener);
    return 0;
}

/**
 * Child with SIGPIPE ignored (a workaround the library must not require its
 * users to install): broadcast must drop the dead peer and keep serving the
 * live one.
 */
static int sigpipe_ign_child(const char *path)
{
    cog_ipc_endpoint_t *listener = NULL;
    cog_ipc_endpoint_t *clientA = NULL;
    cog_ipc_endpoint_t *clientB = NULL;
    cog_ipc_endpoint_t *serverA = NULL;
    cog_ipc_endpoint_t *serverB = NULL;
    cog_ipc_message_t out;
    cog_ipc_message_t in;
    int i;

    (void)signal(SIGPIPE, SIG_IGN);

    if (cog_ipc_endpoint_new(&listener) != COG_OK) {
        return 2;
    }
    if (cog_ipc_endpoint_listen(listener, path) != COG_OK) {
        return 3;
    }
    if (cog_ipc_endpoint_new(&clientA) != COG_OK ||
        cog_ipc_endpoint_new(&clientB) != COG_OK) {
        return 4;
    }
    if (cog_ipc_endpoint_connect(clientA, path, NULL) != COG_OK ||
        cog_ipc_endpoint_connect(clientB, path, NULL) != COG_OK) {
        return 5;
    }
    if (cog_ipc_endpoint_accept(listener, &serverA, NULL) != COG_OK ||
        cog_ipc_endpoint_accept(listener, &serverB, NULL) != COG_OK) {
        return 6;
    }
    if (cog_ipc_message_new(&out, 70000U) != COG_OK ||
        cog_ipc_message_new(&in, 70000U) != COG_OK) {
        return 7;
    }
    (void)memset(out.data, 'y', 4096U);
    out.len = 4096U;

    /* Kill peer A's connection from the client side. */
    if (cog_ipc_endpoint_disconnect(clientA) != COG_OK) {
        return 8;
    }
    (void)nanosleep(&(struct timespec){0, 50000000L}, NULL);

    /* Broadcast repeatedly; serverA must be dropped, serverB must live.
     * Frames are kept small so the live peer's socket buffer never fills
     * while its client is not reading concurrently. */
    for (i = 0; i < 24; i++) {
        if (cog_ipc_broadcast(listener, &out) != COG_OK) {
            return 9;
        }
    }

    /* Peer B must still receive (its connection survived the dead neighbour). */
    if (cog_ipc_endpoint_recv(clientB, &in) != COG_OK) {
        return 10;
    }
    if (in.len != 4096U) {
        return 11;
    }

    (void)cog_ipc_message_free(&out);
    (void)cog_ipc_message_free(&in);
    (void)cog_ipc_endpoint_free(serverA);
    (void)cog_ipc_endpoint_free(serverB);
    (void)cog_ipc_endpoint_free(clientB);
    (void)cog_ipc_endpoint_free(listener);
    return 0;
}

static void test_sigpipe_safety(void)
{
    char path[128];
    pid_t pid;
    int status;

    CHECK("make path", make_socket_path(path, sizeof(path)) == 0);

    /* Default disposition: the transport must not die. */
    pid = fork();
    CHECK("fork default", pid >= 0);
    if (pid == 0) {
        _exit(sigpipe_default_child(path));
    }
    CHECK("wait default", waitpid(pid, &status, 0) == pid);
    if (WIFSIGNALED(status)) {
        (void)fprintf(stderr,
                      "FAIL: send/broadcast to dead peer killed process with "
                      "signal %d (SIGPIPE=%d) under default disposition\n",
                      WTERMSIG(status), SIGPIPE);
        ++g_failures;
    } else {
        CHECK("sigpipe default child ok", WEXITSTATUS(status) == 0);
    }

    /* Ignore-disposition workaround: dead peers dropped, live peers served. */
    pid = fork();
    CHECK("fork ignored", pid >= 0);
    if (pid == 0) {
        _exit(sigpipe_ign_child(path));
    }
    CHECK("wait ignored", waitpid(pid, &status, 0) == pid);
    if (WIFSIGNALED(status)) {
        (void)fprintf(stderr, "FAIL: sigpipe-ignored child killed by signal %d\n",
                      WTERMSIG(status));
        ++g_failures;
    } else {
        CHECK("sigpipe ignored child ok", WEXITSTATUS(status) == 0);
    }

    (void)unlink(path);
}

/* ------------------------------------------------------------------ */
/* Frame boundary: 65535 inclusive max                                 */
/* ------------------------------------------------------------------ */

static void test_frame_boundary(void)
{
    char path[128];
    cog_ipc_endpoint_t *listener = NULL;
    cog_ipc_endpoint_t *client = NULL;
    cog_ipc_endpoint_t *server = NULL;
    cog_ipc_message_t out;
    cog_ipc_message_t in;

    CHECK("make path", make_socket_path(path, sizeof(path)) == 0);
    CHECK_RESULT("new listener", COG_OK, cog_ipc_endpoint_new(&listener));
    CHECK_RESULT("listen", COG_OK, cog_ipc_endpoint_listen(listener, path));
    CHECK_RESULT("new client", COG_OK, cog_ipc_endpoint_new(&client));
    CHECK_RESULT("connect", COG_OK, cog_ipc_endpoint_connect(client, path, NULL));
    CHECK_RESULT("accept", COG_OK, cog_ipc_endpoint_accept(listener, &server, NULL));
    CHECK_RESULT("msg_new out", COG_OK, cog_ipc_message_new(&out, 65535U));
    CHECK_RESULT("msg_new in", COG_OK, cog_ipc_message_new(&in, 65535U));

    (void)memset(out.data, 'b', 65535U);
    out.len = 65535U;

    /* 65535 is the inclusive maximum: must round-trip. */
    CHECK_RESULT("send 65535", COG_OK, cog_ipc_endpoint_send(client, &out));
    CHECK_RESULT("recv 65535", COG_OK, cog_ipc_endpoint_recv(server, &in));
    CHECK("recv 65535 len", in.len == 65535U);

    /* One past the max is rejected before any I/O. */
    out.len = 65536U;
    CHECK_RESULT("send 65536", COG_ERROR_INVALID_ARG,
                 cog_ipc_endpoint_send(client, &out));

    CHECK_RESULT("free msg out", COG_OK, cog_ipc_message_free(&out));
    CHECK_RESULT("free msg in", COG_OK, cog_ipc_message_free(&in));
    CHECK_RESULT("free server", COG_OK, cog_ipc_endpoint_free(server));
    CHECK_RESULT("free client", COG_OK, cog_ipc_endpoint_free(client));
    CHECK_RESULT("free listener", COG_OK, cog_ipc_endpoint_free(listener));
    (void)unlink(path);
}

/* ------------------------------------------------------------------ */
/* recv overflow vs. caller capacity (payload <= MAX but > capacity)   */
/* ------------------------------------------------------------------ */

static void test_recv_capacity_overflow(void)
{
    char path[128];
    cog_ipc_endpoint_t *listener = NULL;
    cog_ipc_endpoint_t *server = NULL;
    cog_ipc_message_t out;
    cog_ipc_message_t in;
    int fd;

    CHECK("make path", make_socket_path(path, sizeof(path)) == 0);
    CHECK_RESULT("new listener", COG_OK, cog_ipc_endpoint_new(&listener));
    CHECK_RESULT("listen", COG_OK, cog_ipc_endpoint_listen(listener, path));
    CHECK_RESULT("msg_new out", COG_OK, cog_ipc_message_new(&out, 60000U));
    CHECK_RESULT("msg_new in small", COG_OK, cog_ipc_message_new(&in, 16U));

    (void)memset(out.data, 'c', 60000U);
    out.len = 60000U;

    fd = raw_connect(path);
    CHECK("raw connect", fd >= 0);
    if (fd >= 0) {
        /* 60000 <= MAX(65535) but > 16-byte caller capacity. */
        uint8_t header[4];
        header[0] = (uint8_t)((60000U >> 24) & 0xFFU);
        header[1] = (uint8_t)((60000U >> 16) & 0xFFU);
        header[2] = (uint8_t)((60000U >> 8) & 0xFFU);
        header[3] = (uint8_t)(60000U & 0xFFU);
        CHECK("write header", write(fd, header, 4) == 4);
        CHECK("write payload", write(fd, out.data, 60000U) == (ssize_t)60000U);
        (void)close(fd);
    }

    CHECK_RESULT("accept", COG_OK, cog_ipc_endpoint_accept(listener, &server, NULL));
    CHECK_RESULT("recv cap overflow", COG_ERROR_OVERFLOW,
                 cog_ipc_endpoint_recv(server, &in));
    /* Overflow closes the connection. */
    CHECK_RESULT("recv after cap overflow", COG_ERROR_INVALID_STATE,
                 cog_ipc_endpoint_recv(server, &in));

    CHECK_RESULT("free msg out", COG_OK, cog_ipc_message_free(&out));
    CHECK_RESULT("free msg in", COG_OK, cog_ipc_message_free(&in));
    CHECK_RESULT("free server", COG_OK, cog_ipc_endpoint_free(server));
    CHECK_RESULT("free listener", COG_OK, cog_ipc_endpoint_free(listener));
    (void)unlink(path);
}

/* ------------------------------------------------------------------ */
/* Freeing an accepted endpoint must remove it from the peer table     */
/* ------------------------------------------------------------------ */

static void test_freed_peer_broadcast(void)
{
    char path[128];
    cog_ipc_endpoint_t *listener = NULL;
    cog_ipc_endpoint_t *client1 = NULL;
    cog_ipc_endpoint_t *client2 = NULL;
    cog_ipc_endpoint_t *server1 = NULL;
    cog_ipc_endpoint_t *server2 = NULL;
    cog_ipc_message_t out;
    cog_ipc_message_t in;
    const char *payload = "to-the-survivor";

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

    /* Free peer 1 outright; its connection must vanish from the table. */
    CHECK_RESULT("free client1", COG_OK, cog_ipc_endpoint_free(client1));
    CHECK_RESULT("free server1", COG_OK, cog_ipc_endpoint_free(server1));

    CHECK_RESULT("broadcast after free", COG_OK, cog_ipc_broadcast(listener, &out));
    CHECK_RESULT("recv survivor", COG_OK, cog_ipc_endpoint_recv(client2, &in));
    CHECK("survivor len", in.len == strlen(payload));
    CHECK("survivor data", memcmp(in.data, payload, strlen(payload)) == 0);

    CHECK_RESULT("free msg out", COG_OK, cog_ipc_message_free(&out));
    CHECK_RESULT("free msg in", COG_OK, cog_ipc_message_free(&in));
    CHECK_RESULT("free server2", COG_OK, cog_ipc_endpoint_free(server2));
    CHECK_RESULT("free client2", COG_OK, cog_ipc_endpoint_free(client2));
    CHECK_RESULT("free listener", COG_OK, cog_ipc_endpoint_free(listener));
    (void)unlink(path);
}

/* ------------------------------------------------------------------ */
/* send_to_session after the hosting peer is freed -> COG_ERROR_IO     */
/* ------------------------------------------------------------------ */

static void test_stale_session(void)
{
    char path[128];
    cog_ipc_endpoint_t *listener = NULL;
    cog_ipc_endpoint_t *client = NULL;
    cog_ipc_endpoint_t *server = NULL;
    cog_peer_id_t pid = COG_PEER_ID_INVALID;
    cog_ipc_message_t out;

    CHECK("make path", make_socket_path(path, sizeof(path)) == 0);
    CHECK_RESULT("new listener", COG_OK, cog_ipc_endpoint_new(&listener));
    CHECK_RESULT("listen", COG_OK, cog_ipc_endpoint_listen(listener, path));
    CHECK_RESULT("new client", COG_OK, cog_ipc_endpoint_new(&client));
    CHECK_RESULT("connect", COG_OK, cog_ipc_endpoint_connect(client, path, &pid));
    CHECK_RESULT("accept", COG_OK, cog_ipc_endpoint_accept(listener, &server, &pid));
    CHECK_RESULT("msg_new out", COG_OK, cog_ipc_message_new(&out, 16U));
    out.len = 0U;

    CHECK_RESULT("register session", COG_OK,
                 cog_ipc_register_session(listener, "sess", pid));

    /* Unregister + not-found path. */
    CHECK_RESULT("send unknown session", COG_ERROR_NOT_FOUND,
                 cog_ipc_send_to_session(listener, "nope", &out));
    CHECK_RESULT("unregister", COG_OK,
                 cog_ipc_unregister_session(listener, "sess"));

    /* Re-register, then kill the hosting peer. */
    CHECK_RESULT("register again", COG_OK,
                 cog_ipc_register_session(listener, "sess", pid));
    CHECK_RESULT("free server", COG_OK, cog_ipc_endpoint_free(server));
    CHECK_RESULT("free client", COG_OK, cog_ipc_endpoint_free(client));

    /* Session still references a peer id that is gone -> IO. */
    CHECK_RESULT("send to stale session", COG_ERROR_IO,
                 cog_ipc_send_to_session(listener, "sess", &out));

    CHECK_RESULT("free msg out", COG_OK, cog_ipc_message_free(&out));
    CHECK_RESULT("free listener", COG_OK, cog_ipc_endpoint_free(listener));
    (void)unlink(path);
}

/* ------------------------------------------------------------------ */
/* Listener free with live accepted endpoints                          */
/* ------------------------------------------------------------------ */

static void test_listener_free_lifetime(void)
{
    char path[128];
    cog_ipc_endpoint_t *listener = NULL;
    cog_ipc_endpoint_t *client = NULL;
    cog_ipc_endpoint_t *server = NULL;
    cog_ipc_message_t out;
    cog_ipc_message_t in;
    const char *payload = "still-alive";

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

    /* Freeing the listener (not just disconnect) must detach, not close,
     * the accepted endpoint's fd. */
    CHECK_RESULT("free listener", COG_OK, cog_ipc_endpoint_free(listener));

    /* The accepted endpoint is now an independent, still-open connection. */
    CHECK_RESULT("send after listener free", COG_OK,
                 cog_ipc_endpoint_send(server, &out));
    CHECK_RESULT("recv after listener free", COG_OK,
                 cog_ipc_endpoint_recv(client, &in));
    CHECK("post-free len", in.len == strlen(payload));

    /* Both ends release cleanly: no double close, no use-after-free. */
    CHECK_RESULT("free server", COG_OK, cog_ipc_endpoint_free(server));
    CHECK_RESULT("free client", COG_OK, cog_ipc_endpoint_free(client));

    CHECK_RESULT("free msg out", COG_OK, cog_ipc_message_free(&out));
    CHECK_RESULT("free msg in", COG_OK, cog_ipc_message_free(&in));
    (void)unlink(path);
}

/* ------------------------------------------------------------------ */
/* Partial frame header then EOF                                       */
/* ------------------------------------------------------------------ */

struct partial_header_arg {
    const char *path;
    int result;
};

static void *partial_header_thread(void *arg)
{
    struct partial_header_arg *a = arg;
    int fd = raw_connect(a->path);

    a->result = 1;
    if (fd < 0) {
        return NULL;
    }
    /* Two bytes of a four-byte header, then EOF. */
    if (write(fd, "\x00\x01", 2) != 2) {
        (void)close(fd);
        return NULL;
    }
    (void)close(fd);
    a->result = 0;
    return NULL;
}

static void test_partial_header(void)
{
    char path[128];
    cog_ipc_endpoint_t *listener = NULL;
    cog_ipc_endpoint_t *server = NULL;
    cog_ipc_message_t in;
    struct partial_header_arg arg;
    pthread_t th;

    CHECK("make path", make_socket_path(path, sizeof(path)) == 0);
    CHECK_RESULT("new listener", COG_OK, cog_ipc_endpoint_new(&listener));
    CHECK_RESULT("listen", COG_OK, cog_ipc_endpoint_listen(listener, path));
    CHECK_RESULT("msg_new in", COG_OK, cog_ipc_message_new(&in, 64U));

    arg.path = path;
    arg.result = -1;
    CHECK("pthread create", pthread_create(&th, NULL, partial_header_thread, &arg) == 0);

    CHECK_RESULT("accept", COG_OK, cog_ipc_endpoint_accept(listener, &server, NULL));
    CHECK_RESULT("recv partial header", COG_ERROR_IO,
                 cog_ipc_endpoint_recv(server, &in));
    /* Connection closed by the transport after the mid-header EOF. */
    CHECK_RESULT("recv after", COG_ERROR_INVALID_STATE,
                 cog_ipc_endpoint_recv(server, &in));

    CHECK("pthread join", pthread_join(th, NULL) == 0);
    CHECK("partial header thread ok", arg.result == 0);

    CHECK_RESULT("free msg in", COG_OK, cog_ipc_message_free(&in));
    CHECK_RESULT("free server", COG_OK, cog_ipc_endpoint_free(server));
    CHECK_RESULT("free listener", COG_OK, cog_ipc_endpoint_free(listener));
    (void)unlink(path);
}

/* ------------------------------------------------------------------ */
/* Session id length boundary                                          */
/* ------------------------------------------------------------------ */

static void test_session_id_length(void)
{
    char path[128];
    cog_ipc_endpoint_t *listener = NULL;
    cog_peer_id_t pid = 1U;
    char id127[128];
    char id128[130];
    cog_ipc_message_t msg;

    CHECK("make path", make_socket_path(path, sizeof(path)) == 0);
    CHECK_RESULT("new listener", COG_OK, cog_ipc_endpoint_new(&listener));
    CHECK_RESULT("listen", COG_OK, cog_ipc_endpoint_listen(listener, path));
    CHECK_RESULT("msg_new", COG_OK, cog_ipc_message_new(&msg, 16U));
    msg.len = 0U;

    (void)memset(id127, 'a', 127);
    id127[127] = '\0';
    (void)memset(id128, 'b', 128);
    id128[128] = '\0';

    CHECK_RESULT("register 127-char id", COG_OK,
                 cog_ipc_register_session(listener, id127, pid));
    CHECK_RESULT("register 128-char id", COG_ERROR_INVALID_ARG,
                 cog_ipc_register_session(listener, id128, pid));

    /* The 127-char id is stored intact and findable: routing to it finds
     * the (missing) host peer -> IO, never NOT_FOUND.  Then it unregisters. */
    CHECK_RESULT("send to 127-char id", COG_ERROR_IO,
                 cog_ipc_send_to_session(listener, id127, &msg));
    CHECK_RESULT("unregister 127-char id", COG_OK,
                 cog_ipc_unregister_session(listener, id127));
    CHECK_RESULT("send after unregister", COG_ERROR_NOT_FOUND,
                 cog_ipc_send_to_session(listener, id127, &msg));

    CHECK_RESULT("free msg", COG_OK, cog_ipc_message_free(&msg));
    CHECK_RESULT("free listener", COG_OK, cog_ipc_endpoint_free(listener));
    (void)unlink(path);
}

/* ------------------------------------------------------------------ */
/* Peer ids strictly increment; no reuse after disconnect              */
/* ------------------------------------------------------------------ */

static void test_peer_id_increment(void)
{
    char path[128];
    cog_ipc_endpoint_t *listener = NULL;
    cog_ipc_endpoint_t *c1 = NULL;
    cog_ipc_endpoint_t *c2 = NULL;
    cog_ipc_endpoint_t *s1 = NULL;
    cog_ipc_endpoint_t *s2 = NULL;
    cog_peer_id_t p1 = COG_PEER_ID_INVALID;
    cog_peer_id_t p2 = COG_PEER_ID_INVALID;

    CHECK("make path", make_socket_path(path, sizeof(path)) == 0);
    CHECK_RESULT("new listener", COG_OK, cog_ipc_endpoint_new(&listener));
    CHECK_RESULT("listen", COG_OK, cog_ipc_endpoint_listen(listener, path));

    CHECK_RESULT("new c1", COG_OK, cog_ipc_endpoint_new(&c1));
    CHECK_RESULT("connect c1", COG_OK, cog_ipc_endpoint_connect(c1, path, NULL));
    CHECK_RESULT("accept s1", COG_OK, cog_ipc_endpoint_accept(listener, &s1, &p1));
    CHECK("first peer id 1", p1 == 1U);

    CHECK_RESULT("new c2", COG_OK, cog_ipc_endpoint_new(&c2));
    CHECK_RESULT("connect c2", COG_OK, cog_ipc_endpoint_connect(c2, path, NULL));
    CHECK_RESULT("accept s2", COG_OK, cog_ipc_endpoint_accept(listener, &s2, &p2));
    CHECK("second peer id 2", p2 == 2U);

    /* After disconnecting the first peer, a new connection must NOT reuse
     * id 1 while s2 (id 2) is still live. */
    CHECK_RESULT("free s1", COG_OK, cog_ipc_endpoint_free(s1));
    CHECK_RESULT("free c1", COG_OK, cog_ipc_endpoint_free(c1));
    {
        cog_ipc_endpoint_t *c3 = NULL;
        cog_ipc_endpoint_t *s3 = NULL;
        cog_peer_id_t p3 = COG_PEER_ID_INVALID;

        CHECK_RESULT("new c3", COG_OK, cog_ipc_endpoint_new(&c3));
        CHECK_RESULT("connect c3", COG_OK, cog_ipc_endpoint_connect(c3, path, NULL));
        CHECK_RESULT("accept s3", COG_OK, cog_ipc_endpoint_accept(listener, &s3, &p3));
        CHECK("third peer id 3 (no reuse)", p3 == 3U);
        CHECK("s2 id untouched", p2 == 2U);

        CHECK_RESULT("free s3", COG_OK, cog_ipc_endpoint_free(s3));
        CHECK_RESULT("free c3", COG_OK, cog_ipc_endpoint_free(c3));
    }

    CHECK_RESULT("free s2", COG_OK, cog_ipc_endpoint_free(s2));
    CHECK_RESULT("free c2", COG_OK, cog_ipc_endpoint_free(c2));
    CHECK_RESULT("free listener", COG_OK, cog_ipc_endpoint_free(listener));
    (void)unlink(path);
}

/* ------------------------------------------------------------------ */
/* Endpoint free(NULL) semantics (header says "no-op", returns NULL_PTR)*/
/* ------------------------------------------------------------------ */

static void test_free_null_semantics(void)
{
    /* The return list documents COG_ERROR_NULL_PTR for endpoint == NULL;
     * the @param text says "may be NULL (no-op)".  Pin the implementation's
     * chosen behaviour: NULL_PTR. */
    CHECK_RESULT("free NULL", COG_ERROR_NULL_PTR,
                 cog_ipc_endpoint_free(NULL));
    /* message_free(NULL), by contrast, is a true no-op returning COG_OK. */
    CHECK_RESULT("msg free NULL", COG_OK, cog_ipc_message_free(NULL));
}

int main(void)
{
    test_sigpipe_safety();
    test_frame_boundary();
    test_recv_capacity_overflow();
    test_freed_peer_broadcast();
    test_stale_session();
    test_listener_free_lifetime();
    test_partial_header();
    test_session_id_length();
    test_peer_id_increment();
    test_free_null_semantics();

    if (g_failures == 0U) {
        (void)printf("test_ipc_critic: ok\n");
        return 0;
    }
    (void)fprintf(stderr, "test_ipc_critic: %u failure(s)\n", g_failures);
    return 1;
}

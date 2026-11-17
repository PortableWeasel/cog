/**
 * @file test_core_ipc.c
 * @brief Cross-module integration test for core.c + ipc.c.
 *
 * Wires the compiled cog-core agent loop to a real cog-ipc endpoint over a
 * local Unix domain socket, exercising the final accepted-endpoint
 * contract:
 *
 *   - cog_core_new accepts and owns a non-NULL ipc_endpoint (the
 *     transitional COG_ERROR_UNAVAILABLE rejection is gone);
 *   - cog_core_run drains the fabric: it accepts an incoming peer
 *     (COG_EVENT_IPC_CONNECTED), receives a framed message
 *     (COG_EVENT_IPC_MESSAGE) carrying peer/session metadata, and reports
 *     the peer's disconnect (COG_EVENT_IPC_DISCONNECTED);
 *   - a malformed frame (oversized length) retires its endpoint and returns
 *     the fatal LIMIT before further dispatch; explicit rebegin is clean;
 *   - cog_core_free destroys the endpoint (the listener's socket path is
 *     unlinked) and its accepted clients.
 *
 * Both endpoint roles are covered: the core as a listener (peers connect
 * to it) and the core as a connected endpoint (it connects to a remote
 * listener).  All sockets are local temporary paths; peers run on helper
 * threads because the transport is blocking.
 *
 * Ticket: dd2f55e (Stage 12.3e — Integrate infrastructure modules and core)
 */

#define _POSIX_C_SOURCE 200809L

#include "cog/core.h"

#include <pthread.h>
#include <poll.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "../unit/fixtures/ipc_driver.h"

/* ------------------------------------------------------------------ */
/* Test application vocabulary.                                        */
/* ------------------------------------------------------------------ */

enum {
    ST_IDLE = 0,
};

/* IPC events map one-to-one to their kinds (the core sets event.id to the
 * kind).  The dispatch table keys on those ids. */
enum {
    EV_IPC_CONNECTED = COG_EVENT_IPC_CONNECTED,
    EV_IPC_MESSAGE = COG_EVENT_IPC_MESSAGE,
    EV_IPC_DISCONNECTED = COG_EVENT_IPC_DISCONNECTED,
};

/* ------------------------------------------------------------------ */
/* Shared handler state (written by handlers on the core thread, read by
 * the wait hook on the same thread and by the test after cog_core_run
 * returns).                                                           */
/* ------------------------------------------------------------------ */

typedef struct ipc_state {
    int connected_count;
    cog_peer_id_t connected_peer;
    int message_count;
    cog_peer_id_t message_peer;
    uint16_t message_type;
    size_t message_len;
    char message_buf[256];
    int session_seen;
    int disconnected_count;
    cog_peer_id_t disconnected_peer;
    int done;
} ipc_state_t;

static ipc_state_t g_state;

/* ------------------------------------------------------------------ */
/* Handlers                                                            */
/* ------------------------------------------------------------------ */

static cog_handler_result_t on_ipc_connected(cog_core_t *core,
                                            const cog_event_t *event)
{
    const cog_event_ipc_t *ipc = (const cog_event_ipc_t *)event->payload;

    (void)core;
    if (ipc != NULL) {
        g_state.connected_peer = ipc->peer;
        g_state.connected_count++;
    }
    return (cog_handler_result_t){ST_IDLE, COG_OK};
}

static cog_handler_result_t on_ipc_message(cog_core_t *core,
                                          const cog_event_t *event)
{
    const cog_event_ipc_t *ipc = (const cog_event_ipc_t *)event->payload;

    if (ipc != NULL) {
        g_state.message_peer = ipc->peer;
        g_state.message_type = ipc->msg_type;
        g_state.message_len = ipc->msg_payload_len;
        if (ipc->msg_payload != NULL &&
            ipc->msg_payload_len <= sizeof(g_state.message_buf)) {
            (void)memcpy(g_state.message_buf, ipc->msg_payload,
                         ipc->msg_payload_len);
        }
        g_state.message_count++;
        /* Session metadata: the core's session is reachable from the
         * handler. */
        g_state.session_seen = (cog_core_session(core) != NULL);
    }
    return (cog_handler_result_t){ST_IDLE, COG_OK};
}

static cog_handler_result_t on_ipc_disconnected(cog_core_t *core,
                                                const cog_event_t *event)
{
    const cog_event_ipc_t *ipc = (const cog_event_ipc_t *)event->payload;

    (void)core;
    if (ipc != NULL) {
        g_state.disconnected_peer = ipc->peer;
        g_state.disconnected_count++;
        g_state.done = 1;
    }
    return (cog_handler_result_t){ST_IDLE, COG_OK};
}

static const cog_state_t stay_transitions[] = {
    ST_IDLE,
    COG_TRANSITION_SELF,
    COG_TRANSITION_END,
};

static const cog_dispatch_entry_t dispatch_table[] = {
    COG_DISPATCH_ENTRY(ST_IDLE, EV_IPC_CONNECTED, on_ipc_connected,
                       stay_transitions),
    COG_DISPATCH_ENTRY(ST_IDLE, EV_IPC_MESSAGE, on_ipc_message,
                       stay_transitions),
    COG_DISPATCH_ENTRY(ST_IDLE, EV_IPC_DISCONNECTED, on_ipc_disconnected,
                       stay_transitions),
};

/* ------------------------------------------------------------------ */
/* Wait hook: keep the loop alive while the peer thread works, stop when
 * the handler has seen the disconnect (or after a safety timeout).     */
/* ------------------------------------------------------------------ */

typedef struct wait_ctx {
    cog_core_t *core;
    int calls;
} wait_ctx_t;

static void ipc_wait_fn(void *arg)
{
    wait_ctx_t *ctx = (wait_ctx_t *)arg;
    struct timespec ts;

    ts.tv_sec = 0;
    ts.tv_nsec = 1000000L; /* 1 ms */
    (void)nanosleep(&ts, NULL);

    ctx->calls++;
    if (g_state.done || ctx->calls > 3000) {
        cog_core_stop(ctx->core);
    }
}

/* ------------------------------------------------------------------ */
/* Assertion helpers.                                                  */
/* ------------------------------------------------------------------ */

static int failures = 0;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,     \
                    #cond);                                             \
            ++failures;                                                 \
        }                                                               \
    } while (0)

#define CHECK_EQ_INT(a, b)                                              \
    do {                                                                \
        long long _a = (long long)(a);                                  \
        long long _b = (long long)(b);                                  \
        if (_a != _b) {                                                 \
            fprintf(stderr, "FAIL %s:%d: %s == %s (%lld != %lld)\n",    \
                    __FILE__, __LINE__, #a, #b, _a, _b);                \
            ++failures;                                                 \
        }                                                               \
    } while (0)

/** Create a unique temporary socket path; caller unlinks it when done. */
static int make_socket_path(char *buf, size_t bufsize)
{
    char tmpl[] = "/tmp/cog-core-ipc-test-XXXXXX";
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

/* ------------------------------------------------------------------ */
/* Peer threads (the transport is blocking, so peers run off-thread).   */
/* ------------------------------------------------------------------ */

typedef struct client_ctx {
    char path[128];
} client_ctx_t;

typedef struct bulk_ctx {
    char path[128];
    int count;
} bulk_ctx_t;

/** Connect to the core's listener, send one message, then close. */
static void *client_thread(void *arg)
{
    client_ctx_t *c = (client_ctx_t *)arg;
    cog_ipc_endpoint_t *ep = NULL;
    cog_ipc_message_t msg;
    struct timespec ts;

    (void)cog_ipc_endpoint_new(&ep);
    if (cog_ipc_endpoint_connect(ep, c->path, NULL) != COG_ERROR_AGAIN) {
        (void)cog_ipc_endpoint_free(ep);
        return NULL;
    }
    (void)cog_ipc_message_new(&msg, 64U);
    (void)memcpy(msg.data, "ping", 4U);
    msg.len = 4U;
    (void)cog_ipc_endpoint_send(ep, &msg);
    (void)cog_ipc_message_free(&msg);

    /* Give the core time to accept and receive before we close. */
    ts.tv_sec = 0;
    ts.tv_nsec = 200000000L; /* 200 ms */
    (void)nanosleep(&ts, NULL);

    (void)cog_ipc_endpoint_free(ep);
    return NULL;
}

/** Connect to the core's listener, send @p count messages, then close. */
static void *bulk_client_thread(void *arg)
{
    bulk_ctx_t *b = (bulk_ctx_t *)arg;
    cog_ipc_endpoint_t *ep = NULL;
    cog_ipc_message_t msg;
    struct timespec ts;
    int i;

    (void)cog_ipc_endpoint_new(&ep);
    if (cog_ipc_endpoint_connect(ep, b->path, NULL) != COG_ERROR_AGAIN) {
        (void)cog_ipc_endpoint_free(ep);
        return NULL;
    }
    (void)cog_ipc_message_new(&msg, 16U);
    for (i = 0; i < b->count; ++i) {
        msg.data[0] = (uint8_t)('0' + i);
        msg.len = 1U;
        (void)cog_ipc_endpoint_send(ep, &msg);
    }
    (void)cog_ipc_message_free(&msg);

    ts.tv_sec = 0;
    ts.tv_nsec = 300000000L; /* 300 ms */
    (void)nanosleep(&ts, NULL);

    (void)cog_ipc_endpoint_free(ep);
    return NULL;
}

/** Accept the core's connection on a test-owned listener, send, close. */
static void *server_thread(void *arg)
{
    cog_ipc_endpoint_t *server = (cog_ipc_endpoint_t *)arg;
    cog_ipc_endpoint_t *client = NULL;
    cog_ipc_message_t msg;
    cog_peer_id_t peer_id = COG_PEER_ID_INVALID;
    struct timespec ts;

    if (cog_ipc_endpoint_accept(server, &client, &peer_id) != COG_OK) {
        return NULL;
    }
    (void)cog_ipc_message_new(&msg, 64U);
    (void)memcpy(msg.data, "pong", 4U);
    msg.len = 4U;
    (void)cog_ipc_endpoint_send(client, &msg);
    (void)cog_ipc_message_free(&msg);

    ts.tv_sec = 0;
    ts.tv_nsec = 200000000L; /* 200 ms */
    (void)nanosleep(&ts, NULL);

    (void)cog_ipc_endpoint_free(client);
    return NULL;
}

/** Connect to the core's listener and write an oversized frame header. */
typedef struct malformed_ctx { const char *path; bool sent, closed; } malformed_ctx_t;
static void *malformed_client_thread(void *arg)
{
    malformed_ctx_t *result = (malformed_ctx_t *)arg;
    const char *path = result->path;
    uint8_t header[4];
    int fd;


    fd = raw_connect(path);
    if (fd < 0) {
        return NULL;
    }
    /* Declared length 0xFFFFFFFF > COG_IPC_MAX_PAYLOAD. */
    header[0] = 0xFFU;
    header[1] = 0xFFU;
    header[2] = 0xFFU;
    header[3] = 0xFFU;
    result->sent = write(fd, header, sizeof(header)) == (ssize_t)sizeof(header);

    struct pollfd watched = {.fd = fd, .events = POLLIN};
    for (unsigned i = 0U; i < 16U && !result->closed; ++i) {
        if (poll(&watched, 1U, 200) <= 0) break;
        /* The server handshake may precede physical EOF. */
        unsigned char bytes[64];
        ssize_t amount = recv(fd, bytes, sizeof(bytes), 0);
        result->closed = amount == 0 || (amount < 0 && errno == ECONNRESET);
        if (amount < 0 && !result->closed) break;
    }

    (void)close(fd);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Tests                                                               */
/* ------------------------------------------------------------------ */

/** Core as a listener: accept, receive, disconnect, destruction. */
static void test_listener_endpoint(void)
{
    char path[128];
    cog_ipc_endpoint_t *listener = NULL;
    cog_session_t *session = NULL;
    cog_core_t *core = NULL;
    cog_core_config_t cfg;
    wait_ctx_t ctx;
    client_ctx_t cctx;
    pthread_t tid;

    CHECK(make_socket_path(path, sizeof(path)) == 0);
    CHECK(cog_ipc_endpoint_new(&listener) == COG_OK);
    CHECK(cog_ipc_endpoint_listen(listener, path) == COG_OK);

    CHECK(cog_session_new(&session, NULL) == COG_OK);

    memset(&cfg, 0, sizeof(cfg));
    cfg.session = session;
    cfg.dispatch_table = dispatch_table;
    cfg.dispatch_table_size = sizeof(dispatch_table) / sizeof(dispatch_table[0]);
    cfg.ipc_endpoint = listener;
    cfg.wait_fn = ipc_wait_fn;
    cfg.wait_arg = &ctx;

    CHECK(cog_core_new(&core, &cfg) == COG_OK);
    CHECK(cog_core_ipc(core) == listener);

    memset(&g_state, 0, sizeof(g_state));
    memset(&ctx, 0, sizeof(ctx));
    ctx.core = core;
    (void)snprintf(cctx.path, sizeof(cctx.path), "%s", path);

    CHECK(pthread_create(&tid, NULL, client_thread, &cctx) == 0);
    CHECK(cog_core_run(core) == COG_OK);
    CHECK(pthread_join(tid, NULL) == 0);

    /* Endpoint acceptance + connect event. */
    CHECK_EQ_INT(g_state.connected_count, 1);
    CHECK(g_state.connected_peer != COG_PEER_ID_INVALID);

    /* Incoming message -> event dispatch with peer/session metadata. */
    CHECK_EQ_INT(g_state.message_count, 1);
    CHECK(g_state.message_peer == g_state.connected_peer);
    CHECK_EQ_INT(g_state.message_len, 4U);
    CHECK(memcmp(g_state.message_buf, "ping", 4U) == 0);
    CHECK(g_state.session_seen);

    /* Disconnect handling. */
    CHECK_EQ_INT(g_state.disconnected_count, 1);
    CHECK(g_state.disconnected_peer == g_state.connected_peer);

    /* Destruction: cog_core_free destroys the listener and unlinks the
     * socket path. */
    cog_core_free(core);
    CHECK(access(path, F_OK) != 0);

    cog_session_free(session);
}

/** Core as a connected endpoint: it connects to a remote listener. */
static void test_connected_endpoint(void)
{
    char path[128];
    cog_ipc_endpoint_t *server = NULL;
    cog_ipc_endpoint_t *conn = NULL;
    cog_session_t *session = NULL;
    cog_core_t *core = NULL;
    cog_core_config_t cfg;
    wait_ctx_t ctx;
    pthread_t tid;

    CHECK(make_socket_path(path, sizeof(path)) == 0);
    CHECK(cog_ipc_endpoint_new(&server) == COG_OK);
    CHECK(cog_ipc_endpoint_listen(server, path) == COG_OK);

    /* The core connects to the already-listening server. */
    CHECK(cog_ipc_endpoint_new(&conn) == COG_OK);
    CHECK(cog_ipc_endpoint_connect(conn, path, NULL) == COG_ERROR_AGAIN);

    CHECK(cog_session_new(&session, NULL) == COG_OK);

    memset(&cfg, 0, sizeof(cfg));
    cfg.session = session;
    cfg.dispatch_table = dispatch_table;
    cfg.dispatch_table_size = sizeof(dispatch_table) / sizeof(dispatch_table[0]);
    cfg.ipc_endpoint = conn;
    cfg.wait_fn = ipc_wait_fn;
    cfg.wait_arg = &ctx;

    CHECK(cog_core_new(&core, &cfg) == COG_OK);
    CHECK(cog_core_ipc(core) == conn);

    memset(&g_state, 0, sizeof(g_state));
    memset(&ctx, 0, sizeof(ctx));
    ctx.core = core;

    CHECK(pthread_create(&tid, NULL, server_thread, server) == 0);
    CHECK(cog_core_run(core) == COG_OK);
    CHECK(pthread_join(tid, NULL) == 0);

    CHECK_EQ_INT(g_state.message_count, 1);
    CHECK_EQ_INT(g_state.message_len, 4U);
    CHECK(memcmp(g_state.message_buf, "pong", 4U) == 0);
    CHECK(g_state.session_seen);
    CHECK_EQ_INT(g_state.disconnected_count, 1);

    cog_core_free(core);
    cog_ipc_endpoint_free(server);
    cog_session_free(session);
}

/** A full event queue backpressures the drain without deadlock or loss. */
static void test_full_queue_backpressure(void)
{
    char path[128];
    cog_ipc_endpoint_t *listener = NULL;
    cog_session_t *session = NULL;
    cog_core_t *core = NULL;
    cog_core_config_t cfg;
    wait_ctx_t ctx;
    bulk_ctx_t bctx;
    pthread_t tid;

    CHECK(make_socket_path(path, sizeof(path)) == 0);
    CHECK(cog_ipc_endpoint_new(&listener) == COG_OK);
    CHECK(cog_ipc_endpoint_listen(listener, path) == COG_OK);

    CHECK(cog_session_new(&session, NULL) == COG_OK);

    memset(&cfg, 0, sizeof(cfg));
    cfg.session = session;
    cfg.dispatch_table = dispatch_table;
    cfg.dispatch_table_size = sizeof(dispatch_table) / sizeof(dispatch_table[0]);
    cfg.ipc_endpoint = listener;
    cfg.event_queue_capacity = 2U; /* force backpressure across drain passes */
    cfg.wait_fn = ipc_wait_fn;
    cfg.wait_arg = &ctx;

    CHECK(cog_core_new(&core, &cfg) == COG_OK);

    memset(&g_state, 0, sizeof(g_state));
    memset(&ctx, 0, sizeof(ctx));
    ctx.core = core;
    (void)snprintf(bctx.path, sizeof(bctx.path), "%s", path);
    bctx.count = 5;

    CHECK(pthread_create(&tid, NULL, bulk_client_thread, &bctx) == 0);
    CHECK(cog_core_run(core) == COG_OK);
    CHECK(pthread_join(tid, NULL) == 0);

    /* All five messages arrived across multiple drain passes despite the
     * tiny event queue; the peer's close is then reported as a disconnect. */
    CHECK_EQ_INT(g_state.message_count, 5);
    CHECK_EQ_INT(g_state.message_len, 1U);
    CHECK(g_state.message_buf[0] == '4'); /* last message received */
    CHECK_EQ_INT(g_state.disconnected_count, 1);

    cog_core_free(core);
    cog_session_free(session);
}

/** A malformed (oversized) frame closes the connection without deadlock. */
static void test_malformed_frame(void)
{
    char path[128];
    cog_ipc_endpoint_t *listener = NULL;
    cog_session_t *session = NULL;
    cog_core_t *core = NULL;
    cog_core_config_t cfg;
    wait_ctx_t ctx;
    pthread_t tid;

    CHECK(make_socket_path(path, sizeof(path)) == 0);
    CHECK(cog_ipc_endpoint_new(&listener) == COG_OK);
    CHECK(cog_ipc_endpoint_listen(listener, path) == COG_OK);

    CHECK(cog_session_new(&session, NULL) == COG_OK);

    memset(&cfg, 0, sizeof(cfg));
    cfg.session = session;
    cfg.dispatch_table = dispatch_table;
    cfg.dispatch_table_size = sizeof(dispatch_table) / sizeof(dispatch_table[0]);
    cfg.ipc_endpoint = listener;
    cfg.wait_fn = ipc_wait_fn;
    cfg.wait_arg = &ctx;

    CHECK(cog_core_new(&core, &cfg) == COG_OK);

    memset(&g_state, 0, sizeof(g_state));
    memset(&ctx, 0, sizeof(ctx));
    ctx.core = core;

    malformed_ctx_t malformed = {.path = path};
    CHECK(pthread_create(&tid, NULL, malformed_client_thread, &malformed) == 0);
    CHECK(cog_core_drive_begin(core) == COG_OK);
    cog_core_step_info_t info = {0};
    cog_result_t rc = COG_OK;
    for (unsigned i = 0U; i < 2000U && rc == COG_OK && !info.done; ++i) {
        rc = cog_core_step(core, &info);
        if (rc == COG_OK && !info.done) {
            const struct timespec pause = {0, 1000000L};
            (void)nanosleep(&pause, NULL);
        }
    }
    CHECK(rc == COG_ERROR_LIMIT);
    CHECK(info.done);
    CHECK_EQ_INT(info.dispatched, 0U);
    CHECK(cog_core_step(core, &info) == COG_ERROR_INVALID_STATE);
    CHECK(cog_core_drive_end(core) == COG_ERROR_LIMIT);
    CHECK(pthread_join(tid, NULL) == 0);

    /* Approved O2 fatal-service contract: immediate endpoint retirement and
     * frozen dispatch, not an ordinary EOF notification. Already copied events
     * retain their owners, but no synthetic disconnect is owed on rebegin.
     * Ordinary EOF CONNECTED/MESSAGE/DISCONNECTED controls above stay intact. */
    CHECK(g_state.connected_count >= 0 && g_state.connected_count <= 1);
    CHECK_EQ_INT(g_state.message_count, 0);
    CHECK_EQ_INT(g_state.disconnected_count, 0);
    CHECK(malformed.sent && malformed.closed);
    int connected_before = g_state.connected_count;
    size_t dispatched = 0U;
    CHECK(cog_core_drive_begin(core) == COG_OK);
    for (unsigned i = 0U; i < 16U; ++i) {
        CHECK(cog_core_step(core, &info) == COG_OK);
        CHECK(!info.done);
        dispatched += info.dispatched;
    }
    CHECK(dispatched == (size_t)(1 - connected_before));
    cog_core_stop(core);
    CHECK(cog_core_step(core, &info) == COG_OK && info.done);
    CHECK(cog_core_drive_end(core) == COG_OK);
    CHECK_EQ_INT(g_state.connected_count, 1);
    CHECK_EQ_INT(g_state.message_count, 0);
    CHECK_EQ_INT(g_state.disconnected_count, 0);

    cog_core_free(core);
    CHECK(access(path, F_OK) != 0);
    cog_session_free(session);
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int main(void)
{
    test_listener_endpoint();
    test_connected_endpoint();
    test_malformed_frame();
    test_full_queue_backpressure();

    if (failures != 0) {
        fprintf(stderr, "test_core_ipc: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_core_ipc: all checks passed\n");
    return 0;
}

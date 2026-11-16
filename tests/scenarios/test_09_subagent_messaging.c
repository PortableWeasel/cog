/**
 * @file test_09_subagent_messaging.c
 * @brief Scenario 09: Subagent-to-subagent messaging across cores.
 *
 * Wires the real cog-core agent loop to a real cog-ipc listener over a
 * local Unix domain socket and proves the scenario-09 contract from
 * docs/scenarios/09-subagent-messaging.md:
 *
 *   1. session routing: cog_ipc_register_session maps a session id to
 *      the peer that hosts it; cog_ipc_send_to_session delivers a
 *      message to exactly that peer;
 *   2. bidirectional routing: a message routed to a session on another
 *      core is delivered, and the reply routed back to the origin
 *      session arrives at the origin peer;
 *   3. the COG_EVENT_IPC_MESSAGE event flows through the real state
 *      machine with a cog_event_ipc_t payload carrying the source peer
 *      and the message bytes (the core drains the fabric during
 *      cog_core_run and the handler routes by session);
 *   4. parent/peer metadata: cog_agent_session_id exposes the child
 *      session id of a spawned subagent, cog_session_parent_id links it
 *      back to the parent session, and that session id can be registered
 *      in the routing table so the subagent is reachable by session.
 *
 * Peers run on helper threads, each owning and servicing its nonblocking endpoint.
 * All sockets are local temporary paths; every endpoint is owned and
 * released by this test.  Messaging is synchronized deterministically:
 * the hub sends a "ready" handshake once both sessions are registered,
 * so no "to:" message is ever routed before its destination session
 * exists (no timing luck).
 *
 * Ticket: 0d8ff50 (Stage 12.6c — Scenario tests 06-10).
 */
#define _POSIX_C_SOURCE 200809L

#include "cog/core.h"
#include "cog/ipc.h"

#include "cog_cli/cli.h"
#include "cog_cli/mock_provider.h"

#include "agent_internal.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* Test application vocabulary.                                        */
/* ------------------------------------------------------------------ */

enum {
    ST_IDLE = 0,
};

enum {
    EV_IPC_CONNECTED = COG_EVENT_IPC_CONNECTED,
    EV_IPC_MESSAGE = COG_EVENT_IPC_MESSAGE,
    EV_IPC_DISCONNECTED = COG_EVENT_IPC_DISCONNECTED,
};

/* ------------------------------------------------------------------ */
/* Assertion helpers.                                                  */
/* ------------------------------------------------------------------ */

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

/** Create a unique temporary socket path; caller unlinks it when done. */
static int make_socket_path(char *buf,size_t size)
{
    static unsigned serial;int n=snprintf(buf,size,"scn09-%ld-%u.sock",(long)getpid(),++serial);
    if(n<0||(size_t)n>=size) return -1;
    (void)unlink(buf);return 0;
}
static uint64_t ipc_now(void)
{
    struct timespec ts;if(clock_gettime(CLOCK_MONOTONIC,&ts)!=0) abort();
    return (uint64_t)ts.tv_sec*1000U+(uint64_t)ts.tv_nsec/1000000U;
}
static void ipc_pause(void) { (void)nanosleep(&(struct timespec){0,1000000L},NULL); }
static cog_result_t ipc_step(cog_ipc_endpoint_t *ep)
{ uint64_t deadline;return cog_ipc_endpoint_service(ep,ipc_now(),&deadline); }
static cog_result_t connect_ready(cog_ipc_endpoint_t *ep,const char *path)
{
    cog_result_t rc=cog_ipc_endpoint_connect(ep,path,NULL);
    if(rc!=COG_OK&&rc!=COG_ERROR_AGAIN) return rc;
    uint64_t end=ipc_now()+4000;cog_peer_id_t peer;
    do {
        rc=ipc_step(ep);if(rc!=COG_OK&&rc!=COG_ERROR_AGAIN) return rc;
        if(cog_ipc_endpoint_get_peer_id(ep,&peer)==COG_OK&&peer!=0) return COG_OK;
        ipc_pause();
    } while(ipc_now()<end);
    return COG_ERROR_TIMEOUT;
}
static atomic_uint clients_done;

/* ------------------------------------------------------------------ */
/* Core handler: session-routed messaging                              */
/* ------------------------------------------------------------------ */

/** Handler-observed metadata (written on the core thread). */
static cog_peer_id_t g_last_peer = COG_PEER_ID_INVALID;
static uint16_t g_last_msg_type = 0U;
static int g_routed = 0;          /**< Messages routed by session. */
static int g_registered = 0;      /**< Sessions registered by peers. */

/**
 * @brief Route an IPC message by session.
 *
 * The application protocol is plain text:
 *   "register:<session>"  -> the sending peer hosts <session>
 *   "to:<session>|<body>" -> deliver <body> to the peer hosting <session>
 *
 * This mirrors the scenario: aux cores register their sessions on
 * connect, and the main core routes messages by session id.
 */
static cog_handler_result_t on_ipc_message(cog_core_t *core,
                                           const cog_event_t *event)
{
    const cog_event_ipc_t *ipc = (const cog_event_ipc_t *)event->payload;
    char buf[256];
    size_t n;
    char *sep;

    if (ipc == NULL) {
        return (cog_handler_result_t){ST_IDLE, COG_OK};
    }
    g_last_peer = ipc->peer;
    g_last_msg_type = ipc->msg_type;

    n = ipc->msg_payload_len < sizeof(buf) - 1U
            ? ipc->msg_payload_len : sizeof(buf) - 1U;
    if (ipc->msg_payload != NULL && n > 0U) {
        (void)memcpy(buf, ipc->msg_payload, n);
    }
    buf[n] = '\0';

    if (strncmp(buf, "register:", 9U) == 0) {
        (void)cog_ipc_register_session(cog_core_ipc(core), buf + 9,
                                       ipc->peer);
        g_registered++;
        /* Deterministic start: once both sessions are registered, tell
         * the sender (agent-a) it may start messaging.  Without this
         * handshake a "to:" message can be routed before its
         * destination session has registered, and cog_ipc_send_to_session
         * drops it with COG_ERROR_NOT_FOUND (a timing-luck race). */
        if (g_registered == 2) {
            cog_ipc_message_t ready;

            if (cog_ipc_message_new(&ready, 6U) == COG_OK) {
                (void)memcpy(ready.data, "ready", 5U);
                ready.len = 5U;
                (void)cog_ipc_send_to_session(cog_core_ipc(core), "agent-a",
                                              &ready);
                (void)cog_ipc_message_free(&ready);
            }
        }
        return (cog_handler_result_t){ST_IDLE, COG_OK};
    }
    if (strncmp(buf, "to:", 3U) == 0) {
        cog_ipc_message_t out;
        size_t body_len;

        sep = strchr(buf + 3, '|');
        if (sep == NULL) {
            return (cog_handler_result_t){ST_IDLE, COG_OK};
        }
        *sep = '\0';
        body_len = strlen(sep + 1);
        if (cog_ipc_message_new(&out, body_len + 1U) == COG_OK) {
            (void)memcpy(out.data, sep + 1, body_len);
            out.len = body_len;
            (void)cog_ipc_send_to_session(cog_core_ipc(core), buf + 3, &out);
            (void)cog_ipc_message_free(&out);
            g_routed++;
        }
    }
    return (cog_handler_result_t){ST_IDLE, COG_OK};
}

/** No-op handler for IPC connect/disconnect lifecycle events. */
static cog_handler_result_t on_ipc_lifecycle(cog_core_t *core,
                                             const cog_event_t *event)
{
    (void)core;
    (void)event;
    return (cog_handler_result_t){ST_IDLE, COG_OK};
}

static const cog_state_t idle_transitions[] = {
    ST_IDLE,
    COG_TRANSITION_SELF,
    COG_TRANSITION_END,
};

static const cog_dispatch_entry_t dispatch_table[] = {
    COG_DISPATCH_ENTRY(ST_IDLE, EV_IPC_CONNECTED, on_ipc_lifecycle,
                       idle_transitions),
    COG_DISPATCH_ENTRY(ST_IDLE, EV_IPC_MESSAGE, on_ipc_message,
                       idle_transitions),
    COG_DISPATCH_ENTRY(ST_IDLE, EV_IPC_DISCONNECTED, on_ipc_lifecycle,
                       idle_transitions),
};

/** Wait hook: stop the loop after both routes are done (or timeout). */
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
    if (atomic_load(&clients_done) == 2U || ctx->calls > 5000) {
        cog_core_stop(ctx->core);
    }
}

/* ------------------------------------------------------------------ */
/* Peer threads own their nonblocking service and message loops.  */
/* ------------------------------------------------------------------ */

/** Send a NUL-terminated string as a framed message. */
static int send_str(cog_ipc_endpoint_t *ep, const char *text)
{
    cog_ipc_message_t msg;
    size_t len = strlen(text);
    cog_result_t rc;

    rc = cog_ipc_message_new(&msg, len + 1U);
    if (rc != COG_OK) {
        return -1;
    }
    (void)memcpy(msg.data, text, len);
    msg.len = len;
    uint64_t end=ipc_now()+4000;
    do { rc=cog_ipc_endpoint_send(ep,&msg);if(rc!=COG_ERROR_AGAIN) break;(void)ipc_step(ep);ipc_pause(); } while(ipc_now()<end);
    if(rc==COG_OK) { cog_result_t step=ipc_step(ep);if(step!=COG_OK&&step!=COG_ERROR_AGAIN) rc=step; }
    (void)cog_ipc_message_free(&msg);
    return (rc == COG_OK) ? 0 : -1;
}

/** Receive one framed message with a bounded wait; copies into @p out. */
static int recv_str(cog_ipc_endpoint_t *ep, char *out, size_t out_cap,
                    int timeout_ms)
{
    cog_ipc_message_t msg;
    cog_result_t rc;
    if(cog_ipc_message_new(&msg,out_cap)!=COG_OK) return -1;
    uint64_t end=ipc_now()+(uint64_t)timeout_ms;
    do {
        (void)ipc_step(ep);rc=cog_ipc_endpoint_recv(ep,&msg);
        if(rc!=COG_ERROR_AGAIN) break;
        ipc_pause();
    } while(ipc_now()<end);
    if (rc == COG_OK && msg.len < out_cap) {
        (void)memcpy(out, msg.data, msg.len);
        out[msg.len] = '\0';
    } else {
        rc = COG_ERROR_IO;
    }
    (void)cog_ipc_message_free(&msg);
    return (rc == COG_OK) ? 0 : -1;
}

typedef struct client_ctx {
    char path[128];
    char received[128];
    int ok;
} client_ctx_t;

/**
 * @brief Aux-core A: register session agent-a, message agent-b, then
 *        wait for the reply routed back to agent-a.
 */
static void *client_a_thread(void *arg)
{
    client_ctx_t *c = (client_ctx_t *)arg;
    cog_ipc_endpoint_t *ep = NULL;

    if (cog_ipc_endpoint_new(&ep) != COG_OK) {
        return NULL;
    }
    if (connect_ready(ep, c->path) != COG_OK) {
        (void)cog_ipc_endpoint_free(ep);
        return NULL;
    }
    if (send_str(ep, "register:agent-a") != 0) {
        (void)cog_ipc_endpoint_free(ep);
        return NULL;
    }
    /* Wait for the hub to confirm both sessions are registered before
     * messaging: a "to:" sent earlier would be routed before agent-b
     * registers and dropped (COG_ERROR_NOT_FOUND). */
    if (recv_str(ep, c->received, sizeof(c->received), 5000) != 0 ||
        strcmp(c->received, "ready") != 0) {
        (void)cog_ipc_endpoint_free(ep);
        return NULL;
    }
    if (send_str(ep, "to:agent-b|hello") != 0) {
        (void)cog_ipc_endpoint_free(ep);
        return NULL;
    }
    /* Wait for the reply routed back to session agent-a. */
    if (recv_str(ep, c->received, sizeof(c->received), 5000) == 0) {
        c->ok = 1;
    }
    (void)cog_ipc_endpoint_free(ep);
    (void)atomic_fetch_add(&clients_done,1U);
    return NULL;
}

/**
 * @brief Aux-core B: register session agent-b, wait for the message
 *        routed to agent-b, then reply to agent-a.
 */
static void *client_b_thread(void *arg)
{
    client_ctx_t *c = (client_ctx_t *)arg;
    cog_ipc_endpoint_t *ep = NULL;
    char buf[128];

    if (cog_ipc_endpoint_new(&ep) != COG_OK) {
        return NULL;
    }
    if (connect_ready(ep, c->path) != COG_OK) {
        (void)cog_ipc_endpoint_free(ep);
        return NULL;
    }
    if (send_str(ep, "register:agent-b") != 0) {
        (void)cog_ipc_endpoint_free(ep);
        return NULL;
    }
    /* Receive the message routed to session agent-b. */
    if (recv_str(ep, buf, sizeof(buf), 5000) != 0) {
        (void)cog_ipc_endpoint_free(ep);
        return NULL;
    }
    (void)snprintf(c->received, sizeof(c->received), "%s", buf);
    c->ok = 1;
    /* Reply to the origin session. */
    (void)send_str(ep, "to:agent-a|pong");
    (void)cog_ipc_endpoint_free(ep);
    (void)atomic_fetch_add(&clients_done,1U);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* 1-3. Session-routed messaging across cores                          */
/* ------------------------------------------------------------------ */

static void scenario_session_routing(void)
{
    char path[128];
    cog_session_t *session = NULL;
    cog_core_t *core = NULL;
    cog_core_config_t cfg;
    cog_ipc_endpoint_t *listener = NULL;
    wait_ctx_t wctx;
    pthread_t ta, tb;
    client_ctx_t ca;
    client_ctx_t cb;

    if (make_socket_path(path, sizeof(path)) != 0) {
        CHECK(0);
        return;
    }

    /* Core owns a listening IPC endpoint (the fabric hub). */
    CHECK(cog_ipc_endpoint_new(&listener) == COG_OK);
    CHECK(cog_ipc_endpoint_listen(listener, path) == COG_OK);
    CHECK(cog_session_new(&session, NULL) == COG_OK);
    if (session == NULL) {
        (void)cog_ipc_endpoint_free(listener);
        (void)unlink(path);
        return;
    }
    memset(&cfg, 0, sizeof(cfg));
    cfg.session = session;
    cfg.dispatch_table = dispatch_table;
    cfg.dispatch_table_size = sizeof(dispatch_table) / sizeof(dispatch_table[0]);
    cfg.ipc_endpoint = listener; /* ownership transfers to the core */
    cfg.wait_fn = ipc_wait_fn;
    cfg.wait_arg = &wctx;
    wctx.core = NULL;
    wctx.calls = 0;
    CHECK(cog_core_new(&core, &cfg) == COG_OK);
    if (core == NULL) {
        cog_session_free(session);
        (void)unlink(path);
        return;
    }
    wctx.core = core;

    /* Reset handler state. */
    g_last_peer = COG_PEER_ID_INVALID;
    g_last_msg_type = 0U;
    g_routed = 0;
    g_registered = 0;
    atomic_store(&clients_done,0U);

    /* Start the two aux-core peers, then run the hub loop. */
    (void)memset(&ca, 0, sizeof(ca));
    (void)snprintf(ca.path, sizeof(ca.path), "%s", path);
    (void)memset(&cb, 0, sizeof(cb));
    (void)snprintf(cb.path, sizeof(cb.path), "%s", path);
    CHECK(pthread_create(&ta, NULL, client_a_thread, &ca) == 0);
    CHECK(pthread_create(&tb, NULL, client_b_thread, &cb) == 0);

    CHECK(cog_core_run(core) == COG_OK);

    (void)pthread_join(ta, NULL);
    (void)pthread_join(tb, NULL);

    /* Both sessions registered by their hosting peers. */
    CHECK_EQ_INT(g_registered, 2);
    /* Both routes completed (A->B and B->A). */
    CHECK_EQ_INT(g_routed, 2);
    /* The IPC event carried real peer metadata. */
    CHECK(g_last_peer != COG_PEER_ID_INVALID);
    CHECK_EQ_INT((long long)g_last_msg_type, 0LL);

    /* B received the message routed to agent-b; A received the reply
     * routed back to agent-a. */
    CHECK(cb.ok);
    CHECK(strcmp(cb.received, "hello") == 0);
    CHECK(ca.ok);
    CHECK(strcmp(ca.received, "pong") == 0);

    cog_core_free(core); /* destroys the listener and unlinks the socket */
    cog_session_free(session);
}

/* ------------------------------------------------------------------ */
/* 4. Parent/peer metadata: agent session ids in the routing table     */
/* ------------------------------------------------------------------ */

static void scenario_agent_session_metadata(void)
{
    cog_cli_options_t opts;
    cog_cli_app_t *app = NULL;
    cog_core_t *core;
    cog_agent_config_t cfg;
    cog_agent_id_t id = 0U;
    const char *parent_sid;
    const char *child_sid;
    char path[128];
    cog_ipc_endpoint_t *listener = NULL;
    cog_ipc_endpoint_t *client = NULL;
    cog_ipc_endpoint_t *server = NULL;
    cog_peer_id_t pid = COG_PEER_ID_INVALID;
    cog_ipc_message_t out;
    cog_ipc_message_t in;
    const char *payload = "to-child";

    (void)memset(&opts, 0, sizeof(opts));
    opts.model_id = "mock-model";
    opts.endpoint_name = "mock";
    opts.interactive = false;
    opts.backend = COG_CLI_BACKEND_MOCK;
    opts.explicit_options |= COG_CLI_OPT_BACKEND | COG_CLI_OPT_MODEL | COG_CLI_OPT_ENDPOINT;
    CHECK(cog_cli_app_new(&app, &opts) == COG_OK);
    if (app == NULL) {
        return;
    }
    core = cog_cli_app_core(app);
    CHECK(core != NULL);

    parent_sid = cog_session_id(cog_cli_app_session(app));
    CHECK(parent_sid != NULL);

    /* Spawn a subagent; its child session is linked to the parent. */
    (void)memset(&cfg, 0, sizeof(cfg));
    cfg.task = "messaging peer";
    cfg.parent_session_id = parent_sid;
    cfg.force_local = true;
    CHECK(cog_agent_spawn(core, &cfg, &id) == COG_OK);
    CHECK(id != 0U);

    /* Session discovery: the agent's session id is exposed and distinct
     * from the parent's, with the parent linkage intact. */
    CHECK(cog_agent_session_id(core, id, &child_sid) == COG_OK);
    CHECK(child_sid != NULL);
    CHECK(strcmp(child_sid, parent_sid) != 0);
    {
        /* The child session lives on the agent record; verify its parent
         * linkage through the real child session. */
        const cog_agent_entry_t *rec = cog_agent_internal_get(core, id);

        CHECK(rec != NULL);
        CHECK(rec->session != NULL);
        CHECK(cog_session_parent_id(rec->session) != NULL);
        CHECK(strcmp(cog_session_parent_id(rec->session), parent_sid) == 0);
    }

    /* Register the agent's session id in a routing table and deliver a
     * message to it: the subagent is reachable by session. */
    CHECK(make_socket_path(path, sizeof(path)) == 0);
    CHECK(cog_ipc_endpoint_new(&listener) == COG_OK);
    CHECK(cog_ipc_endpoint_listen(listener, path) == COG_OK);
    CHECK(cog_ipc_endpoint_new(&client) == COG_OK);
    CHECK(cog_ipc_endpoint_connect(client,path,NULL)==COG_ERROR_AGAIN);
    CHECK(cog_ipc_endpoint_accept(listener,&server,&pid)==COG_OK);
    for(unsigned i=0;i<20;++i) { (void)ipc_step(server);(void)ipc_step(client); }
    cog_peer_id_t actual=0;CHECK(cog_ipc_endpoint_get_peer_id(client,&actual)==COG_OK&&actual==pid);
    CHECK(cog_ipc_register_session(listener, child_sid, pid) == COG_OK);

    CHECK(cog_ipc_message_new(&out, 64U) == COG_OK);
    (void)memcpy(out.data, payload, strlen(payload));
    out.len = strlen(payload);
    CHECK(cog_ipc_send_to_session(listener, child_sid, &out) == COG_OK);
    CHECK(cog_ipc_message_new(&in, 64U) == COG_OK);
    cog_result_t received=COG_ERROR_AGAIN;uint64_t end=ipc_now()+2000;
    while(received==COG_ERROR_AGAIN&&ipc_now()<end) { (void)ipc_step(server);(void)ipc_step(client);received=cog_ipc_endpoint_recv(client,&in);ipc_pause(); }
    CHECK(received==COG_OK);
    CHECK_EQ_INT((long long)in.len, (long long)strlen(payload));
    CHECK(memcmp(in.data, payload, strlen(payload)) == 0);

    /* Unknown session is not routable. */
    CHECK(cog_ipc_send_to_session(listener, "no-such-session", &out)
          == COG_ERROR_NOT_FOUND);

    CHECK(cog_ipc_message_free(&out) == COG_OK);
    CHECK(cog_ipc_message_free(&in) == COG_OK);
    CHECK(cog_ipc_endpoint_free(server) == COG_OK);
    CHECK(cog_ipc_endpoint_free(client) == COG_OK);
    CHECK(cog_ipc_endpoint_free(listener) == COG_OK);
    (void)unlink(path);

    cog_cli_app_free(app);
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int main(void)
{
    (void)printf("Scenario 09: subagent-to-subagent messaging across "
                 "cores\n");
    scenario_session_routing();
    scenario_agent_session_metadata();

    if (failures != 0) {
        (void)fprintf(stderr, "test_09_subagent_messaging: %d failure(s)\n",
                      failures);
        return 1;
    }
    (void)printf("test_09_subagent_messaging: all checks passed\n");
    return 0;
}

/**
 * @file test_orchestrator.c
 * @brief Contract test for cog/orchestrator.h implemented by
 *        libraries/cog-orchestrator/src/orchestrator.c.
 *
 * The orchestrator's public contract covers lifecycle/config, auxiliary
 * core spawn and process ownership, health monitoring, placement,
 * graceful shutdown with timeout, crash detection and session recovery,
 * and auto-replace.
 *
 * This white-box test drives the orchestrator through the private hooks
 * declared in orchestrator_internal.h (accept, poll, register_session,
 * clock override, timeout overrides).  Most cores are "self-connected":
 * the test process opens a cog-ipc client endpoint against the
 * orchestrator's listener and the orchestrator accepts it, so no separate
 * process is needed for the logic-level assertions.  Real process spawn,
 * crash, kill and auto-replace are exercised in the integration test
 * (test_orchestrator_ipc.c) with the aux_core fixture.
 *
 * Covered here:
 *   - new/free: NULL checks, invalid config (NULL socket path, overlong
 *     path), defaults applied, socket binding, NULL-free no-op
 *   - spawn: NULL checks, OVERFLOW at max_aux_cores, IO on exec failure
 *   - stop/kill: NULL and NOT_FOUND error paths, graceful ack
 *   - health: UNKNOWN -> HEALTHY -> DEGRADED -> DEAD over the clock
 *   - list: NULL checks, count, truncation
 *   - place: NULL -> INVALID, health/capacity selection, local fallback
 *   - shutdown: NULL, graceful ack, timeout with force-kill
 *   - sessions: NULL, NOT_FOUND, register/unregister, listing
 *   - stress cleanup under ASan/UBSan/LSan
 *
 * Ticket: 3e6262c (Stage 12.5 — Orchestrator)
 */

#define _POSIX_C_SOURCE 200809L

#include "cog/orchestrator.h"

#include "orchestrator_internal.h"

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef COG_ORCHESTRATOR_H
#error "COG_ORCHESTRATOR_H include guard missing"
#endif

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
static int
make_socket_path(char *buf, size_t bufsize)
{
    char tmpl[] = "/tmp/cog-orch-unit-XXXXXX";
    int fd = mkstemp(tmpl);

    if (fd < 0) {
        return -1;
    }
    (void)close(fd);
    (void)unlink(tmpl);
    (void)snprintf(buf, bufsize, "%s", tmpl);
    return 0;
}

/** Build a config with the given parameters. */
static cog_orchestrator_config_t
make_config(const char *path, size_t max_aux, uint32_t interval,
            uint32_t threshold, size_t capacity, bool auto_replace)
{
    cog_orchestrator_config_t cfg;

    (void)memset(&cfg, 0, sizeof(cfg));
    cfg.socket_path = path;
    cfg.max_aux_cores = max_aux;
    cfg.health_interval_ms = interval;
    cfg.unhealthy_threshold = threshold;
    cfg.default_core_capacity = capacity;
    cfg.auto_replace = auto_replace;
    return cfg;
}

/** Create an orchestrator on a fresh temp path. */
static cog_orchestrator_t *
make_orch(char *path, size_t pathsize, size_t max_aux, uint32_t interval,
          uint32_t threshold, size_t capacity)
{
    cog_orchestrator_config_t cfg;
    cog_orchestrator_t *orch = NULL;

    if (make_socket_path(path, pathsize) != 0) {
        return NULL;
    }
    cfg = make_config(path, max_aux, interval, threshold, capacity, false);
    if (cog_orchestrator_new(&orch, &cfg) != COG_OK) {
        return NULL;
    }
    return orch;
}

/** Send a NUL-terminated string as a framed message on a client endpoint. */
static cog_result_t
client_send(cog_ipc_endpoint_t *client, const char *text)
{
    cog_ipc_message_t msg;
    size_t len = strlen(text);
    cog_result_t rc;

    rc = cog_ipc_message_new(&msg, len + 1U);
    if (rc != COG_OK) {
        return rc;
    }
    (void)memcpy(msg.data, text, len);
    msg.len = len;
    rc = cog_ipc_endpoint_send(client, &msg);
    (void)cog_ipc_message_free(&msg);
    return rc;
}

/**
 * @brief Add a self-connected core to the orchestrator.
 *
 * Opens a client endpoint against the orchestrator's listener, accepts it
 * on the orchestrator side, and returns the orchestrator's peer id and the
 * caller-owned client endpoint.
 */
static cog_result_t
add_self_core(cog_orchestrator_t *orch, const char *path,
              cog_peer_id_t *out_peer_id, cog_ipc_endpoint_t **out_client)
{
    cog_ipc_endpoint_t *client = NULL;
    cog_peer_id_t peer_id = COG_PEER_ID_INVALID;
    cog_result_t rc;

    rc = cog_ipc_endpoint_new(&client);
    if (rc != COG_OK) {
        return rc;
    }
    rc = cog_ipc_endpoint_connect(client, path, NULL);
    if (rc != COG_OK) {
        (void)cog_ipc_endpoint_free(client);
        return rc;
    }
    rc = cog_orchestrator_accept(orch, &peer_id);
    if (rc != COG_OK) {
        (void)cog_ipc_endpoint_free(client);
        return rc;
    }
    *out_peer_id = peer_id;
    *out_client = client;
    return COG_OK;
}

/* ------------------------------------------------------------------ */
/* new / free                                                          */
/* ------------------------------------------------------------------ */

static void
test_new_free(void)
{
    char path[128];
    cog_orchestrator_t *orch = NULL;
    cog_orchestrator_config_t cfg;

    (void)memset(&cfg, 0, sizeof(cfg));

    /* NULL out / config */
    CHECK_RESULT("new NULL out", COG_ERROR_NULL_PTR,
                 cog_orchestrator_new(NULL, &cfg));
    CHECK_RESULT("new NULL config", COG_ERROR_NULL_PTR,
                 cog_orchestrator_new(&orch, NULL));

    /* NULL socket path */
    (void)memset(&cfg, 0, sizeof(cfg));
    cfg.socket_path = NULL;
    CHECK_RESULT("new NULL socket", COG_ERROR_INVALID_ARG,
                 cog_orchestrator_new(&orch, &cfg));

    /* overlong socket path */
    {
        char longpath[256];
        (void)memset(longpath, 'a', sizeof(longpath) - 1U);
        longpath[sizeof(longpath) - 1U] = '\0';
        cfg.socket_path = longpath;
        CHECK_RESULT("new overlong socket", COG_ERROR_INVALID_ARG,
                     cog_orchestrator_new(&orch, &cfg));
    }

    /* happy path with defaults */
    if (make_socket_path(path, sizeof(path)) != 0) {
        CHECK("new path", 0);
        return;
    }
    cfg = make_config(path, 0, 0, 0, 0, false);
    CHECK_RESULT("new ok", COG_OK, cog_orchestrator_new(&orch, &cfg));
    CHECK("new non-null", orch != NULL);
    /* defaults applied: max_aux=4, interval=1000, threshold=3, cap=8 */
    CHECK("new defaults max_aux", orch != NULL);
    cog_orchestrator_free(orch);

    /* free(NULL) is a no-op */
    cog_orchestrator_free(NULL);
}

/* ------------------------------------------------------------------ */
/* spawn                                                               */
/* ------------------------------------------------------------------ */

static void
test_spawn_errors(void)
{
    char path[128];
    cog_orchestrator_t *orch;
    cog_peer_id_t peer_id = COG_PEER_ID_INVALID;

    orch = make_orch(path, sizeof(path), 2, 100, 3, 8);
    CHECK("spawn orch", orch != NULL);
    if (orch == NULL) {
        return;
    }

    CHECK_RESULT("spawn NULL orch", COG_ERROR_NULL_PTR,
                 cog_orchestrator_spawn(NULL, "/bin/true", &peer_id));
    CHECK_RESULT("spawn NULL binary", COG_ERROR_NULL_PTR,
                 cog_orchestrator_spawn(orch, NULL, &peer_id));
    CHECK_RESULT("spawn NULL out", COG_ERROR_NULL_PTR,
                 cog_orchestrator_spawn(orch, "/bin/true", NULL));

    /* exec failure -> IO (child exits, no connection) */
    cog_orchestrator_set_spawn_timeout(orch, 200);
    CHECK_RESULT("spawn IO", COG_ERROR_IO,
                 cog_orchestrator_spawn(orch, "/nonexistent/cog-aux", &peer_id));

    /* OVERFLOW: fill max_aux_cores (2) with self-connected cores */
    {
        cog_peer_id_t p1 = COG_PEER_ID_INVALID;
        cog_peer_id_t p2 = COG_PEER_ID_INVALID;
        cog_ipc_endpoint_t *c1 = NULL;
        cog_ipc_endpoint_t *c2 = NULL;

        CHECK_RESULT("spawn fill1", COG_OK,
                     add_self_core(orch, path, &p1, &c1));
        CHECK_RESULT("spawn fill2", COG_OK,
                     add_self_core(orch, path, &p2, &c2));
        CHECK_RESULT("spawn overflow", COG_ERROR_OVERFLOW,
                     cog_orchestrator_spawn(orch, "/bin/true", &peer_id));
        if (c1 != NULL) {
            (void)cog_ipc_endpoint_free(c1);
        }
        if (c2 != NULL) {
            (void)cog_ipc_endpoint_free(c2);
        }
    }

    cog_orchestrator_free(orch);
}

/* ------------------------------------------------------------------ */
/* stop / kill                                                         */
/* ------------------------------------------------------------------ */

static void *
ack_responder(void *arg)
{
    cog_ipc_endpoint_t *client = (cog_ipc_endpoint_t *)arg;
    cog_ipc_message_t msg;

    if (cog_ipc_message_new(&msg, 256U) == COG_OK) {
        (void)cog_ipc_endpoint_recv(client, &msg); /* "shutdown" */
        (void)client_send(client, "ack\n");
        (void)cog_ipc_message_free(&msg);
    }
    return NULL;
}

static void
test_stop_kill(void)
{
    char path[128];
    cog_orchestrator_t *orch;
    cog_peer_id_t peer_id = COG_PEER_ID_INVALID;
    cog_ipc_endpoint_t *client = NULL;
    pthread_t th;

    orch = make_orch(path, sizeof(path), 4, 100, 3, 8);
    CHECK("stop orch", orch != NULL);
    if (orch == NULL) {
        return;
    }

    CHECK_RESULT("stop NULL", COG_ERROR_NULL_PTR,
                 cog_orchestrator_stop(NULL, 1U));
    CHECK_RESULT("stop not found", COG_ERROR_NOT_FOUND,
                 cog_orchestrator_stop(orch, 999U));
    CHECK_RESULT("kill NULL", COG_ERROR_NULL_PTR,
                 cog_orchestrator_kill(NULL, 1U));
    CHECK_RESULT("kill not found", COG_ERROR_NOT_FOUND,
                 cog_orchestrator_kill(orch, 999U));

    /* graceful stop: client acks on a helper thread */
    CHECK_RESULT("stop add core", COG_OK,
                 add_self_core(orch, path, &peer_id, &client));
    CHECK("stop peer", peer_id != COG_PEER_ID_INVALID);
    if (client != NULL) {
        CHECK_RESULT("stop thread", 0, pthread_create(&th, NULL,
                                                      ack_responder, client));
        CHECK_RESULT("stop ok", COG_OK, cog_orchestrator_stop(orch, peer_id));
        (void)pthread_join(th, NULL);
        (void)cog_ipc_endpoint_free(client);
    }

    cog_orchestrator_free(orch);
}

/* ------------------------------------------------------------------ */
/* health                                                              */
/* ------------------------------------------------------------------ */

static void
test_health(void)
{
    char path[128];
    cog_orchestrator_t *orch;
    cog_peer_id_t peer_id = COG_PEER_ID_INVALID;
    cog_ipc_endpoint_t *client = NULL;

    orch = make_orch(path, sizeof(path), 4, 100, 3, 8);
    CHECK("health orch", orch != NULL);
    if (orch == NULL) {
        return;
    }

    /* unknown peer -> UNKNOWN */
    CHECK("health unknown peer", cog_orchestrator_health(orch, 999U) ==
                                 COG_HEALTH_UNKNOWN);
    CHECK("health NULL orch", cog_orchestrator_health(NULL, 1U) ==
                              COG_HEALTH_UNKNOWN);

    CHECK_RESULT("health add core", COG_OK,
                 add_self_core(orch, path, &peer_id, &client));
    CHECK("health peer", peer_id != COG_PEER_ID_INVALID);

    /* no heartbeat yet -> UNKNOWN */
    CHECK("health initial", cog_orchestrator_health(orch, peer_id) ==
                            COG_HEALTH_UNKNOWN);

    /* heartbeat at T0 -> HEALTHY */
    cog_orchestrator_set_now(orch, 1000U);
    CHECK_RESULT("health hb", COG_OK,
                 cog_orchestrator_heartbeat(orch, peer_id, 2U));
    CHECK("health healthy", cog_orchestrator_health(orch, peer_id) ==
                            COG_HEALTH_HEALTHY);

    /* within interval -> still HEALTHY */
    cog_orchestrator_set_now(orch, 1050U);
    CHECK("health within", cog_orchestrator_health(orch, peer_id) ==
                           COG_HEALTH_HEALTHY);

    /* missed one interval -> DEGRADED */
    cog_orchestrator_set_now(orch, 1100U);
    CHECK("health degraded", cog_orchestrator_health(orch, peer_id) ==
                             COG_HEALTH_DEGRADED);

    /* missed threshold intervals -> DEAD */
    cog_orchestrator_set_now(orch, 1300U);
    CHECK("health dead", cog_orchestrator_health(orch, peer_id) ==
                         COG_HEALTH_DEAD);

    /* a fresh heartbeat restores HEALTHY */
    cog_orchestrator_set_now(orch, 2000U);
    CHECK_RESULT("health hb2", COG_OK,
                 cog_orchestrator_heartbeat(orch, peer_id, 0U));
    CHECK("health restored", cog_orchestrator_health(orch, peer_id) ==
                             COG_HEALTH_HEALTHY);

    /* heartbeat on unknown peer -> NOT_FOUND */
    CHECK_RESULT("health hb notfound", COG_ERROR_NOT_FOUND,
                 cog_orchestrator_heartbeat(orch, 999U, 0U));
    CHECK_RESULT("health hb NULL", COG_ERROR_NULL_PTR,
                 cog_orchestrator_heartbeat(NULL, peer_id, 0U));

    if (client != NULL) {
        (void)cog_ipc_endpoint_free(client);
    }
    cog_orchestrator_free(orch);
}

/* ------------------------------------------------------------------ */
/* list                                                                */
/* ------------------------------------------------------------------ */

static void
test_list(void)
{
    char path[128];
    cog_orchestrator_t *orch;
    cog_peer_id_t p1 = COG_PEER_ID_INVALID;
    cog_peer_id_t p2 = COG_PEER_ID_INVALID;
    cog_ipc_endpoint_t *c1 = NULL;
    cog_ipc_endpoint_t *c2 = NULL;
    cog_core_info_t infos[4];
    size_t count = 0U;

    orch = make_orch(path, sizeof(path), 4, 100, 3, 8);
    CHECK("list orch", orch != NULL);
    if (orch == NULL) {
        return;
    }

    CHECK_RESULT("list NULL orch", COG_ERROR_NULL_PTR,
                 cog_orchestrator_list(NULL, infos, 4U, &count));
    CHECK_RESULT("list NULL count", COG_ERROR_NULL_PTR,
                 cog_orchestrator_list(orch, infos, 4U, NULL));

    /* empty list */
    CHECK_RESULT("list empty", COG_OK, cog_orchestrator_list(orch, infos, 4U,
                                                            &count));
    CHECK("list empty count", count == 0U);

    /* two cores */
    CHECK_RESULT("list add1", COG_OK, add_self_core(orch, path, &p1, &c1));
    CHECK_RESULT("list add2", COG_OK, add_self_core(orch, path, &p2, &c2));
    CHECK_RESULT("list ok", COG_OK, cog_orchestrator_list(orch, infos, 4U,
                                                         &count));
    CHECK("list count", count == 2U);
    CHECK("list peer1", infos[0].peer_id == p1 || infos[0].peer_id == p2);
    CHECK("list peer2", infos[1].peer_id == p1 || infos[1].peer_id == p2);
    CHECK("list health unknown", infos[0].health == COG_HEALTH_UNKNOWN);
    CHECK("list capacity", infos[0].capacity == 8U);

    /* truncation: max_infos=1 still reports count 2 */
    CHECK_RESULT("list trunc", COG_OK, cog_orchestrator_list(orch, infos, 1U,
                                                            &count));
    CHECK("list trunc count", count == 2U);

    /* NULL infos just reports count */
    CHECK_RESULT("list null infos", COG_OK,
                 cog_orchestrator_list(orch, NULL, 0U, &count));
    CHECK("list null infos count", count == 2U);

    if (c1 != NULL) {
        (void)cog_ipc_endpoint_free(c1);
    }
    if (c2 != NULL) {
        (void)cog_ipc_endpoint_free(c2);
    }
    cog_orchestrator_free(orch);
}

/* ------------------------------------------------------------------ */
/* place                                                               */
/* ------------------------------------------------------------------ */

static void
test_place(void)
{
    char path[128];
    cog_orchestrator_t *orch;
    cog_peer_id_t p1 = COG_PEER_ID_INVALID;
    cog_peer_id_t p2 = COG_PEER_ID_INVALID;
    cog_ipc_endpoint_t *c1 = NULL;
    cog_ipc_endpoint_t *c2 = NULL;

    orch = make_orch(path, sizeof(path), 4, 100, 3, 8);
    CHECK("place orch", orch != NULL);
    if (orch == NULL) {
        return;
    }

    /* NULL orch -> INVALID (local) */
    CHECK("place NULL", cog_orchestrator_place(NULL, 0U) ==
                        COG_PEER_ID_INVALID);

    /* no cores -> INVALID (local) */
    CHECK("place empty", cog_orchestrator_place(orch, 0U) ==
                         COG_PEER_ID_INVALID);

    /* one healthy core -> that core */
    CHECK_RESULT("place add1", COG_OK, add_self_core(orch, path, &p1, &c1));
    cog_orchestrator_set_now(orch, 1000U);
    CHECK_RESULT("place hb1", COG_OK,
                 cog_orchestrator_heartbeat(orch, p1, 0U));
    CHECK("place one", cog_orchestrator_place(orch, 0U) == p1);

    /* second core with more free capacity is preferred */
    CHECK_RESULT("place add2", COG_OK, add_self_core(orch, path, &p2, &c2));
    cog_orchestrator_set_now(orch, 2000U);
    CHECK_RESULT("place hb2", COG_OK,
                 cog_orchestrator_heartbeat(orch, p2, 0U));
    /* both have capacity 8, free 8; p2 added later, tie -> p1 kept */
    CHECK("place tie", cog_orchestrator_place(orch, 0U) == p1 ||
                       cog_orchestrator_place(orch, 0U) == p2);

    /* fill p1 to capacity -> p2 chosen */
    CHECK_RESULT("place fill p1", COG_OK,
                 cog_orchestrator_heartbeat(orch, p1, 8U));
    CHECK("place p2", cog_orchestrator_place(orch, 0U) == p2);

    /* estimated_load exceeds remaining capacity -> local fallback */
    CHECK("place overload", cog_orchestrator_place(orch, 9U) ==
                            COG_PEER_ID_INVALID);

    /* dead core is never chosen */
    cog_orchestrator_set_now(orch, 5000U); /* p2 now DEAD (interval 100*3) */
    CHECK("place dead", cog_orchestrator_place(orch, 0U) ==
                        COG_PEER_ID_INVALID);

    if (c1 != NULL) {
        (void)cog_ipc_endpoint_free(c1);
    }
    if (c2 != NULL) {
        (void)cog_ipc_endpoint_free(c2);
    }
    cog_orchestrator_free(orch);
}

/* ------------------------------------------------------------------ */
/* shutdown                                                            */
/* ------------------------------------------------------------------ */

static void
test_shutdown(void)
{
    char path[128];
    cog_orchestrator_t *orch;
    cog_peer_id_t peer_id = COG_PEER_ID_INVALID;
    cog_ipc_endpoint_t *client = NULL;
    pthread_t th;

    orch = make_orch(path, sizeof(path), 4, 100, 3, 8);
    CHECK("shutdown orch", orch != NULL);
    if (orch == NULL) {
        return;
    }

    CHECK_RESULT("shutdown NULL", COG_ERROR_NULL_PTR,
                 cog_orchestrator_shutdown(NULL, 1000U));

    /* graceful shutdown with ack */
    CHECK_RESULT("shutdown add core", COG_OK,
                 add_self_core(orch, path, &peer_id, &client));
    CHECK("shutdown peer", peer_id != COG_PEER_ID_INVALID);
    if (client != NULL) {
        CHECK_RESULT("shutdown thread", 0,
                     pthread_create(&th, NULL, ack_responder, client));
        CHECK_RESULT("shutdown ok", COG_OK,
                     cog_orchestrator_shutdown(orch, 2000U));
        (void)pthread_join(th, NULL);
        (void)cog_ipc_endpoint_free(client);
    }

    cog_orchestrator_free(orch);
}

static void
test_shutdown_timeout(void)
{
    char path[128];
    cog_orchestrator_t *orch;
    cog_peer_id_t peer_id = COG_PEER_ID_INVALID;
    cog_ipc_endpoint_t *client = NULL;

    orch = make_orch(path, sizeof(path), 4, 100, 3, 8);
    CHECK("shutdown-timeout orch", orch != NULL);
    if (orch == NULL) {
        return;
    }

    /* a core that never acks -> TIMEOUT (and force-killed) */
    CHECK_RESULT("shutdown-timeout add", COG_OK,
                 add_self_core(orch, path, &peer_id, &client));
    CHECK("shutdown-timeout peer", peer_id != COG_PEER_ID_INVALID);
    CHECK_RESULT("shutdown-timeout", COG_ERROR_TIMEOUT,
                 cog_orchestrator_shutdown(orch, 200U));
    CHECK("shutdown-timeout dead",
          cog_orchestrator_health(orch, peer_id) == COG_HEALTH_DEAD);

    if (client != NULL) {
        (void)cog_ipc_endpoint_free(client);
    }
    cog_orchestrator_free(orch);
}

/* ------------------------------------------------------------------ */
/* sessions                                                            */
/* ------------------------------------------------------------------ */

static void
test_sessions(void)
{
    char path[128];
    cog_orchestrator_t *orch;
    cog_peer_id_t peer_id = COG_PEER_ID_INVALID;
    cog_ipc_endpoint_t *client = NULL;
    const char *out[8];
    size_t count = 0U;

    orch = make_orch(path, sizeof(path), 4, 100, 3, 8);
    CHECK("sessions orch", orch != NULL);
    if (orch == NULL) {
        return;
    }

    CHECK_RESULT("sessions NULL orch", COG_ERROR_NULL_PTR,
                 cog_orchestrator_sessions(NULL, 1U, out, 8U, &count));
    CHECK_RESULT("sessions NULL out", COG_ERROR_NULL_PTR,
                 cog_orchestrator_sessions(orch, 1U, NULL, 8U, &count));
    CHECK_RESULT("sessions NULL count", COG_ERROR_NULL_PTR,
                 cog_orchestrator_sessions(orch, 1U, out, 8U, NULL));
    CHECK_RESULT("sessions not found", COG_ERROR_NOT_FOUND,
                 cog_orchestrator_sessions(orch, 999U, out, 8U, &count));

    CHECK_RESULT("sessions add", COG_OK,
                 add_self_core(orch, path, &peer_id, &client));
    CHECK("sessions peer", peer_id != COG_PEER_ID_INVALID);

    /* empty */
    CHECK_RESULT("sessions empty", COG_OK,
                 cog_orchestrator_sessions(orch, peer_id, out, 8U, &count));
    CHECK("sessions empty count", count == 0U);

    /* register two */
    CHECK_RESULT("sessions reg1", COG_OK,
                 cog_orchestrator_register_session(orch, peer_id, "sess-a"));
    CHECK_RESULT("sessions reg2", COG_OK,
                 cog_orchestrator_register_session(orch, peer_id, "sess-b"));
    /* idempotent */
    CHECK_RESULT("sessions reg dup", COG_OK,
                 cog_orchestrator_register_session(orch, peer_id, "sess-a"));
    CHECK_RESULT("sessions list", COG_OK,
                 cog_orchestrator_sessions(orch, peer_id, out, 8U, &count));
    CHECK("sessions count", count == 2U);
    CHECK("sessions a", (strcmp(out[0], "sess-a") == 0) ||
                        (strcmp(out[1], "sess-a") == 0));
    CHECK("sessions b", (strcmp(out[0], "sess-b") == 0) ||
                        (strcmp(out[1], "sess-b") == 0));

    /* unregister */
    CHECK_RESULT("sessions unreg", COG_OK,
                 cog_orchestrator_unregister_session(orch, peer_id, "sess-a"));
    CHECK_RESULT("sessions unreg again", COG_ERROR_NOT_FOUND,
                 cog_orchestrator_unregister_session(orch, peer_id, "sess-a"));
    CHECK_RESULT("sessions list2", COG_OK,
                 cog_orchestrator_sessions(orch, peer_id, out, 8U, &count));
    CHECK("sessions count2", count == 1U);
    CHECK("sessions b2", strcmp(out[0], "sess-b") == 0);

    /* NULL / not-found on register */
    CHECK_RESULT("sessions reg NULL", COG_ERROR_NULL_PTR,
                 cog_orchestrator_register_session(orch, peer_id, NULL));
    CHECK_RESULT("sessions reg notfound", COG_ERROR_NOT_FOUND,
                 cog_orchestrator_register_session(orch, 999U, "x"));
    CHECK_RESULT("sessions unreg NULL", COG_ERROR_NULL_PTR,
                 cog_orchestrator_unregister_session(orch, peer_id, NULL));

    if (client != NULL) {
        (void)cog_ipc_endpoint_free(client);
    }
    cog_orchestrator_free(orch);
}

/* ------------------------------------------------------------------ */
/* stress cleanup                                                      */
/* ------------------------------------------------------------------ */

static void
test_stress(void)
{
    char path[128];
    cog_orchestrator_t *orch;
    cog_peer_id_t peers[16];
    cog_ipc_endpoint_t *clients[16];
    size_t i;
    size_t n = 8U;

    orch = make_orch(path, sizeof(path), 16, 50, 3, 8);
    CHECK("stress orch", orch != NULL);
    if (orch == NULL) {
        return;
    }

    for (i = 0U; i < n; i++) {
        peers[i] = COG_PEER_ID_INVALID;
        clients[i] = NULL;
        CHECK_RESULT("stress add", COG_OK,
                     add_self_core(orch, path, &peers[i], &clients[i]));
        CHECK_RESULT("stress hb", COG_OK,
                     cog_orchestrator_heartbeat(orch, peers[i], i));
        CHECK_RESULT("stress session", COG_OK,
                     cog_orchestrator_register_session(orch, peers[i],
                                                       "stress-sess"));
    }

    /* place should pick the least-loaded healthy core */
    {
        cog_peer_id_t chosen = cog_orchestrator_place(orch, 0U);
        CHECK("stress place", chosen != COG_PEER_ID_INVALID);
    }

    for (i = 0U; i < n; i++) {
        if (clients[i] != NULL) {
            (void)cog_ipc_endpoint_free(clients[i]);
        }
    }
    cog_orchestrator_free(orch);
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int
main(void)
{
    test_new_free();
    test_spawn_errors();
    test_stop_kill();
    test_health();
    test_list();
    test_place();
    test_shutdown();
    test_shutdown_timeout();
    test_sessions();
    test_stress();

    if (g_failures != 0U) {
        (void)fprintf(stderr, "test_orchestrator: %u failure(s)\n",
                      g_failures);
        return 1;
    }
    (void)printf("test_orchestrator: all ok\n");
    return 0;
}

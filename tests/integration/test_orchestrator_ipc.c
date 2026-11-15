/**
 * @file test_orchestrator_ipc.c
 * @brief Integration test: real auxiliary-core processes over IPC.
 *
 * Wires the orchestrator to the aux_core fixture process over a local Unix
 * domain socket and proves the full process lifecycle:
 *
 *   - spawn (fork+exec) and process ownership
 *   - health monitoring via real heartbeats
 *   - placement among healthy cores
 *   - graceful shutdown with acknowledgement
 *   - shutdown timeout when a core never acks (force-kill)
 *   - crash detection and session recovery
 *   - force kill
 *   - auto-replace of a dead core
 *
 * The fixture binary path is supplied at compile time via
 * COG_AUX_CORE_BIN.  The fixture is owned exclusively by this test; every
 * spawned pid is recorded and reaped by the orchestrator.  No process is
 * killed by name and no unrelated process is signalled.
 *
 * Ticket: 3e6262c (Stage 12.5 — Orchestrator)
 */

#define _POSIX_C_SOURCE 200809L

#include "cog/orchestrator.h"

#include "orchestrator_internal.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifndef COG_AUX_CORE_BIN
#error "COG_AUX_CORE_BIN must be defined to the aux_core fixture path"
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
    char tmpl[] = "/tmp/cog-orch-ipc-XXXXXX";
    int fd = mkstemp(tmpl);

    if (fd < 0) {
        return -1;
    }
    (void)close(fd);
    (void)unlink(tmpl);
    (void)snprintf(buf, bufsize, "%s", tmpl);
    return 0;
}

/** Set an environment variable for the next spawned fixture. */
static void
set_aux_env(const char *mode, const char *session)
{
    if (mode != NULL) {
        (void)setenv("COG_AUX_MODE", mode, 1);
    }
    if (session != NULL) {
        (void)setenv("COG_AUX_SESSION", session, 1);
    } else {
        (void)unsetenv("COG_AUX_SESSION");
    }
    (void)setenv("COG_AUX_INTERVAL_MS", "40", 1);
}

/**
 * @brief Drive the orchestrator until @p pred returns true or timeout.
 */
static int
poll_until(cog_orchestrator_t *orch, int (*pred)(cog_orchestrator_t *),
           int timeout_ms)
{
    struct timespec ts;
    int waited = 0;

    ts.tv_sec = 0;
    ts.tv_nsec = 10000000L; /* 10 ms */
    while (waited < timeout_ms) {
        (void)cog_orchestrator_poll(orch, 0);
        if (pred(orch)) {
            return 0;
        }
        (void)nanosleep(&ts, NULL);
        waited += 10;
    }
    return -1;
}

/** Predicate: the given peer is HEALTHY. */
static cog_peer_id_t g_healthy_peer = COG_PEER_ID_INVALID;
static int
pred_healthy(cog_orchestrator_t *orch)
{
    return cog_orchestrator_health(orch, g_healthy_peer) == COG_HEALTH_HEALTHY;
}

/** Predicate: the given peer is DEAD. */
static cog_peer_id_t g_dead_peer = COG_PEER_ID_INVALID;
static int
pred_dead(cog_orchestrator_t *orch)
{
    return cog_orchestrator_health(orch, g_dead_peer) == COG_HEALTH_DEAD;
}

/** Predicate: the orchestrator has exactly one core. */
static int
pred_one_core(cog_orchestrator_t *orch)
{
    size_t count = 0U;

    (void)cog_orchestrator_list(orch, NULL, 0U, &count);
    return count == 1U;
}

/** Predicate: the given peer reports exactly one session. */
static cog_peer_id_t g_sess_peer = COG_PEER_ID_INVALID;
static int
pred_has_session(cog_orchestrator_t *orch)
{
    const char *sessions[8];
    size_t count = 0U;

    if (cog_orchestrator_sessions(orch, g_sess_peer, sessions, 8U, &count) !=
        COG_OK) {
        return 0;
    }
    return count == 1U;
}

/* ------------------------------------------------------------------ */
/* Spawn + health + placement                                          */
/* ------------------------------------------------------------------ */

static void
test_spawn_health_place(void)
{
    char path[128];
    cog_orchestrator_config_t cfg;
    cog_orchestrator_t *orch = NULL;
    cog_peer_id_t peer = COG_PEER_ID_INVALID;
    cog_core_info_t infos[4];
    size_t count = 0U;

    if (make_socket_path(path, sizeof(path)) != 0) {
        CHECK("shp path", 0);
        return;
    }
    (void)memset(&cfg, 0, sizeof(cfg));
    cfg.socket_path = path;
    cfg.max_aux_cores = 4;
    cfg.health_interval_ms = 200;
    cfg.unhealthy_threshold = 3;
    cfg.default_core_capacity = 8;
    cfg.auto_replace = false;
    CHECK_RESULT("shp new", COG_OK, cog_orchestrator_new(&orch, &cfg));
    if (orch == NULL) {
        return;
    }

    set_aux_env("hb", NULL);
    CHECK_RESULT("shp spawn", COG_OK,
                 cog_orchestrator_spawn(orch, COG_AUX_CORE_BIN, &peer));
    CHECK("shp peer", peer != COG_PEER_ID_INVALID);

    /* wait for the first heartbeat */
    g_healthy_peer = peer;
    CHECK("shp healthy", poll_until(orch, pred_healthy, 3000) == 0);

    /* placement returns the healthy core */
    CHECK("shp place", cog_orchestrator_place(orch, 0U) == peer);

    /* list reports one core with the right capacity */
    CHECK_RESULT("shp list", COG_OK,
                 cog_orchestrator_list(orch, infos, 4U, &count));
    CHECK("shp list count", count == 1U);
    CHECK("shp list peer", infos[0].peer_id == peer);
    CHECK("shp list cap", infos[0].capacity == 8U);

    /* graceful shutdown */
    CHECK_RESULT("shp shutdown", COG_OK, cog_orchestrator_shutdown(orch, 2000));
    CHECK("shp dead after shutdown",
          cog_orchestrator_health(orch, peer) == COG_HEALTH_DEAD);

    cog_orchestrator_free(orch);
}

/* ------------------------------------------------------------------ */
/* Session recovery on crash                                           */
/* ------------------------------------------------------------------ */

static void
test_crash_recovery(void)
{
    char path[128];
    cog_orchestrator_config_t cfg;
    cog_orchestrator_t *orch = NULL;
    cog_peer_id_t peer = COG_PEER_ID_INVALID;
    const char *sessions[8];
    size_t count = 0U;

    if (make_socket_path(path, sizeof(path)) != 0) {
        CHECK("crash path", 0);
        return;
    }
    (void)memset(&cfg, 0, sizeof(cfg));
    cfg.socket_path = path;
    cfg.max_aux_cores = 4;
    cfg.health_interval_ms = 200;
    cfg.unhealthy_threshold = 3;
    cfg.default_core_capacity = 8;
    cfg.auto_replace = false;
    CHECK_RESULT("crash new", COG_OK, cog_orchestrator_new(&orch, &cfg));
    if (orch == NULL) {
        return;
    }

    /* fixture registers a session, heartbeats once, then crashes */
    set_aux_env("crash", "crash-sess");
    CHECK_RESULT("crash spawn", COG_OK,
                 cog_orchestrator_spawn(orch, COG_AUX_CORE_BIN, &peer));
    CHECK("crash peer", peer != COG_PEER_ID_INVALID);

    g_dead_peer = peer;
    CHECK("crash detected", poll_until(orch, pred_dead, 3000) == 0);

    /* session recovery: the crashed core still reports its sessions.
     * The session message is drained asynchronously, so poll until it
     * is registered before asserting on it. */
    g_sess_peer = peer;
    CHECK("crash session drained", poll_until(orch, pred_has_session, 3000) == 0);
    CHECK_RESULT("crash sessions", COG_OK,
                 cog_orchestrator_sessions(orch, peer, sessions, 8U, &count));
    CHECK("crash sessions count", count == 1U);
    CHECK("crash sessions id", count == 1U &&
          strcmp(sessions[0], "crash-sess") == 0);

    cog_orchestrator_free(orch);
}

/* ------------------------------------------------------------------ */
/* Force kill                                                          */
/* ------------------------------------------------------------------ */

static void
test_kill(void)
{
    char path[128];
    cog_orchestrator_config_t cfg;
    cog_orchestrator_t *orch = NULL;
    cog_peer_id_t peer = COG_PEER_ID_INVALID;

    if (make_socket_path(path, sizeof(path)) != 0) {
        CHECK("kill path", 0);
        return;
    }
    (void)memset(&cfg, 0, sizeof(cfg));
    cfg.socket_path = path;
    cfg.max_aux_cores = 4;
    cfg.health_interval_ms = 200;
    cfg.unhealthy_threshold = 3;
    cfg.default_core_capacity = 8;
    cfg.auto_replace = false;
    CHECK_RESULT("kill new", COG_OK, cog_orchestrator_new(&orch, &cfg));
    if (orch == NULL) {
        return;
    }

    set_aux_env("hb", NULL);
    CHECK_RESULT("kill spawn", COG_OK,
                 cog_orchestrator_spawn(orch, COG_AUX_CORE_BIN, &peer));
    g_healthy_peer = peer;
    CHECK("kill healthy", poll_until(orch, pred_healthy, 3000) == 0);

    CHECK_RESULT("kill ok", COG_OK, cog_orchestrator_kill(orch, peer));
    CHECK("kill dead", cog_orchestrator_health(orch, peer) == COG_HEALTH_DEAD);

    cog_orchestrator_free(orch);
}

/* ------------------------------------------------------------------ */
/* Auto-replace                                                        */
/* ------------------------------------------------------------------ */

static void
test_auto_replace(void)
{
    char path[128];
    cog_orchestrator_config_t cfg;
    cog_orchestrator_t *orch = NULL;
    cog_peer_id_t peer = COG_PEER_ID_INVALID;
    cog_peer_id_t replacement = COG_PEER_ID_INVALID;
    cog_core_info_t infos[4];
    size_t count = 0U;

    if (make_socket_path(path, sizeof(path)) != 0) {
        CHECK("ar path", 0);
        return;
    }
    (void)memset(&cfg, 0, sizeof(cfg));
    cfg.socket_path = path;
    cfg.max_aux_cores = 4;
    cfg.health_interval_ms = 200;
    cfg.unhealthy_threshold = 3;
    cfg.default_core_capacity = 8;
    cfg.auto_replace = true;
    CHECK_RESULT("ar new", COG_OK, cog_orchestrator_new(&orch, &cfg));
    if (orch == NULL) {
        return;
    }

    set_aux_env("hb", NULL);
    CHECK_RESULT("ar spawn", COG_OK,
                 cog_orchestrator_spawn(orch, COG_AUX_CORE_BIN, &peer));
    g_healthy_peer = peer;
    CHECK("ar healthy", poll_until(orch, pred_healthy, 3000) == 0);

    /* kill the core; auto-replace should spawn a healthy replacement.
     * The replacement reuses the dead core's slot, so the old peer id is
     * gone as soon as the replacement connects. */
    CHECK_RESULT("ar kill", COG_OK, cog_orchestrator_kill(orch, peer));

    /* drive until the replacement connects and heartbeats */
    CHECK("ar one core", poll_until(orch, pred_one_core, 3000) == 0);
    CHECK_RESULT("ar list", COG_OK,
                 cog_orchestrator_list(orch, infos, 4U, &count));
    CHECK("ar count", count == 1U);
    replacement = infos[0].peer_id;
    CHECK("ar replaced", replacement != peer);
    g_healthy_peer = replacement;
    CHECK("ar replacement healthy", poll_until(orch, pred_healthy, 3000) == 0);

    cog_orchestrator_free(orch);
}

/* ------------------------------------------------------------------ */
/* Shutdown timeout (noack fixture)                                    */
/* ------------------------------------------------------------------ */

static void
test_shutdown_timeout(void)
{
    char path[128];
    cog_orchestrator_config_t cfg;
    cog_orchestrator_t *orch = NULL;
    cog_peer_id_t peer = COG_PEER_ID_INVALID;

    if (make_socket_path(path, sizeof(path)) != 0) {
        CHECK("st path", 0);
        return;
    }
    (void)memset(&cfg, 0, sizeof(cfg));
    cfg.socket_path = path;
    cfg.max_aux_cores = 4;
    cfg.health_interval_ms = 200;
    cfg.unhealthy_threshold = 3;
    cfg.default_core_capacity = 8;
    cfg.auto_replace = false;
    CHECK_RESULT("st new", COG_OK, cog_orchestrator_new(&orch, &cfg));
    if (orch == NULL) {
        return;
    }

    /* noack fixture heartbeats but never acks shutdown */
    set_aux_env("noack", NULL);
    CHECK_RESULT("st spawn", COG_OK,
                 cog_orchestrator_spawn(orch, COG_AUX_CORE_BIN, &peer));
    g_healthy_peer = peer;
    CHECK("st healthy", poll_until(orch, pred_healthy, 3000) == 0);

    CHECK_RESULT("st timeout", COG_ERROR_TIMEOUT,
                 cog_orchestrator_shutdown(orch, 300));
    CHECK("st dead", cog_orchestrator_health(orch, peer) == COG_HEALTH_DEAD);

    cog_orchestrator_free(orch);
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int
main(void)
{
    test_spawn_health_place();
    test_crash_recovery();
    test_kill();
    test_auto_replace();
    test_shutdown_timeout();

    if (g_failures != 0U) {
        (void)fprintf(stderr, "test_orchestrator_ipc: %u failure(s)\n",
                      g_failures);
        return 1;
    }
    (void)printf("test_orchestrator_ipc: all ok\n");
    return 0;
}

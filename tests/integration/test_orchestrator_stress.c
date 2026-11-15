/**
 * @file test_orchestrator_stress.c
 * @brief Critic process-lifecycle stress tests for the orchestrator.
 *
 * Probes concrete gaps beyond the Writer's coverage:
 *
 *   - kill on a self-connected core must NOT signal the test process
 *     (no signaling of non-spawned / self-connected peers).
 *   - repeated spawn/kill cycles must not leak children or leave zombies;
 *     every recorded pid is reaped by the orchestrator.
 *   - auto-replace under repeated kill must reuse the dead core's slot
 *     (core_count stays 1) without duplicate registration or stale
 *     endpoint/agent state.
 *   - stop/shutdown on a core that acks but never exits must return
 *     (bounded) rather than block forever waiting for the process.
 *
 * The fixture binary path is supplied at compile time via
 * COG_AUX_CORE_BIN.  Every spawned pid is recorded and reaped by the
 * orchestrator; no process is killed by name and no unrelated process is
 * signalled.
 *
 * Ticket: 3e6262c (Stage 12.5 — Orchestrator, critic stress)
 */

#define _POSIX_C_SOURCE 200809L

#include "cog/orchestrator.h"

#include "orchestrator_internal.h"

#include <pthread.h>
#include <signal.h>
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

static int
make_socket_path(char *buf, size_t bufsize)
{
    char tmpl[] = "/tmp/cog-orch-stress-XXXXXX";
    int fd = mkstemp(tmpl);

    if (fd < 0) {
        return -1;
    }
    (void)close(fd);
    (void)unlink(tmpl);
    (void)snprintf(buf, bufsize, "%s", tmpl);
    return 0;
}

static void
set_aux_env(const char *mode)
{
    if (mode != NULL) {
        (void)setenv("COG_AUX_MODE", mode, 1);
    }
    (void)unsetenv("COG_AUX_SESSION");
    (void)setenv("COG_AUX_INTERVAL_MS", "40", 1);
}

static int
poll_until(cog_orchestrator_t *orch, int (*pred)(cog_orchestrator_t *),
           int timeout_ms)
{
    struct timespec ts;
    int waited = 0;

    ts.tv_sec = 0;
    ts.tv_nsec = 10000000L;
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

static cog_peer_id_t g_healthy_peer = COG_PEER_ID_INVALID;
static int
pred_healthy(cog_orchestrator_t *orch)
{
    return cog_orchestrator_health(orch, g_healthy_peer) == COG_HEALTH_HEALTHY;
}

static cog_peer_id_t g_dead_peer = COG_PEER_ID_INVALID;
static int
pred_dead(cog_orchestrator_t *orch)
{
    return cog_orchestrator_health(orch, g_dead_peer) == COG_HEALTH_DEAD;
}

static int
pred_one_core(cog_orchestrator_t *orch)
{
    size_t count = 0U;

    (void)cog_orchestrator_list(orch, NULL, 0U, &count);
    return count == 1U;
}

/* ------------------------------------------------------------------ */
/* 1. kill on a self-connected core must not signal the test process  */
/* ------------------------------------------------------------------ */

static void
test_kill_self_connected(void)
{
    char path[128];
    cog_orchestrator_config_t cfg;
    cog_orchestrator_t *orch = NULL;
    cog_peer_id_t peer = COG_PEER_ID_INVALID;
    cog_ipc_endpoint_t *client = NULL;
    pid_t self = getpid();

    if (make_socket_path(path, sizeof(path)) != 0) {
        CHECK("ksc path", 0);
        return;
    }
    (void)memset(&cfg, 0, sizeof(cfg));
    cfg.socket_path = path;
    cfg.max_aux_cores = 4;
    cfg.health_interval_ms = 200;
    cfg.unhealthy_threshold = 3;
    cfg.default_core_capacity = 8;
    cfg.auto_replace = false;
    CHECK_RESULT("ksc new", COG_OK, cog_orchestrator_new(&orch, &cfg));
    if (orch == NULL) {
        return;
    }

    /* self-connect: the test process is the "core" */
    CHECK_RESULT("ksc connect", COG_OK,
                 cog_ipc_endpoint_new(&client));
    if (client != NULL) {
        CHECK_RESULT("ksc conn2", COG_OK,
                     cog_ipc_endpoint_connect(client, path, NULL));
        CHECK_RESULT("ksc accept", COG_OK,
                     cog_orchestrator_accept(orch, &peer));
        CHECK("ksc peer", peer != COG_PEER_ID_INVALID);

        /* kill must mark the core dead WITHOUT signalling the test process */
        CHECK_RESULT("ksc kill", COG_OK, cog_orchestrator_kill(orch, peer));
        CHECK("ksc dead", cog_orchestrator_health(orch, peer) ==
                          COG_HEALTH_DEAD);
        CHECK("ksc self alive", getpid() == self);
        CHECK("ksc self not killed", kill(self, 0) == 0);

        (void)cog_ipc_endpoint_free(client);
    }

    cog_orchestrator_free(orch);
}

/* ------------------------------------------------------------------ */
/* 2. repeated spawn/kill: no leaked children, no zombies             */
/* ------------------------------------------------------------------ */

#define KSC_CYCLES 5

static void
test_repeated_spawn_kill(void)
{
    char path[128];
    cog_orchestrator_config_t cfg;
    cog_orchestrator_t *orch = NULL;
    pid_t pids[KSC_CYCLES];
    int i;

    if (make_socket_path(path, sizeof(path)) != 0) {
        CHECK("rsk path", 0);
        return;
    }
    (void)memset(&cfg, 0, sizeof(cfg));
    cfg.socket_path = path;
    cfg.max_aux_cores = 8;
    cfg.health_interval_ms = 200;
    cfg.unhealthy_threshold = 3;
    cfg.default_core_capacity = 8;
    cfg.auto_replace = false;
    CHECK_RESULT("rsk new", COG_OK, cog_orchestrator_new(&orch, &cfg));
    if (orch == NULL) {
        return;
    }

    set_aux_env("hb");
    for (i = 0; i < KSC_CYCLES; i++) {
        cog_peer_id_t peer = COG_PEER_ID_INVALID;
        cog_core_info_t info;

        CHECK_RESULT("rsk spawn", COG_OK,
                     cog_orchestrator_spawn(orch, COG_AUX_CORE_BIN, &peer));
        g_healthy_peer = peer;
        CHECK("rsk healthy", poll_until(orch, pred_healthy, 3000) == 0);

        /* record the pid from list */
        {
            size_t count = 0U;
            CHECK_RESULT("rsk list", COG_OK,
                         cog_orchestrator_list(orch, &info, 1U, &count));
            CHECK("rsk list count", count >= 1U);
        }
        pids[i] = (pid_t)info.pid;
        CHECK("rsk pid", pids[i] > 0);

        CHECK_RESULT("rsk kill", COG_OK, cog_orchestrator_kill(orch, peer));
        g_dead_peer = peer;
        CHECK("rsk dead", poll_until(orch, pred_dead, 3000) == 0);
    }

    /* every recorded pid must be reaped (no longer a live process) */
    for (i = 0; i < KSC_CYCLES; i++) {
        if (pids[i] > 0) {
            CHECK("rsk reaped", kill(pids[i], 0) == -1);
        }
    }

    cog_orchestrator_free(orch);

    /* after free, still no leaked children */
    for (i = 0; i < KSC_CYCLES; i++) {
        if (pids[i] > 0) {
            CHECK("rsk reaped after free", kill(pids[i], 0) == -1);
        }
    }
}

/* ------------------------------------------------------------------ */
/* 3. auto-replace under repeated kill: slot reuse, no duplicates     */
/* ------------------------------------------------------------------ */

#define AR_CYCLES 3

static void
test_auto_replace_repeated(void)
{
    char path[128];
    cog_orchestrator_config_t cfg;
    cog_orchestrator_t *orch = NULL;
    cog_peer_id_t peer = COG_PEER_ID_INVALID;
    int i;

    if (make_socket_path(path, sizeof(path)) != 0) {
        CHECK("arr path", 0);
        return;
    }
    (void)memset(&cfg, 0, sizeof(cfg));
    cfg.socket_path = path;
    cfg.max_aux_cores = 4;
    cfg.health_interval_ms = 200;
    cfg.unhealthy_threshold = 3;
    cfg.default_core_capacity = 8;
    cfg.auto_replace = true;
    CHECK_RESULT("arr new", COG_OK, cog_orchestrator_new(&orch, &cfg));
    if (orch == NULL) {
        return;
    }

    set_aux_env("hb");
    CHECK_RESULT("arr spawn", COG_OK,
                 cog_orchestrator_spawn(orch, COG_AUX_CORE_BIN, &peer));
    g_healthy_peer = peer;
    CHECK("arr healthy", poll_until(orch, pred_healthy, 3000) == 0);

    for (i = 0; i < AR_CYCLES; i++) {
        cog_peer_id_t old = peer;
        cog_core_info_t infos[4];
        size_t count = 0U;

        CHECK_RESULT("arr kill", COG_OK, cog_orchestrator_kill(orch, old));

        /* replacement must connect and heartbeat; slot reused -> 1 core */
        CHECK("arr one core", poll_until(orch, pred_one_core, 3000) == 0);
        CHECK_RESULT("arr list", COG_OK,
                     cog_orchestrator_list(orch, infos, 4U, &count));
        CHECK("arr count", count == 1U);
        peer = infos[0].peer_id;
        CHECK("arr replaced", peer != old);
        g_healthy_peer = peer;
        CHECK("arr replacement healthy",
              poll_until(orch, pred_healthy, 3000) == 0);
    }

    cog_orchestrator_free(orch);
}

/* ------------------------------------------------------------------ */
/* 4. stop on a core that acks but never exits must be bounded        */
/* ------------------------------------------------------------------ */

static cog_orchestrator_t *g_stop_orch = NULL;
static cog_peer_id_t g_stop_peer = COG_PEER_ID_INVALID;
static volatile int g_stop_done = 0;

static void *
stop_runner(void *arg)
{
    (void)arg;
    (void)cog_orchestrator_stop(g_stop_orch, g_stop_peer);
    g_stop_done = 1;
    return NULL;
}

static void
test_stop_ackhang(void)
{
    char path[128];
    cog_orchestrator_config_t cfg;
    cog_orchestrator_t *orch = NULL;
    cog_peer_id_t peer = COG_PEER_ID_INVALID;
    pthread_t th;
    struct timespec ts;
    int waited = 0;

    if (make_socket_path(path, sizeof(path)) != 0) {
        CHECK("ah path", 0);
        return;
    }
    (void)memset(&cfg, 0, sizeof(cfg));
    cfg.socket_path = path;
    cfg.max_aux_cores = 4;
    cfg.health_interval_ms = 200;
    cfg.unhealthy_threshold = 3;
    cfg.default_core_capacity = 8;
    cfg.auto_replace = false;
    CHECK_RESULT("ah new", COG_OK, cog_orchestrator_new(&orch, &cfg));
    if (orch == NULL) {
        return;
    }

    /* ackhang fixture acks shutdown but never exits */
    set_aux_env("ackhang");
    CHECK_RESULT("ah spawn", COG_OK,
                 cog_orchestrator_spawn(orch, COG_AUX_CORE_BIN, &peer));
    g_healthy_peer = peer;
    CHECK("ah healthy", poll_until(orch, pred_healthy, 3000) == 0);

    g_stop_orch = orch;
    g_stop_peer = peer;
    g_stop_done = 0;
    CHECK_RESULT("ah thread", 0, pthread_create(&th, NULL, stop_runner, NULL));

    /* stop must return within a bounded window even though the core acks
     * but never exits.  If it blocks forever, this is a hang bug. */
    ts.tv_sec = 0;
    ts.tv_nsec = 10000000L;
    while (waited < 5000 && !g_stop_done) {
        (void)nanosleep(&ts, NULL);
        waited += 10;
    }
    CHECK("ah stop bounded", g_stop_done != 0);

    if (g_stop_done) {
        (void)pthread_join(th, NULL);
    } else {
        (void)pthread_detach(th);
    }

    cog_orchestrator_free(orch);
}

/* ------------------------------------------------------------------ */
/* 5. shutdown on a core that acks but never exits must be bounded    */
/* ------------------------------------------------------------------ */

static cog_orchestrator_t *g_sd_orch = NULL;
static volatile int g_sd_done = 0;

static void *
shutdown_runner(void *arg)
{
    (void)arg;
    (void)cog_orchestrator_shutdown(g_sd_orch, 1000U);
    g_sd_done = 1;
    return NULL;
}

static void
test_shutdown_ackhang(void)
{
    char path[128];
    cog_orchestrator_config_t cfg;
    cog_orchestrator_t *orch = NULL;
    cog_peer_id_t peer = COG_PEER_ID_INVALID;
    pthread_t th;
    struct timespec ts;
    int waited = 0;

    if (make_socket_path(path, sizeof(path)) != 0) {
        CHECK("sah path", 0);
        return;
    }
    (void)memset(&cfg, 0, sizeof(cfg));
    cfg.socket_path = path;
    cfg.max_aux_cores = 4;
    cfg.health_interval_ms = 200;
    cfg.unhealthy_threshold = 3;
    cfg.default_core_capacity = 8;
    cfg.auto_replace = false;
    CHECK_RESULT("sah new", COG_OK, cog_orchestrator_new(&orch, &cfg));
    if (orch == NULL) {
        return;
    }

    set_aux_env("ackhang");
    CHECK_RESULT("sah spawn", COG_OK,
                 cog_orchestrator_spawn(orch, COG_AUX_CORE_BIN, &peer));
    g_healthy_peer = peer;
    CHECK("sah healthy", poll_until(orch, pred_healthy, 3000) == 0);

    g_sd_orch = orch;
    g_sd_done = 0;
    CHECK_RESULT("sah thread", 0,
                 pthread_create(&th, NULL, shutdown_runner, NULL));

    /* shutdown must return within a bounded window even though the core
     * acks but never exits.  If it blocks forever, this is a hang bug. */
    ts.tv_sec = 0;
    ts.tv_nsec = 10000000L;
    while (waited < 5000 && !g_sd_done) {
        (void)nanosleep(&ts, NULL);
        waited += 10;
    }
    CHECK("sah shutdown bounded", g_sd_done != 0);

    if (g_sd_done) {
        (void)pthread_join(th, NULL);
    } else {
        (void)pthread_detach(th);
    }

    cog_orchestrator_free(orch);
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int
main(void)
{
    test_kill_self_connected();
    test_repeated_spawn_kill();
    test_auto_replace_repeated();
    test_stop_ackhang();
    test_shutdown_ackhang();

    if (g_failures != 0U) {
        (void)fprintf(stderr, "test_orchestrator_stress: %u failure(s)\n",
                      g_failures);
        return 1;
    }
    (void)printf("test_orchestrator_stress: all ok\n");
    return 0;
}

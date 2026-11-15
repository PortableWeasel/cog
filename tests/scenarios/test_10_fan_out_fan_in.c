/**
 * @file test_10_fan_out_fan_in.c
 * @brief Scenario 10: Fan-out/fan-in across aux cores.
 *
 * Drives the real cog-orchestrator (against the repo's aux_core fixture
 * over a local Unix domain socket) and the real cog-core subagent
 * registry, and proves the scenario-10 contract from
 * docs/scenarios/10-fan-out-fan-in.md:
 *
 *   1. parallel placement across aux cores: cog_orchestrator_place
 *      distributes new subagents across healthy aux cores, preferring
 *      the least-loaded core as reported by cog_orchestrator_heartbeat;
 *   2. fan-out grouping: cog_agent_spawn with a shared group_id creates
 *      a fan-out group of subagents, each with its own child session;
 *   3. ordered fan-in: each subagent runs to completion and
 *      cog_agent_result returns the collected results in spawn order;
 *   4. broadcast cancel: cog_ipc_broadcast reaches every connected peer,
 *      and cog_agent_cancel / cog_agent_is_cancelled mark the fan-out
 *      agents cancelled so late results are rejected (the
 *      cog_agent_is_cancelled gap fix).
 *
 * The fixture binary path is supplied at compile time via
 * COG_AUX_CORE_BIN (falling back to argv[1]).  Every spawned pid is
 * owned by this test and reaped by the orchestrator; all sockets are
 * local temporary paths.
 *
 * Ticket: 0d8ff50 (Stage 12.6c — Scenario tests 06-10).
 */
#define _POSIX_C_SOURCE 200809L

#include "cog/orchestrator.h"

#include "orchestrator_internal.h"

#include "cog_cli/cli.h"
#include "cog_cli/mock_provider.h"

#include "agent_internal.h"
#include "agent_test_helpers.h"

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifndef COG_AUX_CORE_BIN
#define COG_AUX_CORE_BIN NULL
#endif

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
static int make_socket_path(char *buf, size_t bufsize)
{
    char tmpl[] = "/tmp/cog-scn10-XXXXXX";
    int fd = mkstemp(tmpl);

    if (fd < 0) {
        return -1;
    }
    (void)close(fd);
    (void)unlink(tmpl);
    (void)snprintf(buf, bufsize, "%s", tmpl);
    return 0;
}

/** Set the fixture behaviour environment for the next spawned core. */
static void set_aux_env(const char *mode, const char *session)
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

/** Drive the orchestrator until @p pred returns true or timeout. */
static int poll_until(cog_orchestrator_t *orch, int (*pred)(cog_orchestrator_t *),
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
static int pred_healthy(cog_orchestrator_t *orch)
{
    return cog_orchestrator_health(orch, g_healthy_peer) == COG_HEALTH_HEALTHY;
}

/** Predicate: the orchestrator has exactly @p g_want_cores cores. */
static size_t g_want_cores = 0U;
static int pred_core_count(cog_orchestrator_t *orch)
{
    size_t count = 0U;

    (void)cog_orchestrator_list(orch, NULL, 0U, &count);
    return count == g_want_cores;
}

/* ------------------------------------------------------------------ */
/* 1. Parallel placement across aux cores                              */
/* ------------------------------------------------------------------ */

static void scenario_placement_across_cores(void)
{
    char path[128];
    cog_orchestrator_config_t cfg;
    cog_orchestrator_t *orch = NULL;
    cog_peer_id_t p1 = COG_PEER_ID_INVALID;
    cog_peer_id_t p2 = COG_PEER_ID_INVALID;
    cog_core_info_t infos[4];
    size_t count = 0U;

    if (make_socket_path(path, sizeof(path)) != 0) {
        CHECK(0);
        return;
    }
    (void)memset(&cfg, 0, sizeof(cfg));
    cfg.socket_path = path;
    cfg.max_aux_cores = 4;
    cfg.health_interval_ms = 200;
    cfg.unhealthy_threshold = 3;
    cfg.default_core_capacity = 8;
    cfg.auto_replace = false;
    CHECK(cog_orchestrator_new(&orch, &cfg) == COG_OK);
    if (orch == NULL) {
        return;
    }

    /* Two healthy aux cores. */
    set_aux_env("hb", NULL);
    CHECK(cog_orchestrator_spawn(orch, COG_AUX_CORE_BIN, &p1) == COG_OK);
    CHECK(cog_orchestrator_spawn(orch, COG_AUX_CORE_BIN, &p2) == COG_OK);
    g_want_cores = 2U;
    CHECK(poll_until(orch, pred_core_count, 3000) == 0);
    g_healthy_peer = p1;
    CHECK(poll_until(orch, pred_healthy, 3000) == 0);
    g_healthy_peer = p2;
    CHECK(poll_until(orch, pred_healthy, 3000) == 0);

    /* Both cores start with equal free capacity; placement picks the
     * first healthy core. */
    CHECK(cog_orchestrator_place(orch, 0U) == p1);

    /* Load core 1; placement moves to core 2 (least loaded). */
    CHECK(cog_orchestrator_heartbeat(orch, p1, 5U) == COG_OK);
    CHECK(cog_orchestrator_place(orch, 0U) == p2);

    /* Load core 2; placement moves back to core 1. */
    CHECK(cog_orchestrator_heartbeat(orch, p2, 5U) == COG_OK);
    CHECK(cog_orchestrator_place(orch, 0U) == p1);

    /* A core at capacity is skipped. */
    CHECK(cog_orchestrator_heartbeat(orch, p1, 8U) == COG_OK);
    CHECK(cog_orchestrator_place(orch, 0U) == p2);

    /* List reports both cores with their reported load. */
    CHECK(cog_orchestrator_list(orch, infos, 4U, &count) == COG_OK);
    CHECK_EQ_INT((long long)count, 2LL);

    CHECK(cog_orchestrator_shutdown(orch, 2000) == COG_OK);
    cog_orchestrator_free(orch);
    (void)unlink(path);
}

/* ------------------------------------------------------------------ */
/* 2+3. Fan-out with group_id and ordered fan-in results               */
/* ------------------------------------------------------------------ */

static void scenario_fan_out_fan_in(void)
{
    cog_cli_options_t opts;
    cog_cli_app_t *app = NULL;
    cog_core_t *core;
    cog_agent_config_t cfg;
    cog_agent_id_t ids[3];
    cog_agent_status_t st;
    const char *sids[3];
    const char *parent_sid;
    static const char *const results[3] = { "r1", "r2", "r3" };
    static const size_t result_lens[3] = { 2U, 2U, 2U };
    char buf[32];
    size_t len = 0U;
    size_t i;

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

    /* Fan-out: three subagents in one group, each with its own child
     * session linked to the parent. */
    for (i = 0U; i < 3U; ++i) {
        (void)memset(&cfg, 0, sizeof(cfg));
        cfg.task = "fan-out task";
        cfg.parent_session_id = parent_sid;
        cfg.force_local = true;
        cfg.group_id = 7U;
        CHECK(cog_agent_spawn(core, &cfg, &ids[i]) == COG_OK);
        CHECK(ids[i] != 0U);
        CHECK(cog_agent_session_id(core, ids[i], &sids[i]) == COG_OK);
        CHECK(sids[i] != NULL);
        CHECK(cog_agent_internal_get(core, ids[i]) != NULL);
        CHECK_EQ_INT((long long)cog_agent_internal_get(core, ids[i])->group_id,
                     7LL);
    }
    /* Distinct child sessions. */
    CHECK(strcmp(sids[0], sids[1]) != 0);
    CHECK(strcmp(sids[1], sids[2]) != 0);
    CHECK(strcmp(sids[0], sids[2]) != 0);

    /* Run each to completion and collect the results in spawn order. */
    for (i = 0U; i < 3U; ++i) {
        CHECK(scenario_agent_step(core, ids[i], COG_AGENT_STATUS_RUNNING, COG_OK, "", 0U) == COG_OK);
        CHECK(scenario_agent_step(core, ids[i], COG_AGENT_STATUS_COMPLETED, COG_OK, results[i],
                                  result_lens[i]) == COG_OK);
        CHECK(cog_agent_status(core, ids[i], &st) == COG_OK);
        CHECK_EQ_INT((long long)st, (long long)COG_AGENT_STATUS_COMPLETED);
    }
    for (i = 0U; i < 3U; ++i) {
        CHECK(cog_agent_result(core, ids[i], buf, sizeof(buf), &len) == COG_OK);
        CHECK_EQ_INT((long long)len, (long long)result_lens[i]);
        CHECK(memcmp(buf, results[i], result_lens[i]) == 0);
    }

    cog_cli_app_free(app);
}

/* ------------------------------------------------------------------ */
/* 4. Broadcast cancel + late-result rejection                         */
/* ------------------------------------------------------------------ */

/** Peer thread: connect to a listener and wait for a broadcast. */
typedef struct peer_ctx {
    char path[128];
    char received[64];
    int ok;
} peer_ctx_t;

static void *peer_thread(void *arg)
{
    peer_ctx_t *p = (peer_ctx_t *)arg;
    cog_ipc_endpoint_t *ep = NULL;
    cog_ipc_message_t msg;
    int revents = 0;
    cog_result_t rc;

    if (cog_ipc_endpoint_new(&ep) != COG_OK) {
        return NULL;
    }
    if (cog_ipc_endpoint_connect(ep, p->path, NULL) != COG_OK) {
        (void)cog_ipc_endpoint_free(ep);
        return NULL;
    }
    rc = cog_ipc_endpoint_poll(ep, COG_IPC_POLL_IN, 5000, &revents);
    if (rc == COG_OK && (revents & COG_IPC_POLL_IN) != 0 &&
        cog_ipc_message_new(&msg, 64U) == COG_OK) {
        rc = cog_ipc_endpoint_recv(ep, &msg);
        if (rc == COG_OK && msg.len < sizeof(p->received)) {
            (void)memcpy(p->received, msg.data, msg.len);
            p->received[msg.len] = '\0';
            p->ok = 1;
        }
        (void)cog_ipc_message_free(&msg);
    }
    (void)cog_ipc_endpoint_free(ep);
    return NULL;
}

static void scenario_broadcast_cancel(void)
{
    char path[128];
    cog_ipc_endpoint_t *listener = NULL;
    cog_ipc_endpoint_t *s1 = NULL;
    cog_ipc_endpoint_t *s2 = NULL;
    cog_peer_id_t pid1 = COG_PEER_ID_INVALID;
    cog_peer_id_t pid2 = COG_PEER_ID_INVALID;
    cog_ipc_message_t out;
    peer_ctx_t pa;
    peer_ctx_t pb;
    pthread_t ta, tb;

    /* --- Broadcast reaches every connected peer --- */
    CHECK(make_socket_path(path, sizeof(path)) == 0);
    CHECK(cog_ipc_endpoint_new(&listener) == COG_OK);
    CHECK(cog_ipc_endpoint_listen(listener, path) == COG_OK);

    /* Two peers connect to the listener; the main thread accepts their
     * connections, then broadcasts to all of them. */
    (void)memset(&pa, 0, sizeof(pa));
    (void)snprintf(pa.path, sizeof(pa.path), "%s", path);
    (void)memset(&pb, 0, sizeof(pb));
    (void)snprintf(pb.path, sizeof(pb.path), "%s", path);
    CHECK(pthread_create(&ta, NULL, peer_thread, &pa) == 0);
    CHECK(pthread_create(&tb, NULL, peer_thread, &pb) == 0);
    (void)nanosleep(&(struct timespec){0, 50000000L}, NULL); /* 50 ms */

    CHECK(cog_ipc_endpoint_accept(listener, &s1, &pid1) == COG_OK);
    CHECK(cog_ipc_endpoint_accept(listener, &s2, &pid2) == COG_OK);

    CHECK(cog_ipc_message_new(&out, 64U) == COG_OK);
    (void)memcpy(out.data, "cancel", 6U);
    out.len = 6U;
    CHECK(cog_ipc_broadcast(listener, &out) == COG_OK);
    (void)cog_ipc_message_free(&out);

    (void)pthread_join(ta, NULL);
    (void)pthread_join(tb, NULL);
    CHECK(pa.ok);
    CHECK(pb.ok);
    CHECK(strcmp(pa.received, "cancel") == 0);
    CHECK(strcmp(pb.received, "cancel") == 0);

    CHECK(cog_ipc_endpoint_free(s1) == COG_OK);
    CHECK(cog_ipc_endpoint_free(s2) == COG_OK);
    CHECK(cog_ipc_endpoint_free(listener) == COG_OK);
    (void)unlink(path);

    /* --- Agent cancel semantics: is_cancelled + late-result rejection --- */
    {
        cog_cli_options_t opts;
        cog_cli_app_t *app = NULL;
        cog_core_t *core;
        cog_agent_config_t cfg;
        cog_agent_id_t id = 0U;
        cog_agent_status_t st;
        bool cancelled = false;
        char rbuf[16];
        size_t rlen = 0U;

        (void)memset(&opts, 0, sizeof(opts));
        opts.model_id = "mock-model";
        opts.endpoint_name = "mock";
        opts.interactive = false;
        opts.backend = COG_CLI_BACKEND_MOCK;
    opts.explicit_options |= COG_CLI_OPT_BACKEND | COG_CLI_OPT_MODEL | COG_CLI_OPT_ENDPOINT;
    CHECK(cog_cli_app_new(&app, &opts) == COG_OK);
        if (app != NULL) {
            core = cog_cli_app_core(app);
            CHECK(core != NULL);
            (void)memset(&cfg, 0, sizeof(cfg));
            cfg.task = "fan-out winner";
            cfg.force_local = true;
            cfg.group_id = 7U;
            CHECK(cog_agent_spawn(core, &cfg, &id) == COG_OK);
            CHECK(scenario_agent_step(core, id, COG_AGENT_STATUS_RUNNING, COG_OK, "", 0U) == COG_OK);

            /* Broadcast cancel semantics on the agent: cancel marks it
             * admitted while RUNNING and is_cancelled reports true. */
            CHECK(cog_agent_cancel(core, id) == COG_OK);
            CHECK(cog_agent_status(core, id, &st) == COG_OK);
            CHECK_EQ_INT((long long)st, (long long)COG_AGENT_STATUS_RUNNING);
            CHECK(cog_agent_is_cancelled(core, id, &cancelled) == COG_OK);
            CHECK(cancelled);

            /* A late result from a cancelled fan-out agent is rejected. */
            CHECK(scenario_agent_step(core, id, COG_AGENT_STATUS_COMPLETED, COG_OK, "late", 4U)
                  == COG_ERROR_INVALID_STATE);
            CHECK(cog_agent_result(core, id, rbuf, sizeof(rbuf), &rlen)
                  == COG_ERROR_INVALID_STATE);

            CHECK(scenario_agent_step(core, id, COG_AGENT_STATUS_CANCELLED, COG_ERROR_ABORTED, "", 0U) == COG_OK);
            CHECK(cog_agent_result(core, id, rbuf, sizeof(rbuf), &rlen) == COG_OK);
            cog_cli_app_free(app);
        }
    }
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    const char *aux_bin = COG_AUX_CORE_BIN;

    if (aux_bin == NULL && argc > 1) {
        aux_bin = argv[1];
    }
    if (aux_bin == NULL) {
        (void)fprintf(stderr,
                      "test_10_fan_out_fan_in: COG_AUX_CORE_BIN not defined "
                      "and no argv[1] fixture path given\n");
        return 2;
    }

    (void)printf("Scenario 10: fan-out/fan-in across aux cores\n");
    scenario_placement_across_cores();
    scenario_fan_out_fan_in();
    scenario_broadcast_cancel();

    if (failures != 0) {
        (void)fprintf(stderr, "test_10_fan_out_fan_in: %d failure(s)\n",
                      failures);
        return 1;
    }
    (void)printf("test_10_fan_out_fan_in: all checks passed\n");
    return 0;
}

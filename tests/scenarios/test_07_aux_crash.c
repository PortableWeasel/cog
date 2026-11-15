/**
 * @file test_07_aux_crash.c
 * @brief Scenario 07: Aux core crash -> detection -> recovery.
 *
 * Drives the real cog-orchestrator against the repo's aux_core fixture
 * process (tests/integration/fixtures/aux_core.c) over a local Unix
 * domain socket, and proves the scenario-07 contract from
 * docs/scenarios/07-aux-crash.md:
 *
 *   1. crash detection: a fixture that heartbeats once and then exits
 *      with a crash is detected as COG_HEALTH_DEAD via the real health
 *      monitor (missed-heartbeat threshold);
 *   2. sessions retained: the crashed core's registered sessions are
 *      still reported by cog_orchestrator_sessions after the crash, so
 *      recovery can find the affected subagents;
 *   3. recovery primitives: a subagent that was running on the crashed
 *      core (spawned on the real core via cog_agent_spawn) is marked
 *      FAILED via an authenticated attempt update — the action the parent takes
 *      when the orchestrator reports the dead core's sessions — and a
 *      fresh replacement agent can be spawned and completed from
 *      scratch;
 *   4. auto-replace only after actual reap: with auto_replace enabled, a
 *      SIGKILLed core is not replaced while its process is still
 *      unreaped; the replacement appears only after the orchestrator's
 *      poll reaps the dead child (waitpid WNOHANG) and reuses the slot
 *      with a new peer id.
 *
 * The fixture binary path is supplied at compile time via
 * COG_AUX_CORE_BIN (falling back to argv[1]).  Every spawned pid is
 * owned by this test and reaped by the orchestrator; no process is
 * killed by name and no unrelated process is signalled.
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

#include <signal.h>
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
/* Test harness                                                        */
/* ------------------------------------------------------------------ */

static unsigned g_failures = 0U;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            (void)fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__,         \
                          __LINE__, #cond);                             \
            ++g_failures;                                               \
        }                                                               \
    } while (0)

#define CHECK_EQ_INT(a, b)                                              \
    do {                                                                \
        long long _a = (long long)(a);                                  \
        long long _b = (long long)(b);                                  \
        if (_a != _b) {                                                 \
            (void)fprintf(stderr, "FAIL %s:%d: %s == %s (%lld != %lld)\n", \
                          __FILE__, __LINE__, #a, #b, _a, _b);          \
            ++g_failures;                                               \
        }                                                               \
    } while (0)

/** Create a unique temporary socket path; caller unlinks it when done. */
static int make_socket_path(char *buf, size_t bufsize)
{
    char tmpl[] = "/tmp/cog-scn07-XXXXXX";
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

/**
 * @brief Drive the orchestrator until @p pred returns true or timeout.
 */
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

/** Predicate: the given peer is DEAD. */
static cog_peer_id_t g_dead_peer = COG_PEER_ID_INVALID;
static int pred_dead(cog_orchestrator_t *orch)
{
    return cog_orchestrator_health(orch, g_dead_peer) == COG_HEALTH_DEAD;
}

/** Predicate: the given peer is HEALTHY. */
static cog_peer_id_t g_healthy_peer = COG_PEER_ID_INVALID;
static int pred_healthy(cog_orchestrator_t *orch)
{
    return cog_orchestrator_health(orch, g_healthy_peer) == COG_HEALTH_HEALTHY;
}

/** Predicate: the single managed core is no longer @p g_old_peer. */
static cog_peer_id_t g_old_peer = COG_PEER_ID_INVALID;
static int pred_replaced(cog_orchestrator_t *orch)
{
    cog_core_info_t infos[4];
    size_t count = 0U;

    if (cog_orchestrator_list(orch, infos, 4U, &count) != COG_OK) {
        return 0;
    }
    return count == 1U && infos[0].peer_id != g_old_peer;
}

/** Predicate: the given peer reports exactly one session. */
static cog_peer_id_t g_sess_peer = COG_PEER_ID_INVALID;
static int pred_has_session(cog_orchestrator_t *orch)
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
/* 1. Crash -> detection -> sessions retained                         */
/* ------------------------------------------------------------------ */

static void scenario_crash_detection_sessions(void)
{
    char path[128];
    cog_orchestrator_config_t cfg;
    cog_orchestrator_t *orch = NULL;
    cog_peer_id_t peer = COG_PEER_ID_INVALID;
    const char *sessions[8];
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

    /* The fixture registers a session, heartbeats once, then crashes. */
    set_aux_env("crash", "crash-sess");
    CHECK(cog_orchestrator_spawn(orch, COG_AUX_CORE_BIN, &peer) == COG_OK);
    CHECK(peer != COG_PEER_ID_INVALID);

    /* Detection: the missed-heartbeat threshold marks the core DEAD. */
    g_dead_peer = peer;
    CHECK(poll_until(orch, pred_dead, 3000) == 0);

    /* Sessions retained: the crashed core still reports its sessions.
     * The session message is drained asynchronously, so poll until it is
     * registered before asserting on it. */
    g_sess_peer = peer;
    CHECK(poll_until(orch, pred_has_session, 3000) == 0);
    CHECK(cog_orchestrator_sessions(orch, peer, sessions, 8U, &count) == COG_OK);
    CHECK_EQ_INT((long long)count, 1LL);
    CHECK(count == 1U && strcmp(sessions[0], "crash-sess") == 0);

    cog_orchestrator_free(orch);
    (void)unlink(path);
}

/* ------------------------------------------------------------------ */
/* 2. Recovery: affected subagents marked FAILED, restart from scratch */
/* ------------------------------------------------------------------ */

static void scenario_recovery_restart(void)
{
    cog_cli_options_t opts;
    cog_cli_app_t *app = NULL;
    cog_core_t *core;
    cog_agent_config_t cfg;
    cog_agent_id_t affected = 0U;
    cog_agent_id_t replacement = 0U;
    cog_agent_status_t st;
    char result[32];
    size_t result_len = 0U;

    (void)memset(&opts, 0, sizeof(opts));
    opts.model_id = "mock-model";
    opts.endpoint_name = "mock";
    opts.interactive = false;
    CHECK(cog_cli_app_new(&app, &opts) == COG_OK);
    if (app == NULL) {
        return;
    }
    core = cog_cli_app_core(app);
    CHECK(core != NULL);

    /* A subagent that was running on the crashed core. */
    (void)memset(&cfg, 0, sizeof(cfg));
    cfg.task = "was on crashed core";
    cfg.parent_session_id = cog_session_id(cog_cli_app_session(app));
    cfg.force_local = true;
    CHECK(cog_agent_spawn(core, &cfg, &affected) == COG_OK);
    CHECK(scenario_agent_step(core, affected, COG_AGENT_STATUS_RUNNING, COG_OK, "", 0U) == COG_OK);
    CHECK(cog_agent_status(core, affected, &st) == COG_OK);
    CHECK_EQ_INT((long long)st, (long long)COG_AGENT_STATUS_RUNNING);

    /* Recovery action: the parent marks the subagent that was running
     * on the crashed core as FAILED (the orchestrator reports the dead
     * peer's sessions via cog_orchestrator_sessions; the parent applies
     * the fail transition). */
    CHECK(scenario_agent_step(core, affected, COG_AGENT_STATUS_FAILED, COG_ERROR_IO, "", 0U) == COG_OK);
    CHECK(cog_agent_status(core, affected, &st) == COG_OK);
    CHECK_EQ_INT((long long)st, (long long)COG_AGENT_STATUS_FAILED);
    /* A failed terminal result is readable, including an empty result. */
    CHECK(cog_agent_result(core, affected, result, sizeof(result),
                           &result_len) == COG_OK);

    /* Restart from scratch: a fresh replacement agent runs to completion
     * and delivers its result to the parent. */
    (void)memset(&cfg, 0, sizeof(cfg));
    cfg.task = "replacement task";
    cfg.parent_session_id = cog_session_id(cog_cli_app_session(app));
    cfg.force_local = true;
    CHECK(cog_agent_spawn(core, &cfg, &replacement) == COG_OK);
    CHECK(replacement != affected);
    CHECK(scenario_agent_step(core, replacement, COG_AGENT_STATUS_RUNNING, COG_OK, "", 0U) == COG_OK);
    CHECK(scenario_agent_step(core, replacement, COG_AGENT_STATUS_COMPLETED, COG_OK, "recovered", 9U)
          == COG_OK);
    CHECK(cog_agent_status(core, replacement, &st) == COG_OK);
    CHECK_EQ_INT((long long)st, (long long)COG_AGENT_STATUS_COMPLETED);
    CHECK(cog_agent_result(core, replacement, result, sizeof(result),
                           &result_len) == COG_OK);
    CHECK_EQ_INT((long long)result_len, 9LL);
    CHECK(memcmp(result, "recovered", 9U) == 0);

    cog_cli_app_free(app);
}

/* ------------------------------------------------------------------ */
/* 3. Auto-replace only after actual reap                             */
/* ------------------------------------------------------------------ */

static void scenario_auto_replace_after_reap(void)
{
    char path[128];
    cog_orchestrator_config_t cfg;
    cog_orchestrator_t *orch = NULL;
    cog_peer_id_t peer = COG_PEER_ID_INVALID;
    cog_peer_id_t replacement = COG_PEER_ID_INVALID;
    cog_core_info_t infos[4];
    size_t count = 0U;
    pid_t pid;

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
    cfg.auto_replace = true;
    CHECK(cog_orchestrator_new(&orch, &cfg) == COG_OK);
    if (orch == NULL) {
        return;
    }

    set_aux_env("hb", NULL);
    CHECK(cog_orchestrator_spawn(orch, COG_AUX_CORE_BIN, &peer) == COG_OK);
    g_healthy_peer = peer;
    CHECK(poll_until(orch, pred_healthy, 3000) == 0);

    /* The core's OS pid is owned by this test (spawned via the
     * orchestrator).  SIGKILL it directly: the process dies but is NOT
     * reaped yet, so auto-replace must not fire. */
    CHECK(cog_orchestrator_list(orch, infos, 4U, &count) == COG_OK);
    CHECK_EQ_INT((long long)count, 1LL);
    pid = infos[0].pid;
    CHECK(pid > 0);
    CHECK(kill(pid, SIGKILL) == 0);

    /* Before any poll, the dead-but-unreaped core is still the only core
     * and its slot has not been replaced. */
    CHECK(cog_orchestrator_list(orch, infos, 4U, &count) == COG_OK);
    CHECK_EQ_INT((long long)count, 1LL);
    CHECK(infos[0].peer_id == peer);

    /* Drive the orchestrator: poll reaps the dead child (waitpid WNOHANG)
     * and only then auto-replaces it.  The replacement reuses the dead
     * core's slot, so the old peer id is gone as soon as the replacement
     * connects.  Wait until the slot actually holds a new peer. */
    g_old_peer = peer;
    CHECK(poll_until(orch, pred_replaced, 3000) == 0);
    CHECK(cog_orchestrator_list(orch, infos, 4U, &count) == COG_OK);
    CHECK_EQ_INT((long long)count, 1LL);
    replacement = infos[0].peer_id;
    CHECK(replacement != peer);
    g_healthy_peer = replacement;
    CHECK(poll_until(orch, pred_healthy, 3000) == 0);

    cog_orchestrator_free(orch);
    (void)unlink(path);
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
                      "test_07_aux_crash: COG_AUX_CORE_BIN not defined "
                      "and no argv[1] fixture path given\n");
        return 2;
    }

    (void)printf("Scenario 07: aux core crash -> detection -> recovery\n");
    scenario_crash_detection_sessions();
    scenario_recovery_restart();
    scenario_auto_replace_after_reap();

    if (g_failures != 0U) {
        (void)fprintf(stderr, "test_07_aux_crash: %u failure(s)\n",
                      g_failures);
        return 1;
    }
    (void)printf("test_07_aux_crash: all checks passed\n");
    return 0;
}

/**
 * @file test_03_subagent_lifecycle.c
 * @brief Scenario 03: Subagent spawn -> execute -> status/result -> parent.
 *
 * Drives the real subagent registry (libraries/cog-core/src/agent.c)
 * through the core owned by the CLI app.  The public agent API
 * (cog/agent.h) covers spawn, status, notify_parent, result, session_id,
 * and is_cancelled; the orchestrator layer that normally drives
 * execution (Stage 12.5) is not yet integrated; this test supplies correlated
 * public attempt updates and explicitly acknowledges each snapshot.
 *
 * It verifies:
 *
 *   - spawn creates a PENDING subagent with a unique id and a child
 *     session linked to the parent session (child session's parent id
 *     == parent session id);
 *   - start moves it to RUNNING;
 *   - notify_parent records a status message on the agent record;
 *   - complete moves it to COMPLETED and records the final result;
 *   - cog_agent_result returns that result to the parent;
 *   - the child session is distinct from the parent session;
 *   - the cancel path: a spawned agent can be cancelled (CANCELLED,
 *     is_cancelled true) and a late result is rejected.
 *
 * Ticket: fe958ce (Stage 12.6b — Scenario tests 01-05).
 */
#define _POSIX_C_SOURCE 200809L

#include "cog_cli/cli.h"
#include "cog_cli/mock_provider.h"

#include "agent_internal.h"
#include "agent_test_helpers.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

/** Build a single-prompt app from options (fills parse defaults). */
static cog_cli_app_t *make_app(cog_cli_options_t *opts)
{
    cog_cli_app_t *app = NULL;

    if (opts->model_id == NULL) {
        opts->model_id = "mock-model";
    }
    if (opts->endpoint_name == NULL) {
        opts->endpoint_name = "mock";
    }
    opts->interactive = false;
    opts->backend = COG_CLI_BACKEND_MOCK;
    opts->explicit_options |= COG_CLI_OPT_BACKEND | COG_CLI_OPT_MODEL | COG_CLI_OPT_ENDPOINT;
    if (opts->store_dir != NULL) {
        opts->store_mode = COG_CLI_STORE_DURABLE;
        opts->explicit_options |= COG_CLI_OPT_STORE_MODE | COG_CLI_OPT_STORE_DIR;
    }
    if (cog_cli_app_new(&app, opts) != COG_OK) {
        return NULL;
    }
    return app;
}

/* ------------------------------------------------------------------ */
/* Scenario 03: spawn -> execute -> status/result -> parent            */
/* ------------------------------------------------------------------ */

static void scenario_subagent_lifecycle(void)
{
    cog_cli_options_t opts;
    cog_cli_app_t *app = NULL;
    cog_core_t *core;
    cog_agent_config_t cfg;
    cog_agent_id_t id = 0U;
    cog_agent_status_t status;
    const char *parent_sid;
    const char *child_sid;
    const cog_agent_entry_t *rec;
    char result[64];
    size_t result_len = 0U;
    bool cancelled = false;

    (void)memset(&opts, 0, sizeof(opts));
    app = make_app(&opts);
    CHECK(app != NULL);
    if (app == NULL) return;
    core = cog_cli_app_core(app);
    CHECK(core != NULL);

    parent_sid = cog_session_id(cog_cli_app_session(app));
    CHECK(parent_sid != NULL);

    /* Spawn: PENDING, child session linked to the parent. */
    (void)memset(&cfg, 0, sizeof(cfg));
    cfg.task = "summarize the design";
    cfg.parent_session_id = parent_sid;
    cfg.force_local = true;
    cfg.timeout_ms = 30000U;
    CHECK(cog_agent_spawn(core, &cfg, &id) == COG_OK);
    CHECK(id != 0U);

    CHECK(cog_agent_status(core, id, &status) == COG_OK);
    CHECK_EQ_INT((long long)status, (long long)COG_AGENT_STATUS_PENDING);

    /* Child session linkage: distinct id, parent id == parent session. */
    CHECK(cog_agent_session_id(core, id, &child_sid) == COG_OK);
    CHECK(child_sid != NULL);
    CHECK(strcmp(child_sid, parent_sid) != 0);
    rec = cog_agent_internal_get(core, id);
    CHECK(rec != NULL);
    CHECK(rec->session != NULL);
    CHECK(cog_session_parent_id(rec->session) != NULL);
    CHECK(strcmp(cog_session_parent_id(rec->session), parent_sid) == 0);
    CHECK(rec->force_local);
    CHECK_EQ_INT((long long)rec->timeout_ms, 30000LL);

    /* Execute: start -> RUNNING. */
    CHECK(scenario_agent_step(core, id, COG_AGENT_STATUS_RUNNING, COG_OK, "", 0U) == COG_OK);
    CHECK(cog_agent_status(core, id, &status) == COG_OK);
    CHECK_EQ_INT((long long)status, (long long)COG_AGENT_STATUS_RUNNING);

    /* Status update to the parent is recorded on the agent record. */
    {
        cog_agent_update_t snapshot; char bytes[32]; size_t needed;
        CHECK(cog_agent_copy_update(core, id, &snapshot, bytes, sizeof(bytes), &needed) == COG_OK);
        CHECK(cog_agent_ack_update(core, id, snapshot.execution_epoch, snapshot.sequence) == COG_OK);
    }
    CHECK(cog_agent_notify_parent(core, id, "working on it") == COG_OK);
    rec = cog_agent_internal_get(core, id);
    CHECK(rec != NULL && rec->status_msg != NULL);
    CHECK(strcmp(rec->status_msg, "working on it") == 0);

    /* Complete -> COMPLETED with a final result. */
    CHECK(scenario_agent_step(core, id, COG_AGENT_STATUS_COMPLETED, COG_OK, "42", 2U) == COG_OK);
    CHECK(cog_agent_status(core, id, &status) == COG_OK);
    CHECK_EQ_INT((long long)status, (long long)COG_AGENT_STATUS_COMPLETED);

    /* The parent reads the result. */
    CHECK(cog_agent_result(core, id, result, sizeof(result), &result_len)
          == COG_OK);
    CHECK_EQ_INT((long long)result_len, 2LL);
    CHECK(memcmp(result, "42", 2U) == 0);
    CHECK(result[2] == '\0');

    /* A completed agent is not cancelled. */
    CHECK(cog_agent_is_cancelled(core, id, &cancelled) == COG_OK);
    CHECK(!cancelled);

    cog_cli_app_free(app);
}

/* ------------------------------------------------------------------ */
/* Scenario 03b: cancel path                                          */
/* ------------------------------------------------------------------ */

static void scenario_subagent_cancel(void)
{
    cog_cli_options_t opts;
    cog_cli_app_t *app = NULL;
    cog_core_t *core;
    cog_agent_config_t cfg;
    cog_agent_id_t id = 0U;
    cog_agent_status_t status;
    bool cancelled = false;
    char result[16];
    size_t result_len = 0U;

    (void)memset(&opts, 0, sizeof(opts));
    app = make_app(&opts);
    CHECK(app != NULL);
    if (app == NULL) return;
    core = cog_cli_app_core(app);
    CHECK(core != NULL);

    (void)memset(&cfg, 0, sizeof(cfg));
    cfg.task = "cancel me";
    cfg.force_local = true;
    CHECK(cog_agent_spawn(core, &cfg, &id) == COG_OK);
    CHECK(id != 0U);

    /* Cancellation is admitted while PENDING; completion follows a drain. */
    CHECK(cog_agent_cancel(core, id) == COG_OK);
    CHECK(cog_agent_status(core, id, &status) == COG_OK);
    CHECK_EQ_INT((long long)status, (long long)COG_AGENT_STATUS_PENDING);
    CHECK(cog_agent_is_cancelled(core, id, &cancelled) == COG_OK);
    CHECK(cancelled);

    /* A late result on a cancelled agent is rejected. */
    CHECK(scenario_agent_step(core, id, COG_AGENT_STATUS_COMPLETED, COG_OK, "late", 4U)
          == COG_ERROR_INVALID_STATE);
    CHECK(cog_agent_result(core, id, result, sizeof(result), &result_len)
          == COG_ERROR_INVALID_STATE);

    CHECK(scenario_agent_step(core, id, COG_AGENT_STATUS_CANCELLED, COG_ERROR_ABORTED, "", 0U) == COG_OK);
    CHECK(cog_agent_result(core, id, result, sizeof(result), &result_len) == COG_OK);

    /* Cancelling a terminal agent is an error. */
    CHECK(cog_agent_cancel(core, id) == COG_ERROR_INVALID_STATE);

    cog_cli_app_free(app);
}

int main(void)
{
    (void)printf("Scenario 03: subagent spawn -> execute -> status/result "
                 "-> parent\n");
    scenario_subagent_lifecycle();
    scenario_subagent_cancel();

    if (failures != 0) {
        (void)fprintf(stderr, "test_03_subagent_lifecycle: %d failure(s)\n",
                      failures);
        return 1;
    }
    (void)printf("test_03_subagent_lifecycle: all checks passed\n");
    return 0;
}

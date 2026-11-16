/**
 * @file test_08_config_reload.c
 * @brief Scenario 08: Hot config reload.
 *
 * Drives the real cog-config tree (candidate/commit/rollback with
 * validators) and proves the scenario-08 contract from
 * docs/scenarios/08-config-reload.md:
 *
 *   1. atomic commit: a transaction's staged edits atomically replace
 *      the running tree on cog_config_commit;
 *   2. in-flight isolation: a snapshot transaction taken before a commit
 *      keeps seeing the old values after the commit (an in-flight
 *      request is unaffected by the reload), while a transaction begun
 *      after the commit sees the new values (future behaviour changed);
 *   3. validators: a registered validator rejects an invalid candidate,
 *      the commit fails with COG_ERROR_INVALID_ARG, the running tree is
 *      unchanged, and the transaction remains usable for a corrected
 *      attempt;
 *   4. rollback: cog_config_rollback discards the candidate and leaves
 *      the running tree unchanged;
 *   5. change notification: COG_EVENT_CONFIG_CHANGED flows through the
 *      real core state machine — a handler receives the event and
 *      records it, the notification the application uses to rebuild the
 *      provider registry.
 *
 * Ticket: 0d8ff50 (Stage 12.6c — Scenario tests 06-10).
 */
#define _POSIX_C_SOURCE 200809L

#include "cog/config.h"
#include "cog/core.h"
#include "cog_cli/cli.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

/** Assert that get at @p path in @p txn equals @p want. */
static void expect_value(const cog_config_txn_t *txn, const char *path,
                         const char *want)
{
    const char *v = NULL;

    if (cog_config_get(txn, path, &v) != COG_OK) {
        (void)fprintf(stderr, "FAIL %s:%d: get(%s) != COG_OK\n",
                      __FILE__, __LINE__, path);
        ++failures;
        return;
    }
    if (strcmp(v, want) != 0) {
        (void)fprintf(stderr, "FAIL %s:%d: get(%s) = \"%s\", want \"%s\"\n",
                      __FILE__, __LINE__, path, v, want);
        ++failures;
    }
}

/* ------------------------------------------------------------------ */
/* Validator fixture                                                   */
/* ------------------------------------------------------------------ */

/** Rejects the candidate when providers.openai.base-url is missing or
 *  empty (the "provider registry must stay valid" rule). */
static cog_result_t base_url_validator(const cog_config_txn_t *txn,
                                       const char *prefix)
{
    char path[128];
    const char *v = NULL;

    (void)snprintf(path, sizeof(path), "%s.openai.base-url", prefix);
    if (cog_config_get(txn, path, &v) != COG_OK) {
        return COG_ERROR_INVALID_ARG;
    }
    if (v == NULL || v[0] == '\0') {
        return COG_ERROR_INVALID_ARG;
    }
    return COG_OK;
}

/* ------------------------------------------------------------------ */
/* 1+2. Atomic commit and in-flight isolation                          */
/* ------------------------------------------------------------------ */

static void scenario_atomic_commit_inflight_isolation(void)
{
    cog_config_tree_t *tree = NULL;
    cog_config_txn_t *seed = NULL;
    cog_config_txn_t *inflight = NULL;
    cog_config_txn_t *reload = NULL;
    cog_config_txn_t *after = NULL;

    CHECK(cog_config_tree_new(&tree) == COG_OK);
    if (tree == NULL) {
        return;
    }

    /* Seed the running config. */
    CHECK(cog_config_begin(tree, &seed) == COG_OK);
    CHECK(cog_config_set(seed, "providers.openai.base-url",
                         "https://old.example") == COG_OK);
    CHECK(cog_config_set(seed, "providers.openai.model", "old-model")
          == COG_OK);
    CHECK(cog_config_commit(seed) == COG_OK);

    /* An in-flight request holds a snapshot taken before the reload. */
    CHECK(cog_config_begin(tree, &inflight) == COG_OK);
    expect_value(inflight, "providers.openai.base-url", "https://old.example");
    expect_value(inflight, "providers.openai.model", "old-model");

    /* Hot reload: a new transaction stages and atomically commits. */
    CHECK(cog_config_begin(tree, &reload) == COG_OK);
    CHECK(cog_config_set(reload, "providers.openai.base-url",
                         "https://new.example") == COG_OK);
    CHECK(cog_config_set(reload, "providers.openai.model", "new-model")
          == COG_OK);
    CHECK(cog_config_commit(reload) == COG_OK);

    /* The in-flight snapshot is unaffected: it still sees the old
     * values (the running request keeps its original configuration). */
    expect_value(inflight, "providers.openai.base-url", "https://old.example");
    expect_value(inflight, "providers.openai.model", "old-model");

    /* Future behaviour changed: a transaction begun after the commit
     * sees the new values. */
    CHECK(cog_config_begin(tree, &after) == COG_OK);
    expect_value(after, "providers.openai.base-url", "https://new.example");
    expect_value(after, "providers.openai.model", "new-model");

    cog_config_rollback(inflight);
    cog_config_rollback(after);
    cog_config_tree_free(tree);
}

/* ------------------------------------------------------------------ */
/* 3. Validators reject invalid reloads, running tree unchanged        */
/* ------------------------------------------------------------------ */

static void scenario_validator_rejects(void)
{
    cog_config_tree_t *tree = NULL;
    cog_config_txn_t *seed = NULL;
    cog_config_txn_t *bad = NULL;
    cog_config_txn_t *check = NULL;

    CHECK(cog_config_tree_new(&tree) == COG_OK);
    if (tree == NULL) {
        return;
    }

    /* Seed a valid base-url. */
    CHECK(cog_config_begin(tree, &seed) == COG_OK);
    CHECK(cog_config_set(seed, "providers.openai.base-url",
                         "https://valid.example") == COG_OK);
    CHECK(cog_config_commit(seed) == COG_OK);

    /* Register the validator for the providers subtree. */
    CHECK(cog_config_register_validator(tree, "providers",
                                        base_url_validator) == COG_OK);

    /* A reload that empties the base-url must be rejected. */
    CHECK(cog_config_begin(tree, &bad) == COG_OK);
    CHECK(cog_config_set(bad, "providers.openai.base-url", "") == COG_OK);
    CHECK(cog_config_commit(bad) == COG_ERROR_INVALID_ARG);

    /* The running tree is unchanged and the transaction is still usable:
     * correct the candidate and commit succeeds. */
    CHECK(cog_config_begin(tree, &check) == COG_OK);
    expect_value(check, "providers.openai.base-url", "https://valid.example");
    cog_config_rollback(check);

    CHECK(cog_config_set(bad, "providers.openai.base-url",
                         "https://fixed.example") == COG_OK);
    CHECK(cog_config_commit(bad) == COG_OK);

    CHECK(cog_config_begin(tree, &check) == COG_OK);
    expect_value(check, "providers.openai.base-url", "https://fixed.example");
    cog_config_rollback(check);

    cog_config_tree_free(tree);
}

/* ------------------------------------------------------------------ */
/* 4. Rollback discards the candidate                                  */
/* ------------------------------------------------------------------ */

static void scenario_rollback_discards(void)
{
    cog_config_tree_t *tree = NULL;
    cog_config_txn_t *seed = NULL;
    cog_config_txn_t *txn = NULL;
    cog_config_txn_t *check = NULL;

    CHECK(cog_config_tree_new(&tree) == COG_OK);
    if (tree == NULL) {
        return;
    }

    CHECK(cog_config_begin(tree, &seed) == COG_OK);
    CHECK(cog_config_set(seed, "providers.openai.base-url",
                         "https://keep.example") == COG_OK);
    CHECK(cog_config_commit(seed) == COG_OK);

    /* Stage a change, then roll it back. */
    CHECK(cog_config_begin(tree, &txn) == COG_OK);
    CHECK(cog_config_set(txn, "providers.openai.base-url",
                         "https://discard.example") == COG_OK);
    cog_config_rollback(txn);

    /* The running tree still holds the pre-rollback value. */
    CHECK(cog_config_begin(tree, &check) == COG_OK);
    expect_value(check, "providers.openai.base-url", "https://keep.example");
    cog_config_rollback(check);

    cog_config_tree_free(tree);
}

/* ------------------------------------------------------------------ */
/* 5. COG_EVENT_CONFIG_CHANGED flows through the state machine         */
/* ------------------------------------------------------------------ */

enum {
    ST_IDLE = 0,
};

enum {
    EV_CONFIG_CHANGED = COG_EVENT_CONFIG_CHANGED,
};

static int g_config_changed_seen = 0;

static cog_handler_result_t on_config_changed(cog_core_t *core,
                                              const cog_event_t *event)
{
    (void)core;
    if (event->kind == COG_EVENT_CONFIG_CHANGED) {
        g_config_changed_seen = 1;
    }
    return (cog_handler_result_t){ST_IDLE, COG_OK};
}

static const cog_state_t idle_transitions[] = {
    ST_IDLE,
    COG_TRANSITION_SELF,
    COG_TRANSITION_END,
};

static const cog_dispatch_entry_t dispatch_table[] = {
    COG_DISPATCH_ENTRY(ST_IDLE, EV_CONFIG_CHANGED, on_config_changed,
                       idle_transitions),
};

/** Wait hook: stop the loop after the config-changed event is seen. */
typedef struct wait_ctx {
    cog_core_t *core;
    int calls;
} wait_ctx_t;

static void config_wait_fn(void *arg)
{
    wait_ctx_t *ctx = (wait_ctx_t *)arg;

    ctx->calls++;
    if (g_config_changed_seen || ctx->calls > 100) {
        cog_core_stop(ctx->core);
    }
}

static void scenario_config_changed_event(void)
{
    cog_session_t *session = NULL;
    cog_core_t *core = NULL;
    cog_core_config_t cfg;
    cog_event_t ev;
    wait_ctx_t wctx;

    CHECK(cog_session_new(&session, NULL) == COG_OK);
    if (session == NULL) {
        return;
    }
    memset(&cfg, 0, sizeof(cfg));
    cfg.session = session;
    cfg.dispatch_table = dispatch_table;
    cfg.dispatch_table_size = sizeof(dispatch_table) / sizeof(dispatch_table[0]);
    cfg.wait_fn = config_wait_fn;
    cfg.wait_arg = &wctx;
    wctx.core = NULL;
    wctx.calls = 0;
    CHECK(cog_core_new(&core, &cfg) == COG_OK);
    if (core == NULL) {
        cog_session_free(session);
        return;
    }
    wctx.core = core;

    /* The application enqueues the change notification after a commit. */
    g_config_changed_seen = 0;
    ev.id = (cog_event_id_t)EV_CONFIG_CHANGED;
    ev.kind = COG_EVENT_CONFIG_CHANGED;
    ev.payload = NULL;
    ev.payload_len = 0U;
    ev.source_id = 0U;
    CHECK(cog_core_enqueue(core, &ev) == COG_OK);
    CHECK(cog_core_run(core) == COG_OK);

    CHECK(g_config_changed_seen);

    cog_core_free(core);
    cog_session_free(session);
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

static void scenario_actual_application_reload(void)
{
    cog_cli_options_t options={.backend=COG_CLI_BACKEND_MOCK,.model_id="old/config",
        .explicit_options=COG_CLI_OPT_BACKEND|COG_CLI_OPT_MODEL|COG_CLI_OPT_RETRIES,
        .max_retries=0U,.prompt="old input",.chunks="generation"};
    cog_cli_app_t *app=NULL;
    CHECK(cog_cli_app_new(&app,&options)==COG_OK);
    if(app==NULL)return;
    CHECK(cog_cli_app_run(app)==COG_OK);
    cog_mock_state_t state;
    CHECK(cog_mock_provider_state(cog_cli_app_provider(app),&state)==COG_OK);
    if(state.last_request==NULL){CHECK(false);cog_cli_app_free(app);return;}
    CHECK(state.last_request->configuration_generation==1U);
    CHECK(strcmp(state.last_request->model_id,"old/config")==0);
    cog_provider_t *old=cog_cli_app_provider(app);uint64_t generation=0U;
    const cog_cli_reload_options_t replacement={.model_id="new/config"};
    CHECK(cog_cli_app_reload(app,&replacement,&generation)==COG_OK && generation==2U);
    CHECK(cog_cli_app_provider(app)==old && cog_core_provider(cog_cli_app_core(app))==old);
    CHECK(cog_cli_app_submit(app,"next input",10U)==COG_OK);
    CHECK(cog_cli_app_run(app)==COG_OK);
    CHECK(cog_core_provider(cog_cli_app_core(app))==cog_cli_app_provider(app));
    CHECK(cog_mock_provider_state(cog_cli_app_provider(app),&state)==COG_OK);
    CHECK(state.last_request!=NULL);
    if(state.last_request!=NULL){
        CHECK(state.last_request->configuration_generation==2U);
        CHECK(strcmp(state.last_request->model_id,"new/config")==0);
        CHECK(state.last_request->message_count==3U);
        CHECK(strcmp(state.last_request->messages[0].content,"old input")==0);
        CHECK(strcmp(state.last_request->messages[2].content,"next input")==0);
    }
    cog_cli_app_free(app);
}
int main(void)
{
    (void)printf("Scenario 08: hot config reload\n");
    scenario_atomic_commit_inflight_isolation();
    scenario_validator_rejects();
    scenario_rollback_discards();
    scenario_config_changed_event();
    scenario_actual_application_reload();

    if (failures != 0) {
        (void)fprintf(stderr, "test_08_config_reload: %d failure(s)\n",
                      failures);
        return 1;
    }
    (void)printf("test_08_config_reload: all checks passed\n");
    return 0;
}

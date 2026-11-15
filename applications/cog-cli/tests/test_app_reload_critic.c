/** @file test_app_reload_critic.c @brief Independent EOF and owner-capacity oracles. */
#define _POSIX_C_SOURCE 200809L
#include "app_internal.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); return 1; } } while (0)
static bool inject_live_reload;
static unsigned published, constructed, consumed, live, peak_live;
cog_result_t __real_cog_cli_service(void *arg, uint64_t now, uint64_t *deadline);
cog_result_t __wrap_cog_cli_service(void *arg, uint64_t now, uint64_t *deadline);
cog_result_t __real_cog_provider_connect(const cog_endpoint_t *endpoint, cog_provider_t **provider);
cog_result_t __wrap_cog_provider_connect(const cog_endpoint_t *endpoint, cog_provider_t **provider);
cog_result_t __real_cog_provider_disconnect(cog_provider_t *provider);
cog_result_t __wrap_cog_provider_disconnect(cog_provider_t *provider);
cog_result_t __wrap_cog_provider_connect(const cog_endpoint_t *endpoint, cog_provider_t **provider)
{
    cog_result_t rc = __real_cog_provider_connect(endpoint, provider);
    if (rc == COG_OK) {
        constructed++; live++;
        if (live > peak_live) peak_live = live;
    }
    return rc;
}
cog_result_t __wrap_cog_provider_disconnect(cog_provider_t *provider)
{
    if (provider != NULL) { consumed++; if (live == 0U) return COG_ERROR_PROTOCOL; live--; }
    return __real_cog_provider_disconnect(provider);
}
cog_result_t __wrap_cog_cli_service(void *arg, uint64_t now, uint64_t *deadline)
{
    cog_cli_app_t *app = arg;
    if (inject_live_reload && app->active && published == 0U) {
        uint64_t id = 0U;
        cog_result_t rc = cog_cli_app_reload(app,
            &(cog_cli_reload_options_t){.model_id = "future/model"}, &id);
        if (rc != COG_OK || id != 2U) return COG_ERROR_PROTOCOL;
        published = 1U;
    }
    return __real_cog_cli_service(arg, now, deadline);
}
static cog_cli_options_t options(void)
{
    return (cog_cli_options_t){.backend = COG_CLI_BACKEND_MOCK,
        .model_id = "old/model", .chunks = "answer",
        .explicit_options = COG_CLI_OPT_BACKEND | COG_CLI_OPT_MODEL};
}
/* Actual pipe EOF arrives while an accepted delayed old turn is active.
 * Publishing a future generation is not authorization to activate it afterward
 * when that old turn completes and no final-line/queued input remains. */
static int eof_during_old_turn(void)
{
    const cog_mock_step_t steps[] = {
        {COG_MOCK_STEP_CHUNK, "answer", COG_OK, 0, false, 0U, NULL},
        {COG_MOCK_STEP_COMPLETE, NULL, COG_OK, 0, false, 0U, NULL}};
    const cog_mock_script_t script = {.steps = steps, .step_count = 2U,
        .chunk_delay_ms = 100U};
    cog_cli_options_t opts = options(); opts.interactive = true; opts.script = &script;
    int pipe_fd[2]; CHECK(pipe(pipe_fd) == 0);
    int saved = dup(STDIN_FILENO); CHECK(saved >= 0);
    CHECK(dup2(pipe_fd[0], STDIN_FILENO) >= 0);
    CHECK(close(pipe_fd[0]) == 0 && close(pipe_fd[1]) == 0);
    cog_cli_app_t *app = NULL; CHECK(cog_cli_app_new(&app, &opts) == COG_OK);
    cog_provider_t *old = cog_cli_app_provider(app);
    CHECK(cog_cli_app_submit(app, "old accepted input", 18U) == COG_OK);
    inject_live_reload = true;
    CHECK(cog_cli_app_run(app) == COG_OK);
    inject_live_reload = false;
    CHECK(published == 1U && app->stdin_eof && !app->active);
    CHECK(app->configuration_generation == 1U && app->future_configuration != NULL);
    CHECK(cog_cli_app_provider(app) == old && cog_core_provider(app->core) == old);
    CHECK(app->request_serial == 1U && !app->reload_notice_pending);
    cog_mock_state_t state;
    CHECK(cog_mock_provider_state(old, &state) == COG_OK && state.send_count == 1U);
    CHECK(cog_mock_provider_state(app->future_provider, &state) == COG_OK && state.send_count == 0U);
    CHECK(live == 2U && peak_live == 2U);
    cog_cli_app_free(app);
    CHECK(live == 0U && constructed == 2U && consumed == 2U);
    CHECK(dup2(saved, STDIN_FILENO) >= 0 && close(saved) == 0);
    return 0;
}
/* Successful repeated replacement proves the control tree and ownership can
 * progress to the documented limit, not merely count rejected empty strings.
 * Every source string is mutated after publication, before activation. */
static int all_successful_generations(void)
{
    constructed = consumed = live = peak_live = 0U;
    cog_cli_options_t opts = options(); cog_cli_app_t *app = NULL;
    CHECK(cog_cli_app_new(&app, &opts) == COG_OK);
    for (unsigned i = 0U; i < CLI_RELOAD_ATTEMPTS; i++) {
        char model[64], expected[64];
        int length = snprintf(model, sizeof(model), "next/model-%u", i);
        CHECK(length > 0 && length < (int)sizeof(model)); strcpy(expected, model);
        cog_provider_t *old = cog_cli_app_provider(app); uint64_t id = 900U;
        CHECK(cog_cli_app_reload(app, &(cog_cli_reload_options_t){.model_id = model}, &id) == COG_OK);
        CHECK(id == (uint64_t)i + 2U && app->configuration_generation == id - 1U);
        CHECK(app->future_provider != NULL && app->future_provider != old && live == 2U);
        memset(model, 'x', (size_t)length);
        CHECK(cog_cli_app_run(app) == COG_OK);
        CHECK(app->configuration_generation == id && app->future_configuration == NULL);
        CHECK(strcmp(app->opts.model_id, expected) == 0 && !app->reload_notice_pending);
        CHECK(cog_cli_app_provider(app) == cog_core_provider(app->core));
        CHECK(live == 1U && consumed == i + 1U && peak_live == 2U);
        cog_mock_state_t state;
        CHECK(cog_mock_provider_state(app->provider, &state) == COG_OK && state.send_count == 0U);
    }
    cog_provider_t *last = cog_cli_app_provider(app); uint64_t output = 876U;
    CHECK(cog_cli_app_reload(app, &(cog_cli_reload_options_t){.model_id = "over-limit"}, &output) == COG_ERROR_LIMIT);
    CHECK(output == 876U && cog_cli_app_provider(app) == last && constructed == CLI_RELOAD_ATTEMPTS + 1U);
    cog_cli_app_free(app);
    CHECK(live == 0U && consumed == constructed && peak_live == 2U);
    return 0;
}
int main(void)
{
    CHECK(eof_during_old_turn() == 0);
    CHECK(all_successful_generations() == 0);
    return 0;
}

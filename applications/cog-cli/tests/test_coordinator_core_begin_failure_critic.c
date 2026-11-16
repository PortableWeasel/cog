/** @file test_coordinator_core_begin_failure_critic.c
 * @brief Pre-spawn core failure must also start native shutdown ownership.
 */
#include "../src/composition_internal.h"
#include <assert.h>
#include <curl/curl.h>
#include <stdio.h>
#include <poll.h>

cog_result_t __real_cog_core_drive_begin(cog_core_t *);
cog_result_t __wrap_cog_core_drive_begin(cog_core_t *core);
static bool inject = true;
cog_result_t __wrap_cog_core_drive_begin(cog_core_t *core)
{
    if (inject) { inject = false; return COG_ERROR_IO; }
    return __real_cog_core_drive_begin(core);
}
int main(int argc, char **argv)
{
    assert(argc == 5);
    assert(curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK);
    char ui[4096], worker[4096];
    assert(snprintf(ui, sizeof(ui), "%s/ui.sock", argv[4]) > 0);
    assert(snprintf(worker, sizeof(worker), "%s/worker.sock", argv[4]) > 0);
    cog_cli_options_t app = {.backend = COG_CLI_BACKEND_REAL, .model_id = "fixture/model",
        .credential_ref = "env:COG_NATIVE_KEY", .base_url = "https://localhost:1/v1",
        .workspace_root = argv[2], .store_dir = argv[3], .store_mode = COG_CLI_STORE_DURABLE,
        .tool_policy = COG_CLI_TOOL_POLICY_DENY, .max_context_bytes = 65536U,
        .max_tool_output_bytes = 8192U,
        .explicit_options = COG_CLI_OPT_BACKEND | COG_CLI_OPT_MODEL | COG_CLI_OPT_CREDENTIAL |
            COG_CLI_OPT_BASE_URL | COG_CLI_OPT_WORKSPACE | COG_CLI_OPT_STORE_DIR |
            COG_CLI_OPT_STORE_MODE | COG_CLI_OPT_POLICY | COG_CLI_OPT_CONTEXT | COG_CLI_OPT_TOOL_OUTPUT};
    cog_net_config_t net = {.host_runtime_initialized = true,
        .dependency_memory = COG_NET_DEPENDENCY_MEMORY_DYNAMIC,
        .max_connections = 1U, .max_request_bytes = 1048576U};
    cog_cli_host_runtime_config_t cfg = {.root_options = &app, .ui_socket = ui,
        .worker_socket = worker, .workers = 1U, .children = 2U, .shutdown_ms = 1000U,
        .signal_fd = -1, .executable = argv[1], .net_config = &net};
    cog_cli_host_runtime_t *host = NULL;
    assert(cog_cli_host_runtime_new(&host, &cfg) == COG_OK);
    assert(cog_cli_host_runtime_begin(host) == COG_ERROR_IO);
    assert(cog_cli_host_runtime_request_shutdown(host) == COG_OK);
    assert(!host->workers[0].used);
    bool done = false;
    for (unsigned i = 0U; i < 32U && !done; ++i) {
        cog_cli_host_runtime_step_t step = {0};
        assert(cog_cli_host_runtime_step(host, &step) == COG_ERROR_IO);
        done = step.done;
    }
    bool converged = done;
    /* The negative control owns no child; dispose the actual owner cleanly. */
    if (!done) {
        assert(cog_orchestrator_request_shutdown(host->orchestrator, host->now_ms, 1000U) == COG_OK);
        for (unsigned i = 0U; i < 32U && !done; ++i) {
            cog_cli_host_runtime_step_t step = {0};
            assert(cog_cli_host_runtime_step(host, &step) == COG_ERROR_IO);
            done = step.done;
        }
    }
    assert(done);
    assert(cog_cli_host_runtime_close(&host, COG_ERROR_IO) == COG_ERROR_IO && host == NULL);
    curl_global_cleanup();
    fprintf(stderr, "core begin failure converged=%d\n", (int)converged);
    assert(converged);
    return 0;
}

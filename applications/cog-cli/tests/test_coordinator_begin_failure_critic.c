/** @file test_coordinator_begin_failure_critic.c
 * @brief Actual prefix native startup must drain after a later spawn fails.
 */
int embedded_cli_main(int argc, char **argv);
#define main embedded_cli_main
#include "../src/main.c"
#undef main
#include "../src/composition_internal.h"
#include <assert.h>
#include <sys/wait.h>
#include <string.h>
#include <dirent.h>

static cog_cli_host_runtime_t *retained;
static unsigned spawn_calls;
static bool verified_reaped;
static size_t descriptors(void)
{
    DIR *directory = opendir("/proc/self/fd");
    assert(directory != NULL);
    size_t count = 0U;
    while (readdir(directory) != NULL) ++count;
    assert(closedir(directory) == 0);
    return count;
}
cog_result_t __real_cog_orchestrator_spawn(cog_orchestrator_t *, const char *, cog_worker_id_t *);
cog_result_t __wrap_cog_orchestrator_spawn(cog_orchestrator_t *, const char *, cog_worker_id_t *);
cog_result_t __wrap_cog_orchestrator_spawn(cog_orchestrator_t *o, const char *path, cog_worker_id_t *out)
{
    ++spawn_calls;
    if (spawn_calls == 2U) return COG_ERROR_IO;
    return __real_cog_orchestrator_spawn(o, path, out);
}
cog_result_t __real_cog_cli_host_runtime_new(cog_cli_host_runtime_t **, const cog_cli_host_runtime_config_t *);
cog_result_t __wrap_cog_cli_host_runtime_new(cog_cli_host_runtime_t **, const cog_cli_host_runtime_config_t *);
cog_result_t __wrap_cog_cli_host_runtime_new(cog_cli_host_runtime_t **out, const cog_cli_host_runtime_config_t *cfg)
{
    cog_result_t rc = __real_cog_cli_host_runtime_new(out, cfg);
    if (rc == COG_OK) retained = *out;
    return rc;
}
cog_result_t __real_cog_cli_host_runtime_close(cog_cli_host_runtime_t **, cog_result_t);
cog_result_t __wrap_cog_cli_host_runtime_close(cog_cli_host_runtime_t **, cog_result_t);
cog_result_t __wrap_cog_cli_host_runtime_close(cog_cli_host_runtime_t **owner, cog_result_t primary)
{
    if (*owner != NULL && *owner == retained && retained->done) {
        cog_core_info_t workers[2]; size_t count = 0U;
        assert(cog_orchestrator_list(retained->orchestrator, workers, 2U, &count) == COG_OK);
        assert(count == 1U && workers[0].pid > 0 && workers[0].reaped && !workers[0].ownership_lost);
        verified_reaped = true;
    }
    cog_result_t rc = __real_cog_cli_host_runtime_close(owner, primary);
    if (*owner == NULL) retained = NULL;
    return rc;
}
int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "--worker") == 0) return embedded_cli_main(argc, argv);
    assert(argc == 4);
    assert(curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK);
    size_t before = descriptors();
    char ui[4096], worker[4096];
    assert(snprintf(ui, sizeof(ui), "%s/ui.sock", argv[3]) > 0);
    assert(snprintf(worker, sizeof(worker), "%s/worker.sock", argv[3]) > 0);
    cog_cli_options_t app = {.backend = COG_CLI_BACKEND_REAL, .model_id = "fixture/model",
        .credential_ref = "env:COG_NATIVE_KEY", .base_url = "https://localhost:1/v1",
        .workspace_root = argv[1], .store_dir = argv[2], .store_mode = COG_CLI_STORE_DURABLE,
        .tool_policy = COG_CLI_TOOL_POLICY_DENY,
        .max_context_bytes = 65536U, .max_tool_output_bytes = 8192U,
        .explicit_options = COG_CLI_OPT_BACKEND | COG_CLI_OPT_MODEL | COG_CLI_OPT_CREDENTIAL |
            COG_CLI_OPT_BASE_URL | COG_CLI_OPT_WORKSPACE | COG_CLI_OPT_STORE_DIR |
            COG_CLI_OPT_STORE_MODE | COG_CLI_OPT_POLICY | COG_CLI_OPT_CONTEXT |
            COG_CLI_OPT_TOOL_OUTPUT};
    cog_cli_host_options_t opts = {.runtime = {.root_options = &app, .ui_socket = ui,
        .worker_socket = worker, .workers = 2U, .children = 2U, .shutdown_ms = 1000U}};
    cog_net_config_t net = {.host_runtime_initialized = true,
        .dependency_memory = COG_NET_DEPENDENCY_MEMORY_DYNAMIC,
        .max_connections = 1U, .max_request_bytes = 1048576U};
    cog_result_t result = run_coordinator(&opts, -1, &net);
    bool consumed = retained == NULL;
    /* Broken-code negative control still cleans every actual managed owner. */
    if (retained != NULL) {
        (void)cog_cli_host_runtime_request_shutdown(retained);
        bool done = retained->done;
        for (unsigned i = 0U; i < 10000U && !done; ++i) {
            cog_cli_host_runtime_step_t step = {0};
            (void)cog_cli_host_runtime_step(retained, &step);
            done = step.done;
            if (!done) (void)poll(NULL, 0U, 1);
        }
        assert(done);
        (void)cog_cli_host_runtime_close(&retained, COG_ERROR_IO);
        assert(retained == NULL);
    }
    assert(verified_reaped);
    assert(descriptors() == before);
    curl_global_cleanup();
    fprintf(stderr, "prefix startup calls=%u result=%d consumed=%d\n", spawn_calls, (int)result, (int)consumed);
    assert(spawn_calls == 2U);
    assert(result == COG_ERROR_IO);
    assert(consumed);
    return 0;
}

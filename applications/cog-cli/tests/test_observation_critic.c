/** @file test_observation_critic.c @brief Real ownership/error-path regressions. */
#include "app_internal.h"
#include <curl/curl.h>
#include <stdio.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); return 1; } } while (0)
static bool clock_fails;
static size_t clock_calls;
cog_result_t __real_cog_obs_clock_now_ns(uint64_t *out);
cog_result_t __wrap_cog_obs_clock_now_ns(uint64_t *out);
cog_result_t __wrap_cog_obs_clock_now_ns(uint64_t *out)
{
    clock_calls++;
    return clock_fails ? COG_ERROR_IO : __real_cog_obs_clock_now_ns(out);
}
typedef struct capture {
    size_t calls;
    bool reject;
} capture_t;
static cog_result_t sink(void *user, const cog_obs_conn_event_t *event)
{
    capture_t *capture = user;
    (void)event;
    capture->calls++;
    return capture->reject ? COG_ERROR_IO : COG_OK;
}
static cog_result_t synthetic_secret(void *user, const char *reference, char *output,
    size_t capacity, size_t *required)
{
    (void)user; (void)reference;
    *required = sizeof("synthetic");
    if (capacity < *required) return COG_ERROR_OVERFLOW;
    memcpy(output, "synthetic", *required);
    return COG_OK;
}
static cog_cli_options_t options(const char *configuration, cog_net_ctx_t *net)
{
    return (cog_cli_options_t){.backend = COG_CLI_BACKEND_REAL, .net = net,
        .model_id = "synthetic/model", .base_url = "https://never-serviced.invalid/",
        .store_mode = COG_CLI_STORE_DISABLED, .credential_ref = "test:synthetic",
        .credential_read = synthetic_secret, .config_path = configuration,
        .explicit_options = COG_CLI_OPT_BACKEND | COG_CLI_OPT_MODEL |
            COG_CLI_OPT_BASE_URL | COG_CLI_OPT_STORE_MODE | COG_CLI_OPT_CREDENTIAL};
}
static int bridge(cog_cli_observer_t **observer, cog_obs_t **obs,
    cog_net_ctx_t **net, size_t slots, capture_t *capture)
{
    CHECK(cog_cli_observer_new(observer, slots) == COG_OK);
    cog_obs_config_t config = {.fields = cog_cli_observation_fields(),
        .conn_event_sink = sink, .conn_event_user = capture};
    CHECK(cog_obs_new(obs, &config) == COG_OK);
    CHECK(cog_cli_observer_bind(*observer, *obs) == COG_OK);
    cog_net_config_t transport = {.host_runtime_initialized = true,
        .dependency_memory = COG_NET_DEPENDENCY_MEMORY_DYNAMIC, .max_connections = slots,
        .observer = cog_cli_observer_capture, .observer_user = *observer};
    CHECK(cog_net_new(net, &transport) == COG_OK);
    return 0;
}
static int open_request(cog_cli_observer_t *observer, cog_net_ctx_t *net,
    uint64_t identity, cog_request_id_t *id)
{
    const cog_cli_observation_scope_t scope = {.request = identity, .configuration_generation = 1U};
    CHECK(cog_cli_observer_prepare(observer, net, &scope) == COG_OK);
    CHECK(cog_net_request(net, "GET", "https://never-serviced.invalid/", NULL, NULL, 0U, id) == COG_OK);
    CHECK(cog_cli_observer_drain(observer) == COG_OK);
    return 0;
}
static int release_bridge(cog_cli_observer_t *observer, cog_obs_t *obs,
    cog_net_ctx_t *net, cog_result_t sticky)
{
    CHECK(cog_cli_observer_clear(observer, net) == sticky);
    CHECK(cog_cli_observer_bind(observer, NULL) == COG_OK);
    cog_net_free(net); cog_obs_free(obs); cog_cli_observer_free(observer);
    return 0;
}
static int retained_clear_and_reuse(void)
{
    cog_cli_observer_t *observer = NULL; cog_obs_t *obs = NULL; cog_net_ctx_t *net = NULL;
    capture_t capture = {0}; CHECK(bridge(&observer, &obs, &net, 1U, &capture) == 0);
    cog_request_id_t first = 0U; CHECK(open_request(observer, net, 1U, &first) == 0);
    cog_obs_conn_snapshot_t before; CHECK(cog_obs_diag_at(obs, 0U, &before) == COG_OK);
    CHECK(cog_net_cancel(net, first) == COG_OK);
    CHECK(cog_cli_observer_drain(observer) == COG_OK);
    bool active = true;
    CHECK(cog_net_request_status(net, first, &active) == COG_OK && !active);
    CHECK(cog_net_active_count(net) == 0U);
    CHECK(cog_cli_observer_clear(observer, net) == COG_ERROR_INVALID_STATE);
    CHECK(cog_cli_observer_bind(observer, NULL) == COG_ERROR_INVALID_STATE);
    CHECK(cog_cli_observer_status(observer) == COG_OK);
    cog_obs_conn_snapshot_t retained; CHECK(cog_obs_diag_at(obs, 0U, &retained) == COG_OK);
    CHECK(retained.conn_id == before.conn_id && retained.state == COG_OBS_CONN_FAILED);
    CHECK(cog_net_request_release(net, first) == COG_OK);
    CHECK(cog_net_request_status(net, first, &active) == COG_ERROR_NOT_FOUND);
    cog_request_id_t next = 0U; CHECK(open_request(observer, net, 2U, &next) == 0);
    CHECK(next != first);
    CHECK(cog_obs_diag_at(obs, 0U, &retained) == COG_OK && retained.conn_id > before.conn_id);
    size_t count = 0U; CHECK(cog_obs_diag_count(obs, &count) == COG_OK && count == 1U);
    CHECK(capture.calls == 3U); /* OPEN, FAILED, distinct OPEN; no silent replay. */
    CHECK(cog_net_cancel(net, next) == COG_OK);
    CHECK(cog_cli_observer_drain(observer) == COG_OK);
    CHECK(cog_net_request_release(net, next) == COG_OK);
    return release_bridge(observer, obs, net, COG_OK);
}
static int retained_row_cannot_evict(void)
{
    cog_cli_observer_t *observer = NULL; cog_obs_t *obs = NULL; cog_net_ctx_t *net = NULL;
    capture_t capture = {0}; CHECK(bridge(&observer, &obs, &net, 1U, &capture) == 0);
    cog_request_id_t first = 0U; CHECK(open_request(observer, net, 1U, &first) == 0);
    CHECK(cog_net_cancel(net, first) == COG_OK);
    CHECK(cog_cli_observer_drain(observer) == COG_OK);
    cog_obs_conn_snapshot_t retained; CHECK(cog_obs_diag_at(obs, 0U, &retained) == COG_OK);
    /* A second genuine context has its own slot while the first ID remains
       retained. The shared bridge has no spare row and must refuse eviction. */
    cog_net_ctx_t *other = NULL;
    cog_net_config_t config = {.host_runtime_initialized = true,
        .dependency_memory = COG_NET_DEPENDENCY_MEMORY_DYNAMIC, .max_connections = 1U,
        .observer = cog_cli_observer_capture, .observer_user = observer};
    CHECK(cog_net_new(&other, &config) == COG_OK);
    const cog_cli_observation_scope_t scope = {.request = 2U, .configuration_generation = 1U};
    CHECK(cog_cli_observer_prepare(observer, other, &scope) == COG_OK);
    cog_request_id_t next = 0U;
    CHECK(cog_net_request(other, "GET", "https://never-serviced.invalid/", NULL, NULL, 0U, &next) == COG_OK);
    CHECK(cog_net_observer_status(other) == COG_OK); /* Capture succeeded. */
    CHECK(cog_cli_observer_drain(observer) == COG_ERROR_OVERFLOW);
    CHECK(cog_net_observer_status(other) == COG_OK); /* Deferred error is separate. */
    cog_obs_conn_snapshot_t after; CHECK(cog_obs_diag_at(obs, 0U, &after) == COG_OK);
    CHECK(after.conn_id == retained.conn_id && capture.calls == 2U);
    CHECK(cog_net_request_release(other, next) == COG_OK);
    CHECK(cog_net_observer_status(other) == COG_ERROR_OVERFLOW);
    CHECK(cog_net_request_release(net, first) == COG_OK);
    CHECK(cog_cli_observer_clear(observer, other) == COG_ERROR_OVERFLOW);
    cog_net_free(other);
    return release_bridge(observer, obs, net, COG_ERROR_OVERFLOW);
}
static int foreign_target_teardown(const char *configuration, bool retain_terminal)
{
    cog_cli_observer_t *observer = NULL; CHECK(cog_cli_observer_new(&observer, 1U) == COG_OK);
    cog_net_ctx_t *net = NULL;
    cog_net_config_t transport = {.host_runtime_initialized = true,
        .dependency_memory = COG_NET_DEPENDENCY_MEMORY_DYNAMIC, .max_connections = 2U,
        .observer = cog_cli_observer_capture, .observer_user = observer};
    CHECK(cog_net_new(&net, &transport) == COG_OK);
    cog_cli_options_t opts = options(configuration, net); cog_cli_app_t *app = NULL;
    CHECK(cog_cli_app_new(&app, &opts) == COG_OK);
    CHECK(cog_cli_observer_bind(observer, cog_cli_app_obs(app)) == COG_OK); app->observer = observer;
    cog_request_id_t foreign = 0U; CHECK(open_request(observer, net, 1U, &foreign) == 0);
    if (retain_terminal) {
        CHECK(cog_net_cancel(net, foreign) == COG_OK);
        CHECK(cog_cli_observer_drain(observer) == COG_OK);
    }
    CHECK(cog_cli_observer_clear(observer, net) == COG_ERROR_INVALID_STATE);
    cog_cli_app_free(app); /* Must consume app/obs, not retain a hidden owner. */
    CHECK(cog_cli_observer_status(observer) == COG_ERROR_INVALID_STATE);
    bool active = false;
    CHECK(cog_net_request_status(net, foreign, &active) == COG_OK && active == !retain_terminal);
    /* A later real OPEN exercises the ended callback target after obs is freed.
       The feed fails explicitly; neither the new ID nor foreign work is stolen. */
    cog_request_id_t later = 0U;
    CHECK(cog_net_request(net, "GET", "https://never-serviced.invalid/", NULL, NULL, 0U, &later) == COG_OK);
    CHECK(cog_net_observer_status(net) == COG_ERROR_INVALID_STATE);
    CHECK(cog_net_request_status(net, foreign, &active) == COG_OK && active == !retain_terminal);
    CHECK(cog_net_request_release(net, later) == COG_OK);
    CHECK(cog_net_request_release(net, foreign) == COG_OK);
    CHECK(cog_cli_observer_clear(observer, net) == COG_ERROR_INVALID_STATE);
    CHECK(cog_cli_observer_bind(observer, NULL) == COG_OK);
    cog_net_free(net); cog_cli_observer_free(observer);
    return 0;
}
typedef struct external_error { cog_result_t result; } external_error_t;
static cog_result_t external_observer(void *user, const cog_net_ctx_t *net,
    const cog_net_observation_t *value)
{
    external_error_t *error = user; (void)net; (void)value;
    return error->result;
}
static int again_is_fatal(const char *configuration)
{
    external_error_t error = {COG_OK}; cog_net_ctx_t *net = NULL;
    cog_net_config_t config = {.host_runtime_initialized = true,
        .dependency_memory = COG_NET_DEPENDENCY_MEMORY_DYNAMIC,
        .observer = external_observer, .observer_user = &error};
    CHECK(cog_net_new(&net, &config) == COG_OK);
    cog_cli_options_t opts = options(configuration, net); cog_cli_app_t *app = NULL;
    CHECK(cog_cli_app_new(&app, &opts) == COG_OK);
    cog_request_id_t id = 0U;
    CHECK(cog_net_request(net, "GET", "https://never-serviced.invalid/", NULL, NULL, 0U, &id) == COG_OK);
    error.result = COG_ERROR_AGAIN; CHECK(cog_net_cancel(net, id) == COG_OK);
    CHECK(cog_net_observer_status(net) == COG_ERROR_AGAIN);
    uint64_t deadline = 0U;
    CHECK(cog_cli_service(app, 0U, &deadline) == COG_ERROR_IO);
    CHECK(app->observation_error == COG_ERROR_IO && cog_net_observer_status(net) == COG_ERROR_AGAIN);
    CHECK(cog_cli_observation_check(app, COG_ERROR_PROTOCOL) == COG_ERROR_PROTOCOL);
    CHECK(cog_core_run(app->core) == COG_ERROR_IO); /* Must not spin on core AGAIN. */
    CHECK(cog_cli_app_run(app) == COG_ERROR_IO);
    CHECK(cog_net_active_count(net) == 0U);
    cog_cli_app_free(app);
    bool active = true; CHECK(cog_net_request_status(net, id, &active) == COG_OK && !active);
    CHECK(cog_net_request_release(net, id) == COG_OK); cog_net_free(net);
    return 0;
}
static int deferred_sink_failure(void)
{
    cog_cli_observer_t *observer = NULL; cog_obs_t *obs = NULL; cog_net_ctx_t *net = NULL;
    capture_t capture = {.reject = true}; CHECK(bridge(&observer, &obs, &net, 1U, &capture) == 0);
    const cog_cli_observation_scope_t scope = {.request = 1U, .configuration_generation = 1U};
    CHECK(cog_cli_observer_prepare(observer, net, &scope) == COG_OK);
    cog_request_id_t id = 0U;
    CHECK(cog_net_request(net, "GET", "https://never-serviced.invalid/", NULL, NULL, 0U, &id) == COG_OK);
    CHECK(capture.calls == 0U && cog_net_observer_status(net) == COG_OK);
    CHECK(cog_cli_observer_drain(observer) == COG_ERROR_IO && capture.calls == 1U);
    CHECK(cog_net_observer_status(net) == COG_OK);
    CHECK(cog_cli_observer_drain(observer) == COG_ERROR_IO && capture.calls == 1U);
    CHECK(cog_net_cancel(net, id) == COG_OK); CHECK(cog_net_observer_status(net) == COG_ERROR_IO);
    CHECK(cog_net_request_release(net, id) == COG_OK);
    return release_bridge(observer, obs, net, COG_ERROR_IO);
}
static int callback_clock_failure(void)
{
    cog_cli_observer_t *observer = NULL; cog_obs_t *obs = NULL; cog_net_ctx_t *net = NULL;
    capture_t capture = {0}; CHECK(bridge(&observer, &obs, &net, 1U, &capture) == 0);
    const cog_cli_observation_scope_t scope = {.request = 1U, .configuration_generation = 1U};
    CHECK(cog_cli_observer_prepare(observer, net, &scope) == COG_OK);
    size_t before = clock_calls; clock_fails = true; cog_request_id_t id = 0U;
    CHECK(cog_net_request(net, "GET", "https://never-serviced.invalid/", NULL, NULL, 0U, &id) == COG_OK);
    clock_fails = false;
    CHECK(clock_calls == before + 1U && cog_net_observer_status(net) == COG_ERROR_IO);
    CHECK(cog_cli_observer_status(observer) == COG_ERROR_IO);
    CHECK(cog_cli_observer_drain(observer) == COG_ERROR_IO);
    size_t count = 1U; CHECK(cog_obs_diag_count(obs, &count) == COG_OK && count == 0U && capture.calls == 0U);
    CHECK(cog_net_request_release(net, id) == COG_OK);
    return release_bridge(observer, obs, net, COG_ERROR_IO);
}
static int queue_capacity_failure(void)
{
    cog_cli_observer_t *observer = NULL; cog_obs_t *obs = NULL; cog_net_ctx_t *net = NULL;
    capture_t capture = {0}; CHECK(bridge(&observer, &obs, &net, 1U, &capture) == 0);
    const cog_cli_observation_scope_t scope = {.request = 1U, .configuration_generation = 1U};
    CHECK(cog_cli_observer_prepare(observer, net, &scope) == COG_OK);
    /* Deliberately exceed the host's drain envelope using real admissions and
       releases. Six reserved entries hold three OPEN/FAILED pairs. */
    for (size_t i = 0U; i < 4U; i++) {
        cog_request_id_t id = 0U;
        CHECK(cog_net_request(net, "GET", "https://never-serviced.invalid/", NULL, NULL, 0U, &id) == COG_OK);
        if (i < 3U) CHECK(cog_net_observer_status(net) == COG_OK);
        else CHECK(cog_net_observer_status(net) == COG_ERROR_OVERFLOW);
        CHECK(cog_net_cancel(net, id) == COG_OK); CHECK(cog_net_request_release(net, id) == COG_OK);
    }
    CHECK(cog_cli_observer_drain(observer) == COG_ERROR_OVERFLOW);
    size_t count = 1U; CHECK(cog_obs_diag_count(obs, &count) == COG_OK && count == 0U && capture.calls == 0U);
    return release_bridge(observer, obs, net, COG_ERROR_OVERFLOW);
}
int main(int argc, char **argv)
{
    CHECK(argc == 2); CHECK(curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK);
    CHECK(retained_clear_and_reuse() == 0);
    CHECK(retained_row_cannot_evict() == 0);
    CHECK(foreign_target_teardown(argv[1], true) == 0);
    CHECK(foreign_target_teardown(argv[1], false) == 0);
    CHECK(again_is_fatal(argv[1]) == 0);
    CHECK(deferred_sink_failure() == 0);
    CHECK(callback_clock_failure() == 0);
    CHECK(queue_capacity_failure() == 0);
    curl_global_cleanup();
    puts("observation critic ownership/error regressions passed"); return 0;
}

/** @file cli.c @brief Explicit application ownership and bounded lifecycle. */
#define _POSIX_C_SOURCE 200809L
#include "app_internal.h"
#include "execution_internal.h"
#include "runtime_internal.h"
#include <errno.h>
#include <assert.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

cog_result_t cog_cli_clock(uint64_t *now)
{
    struct timespec ts;
    if (now == NULL) return COG_ERROR_NULL_PTR;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0 || ts.tv_sec < 0 ||
        ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000L ||
        (uint64_t)ts.tv_sec > UINT64_MAX / 1000U) return COG_ERROR_IO;
    uint64_t milliseconds = (uint64_t)ts.tv_sec * 1000U;
    uint64_t fraction = (uint64_t)ts.tv_nsec / 1000000U;
    if (fraction > UINT64_MAX - milliseconds) return COG_ERROR_IO;
    *now = milliseconds + fraction;
    return COG_OK;
}
bool cog_cli_text_valid(const char *text, size_t length)
{
    cog_message_t message = {.role = COG_MESSAGE_ROLE_USER,
        .content = text, .content_len = length};
    return cog_message_validate(&message) == COG_OK;
}
cog_result_t cog_cli_save(cog_cli_app_t *app)
{
    cog_result_t rc = app->store == NULL ? COG_OK : cog_store_save(app->store, app->session);
    if (rc == COG_ERROR_INDETERMINATE) app->recovery = true;
    return rc;
}
static cog_result_t log_deliver(void *arg,const char *text,size_t length)
{
 cog_cli_app_t *app=arg;
 return app->frontend != NULL && app->frontend->log != NULL ? app->frontend->log(app,text,length) : COG_OK;
}
static cog_result_t reserve_state(cog_cli_app_t *app)
{
    size_t cap = app->opts.max_context_bytes;
    if (cap > (SIZE_MAX - 65536U) / 32U || app->opts.max_tool_output_bytes > SIZE_MAX - 512U)
        return COG_ERROR_LIMIT;
    app->delta_capacity = cap + 65536U;
    app->result_capacity = (app->opts.max_tool_output_bytes < CLI_EVIDENCE_BYTES ?
        CLI_EVIDENCE_BYTES : app->opts.max_tool_output_bytes) + 512U;
    app->text = calloc(cap + 1U, 1U);
    app->opaque_storage = malloc(cap);
    app->delta_storage = malloc(app->delta_capacity);
    app->result_storage = malloc(app->result_capacity);
    app->history = calloc(CLI_NODES, sizeof(*app->history));
    app->display_items = calloc(CLI_NODES + 2U, sizeof(*app->display_items));
    app->tools_json_capacity=app->runtime_resolved ? app->opts.max_context_bytes : CLI_SCHEMA_BYTES;
    app->tools_json = malloc(app->tools_json_capacity);
    if (app->text == NULL || app->opaque_storage == NULL || app->delta_storage == NULL ||
        app->result_storage == NULL || app->history == NULL || app->display_items == NULL ||
        app->tools_json == NULL) return COG_ERROR_NOMEM;
    for (size_t i = 0U; i < CLI_CALLS; i++) {
        app->calls[i].arguments = malloc(CLI_ARGUMENT_BYTES);
        if (app->calls[i].arguments == NULL) return COG_ERROR_NOMEM;
    }
    for (size_t i = 0U; i < CLI_INPUTS; i++) {
        app->inputs[i].text = malloc(cap + 1U);
        if (app->inputs[i].text == NULL) return COG_ERROR_NOMEM;
    }
    return cog_cli_tools_json(app);
}
static cog_result_t cli_build_script(cog_cli_app_t *app)
{
    const char *chunks = app->opts.chunks;
    size_t chunk_count = 1U;
    size_t step_count;
    size_t i;
    char *copy;
    char *piece;
    const char *p;

    /* A caller-supplied script takes precedence over the chunk/mode
     * options.  Its steps are borrowed, not owned. */
    if (app->opts.script != NULL) {
        app->script = *app->opts.script;
        return COG_OK;
    }

    if (chunks == NULL) {
        chunks = "Hello from cog-cli!";
    }

    for (p = chunks; *p != '\0'; ++p) {
        if (*p == ',') {
            ++chunk_count;
        }
    }

    switch (app->opts.mock_mode) {
    case COG_CLI_MOCK_ERROR:
    case COG_CLI_MOCK_RATE_LIMIT:
        step_count = 2U;
        break;
    case COG_CLI_MOCK_CANCEL:
        step_count = 1U;
        break;
    case COG_CLI_MOCK_STREAM:
    default:
        step_count = chunk_count;
        break;
    }

    app->steps = (cog_mock_step_t *)calloc(step_count,
                                           sizeof(cog_mock_step_t));
    if (app->steps == NULL) {
        return COG_ERROR_NOMEM;
    }
    app->step_count = step_count;

    copy = strdup(chunks);
    if (copy == NULL) {
        free(app->steps);
        app->steps = NULL;
        app->step_count = 0U;
        return COG_ERROR_NOMEM;
    }
    app->chunks_copy = copy;

    switch (app->opts.mock_mode) {
    case COG_CLI_MOCK_STREAM: {
        size_t idx = 0U;

        piece = copy;
        for (i = 0U; i < chunk_count; ++i) {
            char *comma = strchr(piece, ',');

            if (comma != NULL) {
                *comma = '\0';
            }
            app->steps[idx].kind = COG_MOCK_STEP_CHUNK;
            app->steps[idx].chunk = piece;
            ++idx;
            piece = (comma != NULL) ? comma + 1 : piece + strlen(piece);
        }
        break;
    }
    case COG_CLI_MOCK_ERROR:
        app->steps[0].kind = COG_MOCK_STEP_ERROR;
        app->steps[0].error_code = COG_ERROR_UNAVAILABLE;
        app->steps[0].http_status = 500;
        app->steps[0].retryable = true;
        app->steps[0].retry_after_ms = 10U;
        app->steps[0].error_message = "mock provider error";
        app->steps[1].kind = COG_MOCK_STEP_CHUNK;
        app->steps[1].chunk = "retried ok";
        break;
    case COG_CLI_MOCK_RATE_LIMIT:
        app->steps[0].kind = COG_MOCK_STEP_ERROR;
        app->steps[0].error_code = COG_ERROR_UNAVAILABLE;
        app->steps[0].http_status = 429;
        app->steps[0].retryable = true;
        app->steps[0].retry_after_ms = 10U;
        app->steps[0].error_message = "mock rate limited";
        app->steps[1].kind = COG_MOCK_STEP_CHUNK;
        app->steps[1].chunk = "recovered after rate limit";
        break;
    case COG_CLI_MOCK_CANCEL:
        app->steps[0].kind = COG_MOCK_STEP_CANCEL;
        break;
    }

    app->script.steps = app->steps;
    app->script.step_count = app->step_count;
    /* Replay the script for every turn so interactive mode produces the
     * same deterministic response each time. */
    app->script.repeat = SIZE_MAX;
    app->script.send_error = COG_OK;
    app->script.chunk_delay_ms = 0U;
    return COG_OK;
}


static cog_result_t construct(cog_cli_app_t **out, const cog_cli_options_t *input,
    cog_cli_execution_admission_t *admission, const cli_frontend_t *frontend)
{
    if (out == NULL || (input == NULL && admission == NULL)) return COG_ERROR_NULL_PTR;
    *out = NULL;
    cog_cli_app_t *app = calloc(1U, sizeof(*app));
    if (app == NULL) {
        if (admission != NULL) { cog_cli_configuration_free(admission->configuration); admission->configuration = NULL; }
        return COG_ERROR_NOMEM;
    }
    if (admission != NULL) { app->configuration = admission->configuration; admission->configuration = NULL; }
    if(admission!=NULL && admission->runtime_config!=NULL){
        app->runtime_config=*admission->runtime_config;app->runtime_resolved=true;
        app->runtime_owner=admission->runtime_owner;
        if(app->runtime_owner!=NULL)app->runtime_owner->app=app;
    }
    app->signal_fd = -1;
    app->frontend = frontend;
    atomic_init(&app->stop_requested, false);
    if (!atomic_is_lock_free(&app->stop_requested)) { cog_cli_configuration_free(app->configuration); free(app); return COG_ERROR_UNSUPPORTED; }
    cog_result_t rc = admission == NULL ? cog_cli_configuration_new(&app->configuration, input, 1U) : COG_OK;
    if (rc != COG_OK) goto fail;
    app->opts = *cog_cli_configuration_options(app->configuration);
    if (app->frontend != NULL && app->frontend->open != NULL) {
        rc=app->frontend->open(app); if(rc!=COG_OK) goto fail;
    }
    app->configuration_generation = admission == NULL ? 1U : admission->configuration_generation;
    if (admission != NULL) {
        app->headless = calloc(1U, sizeof(*app->headless));
        if (app->headless == NULL) { rc = COG_ERROR_NOMEM; goto fail; }
        cli_execution_state_t *h = app->headless;
        h->kind = admission->kind;
        h->session_borrowed = admission->kind == COG_CLI_EXECUTION_LOCAL_CHILD;
        h->authority = admission->kind == COG_CLI_EXECUTION_STAGED ? CLI_AUTH_STAGED : CLI_AUTH_NORMAL;
        h->absolute_deadline_ms = admission->absolute_deadline_ms;
        h->expected_owner = admission->expected; h->next_owner = admission->next;
        h->checkpoint = admission->checkpoint;
        rc = cog_cli_configuration_tool_mask(app->configuration, &h->tool_mask);
        if (rc != COG_OK) goto fail;
        app->opts.socket_path = NULL; app->opts.interactive = false;
        app->opts.show_usage = false; app->observer = admission->observer;
        app->reload_attempts = admission->reload_attempts_used;
        if (admission->kind != COG_CLI_EXECUTION_ROOT) {
            app->opts.prompt = NULL; app->opts.queue_text = NULL; app->prompt_submitted = true;
        }
        if (admission->task != NULL) {
            h->task_bytes = strlen(admission->task);
            h->task = malloc(h->task_bytes + 1U);
            if (h->task == NULL) { rc = COG_ERROR_NOMEM; goto fail; }
            memcpy(h->task, admission->task, h->task_bytes + 1U);
        }
        if (admission->provenance != NULL) {
            h->provenance_bytes = admission->provenance_len;
            h->provenance = malloc(h->provenance_bytes + 1U);
            if (h->provenance == NULL) { rc = COG_ERROR_NOMEM; goto fail; }
            memcpy(h->provenance, admission->provenance, h->provenance_bytes);
            h->provenance[h->provenance_bytes] = '\0';
            memcpy(h->parent_session_id, admission->parent_session_id, strlen(admission->parent_session_id) + 1U);
        }
    }
    if ((app->opts.mock_deltas == NULL) != (app->opts.mock_delta_count == 0U) ||
        app->opts.mock_delta_count > 4096U ||
        (app->opts.mock_deltas != NULL && app->opts.script != NULL) ||
        (app->opts.backend == COG_CLI_BACKEND_REAL &&
         (app->opts.mock_deltas != NULL || app->opts.script != NULL))) {
        rc = COG_ERROR_INVALID_ARG; goto fail;
    }
    if (app->opts.max_context_bytes > (SIZE_MAX - 65536U) / 32U) { rc = COG_ERROR_LIMIT; goto fail; }
    if (app->opts.session_action != COG_CLI_SESSION_LIST) {
        rc = reserve_state(app);
        if (rc != COG_OK) goto fail;
    }
    if (app->headless != NULL) {
        cli_execution_state_t *h = app->headless;
        size_t fixed = CLI_NODES + 256U;
        if (app->result_capacity > SIZE_MAX - fixed ||
            app->opts.max_context_bytes > (SIZE_MAX - fixed - app->result_capacity) / 5U) {
            rc = COG_ERROR_LIMIT; goto fail;
        }
        h->frozen_item_capacity = CLI_NODES + 2U;
        h->frozen_text_capacity = app->opts.max_context_bytes * 5U + app->result_capacity + fixed;
        h->frozen_items = calloc(h->frozen_item_capacity, sizeof(*h->frozen_items));
        h->frozen_text = malloc(h->frozen_text_capacity);
        if (h->frozen_items == NULL || h->frozen_text == NULL) { rc = COG_ERROR_NOMEM; goto fail; }
    }
    if(app->runtime_resolved && !cog_cli_metadata(app)) {
        rc=cog_provider_connect(app->runtime_config.provider.endpoint,&app->provider);
        if(rc==COG_OK)rc=cli_runtime_initial_preflight(app);
        if(rc!=COG_OK)goto fail;
    }
    rc = admission == NULL ? cog_cli_sessions_open(app) : cli_sessions_admit(app, admission);
    if (rc != COG_OK) goto fail;
    rc = cog_cli_clock(&app->now);
    if (rc != COG_OK) goto fail;
    int random_fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (random_fd < 0) { rc = COG_ERROR_IO; goto fail; }
    ssize_t random_size = read(random_fd, &app->generation, sizeof(app->generation));
    (void)close(random_fd);
    if (random_size != (ssize_t)sizeof(app->generation) || app->generation == 0U) {
        rc = COG_ERROR_IO; goto fail;
    }
    if (cog_cli_metadata(app)) { *out = app; return COG_OK; }
    rc = app->runtime_resolved ? COG_OK : cog_cli_reload_init(app);
    if (rc != COG_OK) goto fail;
    if (app->opts.backend == COG_CLI_BACKEND_MOCK) {
        rc = cli_build_script(app);
        if (rc != COG_OK) goto fail;
    }
    cog_provider_limits_t limits = {
        .request_bytes = app->opts.max_context_bytes * 4U,
        .parser_bytes = app->opts.max_context_bytes * 16U,
        .writer_bytes = app->opts.max_context_bytes * 8U,
        .max_json_depth = 64U, .max_frame_bytes = app->opts.max_context_bytes,
        .retained_delta_bytes = app->delta_capacity, .retained_delta_count = 16U,
        .max_text_bytes = app->opts.max_context_bytes, .max_tool_calls = CLI_CALLS,
        .max_tool_id_bytes = CLI_ID_BYTES, .max_tool_name_bytes = CLI_ID_BYTES,
        .max_tool_arguments_bytes = CLI_ARGUMENT_BYTES, .max_opaque_bytes = app->opts.max_context_bytes};
    rc = cog_cli_configuration_endpoint(app->configuration)!=NULL ? COG_OK : cog_cli_configuration_bind(app->configuration, &cog_openrouter_provider_vtable,
        &cog_mock_provider_vtable, &app->script, &limits);
    if (rc != COG_OK) goto fail;
    if (admission != NULL && cog_cli_configuration_endpoint(app->configuration)->generation !=
        admission->configuration_generation) { rc = COG_ERROR_INVALID_ARG; goto fail; }
    if(app->provider!=NULL)rc=COG_OK;
    else if (app->opts.mock_deltas != NULL)
        rc = cog_mock_provider_new_deltas(app->opts.mock_deltas, app->opts.mock_delta_count, &app->provider);
    else rc = cog_provider_connect(cog_cli_configuration_endpoint(app->configuration), &app->provider);
    if (rc != COG_OK) goto fail;
    app->log_sink = (cog_obs_sink_t){.name = "log", .deliver = log_deliver,
        .user = app, .min_severity = COG_OBS_TRACE};
    app->sink_table = (cog_obs_sink_table_t){&app->log_sink, 1U};
    cog_obs_config_t obs = {.min_severity = app->opts.verbose || app->opts.log_path != NULL ? COG_OBS_DEBUG : COG_OBS_INFO,
        .fields = cog_cli_observation_fields(), .sinks = &app->sink_table, .service = "cog-cli"};
    rc = cog_obs_new(&app->obs, &obs);
    if (rc != COG_OK) goto fail;
    if (app->observer != NULL) {
        rc = cog_cli_observer_bind(app->observer, app->obs);
        if (rc != COG_OK) goto fail;
    }
    if (app->opts.workspace_root != NULL && app->store != NULL) {
        static const char *const environment[] = {"PATH=/usr/bin:/bin", "LC_ALL=C.UTF-8"};
        cog_workspace_config_t workspace = {.root = app->opts.workspace_root,
            .max_file_bytes = app->opts.max_context_bytes, .max_search_entries = 4096U,
            .service_quantum = 16U, .environment = environment, .environment_count = 2U,
            .enable_commands = true};
        rc = cog_workspace_new(&app->workspace, &workspace);
        if (rc != COG_OK) goto fail;
    }
    if (app->opts.socket_path != NULL) {
        rc = cog_ipc_endpoint_new(&app->ipc);
        if (rc == COG_OK) rc = cog_ipc_endpoint_listen(app->ipc, app->opts.socket_path);
        if (rc != COG_OK) goto fail;
    }
    size_t dispatch_count = 0U;
    const cog_dispatch_entry_t *dispatch = cog_cli_dispatch_table(&dispatch_count);
    cog_core_config_t core = {.session = app->session, .dispatch_table = dispatch,
        .dispatch_table_size = dispatch_count, .provider = app->provider,
        .obs = app->obs, .store = app->store, .ipc_endpoint = app->ipc, .user = app,
        .tool_table = app->runtime_resolved ? app->runtime_config.tools.table : &cog_workspace_tools, .service_fn = cog_cli_service, .service_arg = app,
        .idle_fn = cog_cli_idle, .idle_arg = app, .service_interval = 4U,
        .max_idle_wait_ms = 10U, .event_queue_capacity = app->runtime_resolved ? app->runtime_config.limits.max_events : 64U,
        .step_event_limit = admission == NULL ? 0U : 1U};
    rc = cog_core_new(&app->core, &core);
    if (rc != COG_OK) goto fail;
    app->ipc = NULL;
    rc = cog_cli_observation_check(app, COG_OK);
    if (rc != COG_OK) goto fail;
    if (admission == NULL) {
        rc = cog_cli_sessions_prepare(app);
        if (rc != COG_OK) goto fail;
    }
    if ((admission == NULL || admission->kind == COG_CLI_EXECUTION_ROOT) && app->opts.session_action == COG_CLI_SESSION_FORK) {
        rc = cog_cli_app_fork(app, app->opts.fork_node_id);
        if (rc != COG_OK) goto fail;
    }
    *out = app;
    return COG_OK;
fail:
    cli_app_free(app);
    return rc;
}
cog_result_t cli_app_dispose(cog_cli_app_t **owner, cog_result_t primary)
{
    if (owner == NULL) return COG_ERROR_NULL_PTR;
    cog_cli_app_t *app = *owner;
    if (app == NULL) return primary;
    /* app_run drains on every exit. Never destroy an active borrowed handle. */
    if (app->execution != NULL) {
        cog_result_t released = cog_toolkit_execution_free(app->execution);
        assert(released == COG_OK);
        if (released != COG_OK) {
            if(app->frontend != NULL && app->frontend->error != NULL) app->frontend->error(app,"active execution must be drained before app_free",released);
            return primary != COG_OK ? primary : released;
        }
        app->execution = NULL;
    }
    if (app->headless != NULL && app->headless->delegation != NULL) {
        cog_result_t released = cog_cli_delegation_context_free(app->headless->delegation);
        if (released != COG_OK) return primary != COG_OK ? primary : released;
    }
    if (app->workspace != NULL) {
        cog_result_t released = cog_workspace_free(app->workspace);
        if (released == COG_ERROR_INVALID_STATE) return primary != COG_OK ? primary : released;
        app->workspace = NULL;
        if (primary == COG_OK) primary = released;
    }
    if (app->core != NULL) { cog_core_free(app->core); app->core = NULL; }
    cog_result_t cleanup = cog_cli_reload_free(app);
    cog_result_t ending = app->assistant_pending ? app->assistant_error :
        app->terminal_result_pending ? app->tool_result_error :
        app->deferred_error != COG_OK ? app->deferred_error :
        cleanup != COG_OK ? cleanup : COG_ERROR_ABORTED;
    if (app->tool_timed) cog_cli_observation_tool_end(app, ending, app->effects, false);
    if (app->provider_timed) cog_cli_observation_provider_end(app, ending, 0);
    cog_result_t observed = cog_cli_observation_detach(app, cleanup);
    if (observed != COG_OK && app->frontend != NULL && app->frontend->error != NULL)
        app->frontend->error(app,"provider/observation cleanup",observed);
    if (primary == COG_OK) primary = observed;
    if (app->guard != NULL) {
        cog_result_t released = cog_store_owner_release(app->guard);
        if (released == COG_ERROR_INVALID_STATE) return primary != COG_OK ? primary : released;
        app->guard = NULL;
        if (primary == COG_OK) primary = released;
    }
    if (app->obs != NULL) cog_obs_free(app->obs);
    if (app->store != NULL) cog_store_free(app->store);
    if (app->session != NULL && (app->headless == NULL || !app->headless->session_borrowed)) cog_session_free(app->session);
    if (app->ipc != NULL) { cog_result_t released = cog_ipc_endpoint_free(app->ipc); if (primary == COG_OK) primary = released; }
    if (app->frontend != NULL && app->frontend->close != NULL) { cog_result_t end=app->frontend->close(app); if(primary==COG_OK) primary=end; }
    for (size_t i = 0U; i < CLI_CALLS; i++) free(app->calls[i].arguments);
    for (size_t i = 0U; i < CLI_INPUTS; i++) free(app->inputs[i].text);
    free(app->text); free(app->opaque_storage); free(app->delta_storage);
    free(app->result_storage); free(app->history); free(app->display_items); free(app->tools_json);
    if (app->headless != NULL) {
        free(app->headless->frozen_items); free(app->headless->frozen_text);
        free(app->headless->task); free(app->headless->provenance); free(app->headless);
    }
    free(app->steps); free(app->chunks_copy); free(app); *owner = NULL;
    return primary;
}
cog_result_t cli_app_new_frontend(cog_cli_app_t **out, const cog_cli_options_t *input, const cli_frontend_t *frontend)
{
    return construct(out, input, NULL, frontend);
}
cog_result_t cli_app_construct_resolved(cog_cli_app_t **out, cog_cli_execution_admission_t *admission)
{
    if (admission == NULL) { if (out != NULL) *out = NULL; return COG_ERROR_NULL_PTR; }
    if (out == NULL || admission->configuration == NULL) {
        cog_cli_configuration_free(admission->configuration); admission->configuration = NULL;
        if (out != NULL) *out = NULL;
        return COG_ERROR_NULL_PTR;
    }
    return construct(out, NULL, admission, NULL);
}
void cli_app_free(cog_cli_app_t *app)
{
    if(app!=NULL && app->runtime_owner!=NULL && app->runtime_owner->app==app)app->runtime_owner->app=NULL;
    if (app == NULL) return;
    cog_result_t rc = app->headless == NULL ? cli_app_dispose(&app, COG_OK) :
        cog_cli_execution_close(&app, COG_OK);
    (void)rc; /* Checked close callers retain the result; no runtime terminal IO. */
}
cog_result_t cog_cli_app_run(cog_cli_app_t *app)
{
    if (app == NULL) return COG_ERROR_NULL_PTR;
    if (app->reload_in_progress) return COG_ERROR_INVALID_STATE;
    if (cog_cli_metadata(app)) return app->frontend != NULL && app->frontend->metadata != NULL ? app->frontend->metadata(app) : COG_ERROR_INVALID_STATE;
    if (app->reload_cleanup_error != COG_OK) {
        (void)cog_cli_drain(app); return app->reload_cleanup_error;
    }
    if (app->assistant_pending) return app->assistant_error;
    if (app->recovery) return COG_ERROR_INDETERMINATE;
    cog_result_t observation = cog_cli_observation_check(app, COG_OK);
    if (observation != COG_OK) {
        cog_result_t drained = cog_cli_drain(app);
        return cog_cli_observation_check(app, drained);
    }
    atomic_store_explicit(&app->stop_requested, false, memory_order_release);
    app->shutdown = false;
    if (!app->active && !app->provider_active && app->execution == NULL)
        app->cancelling = false; /* Previous cancellation has already drained. */
    cog_result_t rc = COG_OK;
    if (app->resuming_policy && app->active && !app->event_queued) {
        cog_event_t event = {.id = CLI_EVENT_RESUME_POLICY, .kind = COG_EVENT_STATUS_UPDATE,
            .source_id = app->request_serial};
        rc = cog_core_enqueue(app->core, &event);
        if (rc != COG_OK) return rc;
        app->event_queued = true;
    }
    if (!app->prompt_submitted) {
        if (app->opts.prompt != NULL) rc = cog_cli_app_submit(app, app->opts.prompt, strlen(app->opts.prompt));
        if (rc == COG_OK && app->opts.queue_text != NULL)
            rc = cog_cli_app_queue(app, app->opts.queue_text, strlen(app->opts.queue_text));
        if (rc != COG_OK) return rc;
        app->prompt_submitted = true;
    }
    rc = cog_core_run(app->core);
    cog_result_t drain = cog_cli_drain(app);
    return cog_cli_observation_check(app, drain != COG_OK ? drain : rc);
}
void cog_cli_app_stop(cog_cli_app_t *app)
{
    if (app != NULL) {
        atomic_store_explicit(&app->stop_requested, true, memory_order_release);
        cog_core_stop(app->core);
    }
}
static cog_result_t input_add(cog_cli_app_t *app, const char *text, size_t length, bool queue)
{
    if (app == NULL || text == NULL) return COG_ERROR_NULL_PTR;
    if (cog_cli_metadata(app)) return COG_ERROR_INVALID_STATE;
    if (app->reload_in_progress) return COG_ERROR_INVALID_STATE;
    if (app->reload_cleanup_error != COG_OK) return COG_ERROR_INDETERMINATE;
    if (app->observation_error != COG_OK) return COG_ERROR_INDETERMINATE;
    if (length > app->opts.max_context_bytes) return COG_ERROR_LIMIT;
    if (!cog_cli_text_valid(text, length)) return COG_ERROR_INVALID_ARG;
    if (app->assistant_pending) return app->assistant_error;
    if (app->recovery) return COG_ERROR_INDETERMINATE;
    if (app->input_order == UINT64_MAX) return COG_ERROR_LIMIT;
    for (size_t i = 0U; i < CLI_INPUTS; i++) if (!app->inputs[i].used) {
        cli_input_t *slot = &app->inputs[i];
        memcpy(slot->text, text, length); slot->text[length] = '\0';
        slot->length = length; slot->used = true; slot->queued = queue;
        slot->continuation_authorized = !queue && !app->active && app->continuation_required;
        slot->order = ++app->input_order;
        if (queue) return COG_OK;
        cog_event_t event = {.id = COG_EVENT_USER_INPUT, .kind = COG_EVENT_USER_INPUT,
            .payload = slot->text, .payload_len = length};
        cog_result_t rc = cog_core_enqueue(app->core, &event);
        if (rc != COG_OK) slot->used = false;
        return rc;
    }
    return COG_ERROR_OVERFLOW;
}
cog_result_t cog_cli_app_submit(cog_cli_app_t *app, const char *text, size_t length)
{ return input_add(app, text, length, false); }
cog_result_t cog_cli_app_queue(cog_cli_app_t *app, const char *text, size_t length)
{ return input_add(app, text, length, true); }
static cog_result_t inject(cog_cli_app_t *app, cog_event_kind_t kind)
{
    if (app == NULL) return COG_ERROR_NULL_PTR;
    if (app->reload_in_progress) return COG_ERROR_INVALID_STATE;
    if (cog_cli_metadata(app)) {
        if (kind != COG_EVENT_SHUTDOWN) return COG_ERROR_INVALID_STATE;
        cog_cli_app_stop(app); return COG_OK;
    }
    cog_event_t event = {.id = (cog_event_id_t)kind, .kind = kind};
    return cog_core_enqueue(app->core, &event);
}
cog_result_t cog_cli_app_cancel(cog_cli_app_t *app) { return inject(app, COG_EVENT_CANCEL); }
cog_result_t cog_cli_app_shutdown(cog_cli_app_t *app) { return inject(app, COG_EVENT_SHUTDOWN); }
cog_core_t *cog_cli_app_core(cog_cli_app_t *app) { return app == NULL ? NULL : app->core; }
cog_session_t *cog_cli_app_session(cog_cli_app_t *app) { return app == NULL ? NULL : app->session; }
cog_provider_t *cog_cli_app_provider(cog_cli_app_t *app) { return app == NULL ? NULL : app->provider; }
cog_obs_t *cog_cli_app_obs(cog_cli_app_t *app) { return app == NULL ? NULL : app->obs; }
cog_store_t *cog_cli_app_store(cog_cli_app_t *app) { return app == NULL ? NULL : app->store; }
cog_state_t cog_cli_app_state(cog_cli_app_t *app) { return app == NULL ? 0U : cog_core_state(app->core); }

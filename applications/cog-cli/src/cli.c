/** @file cli.c @brief Explicit application ownership and bounded lifecycle. */
#define _POSIX_C_SOURCE 200809L
#include "app_internal.h"
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
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return COG_ERROR_IO;
    *now = (uint64_t)ts.tv_sec * 1000U + (uint64_t)ts.tv_nsec / 1000000U;
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
    return app->store == NULL ? COG_OK : cog_store_save(app->store, app->session);
}
static cog_result_t log_deliver(void *arg, const char *record, size_t length)
{
    FILE *stream = arg;
    return fwrite(record, 1U, length, stream) == length && fputc('\n', stream) != EOF
        ? COG_OK : COG_ERROR_IO;
}
/* Walk parent directories without changing cwd or following a symlink. New
   directory entries and their parents are fsynced before store construction. */
static cog_result_t durable_directory(const char *path)
{
    char copy[4097];
    size_t length = strlen(path);
    int parent;
    if (length == 0U || length >= sizeof(copy)) return COG_ERROR_LIMIT;
    memcpy(copy, path, length + 1U);
    parent = open(path[0] == '/' ? "/" : ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (parent < 0) return COG_ERROR_IO;
    char *part = copy + (path[0] == '/' ? 1U : 0U);
    cog_result_t rc = COG_OK;
    while (*part != '\0') {
        char *slash = strchr(part, '/');
        if (slash != NULL) *slash = '\0';
        if (*part != '\0' && strcmp(part, ".") != 0) {
            if (strcmp(part, "..") == 0) { rc = COG_ERROR_DENIED; break; }
            bool created = mkdirat(parent, part, 0700) == 0;
            if (!created && errno != EEXIST) { rc = COG_ERROR_IO; break; }
            int next = openat(parent, part, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (next < 0) { rc = COG_ERROR_IO; break; }
            if (created && (fsync(next) != 0 || fsync(parent) != 0)) {
                (void)close(next); rc = COG_ERROR_IO; break;
            }
            (void)close(parent); parent = next;
        }
        if (slash == NULL) break;
        part = slash + 1;
    }
    (void)close(parent);
    return rc;
}
static cog_result_t reserve_state(cog_cli_app_t *app)
{
    size_t cap = app->opts.max_context_bytes;
    if (cap > (SIZE_MAX - 65536U) / 32U || app->opts.max_tool_output_bytes > SIZE_MAX - 512U)
        return COG_ERROR_LIMIT;
    app->delta_capacity = cap + 65536U;
    app->result_capacity = app->opts.max_tool_output_bytes + 512U;
    app->text = calloc(cap + 1U, 1U);
    app->opaque_storage = malloc(cap);
    app->delta_storage = malloc(app->delta_capacity);
    app->result_storage = malloc(app->result_capacity);
    app->history = calloc(CLI_NODES, sizeof(*app->history));
    app->display_items = calloc(CLI_NODES + 1U, sizeof(*app->display_items));
    app->tools_json = malloc(CLI_SCHEMA_BYTES);
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


static cog_result_t make_store(cog_cli_app_t *app)
{
    if (app->opts.store_mode == COG_CLI_STORE_DISABLED) return COG_OK;
    cog_result_t rc = durable_directory(app->opts.store_dir);
    if (rc != COG_OK) return rc;
    cog_store_config_t config = {.base_dir = app->opts.store_dir,
        .require_owner = true, .max_record_bytes = app->opts.max_context_bytes * 16U};
    rc = cog_store_new(&app->store, &config);
    if (rc != COG_OK) return rc;
    /* K creates new sessions; L owns explicit resume/fork UX. A reused ID must
       never replace durable history or silently admit a dead incarnation. */
    cog_store_owner_t old = {0};
    cog_store_checkpoint_t checkpoint = {0};
    rc = cog_store_owner_read(app->store, cog_session_id(app->session), &old, &checkpoint);
    if (rc != COG_OK) return rc;
    if (old.epoch != 0U || checkpoint.generation != 0U) return COG_ERROR_INVALID_STATE;
    uint64_t incarnation = 0U;
    int random_fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (random_fd < 0) return COG_ERROR_IO;
    ssize_t n = read(random_fd, &incarnation, sizeof(incarnation));
    (void)close(random_fd);
    if (n != (ssize_t)sizeof(incarnation) || incarnation == 0U) return COG_ERROR_IO;
    cog_store_owner_t owner = {.epoch = 1U, .instance_id = incarnation};
    rc = cog_store_owner_compare_exchange(app->store, cog_session_id(app->session), old, owner, checkpoint);
    if (rc == COG_OK) rc = cog_store_owner_acquire(app->store, cog_session_id(app->session), owner, &app->guard);
    return rc;
}
cog_result_t cog_cli_app_new(cog_cli_app_t **out, const cog_cli_options_t *input)
{
    if (out == NULL || input == NULL) return COG_ERROR_NULL_PTR;
    *out = NULL;
    cog_cli_app_t *app = calloc(1U, sizeof(*app));
    if (app == NULL) return COG_ERROR_NOMEM;
    app->signal_fd = -1;
    app->out = stdout;
    cog_result_t rc = cog_cli_configuration_new(&app->configuration, input, 1U);
    if (rc != COG_OK) goto fail;
    app->opts = *cog_cli_configuration_options(app->configuration);
    if ((app->opts.mock_deltas == NULL) != (app->opts.mock_delta_count == 0U) ||
        app->opts.mock_delta_count > 4096U ||
        (app->opts.mock_deltas != NULL && app->opts.script != NULL) ||
        (app->opts.backend == COG_CLI_BACKEND_REAL &&
         (app->opts.mock_deltas != NULL || app->opts.script != NULL))) {
        rc = COG_ERROR_INVALID_ARG; goto fail;
    }
    rc = reserve_state(app);
    if (rc != COG_OK) goto fail;
    cog_session_config_t session = {.id = app->opts.session_id, .max_nodes = CLI_NODES,
        .max_payload_bytes = app->opts.max_context_bytes * 4U,
        .max_tool_calls_per_message = CLI_CALLS};
    rc = cog_session_new(&app->session, &session);
    if (rc != COG_OK) goto fail;
    rc = make_store(app);
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
    rc = cog_cli_configuration_bind(app->configuration, &cog_openrouter_provider_vtable,
        &cog_mock_provider_vtable, &app->script, &limits);
    if (rc != COG_OK) goto fail;
    if (app->opts.mock_deltas != NULL)
        rc = cog_mock_provider_new_deltas(app->opts.mock_deltas, app->opts.mock_delta_count, &app->provider);
    else rc = cog_provider_connect(cog_cli_configuration_endpoint(app->configuration), &app->provider);
    if (rc != COG_OK) goto fail;
    app->log_file = app->opts.log_path != NULL ? fopen(app->opts.log_path, "a") : stderr;
    if (app->log_file == NULL) { rc = COG_ERROR_IO; goto fail; }
    app->log_sink = (cog_obs_sink_t){.name = "log", .deliver = log_deliver,
        .user = app->log_file, .min_severity = COG_OBS_TRACE};
    app->sink_table = (cog_obs_sink_table_t){&app->log_sink, 1U};
    cog_obs_config_t obs = {.min_severity = app->opts.verbose ? COG_OBS_DEBUG : COG_OBS_INFO,
        .sinks = &app->sink_table, .service = "cog-cli"};
    rc = cog_obs_new(&app->obs, &obs);
    if (rc != COG_OK) goto fail;
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
        .tool_table = &cog_workspace_tools, .service_fn = cog_cli_service, .service_arg = app,
        .idle_fn = cog_cli_idle, .idle_arg = app, .service_interval = 4U,
        .max_idle_wait_ms = 10U, .event_queue_capacity = 64U};
    rc = cog_core_new(&app->core, &core);
    if (rc != COG_OK) goto fail;
    app->ipc = NULL;
    rc = cog_cli_clock(&app->now);
    if (rc != COG_OK) goto fail;
    int random_fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (random_fd < 0) { rc = COG_ERROR_IO; goto fail; }
    ssize_t random_size = read(random_fd, &app->generation, sizeof(app->generation));
    (void)close(random_fd);
    if (random_size != (ssize_t)sizeof(app->generation) || app->generation == 0U) {
        rc = COG_ERROR_IO; goto fail;
    }
    *out = app;
    return COG_OK;
fail:
    cog_cli_app_free(app);
    return rc;
}
void cog_cli_app_free(cog_cli_app_t *app)
{
    if (app == NULL) return;
    /* app_run drains on every exit. Never destroy an active borrowed handle. */
    if (app->execution != NULL) {
        cog_result_t released = cog_toolkit_execution_free(app->execution);
        assert(released == COG_OK);
        if (released != COG_OK) {
            (void)fprintf(stderr, "cog-cli: active execution must be drained before app_free\n");
            return;
        }
    }
    if (app->workspace != NULL) (void)cog_workspace_free(app->workspace);
    if (app->core != NULL) cog_core_free(app->core);
    if (app->provider != NULL) (void)cog_provider_disconnect(app->provider);
    if (app->guard != NULL) (void)cog_store_owner_release(app->guard);
    if (app->obs != NULL) cog_obs_free(app->obs);
    if (app->store != NULL) cog_store_free(app->store);
    if (app->session != NULL) cog_session_free(app->session);
    if (app->ipc != NULL) (void)cog_ipc_endpoint_free(app->ipc);
    if (app->log_file != NULL && app->log_file != stderr) (void)fclose(app->log_file);
    cog_cli_configuration_free(app->configuration);
    for (size_t i = 0U; i < CLI_CALLS; i++) free(app->calls[i].arguments);
    for (size_t i = 0U; i < CLI_INPUTS; i++) free(app->inputs[i].text);
    free(app->text); free(app->opaque_storage); free(app->delta_storage);
    free(app->result_storage); free(app->history); free(app->display_items); free(app->tools_json);
    free(app->steps); free(app->chunks_copy); free(app);
}
cog_result_t cog_cli_app_run(cog_cli_app_t *app)
{
    if (app == NULL) return COG_ERROR_NULL_PTR;
    app->shutdown = false;
    cog_result_t rc = COG_OK;
    if (!app->prompt_submitted) {
        if (app->opts.prompt != NULL) rc = cog_cli_app_submit(app, app->opts.prompt, strlen(app->opts.prompt));
        if (rc == COG_OK && app->opts.queue_text != NULL)
            rc = cog_cli_app_queue(app, app->opts.queue_text, strlen(app->opts.queue_text));
        if (rc != COG_OK) return rc;
        app->prompt_submitted = true;
    }
    rc = cog_core_run(app->core);
    cog_result_t drain = cog_cli_drain(app);
    return drain != COG_OK ? drain : rc;
}
void cog_cli_app_stop(cog_cli_app_t *app)
{
    if (app != NULL) {
        cog_core_stop(app->core);
    }
}
static cog_result_t input_add(cog_cli_app_t *app, const char *text, size_t length, bool queue)
{
    if (app == NULL || text == NULL) return COG_ERROR_NULL_PTR;
    if (length > app->opts.max_context_bytes) return COG_ERROR_LIMIT;
    if (!cog_cli_text_valid(text, length)) return COG_ERROR_INVALID_ARG;
    if (app->recovery) return COG_ERROR_INDETERMINATE;
    if (app->input_order == UINT64_MAX) return COG_ERROR_LIMIT;
    for (size_t i = 0U; i < CLI_INPUTS; i++) if (!app->inputs[i].used) {
        cli_input_t *slot = &app->inputs[i];
        memcpy(slot->text, text, length); slot->text[length] = '\0';
        slot->length = length; slot->used = true; slot->queued = queue;
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

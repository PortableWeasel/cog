/** @file app_observation.c @brief Owner-drained numeric transport observations. */
#include "app_internal.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

enum { F_SESSION, F_SESSION_AVAILABLE, F_REQUEST, F_GENERATION, F_PROVIDER,
    F_MODEL, F_MODEL_AVAILABLE, F_ATTEMPT, F_DURATION, F_STATUS, F_HTTP,
    F_INPUT_AVAILABLE, F_OUTPUT_AVAILABLE, F_COST_AVAILABLE, F_INPUT, F_OUTPUT, F_COST,
    F_NODE, F_ORDINAL, F_TOOL, F_EXECUTED, F_EFFECTS, F_TRUNCATED,
    F_CONNECTION, F_TRANSPORT, F_BYTES_IN, F_BYTES_OUT, F_TIMESTAMP, F_KIND, F_COUNT };
#define DESC(key, type) {key, COG_OBS_FIELD_##type}
static const cog_obs_field_desc_t field_descriptors[F_COUNT] = {
    DESC("session", STRING), DESC("session_label_available", BOOL), DESC("request", UINT),
    DESC("configuration_generation", UINT), DESC("provider", STRING), DESC("model", STRING),
    DESC("model_label_available", BOOL), DESC("attempt", UINT), DESC("duration_ns", UINT),
    DESC("status", STRING), DESC("http_status", INT), DESC("input_tokens_available", BOOL),
    DESC("output_tokens_available", BOOL), DESC("cost_available", BOOL),
    DESC("input_tokens", UINT), DESC("output_tokens", UINT), DESC("cost", DOUBLE),
    DESC("assistant_node", UINT), DESC("call_ordinal", UINT), DESC("tool", STRING),
    DESC("execution_started", BOOL), DESC("side_effects_possible", BOOL), DESC("truncated", BOOL),
    DESC("connection", UINT), DESC("transport_class", STRING), DESC("bytes_in", UINT),
    DESC("bytes_out", UINT), DESC("timestamp_ns", UINT), DESC("kind", STRING)};
#undef DESC
static const cog_obs_field_table_t field_table = {field_descriptors, F_COUNT};
const cog_obs_field_table_t *cog_cli_observation_fields(void) { return &field_table; }
#define UINT_FIELD(key, val) ((cog_obs_field_t){.index=(uint16_t)(key), .kind=COG_OBS_FIELD_UINT, .value.unsigned_integer=(uint64_t)(val)})
#define INT_FIELD(key, val) ((cog_obs_field_t){.index=(uint16_t)(key), .kind=COG_OBS_FIELD_INT, .value.integer=(int64_t)(val)})
#define BOOL_FIELD(key, val) ((cog_obs_field_t){.index=(uint16_t)(key), .kind=COG_OBS_FIELD_BOOL, .value.boolean=(val)?1:0})
#define STR_FIELD(key, val) ((cog_obs_field_t){.index=(uint16_t)(key), .kind=COG_OBS_FIELD_STRING, .value.string=(val)})
static size_t scope_fields(const cog_cli_observation_scope_t *scope, cog_obs_field_t *fields)
{
    size_t count = 0U;
    fields[count++] = BOOL_FIELD(F_SESSION_AVAILABLE, scope->session_available);
    if (scope->session_available) fields[count++] = STR_FIELD(F_SESSION, scope->session);
    fields[count++] = BOOL_FIELD(F_MODEL_AVAILABLE, scope->model_available);
    if (scope->model_available) fields[count++] = STR_FIELD(F_MODEL, scope->model);
    fields[count++] = UINT_FIELD(F_REQUEST, scope->request);
    fields[count++] = UINT_FIELD(F_GENERATION, scope->configuration_generation);
    fields[count++] = STR_FIELD(F_PROVIDER, scope->mock ? "mock" : "openrouter");
    fields[count++] = UINT_FIELD(F_ATTEMPT, scope->attempt);
    return count;
}
typedef struct observation_entry {
    cog_net_observation_t value;
    uint64_t timestamp;
    const cog_net_ctx_t *net;
    cog_cli_observation_scope_t scope;
} observation_entry_t;
typedef struct observation_row {
    const cog_net_ctx_t *net;
    cog_request_id_t request;
    uint64_t sequence, progress_in, progress_out;
    bool used, terminal, published;
    cog_cli_observation_scope_t scope;
    cog_obs_conn_snapshot_t snapshot;
} observation_row_t;
struct cog_cli_observer {
    cog_obs_t *target;
    observation_entry_t *queue;
    observation_row_t *rows;
    size_t capacity, queued, queue_capacity;
    uint64_t next_id, last_timestamp;
    cog_result_t error;
    const cog_net_ctx_t *prepared_net;
    bool prepared, draining;
    cog_cli_observation_scope_t scope;
};
static cog_result_t observer_error(cog_cli_observer_t *observer, cog_result_t result)
{
    if (observer->error == COG_OK && result != COG_OK) observer->error = result;
    return observer->error;
}
cog_result_t cog_cli_observer_new(cog_cli_observer_t **out, size_t slots)
{
    if (out == NULL) return COG_ERROR_NULL_PTR;
    *out = NULL;
    if (slots == 0U) return COG_ERROR_INVALID_ARG;
    if (slots > (SIZE_MAX - 2U) / 4U || slots > SIZE_MAX / sizeof(observation_row_t) ||
        slots * 4U + 2U > SIZE_MAX / sizeof(observation_entry_t)) return COG_ERROR_LIMIT;
    cog_cli_observer_t *observer = calloc(1U, sizeof(*observer));
    if (observer == NULL) return COG_ERROR_NOMEM;
    observer->capacity = slots; observer->queue_capacity = slots * 4U + 2U; observer->next_id = 1U;
    observer->rows = calloc(slots, sizeof(*observer->rows));
    observer->queue = calloc(observer->queue_capacity, sizeof(*observer->queue));
    if (observer->rows == NULL || observer->queue == NULL) {
        cog_cli_observer_free(observer); return COG_ERROR_NOMEM;
    }
    *out = observer; return COG_OK;
}
void cog_cli_observer_free(cog_cli_observer_t *observer)
{
    if (observer == NULL) return;
    assert(observer->target == NULL);
    free(observer->queue); free(observer->rows); free(observer);
}
cog_result_t cog_cli_observer_bind(cog_cli_observer_t *observer, cog_obs_t *target)
{
    if (observer == NULL) return COG_ERROR_NULL_PTR;
    if (observer->target == target) return COG_OK;
    if (observer->queued != 0U || observer->prepared) return COG_ERROR_INVALID_STATE;
    for (size_t i = 0U; i < observer->capacity; i++)
        if (observer->rows[i].used) return COG_ERROR_INVALID_STATE;
    if (target != NULL && observer->error != COG_OK) return observer->error;
    observer->target = target; return COG_OK;
}
cog_result_t cog_cli_observer_status(const cog_cli_observer_t *observer)
{ return observer == NULL ? COG_ERROR_NULL_PTR : observer->error; }
static bool label_valid(const char label[CLI_OBSERVATION_LABEL_BYTES])
{
    const char *end = memchr(label, '\0', CLI_OBSERVATION_LABEL_BYTES);
    if (end == NULL || end == label) return false;
    for (const char *p = label; p != end; p++) {
        unsigned char ch = (unsigned char)*p;
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9') || ch == '.' || ch == '_' || ch == ':' || ch == '/' || ch == '-'))
            return false;
    }
    return true;
}
cog_result_t cog_cli_observer_prepare(cog_cli_observer_t *observer,
    const cog_net_ctx_t *net, const cog_cli_observation_scope_t *scope)
{
    if (observer == NULL || net == NULL || scope == NULL) return COG_ERROR_NULL_PTR;
    cog_result_t result = cog_cli_observer_drain(observer);
    if (result != COG_OK) return result;
    if (observer->target == NULL) return observer_error(observer, COG_ERROR_INVALID_STATE);
    if (scope->request == 0U || scope->configuration_generation == 0U ||
        (scope->retry && scope->attempt == 0U) ||
        (scope->session_available && !label_valid(scope->session)) ||
        (scope->model_available && !label_valid(scope->model)))
        return observer_error(observer, COG_ERROR_INVALID_ARG);
    observer->scope = *scope; observer->prepared_net = net; observer->prepared = true;
    return COG_OK;
}
cog_result_t cog_cli_observer_capture(void *user, const cog_net_ctx_t *net,
    const cog_net_observation_t *value)
{
    cog_cli_observer_t *observer = user;
    if (observer == NULL || net == NULL || value == NULL) return COG_ERROR_NULL_PTR;
    if (observer->error != COG_OK) return observer->error;
    if (observer->draining) return observer_error(observer, COG_ERROR_INVALID_STATE);
    if (observer->target == NULL || (value->kind == COG_NET_OBSERVATION_OPEN &&
        (!observer->prepared || observer->prepared_net != net)))
        return observer_error(observer, COG_ERROR_INVALID_STATE);
    if (observer->queued == observer->queue_capacity) return observer_error(observer, COG_ERROR_OVERFLOW);
    uint64_t timestamp;
    cog_result_t result = cog_obs_clock_now_ns(&timestamp);
    if (result != COG_OK) return observer_error(observer, result);
    if (timestamp < observer->last_timestamp) return observer_error(observer, COG_ERROR_IO);
    observer->last_timestamp = timestamp;
    observation_entry_t *entry = &observer->queue[observer->queued++];
    entry->net = net; entry->value = *value; entry->timestamp = timestamp;
    if (value->kind == COG_NET_OBSERVATION_OPEN) entry->scope = observer->scope;
    return COG_OK;
}
static cog_result_t clear_row(cog_cli_observer_t *observer, observation_row_t *row)
{
    if (row->published) {
        cog_result_t result = cog_obs_diag_clear(observer->target, row->snapshot.conn_id);
        if (result != COG_OK) return result;
    }
    memset(row, 0, sizeof(*row)); return COG_OK;
}
static cog_result_t deliver(cog_cli_observer_t *observer, observation_row_t *row,
    const observation_entry_t *entry, bool retry)
{
    static const cog_obs_conn_event_kind_t kinds[] = {COG_OBS_CONN_EVENT_OPEN, COG_OBS_CONN_EVENT_CONNECTED,
        COG_OBS_CONN_EVENT_PROGRESS, COG_OBS_CONN_EVENT_STALLED, COG_OBS_CONN_EVENT_CLOSED, COG_OBS_CONN_EVENT_FAILED};
    static const char *const names[] = {"open", "connected", "progress", "stalled", "closed", "failed"};
    static const char *const classes[] = {"none", "dns", "tls", "connect", "timeout", "capacity", "cancelled", "dependency"};
    const cog_net_observation_t *value = &entry->value;
    cog_obs_conn_event_t event = {.kind = retry ? COG_OBS_CONN_EVENT_RETRY : kinds[value->kind],
        .conn_id = row->snapshot.conn_id, .timestamp_ns = entry->timestamp,
        .session_id = row->scope.session_available ? row->scope.session : NULL,
        .endpoint = row->scope.mock ? "mock" : "openrouter", .provider = row->scope.mock ? "mock" : "openrouter",
        .model = row->scope.model_available ? row->scope.model : NULL,
        .bytes_in_delta = retry ? 0U : value->bytes_in_delta, .bytes_out_delta = retry ? 0U : value->bytes_out_delta,
        .bytes_in_total = value->bytes_in_total, .bytes_out_total = value->bytes_out_total,
        .retry_attempt = row->scope.attempt, .status = value->outcome, .http_status = (uint16_t)value->http_status};
    cog_result_t result = cog_obs_conn_event_emit(observer->target, &event);
    if (result != COG_OK) return result;
    cog_obs_field_t fields[20]; size_t count = scope_fields(&row->scope, fields);
    fields[count++] = UINT_FIELD(F_CONNECTION, event.conn_id);
    fields[count++] = STR_FIELD(F_KIND, retry ? "retry" : names[value->kind]);
    fields[count++] = STR_FIELD(F_STATUS, cog_cli_result_name(value->outcome));
    fields[count++] = STR_FIELD(F_TRANSPORT, classes[value->error_kind]);
    fields[count++] = INT_FIELD(F_HTTP, value->http_status);
    fields[count++] = UINT_FIELD(F_BYTES_IN, value->bytes_in_total);
    fields[count++] = UINT_FIELD(F_BYTES_OUT, value->bytes_out_total);
    fields[count++] = UINT_FIELD(F_TIMESTAMP, entry->timestamp);
    return cog_obs_log(observer->target, COG_OBS_DEBUG, "net.lifecycle", fields, count);
}
static cog_result_t consume(cog_cli_observer_t *observer, const observation_entry_t *entry)
{
    const cog_net_observation_t *value = &entry->value;
    if (value->kind < COG_NET_OBSERVATION_OPEN || value->kind > COG_NET_OBSERVATION_FAILED ||
        value->error_kind < COG_NET_ERROR_NONE || value->error_kind > COG_NET_ERROR_DEPENDENCY ||
        value->request_id == 0U || value->http_status < 0 || value->http_status > UINT16_MAX)
        return COG_ERROR_PROTOCOL;
    observation_row_t *row = NULL;
    for (size_t i = 0U; i < observer->capacity; i++)
        if (observer->rows[i].used && observer->rows[i].net == entry->net && observer->rows[i].request == value->request_id)
            row = &observer->rows[i];
    if (value->kind == COG_NET_OBSERVATION_OPEN) {
        if (row != NULL || value->sequence != 1U || value->bytes_in_total != 0U || value->bytes_out_total != 0U)
            return COG_ERROR_PROTOCOL;
        for (size_t i = 0U; i < observer->capacity; i++) if (!observer->rows[i].used || observer->rows[i].terminal) {
            row = &observer->rows[i]; break;
        }
        if (row == NULL) return COG_ERROR_OVERFLOW;
        if (observer->next_id == UINT64_MAX) return COG_ERROR_LIMIT;
        cog_result_t result = clear_row(observer, row); if (result != COG_OK) return result;
        row->used = true; row->net = entry->net; row->request = value->request_id; row->scope = entry->scope;
        row->snapshot = (cog_obs_conn_snapshot_t){.conn_id = observer->next_id++, .state = COG_OBS_CONN_CONNECTING,
            .endpoint = row->scope.mock ? "mock" : "openrouter", .provider = row->scope.mock ? "mock" : "openrouter",
            .started_ns = entry->timestamp, .retries = row->scope.attempt};
    } else if (row == NULL || row->terminal || row->sequence == UINT64_MAX || value->sequence != row->sequence + 1U)
        return COG_ERROR_PROTOCOL;
    if (value->bytes_in_total < row->snapshot.bytes_in || value->bytes_out_total < row->snapshot.bytes_out)
        return COG_ERROR_PROTOCOL;
    if (value->kind == COG_NET_OBSERVATION_PROGRESS) {
        if (value->bytes_in_total < row->progress_in || value->bytes_out_total < row->progress_out ||
            value->bytes_in_delta != value->bytes_in_total - row->progress_in ||
            value->bytes_out_delta != value->bytes_out_total - row->progress_out)
            return COG_ERROR_PROTOCOL;
        row->progress_in = value->bytes_in_total; row->progress_out = value->bytes_out_total;
        row->snapshot.last_progress_ns = entry->timestamp;
    } else if (value->bytes_in_delta != 0U || value->bytes_out_delta != 0U) return COG_ERROR_PROTOCOL;
    if (value->kind == COG_NET_OBSERVATION_CLOSED || value->kind == COG_NET_OBSERVATION_FAILED) {
        if (row->progress_in != value->bytes_in_total || row->progress_out != value->bytes_out_total)
            return COG_ERROR_PROTOCOL;
        row->terminal = true;
    }
    static const cog_obs_conn_state_t states[] = {COG_OBS_CONN_CONNECTING, COG_OBS_CONN_ACTIVE,
        COG_OBS_CONN_ACTIVE, COG_OBS_CONN_STALLED, COG_OBS_CONN_CLOSED, COG_OBS_CONN_FAILED};
    row->sequence = value->sequence; row->snapshot.state = states[value->kind];
    row->snapshot.bytes_in = value->bytes_in_total; row->snapshot.bytes_out = value->bytes_out_total;
    row->snapshot.last_status = value->outcome;
    cog_result_t result = cog_obs_diag_publish(observer->target, &row->snapshot);
    if (result != COG_OK) return result;
    row->published = true;
    result = deliver(observer, row, entry, false);
    if (result == COG_OK && value->kind == COG_NET_OBSERVATION_OPEN && row->scope.retry)
        result = deliver(observer, row, entry, true);
    return result;
}
cog_result_t cog_cli_observer_drain(cog_cli_observer_t *observer)
{
    if (observer == NULL) return COG_ERROR_NULL_PTR;
    if (observer->draining) return observer_error(observer, COG_ERROR_INVALID_STATE);
    observer->draining = true;
    for (size_t i = 0U; i < observer->queued && observer->error == COG_OK; i++)
        (void)observer_error(observer, consume(observer, &observer->queue[i]));
    observer->queued = 0U; observer->prepared = false; observer->prepared_net = NULL;
    observer->draining = false;
    return observer->error;
}

static void app_error(cog_cli_app_t *app, cog_result_t result)
{
    if (app->observation_error == COG_OK && result != COG_OK) app->observation_error = result;
    if (app->observer != NULL && app->observation_error != COG_OK)
        (void)observer_error(app->observer, app->observation_error);
}
cog_result_t cog_cli_observation_check(cog_cli_app_t *app, cog_result_t primary)
{
    if (app == NULL) return COG_ERROR_NULL_PTR;
    if (app->observer != NULL) app_error(app, cog_cli_observer_drain(app->observer));
    if (app->opts.backend == COG_CLI_BACKEND_REAL && app->opts.net != NULL)
        app_error(app, cog_net_observer_status(app->opts.net));
    return primary != COG_OK || app->observation_draining ? primary : app->observation_error;
}
static void copy_scope(cog_cli_app_t *app)
{
    const cog_cli_safe_labels_t *labels = cog_cli_configuration_labels(app->configuration);
    app->observation_scope = (cog_cli_observation_scope_t){
        .request = ++app->observation_request_counter, .configuration_generation = app->configuration_generation,
        .attempt = app->retries, .retry = app->observation_retry, .mock = app->opts.backend == COG_CLI_BACKEND_MOCK};
    if (labels != NULL) {
        app->observation_scope.session_available = labels->session_available;
        app->observation_scope.model_available = labels->model_available;
        if (labels->session_available) memcpy(app->observation_scope.session, labels->session, strlen(labels->session) + 1U);
        if (labels->model_available) memcpy(app->observation_scope.model, labels->model, strlen(labels->model) + 1U);
    }
}
cog_result_t cog_cli_observation_provider_begin(cog_cli_app_t *app)
{
    if (app == NULL) return COG_ERROR_NULL_PTR;
    cog_result_t result = cog_cli_observation_check(app, COG_OK);
    if (result != COG_OK) return result;
    if (app->provider_timed || app->observation_request_counter == UINT64_MAX) {
        app_error(app, app->provider_timed ? COG_ERROR_INVALID_STATE : COG_ERROR_LIMIT);
        return app->observation_error;
    }
    copy_scope(app); app->provider_usage_seen = false;
    result = cog_obs_clock_now_ns(&app->provider_started_ns);
    if (result == COG_OK) result = cog_obs_trace_begin(app->obs, "provider.attempt", &app->provider_span);
    app_error(app, result);
    if (result != COG_OK) return result;
    app->provider_timed = true;
    if (app->observer != NULL && app->opts.backend == COG_CLI_BACKEND_REAL) {
        result = cog_cli_observer_prepare(app->observer, app->opts.net, &app->observation_scope);
        app_error(app, result);
        if (result != COG_OK) cog_cli_observation_provider_end(app, result, 0);
    }
    return app->observation_error;
}
void cog_cli_observation_provider_end(cog_cli_app_t *app, cog_result_t outcome, int http_status)
{
    if (app == NULL || !app->provider_timed) return;
    app->provider_timed = false;
    uint64_t now = 0U; cog_result_t clock = cog_obs_clock_now_ns(&now);
    if (clock == COG_OK && now < app->provider_started_ns) clock = COG_ERROR_IO;
    app_error(app, clock);
    app_error(app, cog_obs_trace_end(app->obs, app->provider_span, outcome));
    app->provider_span = 0U;
    if (clock != COG_OK) return;
    cog_obs_field_t fields[20]; size_t count = scope_fields(&app->observation_scope, fields);
    fields[count++] = UINT_FIELD(F_DURATION, now - app->provider_started_ns);
    fields[count++] = STR_FIELD(F_STATUS, cog_cli_result_name(outcome));
    fields[count++] = INT_FIELD(F_HTTP, http_status);
    app_error(app, cog_obs_log(app->obs, COG_OBS_DEBUG, "provider.completed", fields, count));
    count = scope_fields(&app->observation_scope, fields);
    bool tokens = app->provider_usage_seen && app->usage.tokens_known;
    bool cost = app->provider_usage_seen && app->usage.cost_known;
    fields[count++] = BOOL_FIELD(F_INPUT_AVAILABLE, tokens);
    fields[count++] = BOOL_FIELD(F_OUTPUT_AVAILABLE, tokens);
    fields[count++] = BOOL_FIELD(F_COST_AVAILABLE, cost);
    if (tokens) {
        fields[count++] = UINT_FIELD(F_INPUT, app->usage.input_tokens);
        fields[count++] = UINT_FIELD(F_OUTPUT, app->usage.output_tokens);
    }
    if (cost) fields[count++] = (cog_obs_field_t){.index=F_COST, .kind=COG_OBS_FIELD_DOUBLE, .value.real=app->usage.cost};
    app_error(app, cog_obs_log(app->obs, COG_OBS_DEBUG, "provider.usage", fields, count));
}
static void tool_identity(cog_cli_app_t *app)
{
    uint64_t node = app->assistant == NULL ? 0U : cog_node_data(app->assistant)->node_id;
    size_t ordinal = app->tool_index + 1U;
    if (app->tool_assistant_id != node || app->tool_ordinal != ordinal) {
        app->tool_assistant_id = node; app->tool_ordinal = ordinal;
        app->tool_observed = false; app->tool_execution_started = false;
    }
    app->observed_tool = "unknown";
    if (app->tool_index < app->call_count) for (size_t i = 0U; i < cog_workspace_tools.count; i++)
        if (strcmp(app->call_views[app->tool_index].name, cog_workspace_tools.tools[i].name) == 0)
            app->observed_tool = cog_workspace_tools.tools[i].name;
}
cog_result_t cog_cli_observation_tool_begin(cog_cli_app_t *app)
{
    if (app == NULL) return COG_ERROR_NULL_PTR;
    cog_result_t result = cog_cli_observation_check(app, COG_OK);
    if (result != COG_OK) return result;
    tool_identity(app);
    if (app->observation_scope.configuration_generation == 0U) {
        if (app->observation_request_counter == UINT64_MAX) { app_error(app, COG_ERROR_LIMIT); return app->observation_error; }
        copy_scope(app);
    }
    if (app->tool_timed || app->tool_observed) { app_error(app, COG_ERROR_INVALID_STATE); return app->observation_error; }
    result = cog_obs_clock_now_ns(&app->tool_started_ns);
    if (result == COG_OK) result = cog_obs_trace_begin(app->obs, "tool.attempt", &app->tool_span);
    app_error(app, result);
    if (result == COG_OK) app->tool_timed = true;
    return result;
}
void cog_cli_observation_tool_end(cog_cli_app_t *app, cog_result_t outcome, bool effects, bool truncated)
{
    if (app == NULL) return;
    tool_identity(app);
    if (app->tool_observed) return;
    if (app->observation_scope.configuration_generation == 0U) {
        if (app->observation_request_counter == UINT64_MAX) { app_error(app, COG_ERROR_LIMIT); return; }
        copy_scope(app);
    }
    app->tool_observed = true;
    uint64_t now = 0U; bool duration = app->tool_timed;
    if (duration) {
        cog_result_t result = cog_obs_clock_now_ns(&now);
        if (result == COG_OK && now < app->tool_started_ns) result = COG_ERROR_IO;
        app_error(app, result); duration = result == COG_OK;
        app->tool_timed = false;
        app_error(app, cog_obs_trace_end(app->obs, app->tool_span, outcome)); app->tool_span = 0U;
    }
    cog_obs_field_t fields[20]; size_t count = scope_fields(&app->observation_scope, fields);
    fields[count++] = UINT_FIELD(F_NODE, app->tool_assistant_id);
    fields[count++] = UINT_FIELD(F_ORDINAL, app->tool_ordinal);
    fields[count++] = STR_FIELD(F_TOOL, app->observed_tool);
    fields[count++] = BOOL_FIELD(F_EXECUTED, app->tool_execution_started);
    fields[count++] = STR_FIELD(F_STATUS, cog_cli_result_name(outcome));
    fields[count++] = BOOL_FIELD(F_EFFECTS, effects);
    fields[count++] = BOOL_FIELD(F_TRUNCATED, truncated);
    if (duration && app->tool_execution_started) fields[count++] = UINT_FIELD(F_DURATION, now - app->tool_started_ns);
    app_error(app, cog_obs_log(app->obs, COG_OBS_DEBUG, "tool.completed", fields, count));
}
cog_result_t cog_cli_observation_detach(cog_cli_app_t *app, cog_result_t primary)
{
    if (app == NULL) return COG_ERROR_NULL_PTR;
    if (app->observer != NULL) {
        if (app->opts.backend == COG_CLI_BACKEND_REAL && app->opts.net != NULL)
            app_error(app, cog_cli_observer_clear(app->observer, app->opts.net));
        app_error(app, cog_cli_observer_bind(app->observer, NULL));
        app->observer = NULL;
    }
    return cog_cli_observation_check(app, primary);
}
cog_result_t cog_cli_observer_clear(cog_cli_observer_t *observer, const cog_net_ctx_t *net)
{
    if (observer == NULL || net == NULL) return COG_ERROR_NULL_PTR;
    (void)cog_cli_observer_drain(observer);
    if (cog_net_active_count(net) != 0U) return COG_ERROR_INVALID_STATE;
    for (size_t i = 0U; i < observer->capacity; i++)
        if (observer->rows[i].used && observer->rows[i].net == net)
            (void)observer_error(observer, clear_row(observer, &observer->rows[i]));
    return observer->error;
}

/* Standalone C11 embedder: installed public headers and exported targets only. */
#include <cog_cli/cli.h>
#include <cog/orchestrator.h>
#include <cog/config.h>
#include <cog/toolkit.h>
#include <cog/provider.h>
#include <cog/session_store.h>
#include <cog/obs.h>
#include <cog/ipc.h>
#include <curl/curl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "embedder:%d: %s\n", __LINE__, #c); exit(1); } } while (0)

/* Test-only audit of explicit process-level public dependency calls. */
static size_t host_initializations, host_cleanups;
CURLcode __real_curl_global_init(long flags);
void __real_curl_global_cleanup(void);
CURLcode __wrap_curl_global_init(long flags);
void __wrap_curl_global_cleanup(void);
CURLcode __wrap_curl_global_init(long flags)
{ ++host_initializations; return __real_curl_global_init(flags); }
void __wrap_curl_global_cleanup(void)
{ ++host_cleanups; __real_curl_global_cleanup(); }

/* A deliberately text-only external adapter. No private Cog layout, registry,
 * globals or dynamic callback registration is required. */
typedef struct external_provider {
    cog_provider_t base;
    const cog_endpoint_t *endpoint;
    bool live, active, text_pending, clock_set, retained;
    uint64_t now, sequence;
    size_t connects, sends, preflights;
    cog_provider_request_t accepted;
    cog_message_t accepted_message;
    char model[32], text[64];
    cog_provider_error_t error;
} external_provider_t;
_Static_assert(offsetof(external_provider_t, base) == 0U, "public first-member base");
typedef struct external_provider_config { external_provider_t *instance; } external_provider_config_t;
static const cog_provider_vtable_t provider_ops;
static cog_result_t external_connect(const cog_endpoint_t *endpoint, cog_provider_t **out)
{
    const external_provider_config_t *binding = endpoint->implementation_config;
    external_provider_t *p = binding != NULL ? binding->instance : NULL;
    if (p == NULL || p->live) return COG_ERROR_INVALID_STATE;
    ++p->connects; p->base.vtable = &provider_ops; p->endpoint = endpoint; p->live = true;
    *out = &p->base; return COG_OK;
}
static cog_result_t external_disconnect(cog_provider_t *base)
{ external_provider_t *p = (external_provider_t *)base; p->live = false; p->active = false; return COG_OK; }
static cog_result_t external_preflight(cog_provider_t *base, const cog_provider_request_t *r,
                                       cog_provider_preflight_stage_t stage)
{
    external_provider_t *p = (external_provider_t *)base; ++p->preflights;
    if (p->active) return COG_ERROR_INVALID_STATE;
    if (strcmp(r->model_id, "external") != 0) return COG_ERROR_NOT_FOUND;
    if (r->configuration_generation != p->endpoint->generation) return COG_ERROR_INVALID_ARG;
    if (stage == COG_PROVIDER_PREFLIGHT_PENDING_TOOLS || r->tools_json != NULL || r->message_count != 1U)
        return COG_ERROR_UNSUPPORTED;
    const cog_message_t *m = &r->messages[0];
    if (m->role != COG_MESSAGE_ROLE_USER || m->name != NULL || m->opaque_count != 0U ||
        m->tool_call_count != 0U || m->usage.currency != NULL) return COG_ERROR_UNSUPPORTED;
    return strlen(r->model_id) >= sizeof(p->model) || m->content_len >= sizeof(p->text) ? COG_ERROR_LIMIT : COG_OK;
}
static cog_result_t external_send(cog_provider_t *base, const cog_provider_request_t *r)
{
    external_provider_t *p = (external_provider_t *)base;
    cog_result_t rc = external_preflight(base, r, COG_PROVIDER_PREFLIGHT_COMPLETE);
    if (rc != COG_OK) return rc;
    strcpy(p->model, r->model_id);
    if (r->messages[0].content_len != 0U) memcpy(p->text, r->messages[0].content, r->messages[0].content_len);
    p->text[r->messages[0].content_len] = '\0';
    p->accepted = *r; p->accepted.model_id = p->model;
    p->accepted_message = r->messages[0]; p->accepted_message.content = p->text;
    p->accepted_message.tool_calls = NULL; p->accepted_message.opaque = NULL;
    p->accepted.messages = &p->accepted_message;
    ++p->sends; p->active = p->text_pending = true; p->sequence = 0U; p->retained = false;
    p->error = (cog_provider_error_t){0}; return COG_OK;
}
static cog_result_t external_poll(cog_provider_t *base, uint64_t now, uint64_t *deadline)
{
    external_provider_t *p = (external_provider_t *)base;
    if (p->clock_set && now < p->now) return COG_ERROR_INVALID_ARG;
    p->clock_set = true; p->now = now; *deadline = UINT64_MAX; return COG_OK;
}
static cog_result_t external_cancel(cog_provider_t *base)
{
    external_provider_t *p = (external_provider_t *)base;
    if (p->active) {
        if (!p->retained) p->text_pending = false;
        p->error = (cog_provider_error_t){.code = COG_ERROR_ABORTED,
        .message = "external cancelled", .kind = COG_PROVIDER_ERROR_CANCELLED,
        .output_accepted = p->sequence != 0U || p->retained,
        .replay_safe = p->sequence == 0U && !p->retained};
    }
    return COG_OK;
}
static cog_result_t external_next(cog_provider_t *base, cog_provider_delta_t *out,
                                  void *storage, size_t capacity, size_t *required)
{
    external_provider_t *p = (external_provider_t *)base;
    if (!p->active) return COG_ERROR_INVALID_STATE;
    if (p->text_pending) {
        p->retained = true; *required = sizeof("embedded");
        if (capacity < *required) return COG_ERROR_OVERFLOW;
        memcpy(storage, "embedded", *required);
        *out = (cog_provider_delta_t){.kind = COG_PROVIDER_DELTA_TEXT,
            .sequence = ++p->sequence, .data = storage, .data_len = 8U};
        p->text_pending = p->retained = false; return COG_OK;
    }
    *required = 0U;
    *out = (cog_provider_delta_t){.kind = COG_PROVIDER_DELTA_FINISH, .sequence = ++p->sequence,
        .outcome = p->error.code, .finish_reason = p->error.code == COG_OK ? COG_FINISH_REASON_STOP : COG_FINISH_REASON_CANCELLED};
    p->active = false; return COG_OK;
}
static cog_result_t external_receive(cog_provider_t *base, cog_provider_response_t *out)
{
    external_provider_t *p = (external_provider_t *)base;
    if (!p->active) return COG_ERROR_INVALID_STATE;
    if (p->error.code != COG_OK) {
        out->content_len = 0U; out->is_complete = true;
        out->finish_reason = COG_FINISH_REASON_CANCELLED; p->active = false; return p->error.code;
    }
    out->content_len = 8U;
    if (out->content_capacity < sizeof("embedded")) return COG_ERROR_OVERFLOW;
    memcpy(out->content, "embedded", sizeof("embedded")); out->is_complete = true;
    out->finish_reason = COG_FINISH_REASON_STOP; p->active = false; return COG_OK;
}
static cog_result_t external_error(const cog_provider_t *base, cog_provider_error_t *out)
{
    const external_provider_t *p = (const external_provider_t *)base;
    if (p->error.code == COG_OK) return COG_ERROR_NOT_FOUND;
    *out = p->error; return COG_OK;
}
static const cog_provider_vtable_t provider_ops = {external_connect, external_disconnect,
    external_send, external_receive, external_poll, external_cancel, external_next, external_error, external_preflight};
static void provider_fixture(void)
{
    external_provider_t context = {0};
    const external_provider_config_t binding = {&context};
    const cog_endpoint_t endpoint = {.name = "external", .vtable = &provider_ops,
        .implementation_config = &binding, .generation = 17U};
    static const cog_provider_vtable_t missing = {external_connect, external_disconnect,
        external_send, external_receive, external_poll, external_cancel, external_next, external_error, NULL};
    cog_endpoint_t bad = endpoint; bad.vtable = &missing; cog_provider_t *p = NULL;
    CHECK(cog_provider_connect(&bad, &p) == COG_ERROR_INVALID_ARG && p == NULL && context.connects == 0U);
    CHECK(cog_provider_connect(&endpoint, &p) == COG_OK && p == &context.base);
    char model[] = "external", text[] = "copied prompt";
    cog_message_t message = {.role = COG_MESSAGE_ROLE_USER, .content = text, .content_len = strlen(text)};
    cog_provider_request_t request = {.model_id = model, .messages = &message, .message_count = 1U,
        .stream = true, .configuration_generation = 17U};
    cog_message_tool_call_t pending_call = {"pending", "tool", "{}", 2U};
    cog_message_t pending = {.role = COG_MESSAGE_ROLE_ASSISTANT, .tool_calls = &pending_call, .tool_call_count = 1U};
    cog_provider_request_t staged = request; staged.messages = &pending;
    CHECK(cog_provider_preflight(p, &staged, COG_PROVIDER_PREFLIGHT_PENDING_TOOLS) == COG_ERROR_UNSUPPORTED);
    CHECK(cog_provider_send(p, &staged) == COG_ERROR_INVALID_STATE && context.sends == 0U);
    CHECK(cog_provider_preflight(p, &request, COG_PROVIDER_PREFLIGHT_COMPLETE) == COG_OK && context.sends == 0U);
    CHECK(cog_provider_preflight(p, &request, COG_PROVIDER_PREFLIGHT_PENDING_TOOLS) == COG_ERROR_INVALID_STATE);
    CHECK(cog_provider_send(p, &request) == COG_OK);
    memset(model, 'x', strlen(model)); memset(text, 'x', strlen(text));
    CHECK(strcmp(context.accepted.model_id, "external") == 0 && strcmp(context.accepted.messages[0].content, "copied prompt") == 0);
    cog_provider_delta_t delta, before; memset(&delta, 0x5a, sizeof(delta)); before = delta;
    size_t required; char storage[32];
    CHECK(cog_provider_next(p, &delta, storage, 1U, &required) == COG_ERROR_OVERFLOW && required == sizeof("embedded"));
    CHECK(memcmp(&delta, &before, sizeof(delta)) == 0);
    CHECK(cog_provider_next(p, &delta, storage, sizeof(storage), &required) == COG_OK && delta.sequence == 1U && strcmp(delta.data, "embedded") == 0);
    uint64_t deadline; CHECK(cog_provider_poll(p, 100U, &deadline) == COG_OK && deadline == UINT64_MAX);
    CHECK(cog_provider_cancel(p) == COG_OK);
    CHECK(cog_provider_next(p, &delta, storage, sizeof(storage), &required) == COG_OK && delta.kind == COG_PROVIDER_DELTA_FINISH && delta.outcome == COG_ERROR_ABORTED);
    cog_provider_error_t error; CHECK(cog_provider_get_error(p, &error) == COG_OK && error.output_accepted);
    request.model_id = "external";
    CHECK(cog_provider_preflight(p, &request, COG_PROVIDER_PREFLIGHT_COMPLETE) == COG_OK);
    CHECK(cog_provider_get_error(p, &error) == COG_OK && error.code == COG_ERROR_ABORTED);
    CHECK(cog_provider_send(p, &request) == COG_OK);
    cog_provider_response_t response = {.content = storage, .content_capacity = sizeof(storage)};
    CHECK(cog_provider_receive(p, &response) == COG_OK && response.is_complete && strcmp(storage, "embedded") == 0);
    CHECK(cog_provider_disconnect(p) == COG_OK && !context.live && context.connects == 1U && context.sends == 2U);
}

/* Custom backend state belongs to the embedder, never to Cog's wrapper. */
typedef struct external_store {
    struct external_store *identity;
    const char *id;
    cog_session_t *saved;
    bool live, wrapper_released;
    size_t callbacks, destroyed_at;
    size_t *destruction_clock;
} external_store_t;
static external_store_t *store_context(void *value)
{
    external_store_t *ctx = value;
    CHECK(ctx != NULL && ctx->identity == ctx && ctx->live && !ctx->wrapper_released);
    ++ctx->callbacks; return ctx;
}
static cog_result_t clone_session(const cog_session_t *source, cog_session_t **out)
{
    cog_session_snapshot_t snapshot;
    cog_result_t rc = cog_session_snapshot_view(source, &snapshot);
    if (rc != COG_OK) return rc;
    cog_session_config_t limits = {.id = snapshot.session_id, .max_nodes = 8U,
        .max_payload_bytes = 4096U, .max_tool_calls_per_message = 2U};
    return cog_session_restore(out, &limits, &snapshot);
}
static cog_result_t store_save(void *value, const cog_session_t *session)
{
    external_store_t *ctx = store_context(value); cog_session_t *copy = NULL;
    CHECK(strcmp(ctx->id, cog_session_id(session)) == 0);
    cog_result_t rc = clone_session(session, &copy);
    if (rc == COG_OK) { cog_session_free(ctx->saved); ctx->saved = copy; }
    return rc;
}
static cog_result_t store_load(void *value, const char *id, cog_session_t **out)
{
    external_store_t *ctx = store_context(value);
    if (strcmp(ctx->id, id) != 0 || ctx->saved == NULL) { *out = NULL; return COG_ERROR_NOT_FOUND; }
    return clone_session(ctx->saved, out);
}
static cog_result_t store_append(void *value, const cog_session_t *session, const cog_session_node_t *node)
{ CHECK(node != NULL); return store_save(value, session); }
static cog_result_t store_delete(void *value, const char *id)
{
    external_store_t *ctx = store_context(value);
    if (strcmp(ctx->id, id) != 0 || ctx->saved == NULL) return COG_ERROR_NOT_FOUND;
    cog_session_free(ctx->saved); ctx->saved = NULL; return COG_OK;
}
static cog_result_t store_list(void *value, char ***out, size_t *count)
{
    external_store_t *ctx = store_context(value); *out = NULL; *count = 0U;
    if (ctx->saved == NULL) return COG_OK;
    char **ids = malloc(sizeof(*ids)); if (ids == NULL) return COG_ERROR_NOMEM;
    ids[0] = malloc(strlen(ctx->id) + 1U);
    if (ids[0] == NULL) { free(ids); return COG_ERROR_NOMEM; }
    strcpy(ids[0], ctx->id); *out = ids; *count = 1U; return COG_OK;
}
static bool store_exists(void *value, const char *id)
{ external_store_t *ctx = store_context(value); return ctx->saved != NULL && strcmp(ctx->id, id) == 0; }
static cog_result_t store_load_ex(void *value, const char *id, cog_store_load_mode_t mode,
                                 cog_session_t **out, cog_store_load_report_t *report)
{
    (void)mode;
    cog_result_t rc = store_load(value, id, out);
    if (rc == COG_OK && report != NULL) *report = (cog_store_load_report_t){.source_version = COG_STORE_FORMAT_V2, .replay_safe = true};
    return rc;
}
static const cog_store_vtable_t store_ops = {.name = "external", .save = store_save,
    .load = store_load, .append = store_append, .delete_ = store_delete,
    .list = store_list, .exists = store_exists, .load_ex = store_load_ex};
static void store_destroy(external_store_t *ctx)
{
    CHECK(ctx->live && ctx->wrapper_released);
    cog_session_free(ctx->saved); ctx->saved = NULL; ctx->live = false;
    ctx->destroyed_at = ++*ctx->destruction_clock;
}
static void save_text(cog_store_t *store, const char *id, const char *text)
{
    cog_session_t *session = NULL; cog_session_node_t *node = NULL;
    const cog_session_config_t config = {.id = id, .max_nodes = 8U,
        .max_payload_bytes = 4096U, .max_tool_calls_per_message = 2U};
    CHECK(cog_session_new(&session, &config) == COG_OK);
    cog_message_t message = {.role = COG_MESSAGE_ROLE_USER, .content = text, .content_len = strlen(text)};
    CHECK(cog_session_append(session, &message, &node) == COG_OK);
    CHECK(cog_store_save(store, session) == COG_OK);
    CHECK(cog_store_append(store, session, node) == COG_OK);
    cog_session_free(session); /* Backend restore must not borrow these bytes. */
}
static void stores_fixture(void)
{
    size_t clock = 0U;
    external_store_t a = {.id = "store-a", .live = true, .destruction_clock = &clock}; a.identity = &a;
    external_store_t b = {.id = "store-b", .live = true, .destruction_clock = &clock}; b.identity = &b;
    cog_store_t *first = NULL, *second = NULL;
    cog_store_config_t config = {.backend = &store_ops, .backend_context = &a};
    CHECK(cog_store_new(&first, &config) == COG_OK);
    config.backend_context = &b; CHECK(cog_store_new(&second, &config) == COG_OK);
    save_text(first, a.id, "alpha"); save_text(second, b.id, "beta");
    CHECK(cog_store_exists(first, a.id) && !cog_store_exists(first, b.id));
    cog_session_t *loaded = NULL; cog_store_load_report_t report;
    CHECK(cog_store_load_ex(second, b.id, COG_STORE_LOAD_REPLAY, &loaded, &report) == COG_OK);
    CHECK(report.source_version == COG_STORE_FORMAT_V2 && report.replay_safe);
    cog_message_t history[8]; size_t count = 0U;
    CHECK(cog_session_copy_history(loaded, NULL, history, 8U, &count) == COG_OK && count == 1U);
    CHECK(history[0].content_len == 4U && memcmp(history[0].content, "beta", 4U) == 0);
    cog_session_free(loaded);
    char **ids = NULL; CHECK(cog_store_list(second, &ids, &count) == COG_OK && count == 1U);
    CHECK(strcmp(ids[0], b.id) == 0); free(ids[0]); free(ids);
    cog_store_free(second); CHECK(b.live && b.callbacks != 0U); b.wrapper_released = true; store_destroy(&b);
    CHECK(cog_store_load(first, a.id, &loaded) == COG_OK && strcmp(cog_session_id(loaded), a.id) == 0);
    cog_session_free(loaded);
    CHECK(cog_store_delete(first, a.id) == COG_OK && !cog_store_exists(first, a.id));
    cog_store_free(first); CHECK(a.live && a.callbacks != 0U); a.wrapper_released = true; store_destroy(&a);
    CHECK(b.destroyed_at == 1U && a.destroyed_at == 2U);
}
static void network_fixture(void)
{
    cog_net_ctx_t *a = NULL, *b = NULL;
    cog_net_config_t config = {.host_runtime_initialized = true,
        .dependency_memory = COG_NET_DEPENDENCY_MEMORY_DYNAMIC, .max_connections = 1U};
    CHECK(cog_net_new(&a, &config) == COG_OK && cog_net_new(&b, &config) == COG_OK);
    cog_request_id_t x = 0U, y = 0U;
    CHECK(cog_net_request(a, "POST", "https://never-serviced.invalid/", NULL, "body", 4U, &x) == COG_OK);
    CHECK(cog_net_request(b, "GET", "https://never-serviced.invalid/", NULL, NULL, 0U, &y) == COG_OK);
    CHECK(x == 1U && y == 1U && cog_net_active_count(a) == 1U && cog_net_active_count(b) == 1U);
    CHECK(cog_net_cancel(b, y) == COG_OK); cog_net_response_t terminal;
    CHECK(cog_net_peek_request(b, y, &terminal) == COG_OK && terminal.is_complete && terminal.outcome == COG_ERROR_ABORTED);
    CHECK(cog_net_ack(b, y, terminal.sequence) == COG_OK && cog_net_request_release(b, y) == COG_OK);
    cog_net_free(b); CHECK(host_cleanups == 0U && cog_net_active_count(a) == 1U);
    bool active = false; CHECK(cog_net_request_status(a, x, &active) == COG_OK && active);
    CHECK(cog_net_cancel(a, x) == COG_OK);
    CHECK(cog_net_peek_request(a, x, &terminal) == COG_OK && terminal.outcome == COG_ERROR_ABORTED);
    CHECK(cog_net_ack(a, x, terminal.sequence) == COG_OK && cog_net_request_release(a, x) == COG_OK);
    cog_net_free(a); CHECK(host_initializations == 1U && host_cleanups == 0U);
}
static void module_and_cli_fixture(const char *config_path)
{
    cog_config_tree_t *tree = NULL; cog_config_txn_t *transaction = NULL;
    CHECK(cog_config_tree_new(&tree) == COG_OK && cog_config_begin(tree, &transaction) == COG_OK);
    CHECK(cog_config_set(transaction, "embedding.value", "ok") == COG_OK);
    CHECK(cog_config_commit(transaction) == COG_OK); cog_config_tree_free(tree);
    cog_ipc_message_t message; CHECK(cog_ipc_message_new(&message, 32U) == COG_OK);
    CHECK(cog_ipc_message_free(&message) == COG_OK);
    const cog_obs_config_t observation = {.service = "installed-embedder"};
    cog_obs_t *obs = NULL; CHECK(cog_obs_new(&obs, &observation) == COG_OK);
    size_t count; CHECK(cog_obs_diag_count(obs, &count) == COG_OK && count == 0U); cog_obs_free(obs);
    cog_toolkit_call_t *call = NULL; CHECK(cog_toolkit_call_new(&call, "external", "{}", 2U) == COG_OK); cog_toolkit_call_free(call);
    cog_orchestrator_t *orchestrator = NULL;
    CHECK(cog_orchestrator_new(&orchestrator, NULL) == COG_ERROR_NULL_PTR); cog_orchestrator_free(NULL);
    /* O's new async/handoff surface is pending; this is symbol/link closure. */
    cog_cli_options_t options = {.backend = COG_CLI_BACKEND_MOCK, .explicit_options = COG_CLI_OPT_BACKEND,
        .prompt = "embedding", .config_path = config_path, .chunks = "installed"};
    cog_cli_app_t *app = NULL; CHECK(cog_cli_app_new(&app, &options) == COG_OK);
    CHECK(cog_cli_app_run(app) == COG_OK); cog_cli_app_free(app);
}
int main(int argc, char **argv)
{
    CHECK(argc == 2 && curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK);
    const curl_version_info_data *runtime = curl_version_info(CURLVERSION_NOW);
    CHECK(runtime != NULL && runtime->ares != NULL && (runtime->features & CURL_VERSION_ASYNCHDNS) != 0);
    printf("curl=%s c-ares=%s\n", runtime->version, runtime->ares);
    provider_fixture(); stores_fixture(); network_fixture(); module_and_cli_fixture(argv[1]);
    CHECK(host_initializations == 1U && host_cleanups == 0U);
    curl_global_cleanup(); CHECK(host_cleanups == 1U);
    puts("installed embedder lifecycle passed"); return 0;
}

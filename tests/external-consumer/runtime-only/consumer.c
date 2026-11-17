/* Installed runtime-only consumer: no CLI or private Cog headers. */
#include <cog/runtime.h>
#include <cog/provider_vtable.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "runtime:%d: %s\n", __LINE__, #c); exit(1); } } while (0)
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
    if (stage == COG_PROVIDER_PREFLIGHT_PENDING_TOOLS || r->tools_json != NULL || r->message_count > 1U)
        return COG_ERROR_UNSUPPORTED;
    if (r->message_count == 0U) return COG_OK;
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

int main(void) {
    external_provider_t state = {0};
    const external_provider_config_t binding = {&state};
    static const cog_model_t model = {.id="external", .name="external", .capabilities=COG_MODEL_CAPABILITY_STREAMING};
    static const cog_model_table_t models = {&model, 1U};
    const cog_endpoint_t endpoint = {.name="external", .vtable=&provider_ops,
        .models=&models, .implementation_config=&binding, .generation=17U};
    cog_runtime_config_t c = {0};
    c.mode=COG_RUNTIME_NEW; c.instruction_mode=COG_RUNTIME_INSTRUCTIONS_DISABLED;
    c.session_id="installed-runtime"; c.store.backend=&cog_store_null_backend;
    c.session.max_nodes=32U; c.session.max_payload_bytes=262144U;
    c.session.max_tool_calls_per_message=4U;
    c.provider.endpoint=&endpoint; c.provider.model_id="external";
    c.limits=(cog_runtime_limits_t){.max_input_bytes=4096U,.max_queued_inputs=4U,
        .max_view_bytes=1048576U,.max_context_bytes=65536U,.max_tool_output_bytes=4096U,
        .max_instruction_bytes=65536U,.max_events=64U,.max_dispatch_per_step=1U,
        .max_output_tokens=128U,.max_tool_rounds=4U,.max_turn_ms=10000U,
        .max_tool_ms=1000U,.max_retry_elapsed_ms=1000U};
    cog_runtime_t *r=NULL;
    CHECK(cog_runtime_new(&r,&c)==COG_OK);
    CHECK(cog_runtime_begin(r)==COG_OK);
    bool submitted=false, seen=false, stopping=false, done=false;
    for(size_t i=0U;i<10000U && !done;i++) {
        cog_runtime_view_t view;
        cog_result_t rc=cog_runtime_view(r,&view);
        if(rc==COG_OK) {
            for(size_t j=0U;j<view.item_count;j++)
                if(view.items[j].role==COG_MESSAGE_ROLE_ASSISTANT && view.items[j].text_bytes==8U &&
                    memcmp(view.items[j].text,"embedded",8U)==0) seen=true;
            CHECK(cog_runtime_ack(r,view.serial)==COG_OK);
            if(!submitted) { CHECK(cog_runtime_submit(r,"hello",5U,false)==COG_OK); submitted=true; }
        } else CHECK(rc==COG_ERROR_AGAIN);
        if(seen && !stopping) { CHECK(cog_runtime_shutdown(r)==COG_OK); stopping=true; }
        cog_runtime_step_t step;
        rc=cog_runtime_step(r,&step);
        CHECK(rc==COG_OK || rc==COG_ERROR_AGAIN); done=step.done;
    }
    CHECK(done && submitted && seen && state.sends==1U);
    CHECK(cog_runtime_close(&r,COG_OK)==COG_OK && r==NULL && !state.live);
    return 0;
}

/** @file mock_provider.c @brief Retained nonblocking deterministic provider. */
#include "cog_cli/mock_provider.h"
#include <stdalign.h>
#include <stdlib.h>
#include <string.h>
#define MOCK_REQUEST_BYTES 131072U
static const cog_mock_step_t default_steps[] = {
    {COG_MOCK_STEP_CHUNK, "mock response", COG_OK, 0, false, 0U, NULL}};
typedef struct
{
    cog_provider_t base;
    cog_mock_script_t script;
    const cog_provider_delta_t *deltas;
    size_t delta_count, step_index, passes_left;
    size_t send_count, receive_count, chunk_count, error_count, cancel_count;
    bool sent, terminal_pending, cancel_requested, used_next, used_receive,
        clock_seen, retained, cancel_after;
    uint64_t sequence, now, ready_at;
    cog_provider_request_t request;
    cog_provider_error_t error;
    char *request_arena, *preflight_arena;
    size_t used;
} mock;
static void *reserve(mock *p, size_t n)
{
    size_t alignment = alignof(max_align_t);
    size_t padding = (alignment - p->used % alignment) % alignment;
    if (padding > MOCK_REQUEST_BYTES - p->used ||
        n > MOCK_REQUEST_BYTES - p->used - padding)
        return NULL;
    void *out = p->request_arena + p->used + padding;
    p->used += padding + n;
    return out;
}
static void *copy(mock *p, const void *data, size_t n, bool nul)
{
    if (n == SIZE_MAX)
        return NULL;
    char *out = reserve(p, n + (nul ? 1U : 0U));
    if (out == NULL)
        return NULL;
    if (n != 0U)
        memcpy(out, data, n);
    if (nul)
        out[n] = '\0';
    return out;
}
static const char *copy_str(mock *p, const char *s)
{
    return s == NULL ? NULL : copy(p, s, strlen(s), true);
}
static cog_result_t copy_request(mock *p, const cog_provider_request_t *r)
{
    p->used = 0U;
    cog_provider_request_t out = *r;
    out.model_id = copy_str(p, r->model_id);
    if (out.model_id == NULL)
        return COG_ERROR_LIMIT;
    if (r->tools_json != NULL)
    {
        out.tools_json = copy(p, r->tools_json, r->tools_json_len, true);
        if (out.tools_json == NULL)
            return COG_ERROR_LIMIT;
    }
    if (r->message_count > MOCK_REQUEST_BYTES / sizeof(cog_message_t))
        return COG_ERROR_LIMIT;
    cog_message_t *messages = reserve(p, r->message_count * sizeof(*messages));
    if (messages == NULL)
        return COG_ERROR_LIMIT;
    out.messages = messages;
    for (size_t i = 0; i < r->message_count; ++i)
    {
        const cog_message_t *m = &r->messages[i];
        messages[i] = *m;
        cog_message_t *v = &messages[i];
        if (m->content != NULL)
        {
            v->content = copy(p, m->content, m->content_len, true);
            if (v->content == NULL)
                return COG_ERROR_LIMIT;
        }
        if (m->name != NULL)
        {
            v->name = copy_str(p, m->name);
            if (v->name == NULL)
                return COG_ERROR_LIMIT;
        }
        if (m->tool_call_id != NULL)
        {
            v->tool_call_id = copy_str(p, m->tool_call_id);
            if (v->tool_call_id == NULL)
                return COG_ERROR_LIMIT;
        }
        if (m->usage.currency != NULL)
        {
            v->usage.currency = copy_str(p, m->usage.currency);
            if (v->usage.currency == NULL)
                return COG_ERROR_LIMIT;
        }
        if (m->tool_call_count >
            MOCK_REQUEST_BYTES / sizeof(cog_message_tool_call_t))
            return COG_ERROR_LIMIT;
        cog_message_tool_call_t *calls =
            reserve(p, m->tool_call_count * sizeof(*calls));
        if (calls == NULL)
            return COG_ERROR_LIMIT;
        v->tool_calls = calls;
        for (size_t j = 0; j < m->tool_call_count; ++j)
        {
            calls[j] = m->tool_calls[j];
            calls[j].id = copy_str(p, calls[j].id);
            calls[j].name = copy_str(p, calls[j].name);
            calls[j].arguments =
                copy(p, calls[j].arguments, calls[j].arguments_len, true);
            if (calls[j].id == NULL || calls[j].name == NULL ||
                calls[j].arguments == NULL)
                return COG_ERROR_LIMIT;
        }
        if (m->opaque_count > MOCK_REQUEST_BYTES / sizeof(cog_message_opaque_t))
            return COG_ERROR_LIMIT;
        cog_message_opaque_t *opaque =
            reserve(p, m->opaque_count * sizeof(*opaque));
        if (opaque == NULL)
            return COG_ERROR_LIMIT;
        v->opaque = opaque;
        for (size_t j = 0; j < m->opaque_count; ++j)
        {
            opaque[j] = m->opaque[j];
            opaque[j].namespace_id = copy_str(p, opaque[j].namespace_id);
            opaque[j].format = copy_str(p, opaque[j].format);
            opaque[j].data = copy(p, opaque[j].data, opaque[j].size, false);
            if (opaque[j].namespace_id == NULL || opaque[j].format == NULL ||
                opaque[j].data == NULL)
                return COG_ERROR_LIMIT;
        }
    }
    p->request = out;
    return COG_OK;
}
static cog_result_t connect_mock(const cog_endpoint_t *e, cog_provider_t **out)
{
    if (e == NULL || out == NULL)
        return COG_ERROR_NULL_PTR;
    return cog_mock_provider_new(e->implementation_config, out);
}
static cog_result_t disconnect_mock(cog_provider_t *base)
{
    mock *p = (mock *)base;
    if (p != NULL)
    {
        free(p->request_arena);
        free(p->preflight_arena);
        free(p);
    }
    return COG_OK;
}
static cog_result_t preflight_mock(cog_provider_t *base,
    const cog_provider_request_t *r, cog_provider_preflight_stage_t stage)
{
    mock *p = (mock *)base;
    (void)stage; /* Generic dispatch validates stage/history before callback. */
    if (p->sent)
        return COG_ERROR_INVALID_STATE;
    /* Copy through the identical bounded representation into isolated scratch.
     * Never replace last_request, alter cursors, or consult future send_error. */
    mock scratch = *p;
    scratch.request_arena = p->preflight_arena;
    return copy_request(&scratch, r);
}
static cog_result_t send_mock(cog_provider_t *base,
                              const cog_provider_request_t *r)
{
    mock *p = (mock *)base;
    if (p == NULL || r == NULL)
        return COG_ERROR_NULL_PTR;
    if (p->sent)
        return COG_ERROR_INVALID_STATE;
    cog_result_t rc = cog_provider_request_validate(r);
    if (rc != COG_OK)
        return rc;
    if (p->script.send_error != COG_OK)
        return p->script.send_error;
    rc = copy_request(p, r);
    if (rc != COG_OK)
        return rc;
    ++p->send_count;
    p->sent = true;
    p->terminal_pending = false;
    p->cancel_requested = false;
    p->sequence = 0U;
    p->used_next = false;
    p->used_receive = false;
    p->retained = false;
    p->cancel_after = false;
    p->error = (cog_provider_error_t){0};
    if (p->deltas == NULL && p->step_index >= p->script.step_count &&
        p->passes_left > 0U)
    {
        p->step_index = 0U;
        if (p->passes_left != SIZE_MAX)
            --p->passes_left;
    }
    p->ready_at = p->now > UINT64_MAX - p->script.chunk_delay_ms
                      ? UINT64_MAX
                      : p->now + p->script.chunk_delay_ms;
    return COG_OK;
}
static cog_result_t poll_mock(cog_provider_t *base, uint64_t now,
                              uint64_t *deadline)
{
    mock *p = (mock *)base;
    if (p == NULL || deadline == NULL)
        return COG_ERROR_NULL_PTR;
    if (p->clock_seen && now < p->now)
        return COG_ERROR_INVALID_ARG;
    p->clock_seen = true;
    p->now = now;
    *deadline = p->sent && !p->terminal_pending ? p->ready_at : UINT64_MAX;
    return COG_OK;
}
static cog_result_t cancel_mock(cog_provider_t *base)
{
    mock *p = (mock *)base;
    if (p == NULL)
        return COG_ERROR_NULL_PTR;
    if (p->sent && !p->terminal_pending)
    {
        p->cancel_requested = true;
        if (p->retained)
            p->cancel_after = true;
        else
            p->terminal_pending = true;
        p->error = (cog_provider_error_t){
            .code = COG_ERROR_ABORTED,
            .message = "mock turn cancelled",
            .kind = COG_PROVIDER_ERROR_CANCELLED,
            .output_accepted = p->sequence != 0U || p->retained,
            .replay_safe = p->sequence == 0U && !p->retained};
    }
    return COG_OK;
}
static cog_provider_delta_t pending(mock *p)
{
    cog_provider_delta_t d = {.kind = COG_PROVIDER_DELTA_FINISH,
                              .finish_reason = COG_FINISH_REASON_STOP,
                              .outcome = COG_OK};
    if (p->terminal_pending)
    {
        if (p->error.code != COG_OK)
        {
            d.outcome = p->error.code;
            d.finish_reason = d.outcome == COG_ERROR_ABORTED
                                  ? COG_FINISH_REASON_CANCELLED
                                  : COG_FINISH_REASON_ERROR;
        }
        return d;
    }
    if (p->deltas != NULL)
        return p->step_index < p->delta_count ? p->deltas[p->step_index] : d;
    if (p->step_index >= p->script.step_count)
        return d;
    const cog_mock_step_t *s = &p->script.steps[p->step_index];
    if (s->kind == COG_MOCK_STEP_CHUNK)
    {
        d.kind = COG_PROVIDER_DELTA_TEXT;
        d.data = s->chunk;
        d.data_len = strlen(s->chunk);
        d.finish_reason = COG_FINISH_REASON_NONE;
    }
    else if (s->kind == COG_MOCK_STEP_ERROR)
    {
        d.outcome = s->error_code;
        d.finish_reason = COG_FINISH_REASON_ERROR;
    }
    else if (s->kind == COG_MOCK_STEP_CANCEL)
    {
        d.outcome = COG_ERROR_ABORTED;
        d.finish_reason = COG_FINISH_REASON_CANCELLED;
    }
    return d;
}
static size_t required_bytes(const cog_provider_delta_t *d)
{
    size_t n = d->data != NULL ? d->data_len + 1U : 0U;
    if (d->tool_call_id != NULL)
        n += strlen(d->tool_call_id) + 1U;
    if (d->tool_name != NULL)
        n += strlen(d->tool_name) + 1U;
    if (d->opaque.namespace_id != NULL)
        n += strlen(d->opaque.namespace_id) + 1U + strlen(d->opaque.format) +
             1U + d->opaque.size;
    if (d->usage.currency != NULL)
        n += strlen(d->usage.currency) + 1U;
    return n;
}
static void consume(mock *p, const cog_provider_delta_t *d)
{
    p->retained = false;
    ++p->sequence;
    if (d->kind == COG_PROVIDER_DELTA_FINISH)
    {
        if (d->outcome != COG_OK)
        {
            if (d->outcome == COG_ERROR_ABORTED)
            {
                ++p->cancel_count;
                p->cancel_requested = false;
            }
            else
                ++p->error_count;
            if (p->error.code == COG_OK)
            {
                const cog_mock_step_t *s =
                    p->deltas == NULL && p->step_index < p->script.step_count
                        ? &p->script.steps[p->step_index]
                        : NULL;
                p->error =
                    (cog_provider_error_t){.code = d->outcome,
                                           .message = "mock turn failed",
                                           .kind = COG_PROVIDER_ERROR_HTTP,
                                           .output_accepted = p->sequence > 1U,
                                           .replay_safe = p->sequence == 1U};
                if (s != NULL)
                {
                    p->error.http_status = s->http_status;
                    p->error.retryable = s->retryable;
                    p->error.retry_after_ms = s->retry_after_ms;
                }
            }
        }
        if (!p->terminal_pending &&
            ((p->deltas != NULL && p->step_index < p->delta_count) ||
             (p->deltas == NULL && p->step_index < p->script.step_count)))
            ++p->step_index;
        p->sent = false;
        p->terminal_pending = false;
    }
    else
    {
        /* A retained ordinary item remains accepted output when cancellation
         * is already pending; terminal diagnostics must reflect its delivery.
         */
        if (p->error.code != COG_OK)
        {
            p->error.output_accepted = true;
            p->error.replay_safe = false;
        }
        if (d->kind == COG_PROVIDER_DELTA_TEXT)
            ++p->chunk_count;
        ++p->step_index;
        if (p->cancel_after ||
            (p->deltas == NULL && p->step_index >= p->script.step_count))
            p->terminal_pending = true;
        p->ready_at = p->now > UINT64_MAX - p->script.chunk_delay_ms
                          ? UINT64_MAX
                          : p->now + p->script.chunk_delay_ms;
    }
}
static cog_result_t next_mock(cog_provider_t *base, cog_provider_delta_t *out,
                              void *storage, size_t cap, size_t *required)
{
    mock *p = (mock *)base;
    if (p == NULL || out == NULL || required == NULL)
        return COG_ERROR_NULL_PTR;
    if (storage == NULL && cap != 0U)
        return COG_ERROR_INVALID_ARG;
    if (p->used_receive)
        return COG_ERROR_INVALID_STATE;
    p->used_next = true;
    if (!p->sent)
        return COG_ERROR_INVALID_STATE;
    *required = 0U;
    if (!p->terminal_pending && p->now < p->ready_at)
        return COG_ERROR_AGAIN;
    p->retained = true;
    cog_provider_delta_t d = pending(p);
    size_t n = required_bytes(&d);
    *required = n;
    if (n > cap)
        return COG_ERROR_OVERFLOW;
    char *b = storage;
    *out = d;
    out->sequence = p->sequence + 1U;
#define COPY(member)                                                           \
    do                                                                         \
    {                                                                          \
        if (d.member != NULL)                                                  \
        {                                                                      \
            size_t z = strlen(d.member) + 1U;                                  \
            memcpy(b, d.member, z);                                            \
            out->member = b;                                                   \
            b += z;                                                            \
        }                                                                      \
    } while (0)
    COPY(tool_call_id);
    COPY(tool_name);
    if (d.data != NULL)
    {
        memcpy(b, d.data, d.data_len);
        b[d.data_len] = '\0';
        out->data = b;
        b += d.data_len + 1U;
    }
    COPY(opaque.namespace_id);
    COPY(opaque.format);
    if (d.opaque.size != 0U)
    {
        memcpy(b, d.opaque.data, d.opaque.size);
        out->opaque.data = (const unsigned char *)b;
        b += d.opaque.size;
    }
    COPY(usage.currency);
#undef COPY
    consume(p, &d);
    return COG_OK;
}
static cog_result_t receive_mock(cog_provider_t *base,
                                 cog_provider_response_t *r)
{
    mock *p = (mock *)base;
    if (p == NULL || r == NULL)
        return COG_ERROR_NULL_PTR;
    cog_result_t rc = cog_provider_response_validate(r);
    if (rc != COG_OK)
        return rc;
    if (p->used_next)
        return COG_ERROR_INVALID_STATE;
    p->used_receive = true;
    if (!p->sent)
        return COG_ERROR_INVALID_STATE;
    ++p->receive_count;
    if (!p->terminal_pending && p->now < p->ready_at)
        return COG_ERROR_AGAIN;
    p->retained = true;
    cog_provider_delta_t d = pending(p);
    if (d.kind != COG_PROVIDER_DELTA_TEXT &&
        d.kind != COG_PROVIDER_DELTA_FINISH)
        return COG_ERROR_UNSUPPORTED;
    if (d.kind == COG_PROVIDER_DELTA_TEXT)
    {
        if (d.data_len >= r->content_capacity)
        {
            r->content_len = d.data_len;
            return COG_ERROR_OVERFLOW;
        }
        memcpy(r->content, d.data, d.data_len);
        r->content[d.data_len] = '\0';
        r->content_len = d.data_len;
        consume(p, &d);
        /* Legacy receive may combine text with a successful STOP only.
         * Cancellation keeps the retained text nonterminal, then ABORTED. */
        r->is_complete = p->terminal_pending && p->error.code == COG_OK;
        r->finish_reason =
            r->is_complete ? COG_FINISH_REASON_STOP : COG_FINISH_REASON_NONE;
        if (r->is_complete)
        {
            p->sent = false;
            p->terminal_pending = false;
        }
        return COG_OK;
    }
    r->content_len = 0U;
    if (r->content_capacity != 0U)
        r->content[0] = '\0';
    r->finish_reason = d.finish_reason;
    r->is_complete = true;
    consume(p, &d);
    return d.outcome;
}
static cog_result_t error_mock(const cog_provider_t *base,
                               cog_provider_error_t *out)
{
    const mock *p = (const mock *)base;
    if (p == NULL || out == NULL)
        return COG_ERROR_NULL_PTR;
    if (p->error.code == COG_OK)
        return COG_ERROR_NOT_FOUND;
    *out = p->error;
    return COG_OK;
}
const cog_provider_vtable_t cog_mock_provider_vtable = {
    connect_mock, disconnect_mock, send_mock, receive_mock,
    poll_mock,    cancel_mock,     next_mock, error_mock, preflight_mock};
const cog_endpoint_t cog_mock_endpoint = {.name = "mock",
                                          .vtable = &cog_mock_provider_vtable};
cog_result_t cog_mock_provider_new(const cog_mock_script_t *script,
                                   cog_provider_t **out)
{
    if (out == NULL)
        return COG_ERROR_NULL_PTR;
    *out = NULL;
    if (script != NULL)
    {
        if (script->steps == NULL && script->step_count != 0U)
            return COG_ERROR_INVALID_ARG;
        for (size_t i = 0; i < script->step_count; ++i)
            if (script->steps[i].kind == COG_MOCK_STEP_CHUNK &&
                script->steps[i].chunk == NULL)
                return COG_ERROR_INVALID_ARG;
    }
    mock *p = calloc(1U, sizeof(*p));
    if (p == NULL)
        return COG_ERROR_NOMEM;
    p->request_arena = malloc(MOCK_REQUEST_BYTES);
    p->preflight_arena = malloc(MOCK_REQUEST_BYTES);
    if (p->request_arena == NULL || p->preflight_arena == NULL)
    {
        free(p->request_arena);
        free(p->preflight_arena);
        free(p);
        return COG_ERROR_NOMEM;
    }
    p->base.vtable = &cog_mock_provider_vtable;
    if (script != NULL)
        p->script = *script;
    else
    {
        p->script.steps = default_steps;
        p->script.step_count = 1U;
    }
    p->passes_left = p->script.repeat;
    *out = &p->base;
    return COG_OK;
}
cog_result_t cog_mock_provider_new_deltas(const cog_provider_delta_t *deltas,
                                          size_t count, cog_provider_t **out)
{
    if (out == NULL)
        return COG_ERROR_NULL_PTR;
    *out = NULL;
    if (deltas == NULL || count == 0U ||
        deltas[count - 1U].kind != COG_PROVIDER_DELTA_FINISH)
        return COG_ERROR_INVALID_ARG;
    for (size_t i = 0; i < count; ++i)
    {
        const cog_provider_delta_t *d = &deltas[i];
        if (d->kind < COG_PROVIDER_DELTA_TEXT ||
            d->kind > COG_PROVIDER_DELTA_FINISH ||
            (d->data == NULL && d->data_len != 0U) || d->data_len == SIZE_MAX ||
            (d->opaque.namespace_id != NULL && d->opaque.format == NULL) ||
            (d->opaque.data == NULL && d->opaque.size != 0U) ||
            d->opaque.size > MOCK_REQUEST_BYTES ||
            d->data_len > MOCK_REQUEST_BYTES)
            return COG_ERROR_INVALID_ARG;
        if ((d->kind == COG_PROVIDER_DELTA_TOOL_BEGIN ||
             d->kind == COG_PROVIDER_DELTA_TOOL_ARGUMENTS ||
             d->kind == COG_PROVIDER_DELTA_TOOL_END) &&
            (d->tool_call_id == NULL || d->tool_call_id[0] == '\0'))
            return COG_ERROR_INVALID_ARG;
        if (d->kind == COG_PROVIDER_DELTA_TOOL_BEGIN &&
            (d->tool_name == NULL || d->tool_name[0] == '\0'))
            return COG_ERROR_INVALID_ARG;
        if (d->kind == COG_PROVIDER_DELTA_OPAQUE &&
            (d->opaque.namespace_id == NULL || d->opaque.format == NULL))
            return COG_ERROR_INVALID_ARG;
    }
    cog_result_t rc = cog_mock_provider_new(NULL, out);
    if (rc == COG_OK)
    {
        mock *p = (mock *)*out;
        p->deltas = deltas;
        p->delta_count = count;
    }
    return rc;
}
cog_result_t cog_mock_provider_connect(const cog_endpoint_t *e,
                                       cog_provider_t **out)
{
    return connect_mock(e, out);
}
cog_result_t cog_mock_provider_disconnect(cog_provider_t *p)
{
    return disconnect_mock(p);
}
cog_result_t cog_mock_provider_send(cog_provider_t *p,
                                    const cog_provider_request_t *r)
{
    return cog_provider_send(p, r);
}
cog_result_t cog_mock_provider_receive(cog_provider_t *p,
                                       cog_provider_response_t *r)
{
    return receive_mock(p, r);
}
cog_result_t cog_mock_provider_request_cancel(cog_provider_t *p)
{
    return cancel_mock(p);
}
cog_result_t cog_mock_provider_state(cog_provider_t *base,
                                     cog_mock_state_t *out)
{
    mock *p = (mock *)base;
    if (p == NULL || out == NULL)
        return COG_ERROR_NULL_PTR;
    *out = (cog_mock_state_t){.send_count = p->send_count,
                              .receive_count = p->receive_count,
                              .chunk_count = p->chunk_count,
                              .error_count = p->error_count,
                              .cancel_count = p->cancel_count,
                              .cancel_requested = p->cancel_requested,
                              .step_index = p->step_index,
                              .last_request = &p->request,
                              .last_error = p->error};
    return COG_OK;
}

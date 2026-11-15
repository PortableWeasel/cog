/** @file openrouter.c @brief Explicit nonblocking OpenRouter transport adapter.
 */
#include "openrouter_internal.h"
#include <arpa/inet.h>
#include <stdlib.h>
#include <string.h>

static void wipe(void *bytes, size_t n)
{
    volatile unsigned char *p = bytes;
    if (p == NULL)
        return;
    while (n-- != 0U)
        *p++ = 0U;
}
static void release_request(or_provider *p)
{
    if (p->request_id != 0U)
    {
        (void)cog_net_request_release(p->config.net, p->request_id);
        p->request_id = 0U;
    }
    wipe(p->headers, p->config.limits.request_bytes);
}
void or_fail(or_provider *p, cog_result_t code, cog_provider_error_kind_t kind,
             int status, uint32_t retry)
{
    if (p->terminal)
        return;
    release_request(p);
    p->finalizing = false;
    p->terminal = true;
    p->outcome = code;
    p->error = (cog_provider_error_t){.code = code,
                                      .http_status = status,
                                      .message = "provider turn failed",
                                      .retryable =
                                          status == 429 || status >= 500 ||
                                          kind == COG_PROVIDER_ERROR_DNS ||
                                          kind == COG_PROVIDER_ERROR_CONNECT ||
                                          kind == COG_PROVIDER_ERROR_TIMEOUT,
                                      .retry_after_ms = retry,
                                      .output_accepted = p->accepted,
                                      .replay_safe = !p->accepted,
                                      .kind = kind};
    if (code == COG_ERROR_ABORTED)
        p->error.message = "provider turn cancelled";
    p->finish = code == COG_ERROR_ABORTED ? COG_FINISH_REASON_CANCELLED
                                          : COG_FINISH_REASON_ERROR;
}
static cog_result_t disconnect(cog_provider_t *base)
{
    or_provider *p = (or_provider *)base;
    if (p == NULL)
        return COG_OK;
    release_request(p);
    if (p->opaque_doc != NULL)
        yyjson_mut_doc_free(p->opaque_doc);
    if (p->tools != NULL)
        for (size_t i = 0; i < p->config.limits.max_tool_calls; ++i)
        {
            free(p->tools[i].id);
            free(p->tools[i].name);
            free(p->tools[i].arguments);
        }
    free(p->tools);
    free(p->request);
    free(p->line);
    free(p->frame);
    free(p->item_bytes);
    free(p->opaque_bytes);
    free(p->parser_pool);
    free(p->writer_pool);
    free(p->url);
    free(p->headers);
    free(p);
    return COG_OK;
}
static bool valid_base_url(const char *url)
{
    if (strncmp(url, "https://", 8U) != 0)
        return false;
    const char *host = url + 8U, *end = host;
    while (*end != '\0' && *end != '/')
    {
        unsigned char c = (unsigned char)*end;
        if (c <= 32U || c >= 127U || c == '@' || c == '?' || c == '#' ||
            c == '\\')
            return false;
        ++end;
    }
    if (end == host)
        return false;
    const char *port = NULL;
    if (*host == '[')
    {
        const char *close = memchr(host, ']', (size_t)(end - host));
        if (close == NULL || close == host + 1)
            return false;
        size_t literal_len = (size_t)(close - host - 1);
        char literal[INET6_ADDRSTRLEN];
        struct in6_addr address;
        if (literal_len >= sizeof(literal))
            return false;
        memcpy(literal, host + 1, literal_len);
        literal[literal_len] = '\0';
        if (inet_pton(AF_INET6, literal, &address) != 1)
            return false;
        if (close + 1 != end)
        {
            if (close[1] != ':')
                return false;
            port = close + 2;
        }
    }
    else
    {
        const char *colon = memchr(host, ':', (size_t)(end - host));
        if (colon == host)
            return false;
        if (colon != NULL)
            port = colon + 1;
    }
    if (port != NULL)
    {
        unsigned value = 0U;
        if (port == end)
            return false;
        for (const char *c = port; c < end; ++c)
        {
            if (*c < '0' || *c > '9' || value > 6553U)
                return false;
            value = value * 10U + (unsigned)(*c - '0');
            if (value > 65535U)
                return false;
        }
        if (value == 0U)
            return false;
    }
    for (const char *c = end; *c != '\0'; ++c)
        if ((unsigned char)*c <= 32U || *c == '?' || *c == '#' || *c == '\\')
            return false;
    return true;
}
static cog_result_t or_connect(const cog_endpoint_t *e, cog_provider_t **out)
{
    if (out == NULL)
        return COG_ERROR_NULL_PTR;
    *out = NULL;
    if (e == NULL)
        return COG_ERROR_NULL_PTR;
    if (e->implementation_config == NULL || e->base_url == NULL ||
        e->api_key == NULL || e->api_key[0] == '\0')
        return COG_ERROR_INVALID_ARG;
    if (!valid_base_url(e->base_url) || strchr(e->api_key, '\r') != NULL ||
        strchr(e->api_key, '\n') != NULL)
        return COG_ERROR_INVALID_ARG;
    const cog_openrouter_config_t *cfg = e->implementation_config;
    const cog_provider_limits_t *l = &cfg->limits;
    if (cfg->net == NULL)
        return COG_ERROR_INVALID_ARG;
    const size_t sizes[] = {
        l->request_bytes,        l->parser_bytes,
        l->writer_bytes,         l->max_json_depth,
        l->max_frame_bytes,      l->retained_delta_bytes,
        l->retained_delta_count, l->max_text_bytes,
        l->max_tool_calls,       l->max_tool_id_bytes,
        l->max_tool_name_bytes,  l->max_tool_arguments_bytes,
        l->max_opaque_bytes};
    size_t total = sizeof(or_provider);
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i)
    {
        if (sizes[i] == 0U || sizes[i] > (SIZE_MAX - total) / 8U)
            return COG_ERROR_INVALID_ARG;
        total += sizes[i] * 8U;
    }
    if (l->max_json_depth > 256U ||
        l->max_tool_calls > SIZE_MAX / sizeof(or_tool) ||
        l->max_tool_calls > SIZE_MAX / l->max_tool_arguments_bytes ||
        l->max_tool_calls > SIZE_MAX / l->max_tool_id_bytes ||
        l->max_tool_calls > SIZE_MAX / l->max_tool_name_bytes ||
        strlen(e->base_url) > SIZE_MAX - 32U)
        return COG_ERROR_INVALID_ARG;
    or_provider *p = calloc(1U, sizeof(*p));
    if (p == NULL)
        return COG_ERROR_NOMEM;
    p->base.vtable = &cog_openrouter_provider_vtable;
    p->endpoint = e;
    p->config = *cfg;
    p->request = calloc(l->request_bytes, 1U);
    p->headers = calloc(l->request_bytes, 1U);
    p->line = calloc(l->max_frame_bytes, 1U);
    p->frame = calloc(l->max_frame_bytes, 1U);
    p->item_bytes = calloc(l->retained_delta_bytes, 1U);
    p->opaque_bytes = calloc(l->max_opaque_bytes, 1U);
    p->parser_pool = malloc(l->parser_bytes);
    p->writer_pool = malloc(l->writer_bytes);
    p->url = calloc(strlen(e->base_url) + 32U, 1U);
    p->tools = calloc(l->max_tool_calls, sizeof(*p->tools));
    if (p->request == NULL || p->headers == NULL || p->line == NULL ||
        p->frame == NULL || p->item_bytes == NULL || p->opaque_bytes == NULL ||
        p->parser_pool == NULL || p->writer_pool == NULL || p->url == NULL ||
        p->tools == NULL)
    {
        (void)disconnect(&p->base);
        return COG_ERROR_NOMEM;
    }
    for (size_t i = 0; i < l->max_tool_calls; ++i)
    {
        p->tools[i].id = calloc(l->max_tool_id_bytes, 1U);
        p->tools[i].name = calloc(l->max_tool_name_bytes, 1U);
        p->tools[i].arguments = calloc(l->max_tool_arguments_bytes, 1U);
        if (p->tools[i].id == NULL || p->tools[i].name == NULL ||
            p->tools[i].arguments == NULL)
        {
            (void)disconnect(&p->base);
            return COG_ERROR_NOMEM;
        }
    }
    if (!yyjson_alc_pool_init(&p->parser, p->parser_pool, l->parser_bytes) ||
        !yyjson_alc_pool_init(&p->writer, p->writer_pool, l->writer_bytes))
    {
        (void)disconnect(&p->base);
        return COG_ERROR_INVALID_ARG;
    }
    size_t n = strlen(e->base_url);
    memcpy(p->url, e->base_url, n);
    if (n != 0U && p->url[n - 1U] == '/')
        --n;
    memcpy(p->url + n, "/chat/completions", sizeof("/chat/completions"));
    *out = &p->base;
    return COG_OK;
}
static cog_result_t send_request(cog_provider_t *base,
                                 const cog_provider_request_t *r)
{
    or_provider *p = (or_provider *)base;
    if (p == NULL || r == NULL)
        return COG_ERROR_NULL_PTR;
    if (p->active)
        return COG_ERROR_INVALID_STATE;
    cog_result_t rc = cog_provider_request_validate(r);
    if (rc != COG_OK)
        return rc;
    if (r->max_tokens == 0U ||
        r->configuration_generation != p->endpoint->generation)
        return COG_ERROR_INVALID_ARG;
    const cog_model_t *model = NULL;
    rc = cog_endpoint_model_lookup(p->endpoint, r->model_id, &model);
    if (rc != COG_OK)
        return rc;
    if ((r->stream &&
         !cog_model_has_capability(model, COG_MODEL_CAPABILITY_STREAMING)) ||
        (r->tools_json != NULL &&
         !cog_model_has_capability(model, COG_MODEL_CAPABILITY_TOOLS)))
        return COG_ERROR_UNSUPPORTED;
    if (model->context_window != 0U && r->max_tokens > model->context_window)
        return COG_ERROR_LIMIT;
    if (p->opaque_doc != NULL)
    {
        yyjson_mut_doc_free(p->opaque_doc);
        p->opaque_doc = NULL;
    }
    size_t len = 0U;
    rc = or_encode(p, r, &len);
    if (rc != COG_OK)
        return rc;
    p->opaque_doc = yyjson_mut_doc_new(&p->writer);
    if (p->opaque_doc == NULL)
        return COG_ERROR_LIMIT;
    yyjson_mut_val *root = yyjson_mut_obj(p->opaque_doc);
    p->details = yyjson_mut_arr(p->opaque_doc);
    yyjson_mut_doc_set_root(p->opaque_doc, root);
    if (root == NULL || p->details == NULL ||
        !yyjson_mut_obj_add_val(p->opaque_doc, root, "reasoning_details",
                                p->details))
        return COG_ERROR_LIMIT;
    size_t kn = strlen(p->endpoint->api_key);
    static const char prefix[] = "Authorization: Bearer ";
    static const char suffix[] =
        "\nContent-Type: application/json\nAccept: text/event-stream";
    if (kn > SIZE_MAX - sizeof(prefix) - sizeof(suffix) ||
        sizeof(prefix) + kn + sizeof(suffix) > p->config.limits.request_bytes)
        return COG_ERROR_LIMIT;
    memcpy(p->headers, prefix, sizeof(prefix) - 1U);
    memcpy(p->headers + sizeof(prefix) - 1U, p->endpoint->api_key, kn);
    memcpy(p->headers + sizeof(prefix) - 1U + kn, suffix, sizeof(suffix));
    rc = cog_net_request(p->config.net, "POST", p->url, p->headers, p->request,
                         len, &p->request_id);
    wipe(p->headers, p->config.limits.request_bytes);
    if (rc != COG_OK)
        return rc;
    p->active = true;
    p->terminal = false;
    p->item_ready = false;
    p->done = false;
    p->skip_lf = false;
    p->finish_seen = false;
    p->accepted = false;
    p->finalizing = false;
    p->opaque_present = false;
    p->usage_present = false;
    p->usage_final = false;
    p->used_next = false;
    p->used_receive = false;
    p->stream = r->stream;
    p->line_len = 0U;
    p->frame_len = 0U;
    p->body_offset = 0U;
    p->text_len = 0U;
    p->opaque_len = 0U;
    p->sequence = 0U;
    p->finish = COG_FINISH_REASON_NONE;
    p->outcome = COG_OK;
    p->usage = (cog_usage_t){0};
    p->error = (cog_provider_error_t){0};
    for (size_t i = 0; i < p->config.limits.max_tool_calls; ++i)
    {
        or_tool *t = &p->tools[i];
        t->id_len = t->name_len = t->arguments_len = 0U;
        t->id[0] = t->name[0] = t->arguments[0] = '\0';
        t->seen = false;
    }
    return COG_OK;
}
/* Compact one item into provider-owned storage; no runtime growth. */
static size_t needed(const cog_provider_delta_t *d)
{
    size_t n = 0U;
    if (d->tool_call_id != NULL)
        n += strlen(d->tool_call_id) + 1U;
    if (d->tool_name != NULL)
        n += strlen(d->tool_name) + 1U;
    if (d->data != NULL)
        n += d->data_len + 1U;
    if (d->opaque.namespace_id != NULL)
        n += strlen(d->opaque.namespace_id) + 1U + strlen(d->opaque.format) +
             1U + d->opaque.size;
    if (d->usage.currency != NULL)
        n += strlen(d->usage.currency) + 1U;
    return n;
}
static void copy_item(cog_provider_delta_t *out, const cog_provider_delta_t *in,
                      void *storage)
{
    *out = *in;
    char *b = storage;
#define COPY_STR(member)                                                       \
    do                                                                         \
    {                                                                          \
        if (in->member != NULL)                                                \
        {                                                                      \
            size_t n = strlen(in->member) + 1U;                                \
            memcpy(b, in->member, n);                                          \
            out->member = b;                                                   \
            b += n;                                                            \
        }                                                                      \
    } while (0)
    COPY_STR(tool_call_id);
    COPY_STR(tool_name);
    if (in->data != NULL)
    {
        memcpy(b, in->data, in->data_len);
        b[in->data_len] = '\0';
        out->data = b;
        b += in->data_len + 1U;
    }
    COPY_STR(opaque.namespace_id);
    COPY_STR(opaque.format);
    if (in->opaque.size != 0U)
    {
        memcpy(b, in->opaque.data, in->opaque.size);
        out->opaque.data = (const unsigned char *)b;
        b += in->opaque.size;
    }
    COPY_STR(usage.currency);
#undef COPY_STR
}
cog_result_t or_emit(or_provider *p, const cog_provider_delta_t *item)
{
    if (p->item_ready)
        return COG_ERROR_AGAIN;
    size_t n = needed(item);
    if (n > p->config.limits.retained_delta_bytes)
        return COG_ERROR_LIMIT;
    copy_item(&p->item, item, p->item_bytes);
    p->item.sequence = ++p->sequence;
    p->item_size = n;
    p->item_ready = true;
    p->accepted = true;
    return COG_OK;
}
static cog_result_t sse_line(or_provider *p)
{
    if (p->line_len == 0U)
    {
        if (p->frame_len == 0U)
            return COG_OK;
        --p->frame_len;
        p->frame[p->frame_len] = '\0';
        cog_result_t rc;
        if (p->done)
            rc = COG_ERROR_PROTOCOL;
        else if (p->frame_len == 6U && memcmp(p->frame, "[DONE]", 6U) == 0)
        {
            rc = p->finish_seen ? COG_OK : COG_ERROR_PROTOCOL;
            if (rc == COG_OK)
                p->done = true;
        }
        else
            rc = or_decode(p, p->frame, p->frame_len);
        p->frame_len = 0U;
        return rc;
    }
    size_t colon = 0U;
    while (colon < p->line_len && p->line[colon] != ':')
        ++colon;
    if (colon == 4U && memcmp(p->line, "data", 4U) == 0)
    {
        size_t start = colon < p->line_len ? colon + 1U : colon;
        if (start < p->line_len && p->line[start] == ' ')
            ++start;
        size_t n = p->line_len - start;
        if (p->frame_len >= p->config.limits.max_frame_bytes - 1U ||
            n >= p->config.limits.max_frame_bytes - p->frame_len - 1U)
            return COG_ERROR_LIMIT;
        memcpy(p->frame + p->frame_len, p->line + start, n);
        p->frame_len += n;
        p->frame[p->frame_len++] = '\n';
        p->frame[p->frame_len] = '\0';
    }
    return COG_OK;
}
static cog_provider_error_kind_t net_kind(cog_net_error_kind_t k)
{
    switch (k)
    {
    case COG_NET_ERROR_DNS:
        return COG_PROVIDER_ERROR_DNS;
    case COG_NET_ERROR_TLS:
        return COG_PROVIDER_ERROR_TLS;
    case COG_NET_ERROR_CONNECT:
        return COG_PROVIDER_ERROR_CONNECT;
    case COG_NET_ERROR_TIMEOUT:
        return COG_PROVIDER_ERROR_TIMEOUT;
    case COG_NET_ERROR_CAPACITY:
        return COG_PROVIDER_ERROR_CAPACITY;
    case COG_NET_ERROR_CANCELLED:
        return COG_PROVIDER_ERROR_CANCELLED;
    default:
        return COG_PROVIDER_ERROR_DEPENDENCY;
    }
}
static cog_result_t poll_provider(cog_provider_t *base, uint64_t now,
                                  uint64_t *deadline)
{
    or_provider *p = (or_provider *)base;
    if (p == NULL || deadline == NULL)
        return COG_ERROR_NULL_PTR;
    if (p->clock_seen && now < p->now)
        return COG_ERROR_INVALID_ARG;
    p->now = now;
    p->clock_seen = true;
    *deadline = UINT64_MAX;
    cog_result_t rc = cog_net_service(p->config.net, now, deadline);
    if (rc != COG_OK)
        return rc;
    if (!p->active || p->terminal)
        return COG_OK;
    if (p->finalizing)
    {
        rc = or_expand(p);
        if (rc != COG_OK)
            or_fail(p, rc, COG_PROVIDER_ERROR_CAPACITY, 0, 0U);
        return COG_OK;
    }
    uint32_t quantum =
        p->config.service_quantum == 0U ? 16U : p->config.service_quantum;
    uint32_t events = 0U;
    for (uint32_t q = 0U;
         q < quantum && events < quantum && !p->item_ready && !p->terminal; ++q)
    {
        cog_net_response_t response;
        rc = cog_net_peek_request(p->config.net, p->request_id, &response);
        if (rc == COG_ERROR_AGAIN)
            break;
        if (rc != COG_OK)
            return rc;
        if (response.http_status != 0 &&
            (response.http_status < 200 || response.http_status >= 300))
        {
            or_fail(p, COG_ERROR_UNAVAILABLE, COG_PROVIDER_ERROR_HTTP,
                    response.http_status, response.retry_after_ms);
            break;
        }
        size_t off = p->body_offset;
        while (off < response.chunk_len && !p->item_ready && events < quantum)
        {
            char c = response.chunk[off++];
            if (!p->stream)
            {
                if (p->frame_len >= p->config.limits.max_frame_bytes - 1U)
                {
                    rc = COG_ERROR_LIMIT;
                    break;
                }
                p->frame[p->frame_len++] = c;
                continue;
            }
            if (p->skip_lf && c == '\n')
            {
                p->skip_lf = false;
                continue;
            }
            p->skip_lf = false;
            if (c == '\r' || c == '\n')
            {
                if (p->line_len == 0U && p->frame_len != 0U)
                    ++events;
                rc = sse_line(p);
                p->line_len = 0U;
                p->skip_lf = c == '\r';
                if (rc != COG_OK)
                    break;
            }
            else
            {
                if (p->line_len >= p->config.limits.max_frame_bytes - 1U)
                {
                    rc = COG_ERROR_LIMIT;
                    break;
                }
                p->line[p->line_len++] = c;
            }
        }
        p->body_offset = off;
        if (rc != COG_OK)
        {
            or_fail(p, rc,
                    rc == COG_ERROR_LIMIT ? COG_PROVIDER_ERROR_CAPACITY
                                          : COG_PROVIDER_ERROR_PROTOCOL,
                    response.http_status, 0U);
            break;
        }
        if (off < response.chunk_len)
            break;
        if (response.is_complete)
        {
            if (response.outcome != COG_OK)
            {
                or_fail(p, response.outcome, net_kind(response.error_kind),
                        response.http_status, response.retry_after_ms);
                break;
            }
            if (!p->stream)
                rc = or_decode(p, p->frame, p->frame_len);
            if (rc == COG_OK)
                rc = or_eof(p);
            release_request(p);
            if (rc != COG_OK)
                or_fail(p, rc,
                        rc == COG_ERROR_LIMIT ? COG_PROVIDER_ERROR_CAPACITY
                                              : COG_PROVIDER_ERROR_PROTOCOL,
                        response.http_status, 0U);
            else
            {
                rc = or_expand(p);
                if (rc != COG_OK)
                    or_fail(p, rc, COG_PROVIDER_ERROR_CAPACITY,
                            response.http_status, 0U);
            }
            break;
        }
        rc = cog_net_ack(p->config.net, p->request_id, response.sequence);
        if (rc != COG_OK)
            return rc;
        p->body_offset = 0U;
    }
    /* A retained item/final cursor needs consumer progress, not a timed sleep.
     */
    if (p->item_ready || p->finalizing)
        *deadline = now;
    return COG_OK;
}
static cog_result_t cancel(cog_provider_t *base)
{
    or_provider *p = (or_provider *)base;
    if (p == NULL)
        return COG_ERROR_NULL_PTR;
    if (p->active && !p->terminal)
        or_fail(p, COG_ERROR_ABORTED, COG_PROVIDER_ERROR_CANCELLED, 0, 0U);
    return COG_OK;
}
static cog_result_t take(or_provider *p, cog_provider_delta_t *out,
                         void *storage, size_t cap, size_t *required)
{
    if (!p->active)
        return COG_ERROR_INVALID_STATE;
    *required = 0U;
    if (p->item_ready)
    {
        *required = p->item_size;
        if (cap < p->item_size)
            return COG_ERROR_OVERFLOW;
        copy_item(out, &p->item, storage);
        p->item_ready = false;
        return COG_OK;
    }
    if (!p->terminal)
        return COG_ERROR_AGAIN;
    *out = (cog_provider_delta_t){.kind = COG_PROVIDER_DELTA_FINISH,
                                  .sequence = ++p->sequence,
                                  .finish_reason = p->finish,
                                  .outcome = p->outcome};
    p->active = false;
    return COG_OK;
}
static cog_result_t next(cog_provider_t *base, cog_provider_delta_t *d,
                         void *storage, size_t cap, size_t *required)
{
    or_provider *p = (or_provider *)base;
    if (p == NULL || d == NULL || required == NULL)
        return COG_ERROR_NULL_PTR;
    if (storage == NULL && cap != 0U)
        return COG_ERROR_INVALID_ARG;
    if (p->used_receive)
        return COG_ERROR_INVALID_STATE;
    p->used_next = true;
    return take(p, d, storage, cap, required);
}
static cog_result_t receive(cog_provider_t *base, cog_provider_response_t *r)
{
    or_provider *p = (or_provider *)base;
    if (p == NULL || r == NULL)
        return COG_ERROR_NULL_PTR;
    if (p->used_next)
        return COG_ERROR_INVALID_STATE;
    cog_result_t rc = cog_provider_response_validate(r);
    if (rc != COG_OK)
        return rc;
    p->used_receive = true;
    if (!p->active)
        return COG_ERROR_INVALID_STATE;
    if (p->item_ready)
    {
        if (p->item.kind != COG_PROVIDER_DELTA_TEXT)
            return COG_ERROR_UNSUPPORTED;
        size_t n = p->item.data_len;
        if (r->content_capacity <= n)
        {
            r->content_len = n;
            return COG_ERROR_OVERFLOW;
        }
        memcpy(r->content, p->item.data, n);
        r->content[n] = '\0';
        r->content_len = n;
        r->finish_reason = COG_FINISH_REASON_NONE;
        r->is_complete = false;
        p->item_ready = false;
        return COG_OK;
    }
    if (!p->terminal)
        return COG_ERROR_AGAIN;
    r->content_len = 0U;
    if (r->content_capacity != 0U)
        r->content[0] = '\0';
    r->finish_reason = p->finish;
    r->is_complete = true;
    p->active = false;
    return p->outcome;
}
static cog_result_t error(const cog_provider_t *base, cog_provider_error_t *out)
{
    const or_provider *p = (const or_provider *)base;
    if (p == NULL || out == NULL)
        return COG_ERROR_NULL_PTR;
    if (p->error.code == COG_OK)
        return COG_ERROR_NOT_FOUND;
    *out = p->error;
    return COG_OK;
}
const cog_provider_vtable_t cog_openrouter_provider_vtable = {
    or_connect,    disconnect, send_request, receive,
    poll_provider, cancel,     next,         error};

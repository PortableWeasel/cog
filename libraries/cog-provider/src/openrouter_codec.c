/** @file openrouter_codec.c @brief Strict fixed-pool Chat Completions codec. */
#include "openrouter_internal.h"
#include <math.h>
#include <string.h>

/* Depth preflight does not replace JSON validation. Duplicate detection
 * compares decoded key lengths/bytes, including escaped spellings and embedded
 * NUL. */
static bool depth_ok(const char *s, size_t n, size_t max)
{
    size_t depth = 0U;
    bool quoted = false, escape = false;
    for (size_t i = 0; i < n; ++i)
    {
        char c = s[i];
        if (quoted)
        {
            if (escape)
                escape = false;
            else if (c == '\\')
                escape = true;
            else if (c == '"')
                quoted = false;
        }
        else if (c == '"')
            quoted = true;
        else if (c == '{' || c == '[')
        {
            if (++depth > max)
                return false;
        }
        else if (c == '}' || c == ']')
        {
            if (depth == 0U)
                return false;
            --depth;
        }
    }
    return depth == 0U && !quoted;
}
static bool unique(yyjson_val *v)
{
    size_t i, n;
    yyjson_val *a;
    if (yyjson_is_arr(v))
    {
        yyjson_arr_foreach(v, i, n, a)
        {
            if (!unique(a))
                return false;
        }
    }
    else if (yyjson_is_obj(v))
    {
        yyjson_val *av;
        yyjson_obj_foreach(v, i, n, a, av)
        {
            size_t j, m;
            yyjson_val *b, *bv;
            yyjson_obj_foreach(v, j, m, b, bv)
            {
                (void)bv;
                if (j >= i)
                    break;
                if (yyjson_get_len(a) == yyjson_get_len(b) &&
                    memcmp(yyjson_get_str(a), yyjson_get_str(b),
                           yyjson_get_len(a)) == 0)
                    return false;
            }
            if (!unique(av))
                return false;
        }
    }
    return true;
}
cog_result_t or_json_read(or_provider *p, const char *s, size_t n,
                          yyjson_doc **out)
{
    *out = NULL;
    if (!depth_ok(s, n, p->config.limits.max_json_depth))
        return COG_ERROR_PROTOCOL;
    yyjson_read_err err;
    *out = yyjson_read_opts((char *)(void *)s, n, 0U, &p->parser, &err);
    if (*out == NULL)
        return err.code == YYJSON_READ_ERROR_MEMORY_ALLOCATION
                   ? COG_ERROR_LIMIT
                   : COG_ERROR_PROTOCOL;
    if (!unique(yyjson_doc_get_root(*out)))
    {
        yyjson_doc_free(*out);
        *out = NULL;
        return COG_ERROR_PROTOCOL;
    }
    return COG_OK;
}
static yyjson_mut_val *str(yyjson_mut_doc *d, const char *s, size_t n)
{
    return yyjson_mut_strncpy(d, s == NULL ? "" : s, n);
}
static bool add(yyjson_mut_doc *d, yyjson_mut_val *obj, const char *k,
                yyjson_mut_val *v)
{
    return v != NULL && yyjson_mut_obj_add_val(d, obj, k, v);
}
static cog_result_t json_copy(or_provider *p, yyjson_mut_doc *d, const char *s,
                              size_t n, bool object, yyjson_mut_val **out)
{
    yyjson_doc *parsed = NULL;
    cog_result_t rc = or_json_read(p, s, n, &parsed);
    if (rc != COG_OK)
        return rc;
    yyjson_val *v = yyjson_doc_get_root(parsed);
    if ((object && !yyjson_is_obj(v)) || (!object && !yyjson_is_arr(v)))
        rc = COG_ERROR_PROTOCOL;
    else
    {
        *out = yyjson_val_mut_copy(d, v);
        if (*out == NULL)
            rc = COG_ERROR_LIMIT;
    }
    yyjson_doc_free(parsed);
    return rc;
}
static bool opaque_entries_supported(yyjson_val *details)
{
    size_t i, n;
    yyjson_val *entry;
    yyjson_arr_foreach(details, i, n, entry)
    {
        if (!yyjson_is_obj(entry))
            return false;
        yyjson_val *type = yyjson_obj_get(entry, "type");
        if (!yyjson_equals_str(type, "reasoning.text") &&
            !yyjson_equals_str(type, "reasoning.summary") &&
            !yyjson_equals_str(type, "reasoning.encrypted"))
            return false;
    }
    return true;
}
cog_result_t or_encode(or_provider *p, const cog_provider_request_t *r,
                       size_t *len)
{
    yyjson_mut_doc *d = yyjson_mut_doc_new(&p->writer);
    if (d == NULL)
        return COG_ERROR_LIMIT;
    cog_result_t rc = COG_ERROR_LIMIT;
    yyjson_mut_val *root = yyjson_mut_obj(d), *messages = yyjson_mut_arr(d);
    yyjson_mut_doc_set_root(d, root);
    if (!add(d, root, "model", str(d, r->model_id, strlen(r->model_id))) ||
        !add(d, root, "messages", messages) ||
        !add(d, root, "stream", yyjson_mut_bool(d, r->stream)) ||
        !add(d, root, "max_tokens", yyjson_mut_uint(d, r->max_tokens)))
        goto end;
    yyjson_mut_val *reasoning = yyjson_mut_obj(d);
    if (!add(d, reasoning, "enabled", yyjson_mut_false(d)) ||
        !add(d, root, "reasoning", reasoning))
        goto end;
    if (r->temperature != 0.0f &&
        !add(d, root, "temperature",
             yyjson_mut_real(d, (double)r->temperature)))
        goto end;
    /* No reasoning is requested. Preserve only the pinned continuation field.
     */
    if (r->stream)
    {
        yyjson_mut_val *opts = yyjson_mut_obj(d);
        if (!add(d, opts, "include_usage", yyjson_mut_true(d)) ||
            !add(d, root, "stream_options", opts))
            goto end;
    }
    if (r->tools_json != NULL)
    {
        yyjson_mut_val *tools = NULL;
        rc = json_copy(p, d, r->tools_json, r->tools_json_len, false, &tools);
        if (rc != COG_OK)
            goto end;
        rc = COG_ERROR_LIMIT;
        if (!add(d, root, "tools", tools))
            goto end;
    }
    static const char *const roles[] = {"system", "user", "assistant", "tool"};
    for (size_t i = 0; i < r->message_count; ++i)
    {
        const cog_message_t *m = &r->messages[i];
        if (m->status != COG_MESSAGE_COMPLETE)
        {
            rc = COG_ERROR_INVALID_ARG;
            goto end;
        }
        yyjson_mut_val *msg = yyjson_mut_obj(d);
        if (!yyjson_mut_arr_append(messages, msg) ||
            !add(d, msg, "role", yyjson_mut_str(d, roles[m->role])) ||
            !add(d, msg, "content", str(d, m->content, m->content_len)))
            goto end;
        if (m->name != NULL &&
            !add(d, msg, "name", str(d, m->name, strlen(m->name))))
            goto end;
        if (m->tool_call_id != NULL &&
            !add(d, msg, "tool_call_id",
                 str(d, m->tool_call_id, strlen(m->tool_call_id))))
            goto end;
        if (m->tool_call_count != 0U)
        {
            yyjson_mut_val *calls = yyjson_mut_arr(d);
            if (!add(d, msg, "tool_calls", calls))
                goto end;
            for (size_t j = 0; j < m->tool_call_count; ++j)
            {
                const cog_message_tool_call_t *t = &m->tool_calls[j];
                yyjson_mut_val *validated = NULL;
                rc = json_copy(p, d, t->arguments, t->arguments_len, true,
                               &validated);
                if (rc != COG_OK)
                    goto end;
                rc = COG_ERROR_LIMIT;
                yyjson_mut_val *call = yyjson_mut_obj(d),
                               *fn = yyjson_mut_obj(d);
                if (!yyjson_mut_arr_append(calls, call) ||
                    !add(d, call, "id", str(d, t->id, strlen(t->id))) ||
                    !add(d, call, "type", yyjson_mut_str(d, "function")) ||
                    !add(d, call, "function", fn) ||
                    !add(d, fn, "name", str(d, t->name, strlen(t->name))) ||
                    !add(d, fn, "arguments",
                         str(d, t->arguments, t->arguments_len)))
                    goto end;
            }
        }
        if (m->opaque_count > 1U)
        {
            rc = COG_ERROR_UNSUPPORTED;
            goto end;
        }
        for (size_t j = 0; j < m->opaque_count; ++j)
        {
            const cog_message_opaque_t *o = &m->opaque[j];
            if (strcmp(o->namespace_id, "openrouter.chat-completions") != 0 ||
                strcmp(o->format, "json-v1") != 0)
            {
                rc = COG_ERROR_UNSUPPORTED;
                goto end;
            }
            if (o->size > p->config.limits.max_opaque_bytes)
            {
                rc = COG_ERROR_LIMIT;
                goto end;
            }
            yyjson_doc *od = NULL;
            rc = or_json_read(p, (const char *)o->data, o->size, &od);
            if (rc != COG_OK)
                goto end;
            yyjson_val *ov = yyjson_doc_get_root(od),
                       *details = yyjson_obj_get(ov, "reasoning_details");
            if (!yyjson_is_obj(ov) || yyjson_obj_size(ov) != 1U ||
                !yyjson_is_arr(details) || !opaque_entries_supported(details))
                rc = COG_ERROR_UNSUPPORTED;
            else
            {
                yyjson_mut_val *copy = yyjson_val_mut_copy(d, details);
                rc = add(d, msg, "reasoning_details", copy) ? COG_OK
                                                            : COG_ERROR_LIMIT;
            }
            yyjson_doc_free(od);
            if (rc != COG_OK)
                goto end;
            rc = COG_ERROR_LIMIT;
        }
    }
    {
        char *bytes = yyjson_mut_write_opts(d, 0U, &p->writer, len, NULL);
        if (bytes == NULL)
            goto end;
        if (*len >= p->config.limits.request_bytes)
            rc = COG_ERROR_LIMIT;
        else
        {
            memcpy(p->request, bytes, *len + 1U);
            rc = COG_OK;
        }
        p->writer.free(p->writer.ctx, bytes);
    }
end:
    yyjson_mut_doc_free(d);
    return rc;
}
static cog_result_t append_value(yyjson_val *v, char *buf, size_t *len,
                                 size_t cap)
{
    if (v == NULL || yyjson_is_null(v))
        return COG_OK;
    if (!yyjson_is_str(v) ||
        memchr(yyjson_get_str(v), 0, yyjson_get_len(v)) != NULL)
        return COG_ERROR_PROTOCOL;
    size_t n = yyjson_get_len(v);
    if (n >= cap - *len)
        return COG_ERROR_LIMIT;
    memcpy(buf + *len, yyjson_get_str(v), n);
    *len += n;
    buf[*len] = '\0';
    return COG_OK;
}
static cog_result_t opaque_update(or_provider *p, yyjson_val *incoming)
{
    if (!yyjson_is_arr(incoming))
        return COG_ERROR_PROTOCOL;
    p->opaque_present = true;
    size_t i, n;
    yyjson_val *entry;
    yyjson_arr_foreach(incoming, i, n, entry)
    {
        if (!yyjson_is_obj(entry))
            return COG_ERROR_PROTOCOL;
        yyjson_val *type = yyjson_obj_get(entry, "type");
        const char *payload = NULL;
        if (yyjson_equals_str(type, "reasoning.text"))
            payload = "text";
        else if (yyjson_equals_str(type, "reasoning.summary"))
            payload = "summary";
        else if (!yyjson_equals_str(type, "reasoning.encrypted"))
            return COG_ERROR_UNSUPPORTED;
        yyjson_mut_val *last = yyjson_mut_arr_get_last(p->details);
        bool merge = payload != NULL && last != NULL &&
                     yyjson_mut_equals_str(yyjson_mut_obj_get(last, "type"),
                                           yyjson_get_str(type)) &&
                     yyjson_mut_obj_get(last, "signature") == NULL;
        yyjson_val *pv =
            payload == NULL ? NULL : yyjson_obj_get(entry, payload);
        yyjson_val *signature = yyjson_obj_get(entry, "signature");
        if ((pv != NULL && !yyjson_is_str(pv)) ||
            (signature != NULL && !yyjson_is_str(signature)))
            return COG_ERROR_PROTOCOL;
        if (!merge)
        {
            yyjson_mut_val *copy = yyjson_val_mut_copy(p->opaque_doc, entry);
            if (copy == NULL || !yyjson_mut_arr_append(p->details, copy))
                return COG_ERROR_LIMIT;
            continue;
        }
        size_t j, m;
        yyjson_val *key, *value;
        yyjson_obj_foreach(entry, j, m, key, value)
        {
            const char *k = yyjson_get_str(key);
            size_t kn = yyjson_get_len(key);
            yyjson_mut_val *old = yyjson_mut_obj_getn(last, k, kn);
            yyjson_mut_val *copy = NULL;
            if (kn == strlen(payload) && memcmp(k, payload, kn) == 0 &&
                old != NULL)
            {
                if (!yyjson_mut_is_str(old))
                    return COG_ERROR_PROTOCOL;
                size_t a = yyjson_mut_get_len(old), b = yyjson_get_len(value);
                if (a >= p->config.limits.max_opaque_bytes ||
                    b >= p->config.limits.max_opaque_bytes - a)
                    return COG_ERROR_LIMIT;
                memcpy(p->opaque_bytes, yyjson_mut_get_str(old), a);
                memcpy(p->opaque_bytes + a, yyjson_get_str(value), b);
                copy = str(p->opaque_doc, p->opaque_bytes, a + b);
            }
            else
            {
                copy = yyjson_val_mut_copy(p->opaque_doc, value);
                if (copy == NULL)
                    return COG_ERROR_LIMIT;
                if (old != NULL)
                {
                    if (!yyjson_mut_equals(old, copy))
                        return COG_ERROR_UNSUPPORTED;
                    continue;
                }
            }
            yyjson_mut_val *mk = str(p->opaque_doc, k, kn);
            if (mk == NULL || copy == NULL ||
                !yyjson_mut_obj_put(last, mk, copy))
                return COG_ERROR_LIMIT;
        }
    }
    /* Enforce the opaque byte ceiling on every frame, independently of pool. */
    size_t bytes = 0U;
    char *encoded =
        yyjson_mut_write_opts(p->opaque_doc, 0U, &p->writer, &bytes, NULL);
    if (encoded == NULL)
        return COG_ERROR_LIMIT;
    bool fits = bytes < p->config.limits.max_opaque_bytes;
    p->writer.free(p->writer.ctx, encoded);
    return fits ? COG_OK : COG_ERROR_LIMIT;
}
static cog_result_t usage_read(or_provider *p, yyjson_val *u)
{
    if (u == NULL || yyjson_is_null(u))
        return COG_OK;
    if (!yyjson_is_obj(u))
        return COG_ERROR_PROTOCOL;
    cog_usage_t next = {0};
    yyjson_val *a = yyjson_obj_get(u, "prompt_tokens"),
               *b = yyjson_obj_get(u, "completion_tokens");
    if (a != NULL || b != NULL)
    {
        if (!yyjson_is_uint(a) || !yyjson_is_uint(b))
            return COG_ERROR_PROTOCOL;
        next.tokens_known = true;
        next.input_tokens = yyjson_get_uint(a);
        next.output_tokens = yyjson_get_uint(b);
    }
    yyjson_val *reason = yyjson_obj_get(
        yyjson_obj_get(u, "completion_tokens_details"), "reasoning_tokens");
    if (reason != NULL)
    {
        if (!yyjson_is_uint(reason))
            return COG_ERROR_PROTOCOL;
        next.reasoning_tokens_known = true;
        next.reasoning_tokens = yyjson_get_uint(reason);
    }
    yyjson_val *cost = yyjson_obj_get(u, "cost");
    if (cost != NULL && !yyjson_is_null(cost))
    {
        if (!yyjson_is_num(cost) || !isfinite(yyjson_get_num(cost)) ||
            yyjson_get_num(cost) < 0.0)
            return COG_ERROR_PROTOCOL;
        next.cost_known = true;
        next.cost = yyjson_get_num(cost);
        next.currency = "USD";
    }
    if (p->usage_final &&
        ((p->usage.tokens_known && next.tokens_known &&
          (p->usage.input_tokens != next.input_tokens ||
           p->usage.output_tokens != next.output_tokens)) ||
         (p->usage.cost_known && next.cost_known &&
          p->usage.cost != next.cost) ||
         (p->usage.reasoning_tokens_known && next.reasoning_tokens_known &&
          p->usage.reasoning_tokens != next.reasoning_tokens)))
        return COG_ERROR_PROTOCOL;
    /* Later snapshots may add availability but must not erase prior facts. */
    if (!next.tokens_known && p->usage.tokens_known)
    {
        next.tokens_known = true;
        next.input_tokens = p->usage.input_tokens;
        next.output_tokens = p->usage.output_tokens;
    }
    if (!next.cost_known && p->usage.cost_known)
    {
        next.cost_known = true;
        next.cost = p->usage.cost;
        next.currency = "USD";
    }
    if (!next.reasoning_tokens_known && p->usage.reasoning_tokens_known)
    {
        next.reasoning_tokens_known = true;
        next.reasoning_tokens = p->usage.reasoning_tokens;
    }
    p->usage = next;
    p->usage_present = true;
    if (p->finish_seen)
        p->usage_final = true;
    return COG_OK;
}
cog_result_t or_decode(or_provider *p, const char *s, size_t n)
{
    yyjson_doc *d = NULL;
    cog_result_t rc = or_json_read(p, s, n, &d);
    if (rc != COG_OK)
        return rc;
    yyjson_val *root = yyjson_doc_get_root(d), *text = NULL;
    rc = COG_ERROR_PROTOCOL;
    if (!yyjson_is_obj(root))
        goto end;
    if (yyjson_obj_get(root, "error") != NULL)
    {
        rc = COG_ERROR_UNAVAILABLE;
        goto end;
    }
    yyjson_val *choices = yyjson_obj_get(root, "choices");
    if (!yyjson_is_arr(choices) || yyjson_arr_size(choices) > 1U)
        goto end;
    yyjson_val *choice = yyjson_arr_get_first(choices);
    if (choice != NULL)
    {
        yyjson_val *idx = yyjson_obj_get(choice, "index");
        if (!yyjson_is_uint(idx) || yyjson_get_uint(idx) != 0U)
            goto end;
        yyjson_val *delta =
            yyjson_obj_get(choice, p->stream ? "delta" : "message");
        if (!yyjson_is_obj(delta))
            goto end;
        yyjson_val *role = yyjson_obj_get(delta, "role");
        if (role != NULL && !yyjson_equals_str(role, "assistant") &&
            !yyjson_equals_str(role, ""))
            goto end;
        text = yyjson_obj_get(delta, "content");
        if (text != NULL && !yyjson_is_null(text) &&
            (!yyjson_is_str(text) ||
             memchr(yyjson_get_str(text), 0, yyjson_get_len(text)) != NULL))
            goto end;
        size_t tn = yyjson_is_str(text) ? yyjson_get_len(text) : 0U;
        yyjson_val *tools = yyjson_obj_get(delta, "tool_calls");
        if (tools != NULL && !yyjson_is_null(tools) && !yyjson_is_arr(tools))
            goto end;
        yyjson_val *opaque = yyjson_obj_get(delta, "reasoning_details");
        if (p->finish_seen &&
            (tn != 0U || yyjson_arr_size(tools) != 0U || opaque != NULL))
            goto end;
        /* Unknown explicit continuation/signature channels are not discarded.
         */
        if (yyjson_obj_get(delta, "reasoning") != NULL ||
            yyjson_obj_get(delta, "signature") != NULL ||
            yyjson_obj_get(delta, "reasoning_content") != NULL)
        {
            rc = COG_ERROR_UNSUPPORTED;
            goto end;
        }
        size_t i, count;
        yyjson_val *call;
        yyjson_arr_foreach(tools, i, count, call)
        {
            yyjson_val *index = yyjson_obj_get(call, "index");
            size_t slot = i;
            if (p->stream)
            {
                if (!yyjson_is_uint(index) ||
                    yyjson_get_uint(index) >= p->config.limits.max_tool_calls)
                    goto end;
                slot = (size_t)yyjson_get_uint(index);
            }
            else if (slot >= p->config.limits.max_tool_calls)
            {
                rc = COG_ERROR_LIMIT;
                goto end;
            }
            or_tool *t = &p->tools[slot];
            yyjson_val *type = yyjson_obj_get(call, "type"),
                       *fn = yyjson_obj_get(call, "function");
            if ((type != NULL && !yyjson_equals_str(type, "function")) ||
                (fn != NULL && !yyjson_is_obj(fn)))
                goto end;
            rc = append_value(yyjson_obj_get(call, "id"), t->id, &t->id_len,
                              p->config.limits.max_tool_id_bytes);
            if (rc != COG_OK)
                goto end;
            rc = append_value(yyjson_obj_get(fn, "name"), t->name, &t->name_len,
                              p->config.limits.max_tool_name_bytes);
            if (rc != COG_OK)
                goto end;
            rc = append_value(yyjson_obj_get(fn, "arguments"), t->arguments,
                              &t->arguments_len,
                              p->config.limits.max_tool_arguments_bytes);
            if (rc != COG_OK)
                goto end;
            t->seen = true;
            rc = COG_ERROR_PROTOCOL;
        }
        if (opaque != NULL)
        {
            rc = opaque_update(p, opaque);
            if (rc != COG_OK)
                goto end;
            rc = COG_ERROR_PROTOCOL;
        }
        yyjson_val *finish = yyjson_obj_get(choice, "finish_reason");
        if (finish != NULL && !yyjson_is_null(finish))
        {
            cog_finish_reason_t f;
            if (yyjson_equals_str(finish, "stop"))
                f = COG_FINISH_REASON_STOP;
            else if (yyjson_equals_str(finish, "tool_calls"))
                f = COG_FINISH_REASON_TOOL_CALLS;
            else if (yyjson_equals_str(finish, "length"))
                f = COG_FINISH_REASON_LENGTH;
            else if (yyjson_equals_str(finish, "content_filter"))
                f = COG_FINISH_REASON_CONTENT_FILTER;
            else
                goto end;
            if (p->finish_seen && p->finish != f)
                goto end;
            p->finish = f;
            p->finish_seen = true;
        }
        if (tn > p->config.limits.max_text_bytes - p->text_len)
        {
            rc = COG_ERROR_LIMIT;
            goto end;
        }
    }
    rc = usage_read(p, yyjson_obj_get(root, "usage"));
    if (rc != COG_OK)
        goto end;
    if (yyjson_is_str(text) && yyjson_get_len(text) != 0U)
    {
        cog_provider_delta_t item = {.kind = COG_PROVIDER_DELTA_TEXT,
                                     .data = yyjson_get_str(text),
                                     .data_len = yyjson_get_len(text)};
        rc = or_emit(p, &item);
        if (rc == COG_OK)
            p->text_len += item.data_len;
    }
    else
        rc = COG_OK;
end:
    yyjson_doc_free(d);
    return rc;
}
cog_result_t or_eof(or_provider *p)
{
    if (!p->finish_seen ||
        (p->stream && (!p->done || p->line_len != 0U || p->frame_len != 0U)))
        return COG_ERROR_PROTOCOL;
    size_t seen = 0U;
    for (size_t i = 0; i < p->config.limits.max_tool_calls; ++i)
    {
        or_tool *t = &p->tools[i];
        if (!t->seen)
            continue;
        ++seen;
        if (t->id_len == 0U || t->name_len == 0U)
            return COG_ERROR_PROTOCOL;
        for (size_t j = 0; j < i; ++j)
            if (p->tools[j].seen && strcmp(t->id, p->tools[j].id) == 0)
                return COG_ERROR_PROTOCOL;
        yyjson_doc *d = NULL;
        cog_result_t rc = or_json_read(p, t->arguments, t->arguments_len, &d);
        if (rc != COG_OK)
            return rc;
        bool object = yyjson_is_obj(yyjson_doc_get_root(d));
        yyjson_doc_free(d);
        if (!object)
            return COG_ERROR_PROTOCOL;
    }
    if ((seen != 0U) != (p->finish == COG_FINISH_REASON_TOOL_CALLS))
        return COG_ERROR_PROTOCOL;
    if (p->opaque_present)
    {
        size_t n = 0U;
        char *s =
            yyjson_mut_write_opts(p->opaque_doc, 0U, &p->writer, &n, NULL);
        if (s == NULL)
            return COG_ERROR_LIMIT;
        if (n >= p->config.limits.max_opaque_bytes)
        {
            p->writer.free(p->writer.ctx, s);
            return COG_ERROR_LIMIT;
        }
        memcpy(p->opaque_bytes, s, n + 1U);
        p->opaque_len = n;
        p->writer.free(p->writer.ctx, s);
    }
    p->finalizing = true;
    p->final_stage = 0U;
    p->final_tool = 0U;
    return COG_OK;
}
cog_result_t or_expand(or_provider *p)
{
    if (p->item_ready || !p->finalizing)
        return COG_OK;
    cog_provider_delta_t item = {0};
    while (p->final_tool < p->config.limits.max_tool_calls)
    {
        size_t i = p->final_tool;
        or_tool *t = &p->tools[i];
        if (!t->seen)
        {
            ++p->final_tool;
            continue;
        }
        item.tool_index = i;
        item.tool_call_id = t->id;
        if (p->final_stage == 0U)
        {
            item.kind = COG_PROVIDER_DELTA_TOOL_BEGIN;
            item.tool_name = t->name;
        }
        else if (p->final_stage == 1U)
        {
            item.kind = COG_PROVIDER_DELTA_TOOL_ARGUMENTS;
            item.data = t->arguments;
            item.data_len = t->arguments_len;
        }
        else
            item.kind = COG_PROVIDER_DELTA_TOOL_END;
        cog_result_t rc = or_emit(p, &item);
        if (rc != COG_OK)
            return rc;
        if (++p->final_stage == 3U)
        {
            p->final_stage = 0U;
            ++p->final_tool;
        }
        return COG_OK;
    }
    if (p->final_stage == 0U)
    {
        p->final_stage = 1U;
        if (p->opaque_present)
        {
            item.kind = COG_PROVIDER_DELTA_OPAQUE;
            item.opaque = (cog_message_opaque_t){
                "openrouter.chat-completions", "json-v1",
                (const unsigned char *)p->opaque_bytes, p->opaque_len};
            return or_emit(p, &item);
        }
    }
    if (p->final_stage == 1U)
    {
        p->final_stage = 2U;
        if (p->usage_present)
        {
            item.kind = COG_PROVIDER_DELTA_USAGE;
            item.usage = p->usage;
            return or_emit(p, &item);
        }
    }
    p->finalizing = false;
    p->terminal = true;
    p->outcome = COG_OK;
    return COG_OK;
}

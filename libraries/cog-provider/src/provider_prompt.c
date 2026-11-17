/** @file provider_prompt.c @brief Transactional, fixed-reservation prompt compiler. */
#include "prompt_internal.h"
#include "provider_prefix.h"
#include <yyjson.h>
#include <math.h>
#include <stdlib.h>

struct cog_provider_prompt_compiler {
    const cog_provider_prompt_template_t *format;
    cog_provider_prompt_limits_t limits;
    unsigned char *lanes[2];
    void *scratch;
    unsigned active;
};

cog_result_t prompt_json(const char *s, size_t n, bool object,
    size_t maximum, void *scratch, size_t capacity)
{
    size_t depth = 0U;
    bool quoted = false, escape = false;
    for (size_t i = 0U; i < n; ++i) {
        const char c = s[i];
        if (quoted) {
            if (escape) escape = false;
            else if (c == '\\') escape = true;
            else if (c == '"') quoted = false;
        } else if (c == '"') quoted = true;
        else if (c == '[' || c == '{') { if (depth == maximum) return COG_ERROR_LIMIT; ++depth; }
        else if (c == ']' || c == '}') { if (depth == 0U) return COG_ERROR_PROTOCOL; --depth; }
    }
    if (quoted || depth != 0U) return COG_ERROR_PROTOCOL;
    yyjson_alc allocator;
    if (!yyjson_alc_pool_init(&allocator, scratch, capacity)) return COG_ERROR_LIMIT;
    yyjson_read_err error;
    yyjson_doc *doc = yyjson_read_opts((char *)(uintptr_t)s, n, 0U, &allocator, &error);
    if (doc == NULL) return error.code == YYJSON_READ_ERROR_MEMORY_ALLOCATION ? COG_ERROR_LIMIT : COG_ERROR_PROTOCOL;
    yyjson_val *root = yyjson_doc_get_root(doc);
    cog_result_t result = object && !yyjson_is_obj(root) ? COG_ERROR_UNSUPPORTED : COG_OK;
    /* The pinned yyjson immutable document stores its values contiguously.
       Scan that private representation instead of recursively walking arbitrary
       input depth on the C stack. Keys count as values and are harmless here. */
    const size_t count = yyjson_doc_get_val_count(doc);
    for (size_t k = 0U; k < count && result == COG_OK; ++k) {
        yyjson_val *v = root + k;
        if (!yyjson_is_obj(v)) continue;
        size_t i, total;
        yyjson_val *key, *value;
        yyjson_obj_foreach(v, i, total, key, value) {
            (void)value;
            size_t j, other_total;
            yyjson_val *other, *other_value;
            yyjson_obj_foreach(v, j, other_total, other, other_value) {
                (void)other_value;
                if (j >= i) break;
                if (yyjson_get_len(key) == yyjson_get_len(other) &&
                    memcmp(yyjson_get_str(key), yyjson_get_str(other), yyjson_get_len(key)) == 0) {
                    result = COG_ERROR_PROTOCOL; break;
                }
            }
            if (result != COG_OK) break;
        }
    }
    yyjson_doc_free(doc);
    return result;
}

cog_result_t prompt_copy_history(prompt_arena_t *a, const cog_provider_request_t *in,
    size_t prefix, cog_provider_request_t *out)
{
    if (in->message_count > SIZE_MAX - prefix) return COG_ERROR_LIMIT;
    cog_provider_request_t result = *in;
    result.model_id = prompt_string(a, in->model_id);
    result.tools_json = NULL; result.tools_json_len = 0U;
    result.message_count += prefix;
    cog_message_t *messages = prompt_array(a, result.message_count, sizeof(*messages));
    if (a->error != COG_OK) return a->error;
    result.messages = messages;
    for (size_t i = 0U; i < prefix; ++i) messages[i] = (cog_message_t){0};
    for (size_t i = 0U; i < in->message_count; ++i) {
        cog_message_t m = in->messages[i];
        m.content = prompt_copy(a, m.content, m.content_len);
        m.name = prompt_string(a, m.name);
        m.tool_call_id = prompt_string(a, m.tool_call_id);
        m.usage.currency = prompt_string(a, m.usage.currency);
        cog_message_tool_call_t *calls = prompt_array(a, m.tool_call_count, sizeof(*calls));
        cog_message_opaque_t *opaque = prompt_array(a, m.opaque_count, sizeof(*opaque));
        if (a->error != COG_OK) return a->error;
        for (size_t j = 0U; j < m.tool_call_count; ++j) {
            calls[j] = m.tool_calls[j];
            calls[j].id = prompt_string(a, calls[j].id);
            calls[j].name = prompt_string(a, calls[j].name);
            calls[j].arguments = prompt_copy(a, calls[j].arguments, calls[j].arguments_len);
        }
        for (size_t j = 0U; j < m.opaque_count; ++j) {
            opaque[j] = m.opaque[j];
            opaque[j].namespace_id = prompt_string(a, opaque[j].namespace_id);
            opaque[j].format = prompt_string(a, opaque[j].format);
            opaque[j].data = (const unsigned char *)prompt_copy(a,
                (const char *)opaque[j].data, opaque[j].size);
        }
        m.tool_calls = calls; m.opaque = opaque;
        messages[i + prefix] = m;
    }
    if (a->error != COG_OK) return a->error;
    *out = result;
    return COG_OK;
}
static bool sized_string(const char *s, size_t limit, bool empty)
{
    if (s == NULL) return false;
    size_t n = 0U;
    while (n < limit && s[n] != '\0') ++n;
    if (n == limit || (!empty && n == 0U)) return false;
    const cog_message_t m = {.role = COG_MESSAGE_ROLE_USER, .content = s, .content_len = n};
    return cog_message_validate(&m) == COG_OK;
}
static bool optional_string(const char *s, size_t limit)
{ return s == NULL || sized_string(s, limit, true); }
static cog_result_t validate_message_input(cog_provider_prompt_compiler_t *c, const cog_message_t *m)
{
    size_t cap = c->limits.max_request_bytes;
    if (m->content_len > cap || m->tool_call_count > cap / sizeof(*m->tool_calls) ||
        m->opaque_count > cap / sizeof(*m->opaque)) return COG_ERROR_LIMIT;
    if ((m->tool_call_count != 0U && m->tool_calls == NULL) ||
        (m->opaque_count != 0U && m->opaque == NULL) ||
        !optional_string(m->name, cap) || !optional_string(m->tool_call_id, cap) ||
        !optional_string(m->usage.currency, cap)) return COG_ERROR_INVALID_ARG;
    for (size_t i = 0U; i < m->tool_call_count; ++i) {
        const cog_message_tool_call_t *t = &m->tool_calls[i];
        if (t->arguments_len > cap) return COG_ERROR_LIMIT;
        if (!sized_string(t->id, cap, false) || !sized_string(t->name, cap, false) ||
            t->arguments == NULL) return COG_ERROR_INVALID_ARG;
        cog_result_t rc = prompt_json(t->arguments, t->arguments_len, true,
            c->limits.max_json_depth, c->scratch, c->limits.scratch_bytes);
        if (rc != COG_OK) return rc;
    }
    for (size_t i = 0U; i < m->opaque_count; ++i) {
        const cog_message_opaque_t *o = &m->opaque[i];
        if (o->size > cap) return COG_ERROR_LIMIT;
        if (!sized_string(o->namespace_id, cap, false) || !sized_string(o->format, cap, false))
            return COG_ERROR_INVALID_ARG;
    }
    return cog_message_validate(m);
}
static cog_result_t validate_input(cog_provider_prompt_compiler_t *c,
    const cog_provider_vtable_t *selected, const cog_provider_prompt_input_t *in)
{
    const cog_prompt_bundle_t *b = &in->prompt;
    const cog_provider_request_t *r = &in->request;
    const size_t cap = c->limits.max_request_bytes;
    if (selected != c->format->provider || b->template_revision != c->format->revision ||
        b->template_id == NULL || strcmp(b->template_id, c->format->id) != 0)
        return COG_ERROR_UNSUPPORTED;
    if (!sized_string(r->model_id, cap, false) || !sized_string(b->model_id, cap, false) ||
        strcmp(r->model_id, b->model_id) != 0 || b->catalog_generation == 0U || b->snapshot_id == 0U ||
        b->max_context_bytes == 0U || b->max_output_tokens == 0U || b->max_tool_rounds == 0U ||
        r->max_tokens == 0U || r->max_tokens > b->max_output_tokens || !isfinite(r->temperature) || r->temperature < 0.0f || r->temperature > 2.0f ||
        r->tools_json != NULL || r->tools_json_len != 0U) return COG_ERROR_INVALID_ARG;
    if (r->message_count > c->limits.max_messages || b->fragment_count > c->limits.max_fragments ||
        b->tool_count > c->limits.max_tools) return COG_ERROR_LIMIT;
    if ((r->message_count != 0U && r->messages == NULL) ||
        (b->fragment_count != 0U && b->fragments == NULL) ||
        (b->tool_count != 0U && b->tools == NULL)) return COG_ERROR_INVALID_ARG;
    for (size_t i = 0U; i < r->message_count; ++i) {
        cog_result_t rc = validate_message_input(c, &r->messages[i]);
        if (rc != COG_OK) return rc;
    }
    for (size_t i = 0U; i < b->fragment_count; ++i) {
        const cog_prompt_fragment_t *f = &b->fragments[i];
        if (f->text_bytes > cap) return COG_ERROR_LIMIT;
        const cog_message_t m = {.role = COG_MESSAGE_ROLE_SYSTEM, .content = f->text, .content_len = f->text_bytes};
        if (f->origin < COG_PROMPT_HOST || f->origin > COG_PROMPT_TOOL_GUIDANCE ||
            !sized_string(f->source_id, cap, false) || !sized_string(f->scope, cap, true) ||
            cog_message_validate(&m) != COG_OK) return COG_ERROR_INVALID_ARG;
    }
    for (size_t i = 0U; i < b->tool_count; ++i) {
        const cog_prompt_tool_binding_t *binding = &b->tools[i];
        const cog_tool_t *t = binding->descriptor;
        if (t == NULL || binding->policy < COG_PROMPT_TOOL_ALLOW || binding->policy > COG_PROMPT_TOOL_DENY ||
            !sized_string(t->name, cap, false) || !optional_string(t->description, cap)) return COG_ERROR_INVALID_ARG;
        for (size_t j = 0U; j < i; ++j)
            if (strcmp(t->name, b->tools[j].descriptor->name) == 0) return COG_ERROR_INVALID_ARG;
        if (binding->policy == COG_PROMPT_TOOL_DENY) continue;
        if (t->input_schema == NULL) return COG_ERROR_UNSUPPORTED;
        if (!sized_string(t->input_schema, cap, false)) return COG_ERROR_INVALID_ARG;
        cog_result_t rc = prompt_json(t->input_schema, strlen(t->input_schema), true,
            c->limits.max_json_depth, c->scratch, c->limits.scratch_bytes);
        if (rc != COG_OK) return rc;
    }
    return COG_OK;
}
static bool same_string(const char *a, const char *b)
{ return a == NULL || b == NULL ? a == b : strcmp(a, b) == 0; }
static bool same_bytes(const void *a, const void *b, size_t n)
{ return n == 0U || (a != NULL && b != NULL && memcmp(a, b, n) == 0); }
static bool same_usage(const cog_usage_t *a, const cog_usage_t *b)
{
    return a->tokens_known == b->tokens_known && a->input_tokens == b->input_tokens &&
        a->output_tokens == b->output_tokens && a->reasoning_tokens_known == b->reasoning_tokens_known &&
        a->reasoning_tokens == b->reasoning_tokens && a->cost_known == b->cost_known &&
        memcmp(&a->cost, &b->cost, sizeof(a->cost)) == 0 && same_string(a->currency, b->currency);
}
static bool same_message(const cog_message_t *a, const cog_message_t *b)
{
    if (a->role != b->role || a->status != b->status || a->content_len != b->content_len ||
        !same_bytes(a->content, b->content, a->content_len) || !same_string(a->name, b->name) ||
        !same_string(a->tool_call_id, b->tool_call_id) || a->tool_call_count != b->tool_call_count ||
        a->opaque_count != b->opaque_count || !same_usage(&a->usage, &b->usage)) return false;
    for (size_t i = 0U; i < a->tool_call_count; ++i) {
        const cog_message_tool_call_t *x = &a->tool_calls[i], *y = &b->tool_calls[i];
        if (!same_string(x->id, y->id) || !same_string(x->name, y->name) ||
            x->arguments_len != y->arguments_len || !same_bytes(x->arguments, y->arguments, x->arguments_len)) return false;
    }
    for (size_t i = 0U; i < a->opaque_count; ++i) {
        const cog_message_opaque_t *x = &a->opaque[i], *y = &b->opaque[i];
        if (!same_string(x->namespace_id, y->namespace_id) || !same_string(x->format, y->format) ||
            x->size != y->size || !same_bytes(x->data, y->data, x->size)) return false;
    }
    return true;
}
static bool in_range(const prompt_arena_t *a, const void *p, size_t bytes, size_t alignment)
{
    if (p == NULL) return bytes == 0U;
    uintptr_t start = (uintptr_t)a->data, value = (uintptr_t)p;
    if (value < start || value % alignment != 0U || value - start > a->capacity) return false;
    return bytes <= a->capacity - (size_t)(value - start);
}
static bool add_sum(size_t *sum, size_t n)
{ if (n > SIZE_MAX - *sum) return false; *sum += n; return true; }
static bool output_string(const prompt_arena_t *a, const char *s, size_t *sum)
{
    if (s == NULL) return true;
    if (!in_range(a, s, 1U, 1U)) return false;
    size_t left = a->capacity - (size_t)((uintptr_t)s - (uintptr_t)a->data);
    const char *end = memchr(s, '\0', left);
    return end != NULL && add_sum(sum, (size_t)(end - s));
}
static bool output_blob(const prompt_arena_t *a, const void *s, size_t n, size_t *sum)
{ return in_range(a, s, n, 1U) && add_sum(sum, n); }
static bool output_message(const prompt_arena_t *a, const cog_message_t *m, size_t *sum)
{
    if (!output_blob(a, m->content, m->content_len, sum) || !output_string(a, m->name, sum) ||
        !output_string(a, m->tool_call_id, sum) || !output_string(a, m->usage.currency, sum) ||
        m->tool_call_count > a->capacity / sizeof(*m->tool_calls) ||
        m->opaque_count > a->capacity / sizeof(*m->opaque) ||
        !in_range(a, m->tool_calls, m->tool_call_count * sizeof(*m->tool_calls), alignof(cog_message_tool_call_t)) ||
        !in_range(a, m->opaque, m->opaque_count * sizeof(*m->opaque), alignof(cog_message_opaque_t))) return false;
    for (size_t j = 0U; j < m->tool_call_count; ++j) {
        const cog_message_tool_call_t *t = &m->tool_calls[j];
        if (!output_string(a, t->id, sum) || !output_string(a, t->name, sum) ||
            !output_blob(a, t->arguments, t->arguments_len, sum)) return false;
    }
    for (size_t j = 0U; j < m->opaque_count; ++j) {
        const cog_message_opaque_t *o = &m->opaque[j];
        if (!output_string(a, o->namespace_id, sum) || !output_string(a, o->format, sum) ||
            !output_blob(a, o->data, o->size, sum)) return false;
    }
    return cog_message_validate(m) == COG_OK;
}
static cog_result_t validate_output(cog_provider_prompt_compiler_t *c,
    unsigned lane, const cog_provider_prompt_input_t *in, const cog_provider_request_t *out)
{
    prompt_arena_t a = {.data = c->lanes[lane], .capacity = c->limits.max_request_bytes};
    size_t sum = 0U;
    if (out->message_count < in->request.message_count || out->message_count > c->limits.max_messages ||
        out->message_count > a.capacity / sizeof(*out->messages) ||
        !in_range(&a, out->messages, out->message_count * sizeof(*out->messages), alignof(cog_message_t)) ||
        !output_string(&a, out->model_id, &sum) || out->model_id == NULL ||
        !output_blob(&a, out->tools_json, out->tools_json_len, &sum)) return COG_ERROR_PROTOCOL;
    if (!same_string(out->model_id, in->request.model_id) || out->stream != in->request.stream ||
        out->temperature != in->request.temperature || out->max_tokens != in->request.max_tokens ||
        out->configuration_generation != in->request.configuration_generation) return COG_ERROR_PROTOCOL;
    const size_t prefix = out->message_count - in->request.message_count;
    if (in->prompt.fragment_count == 0U && prefix != 0U) return COG_ERROR_PROTOCOL;
    for (size_t i = 0U; i < out->message_count; ++i) {
        const cog_message_t *m = &out->messages[i];
        if (!output_message(&a, m, &sum)) return COG_ERROR_PROTOCOL;
        if (i < prefix) {
            if (m->role != COG_MESSAGE_ROLE_SYSTEM || m->status != COG_MESSAGE_COMPLETE ||
                m->tool_call_count != 0U || m->opaque_count != 0U) return COG_ERROR_PROTOCOL;
        } else if (!same_message(m, &in->request.messages[i - prefix])) return COG_ERROR_PROTOCOL;
    }
    if (sum > c->limits.max_context_bytes || sum > in->prompt.max_context_bytes) return COG_ERROR_LIMIT;
    if (out->tools_json != NULL) {
        cog_result_t rc = prompt_json(out->tools_json, out->tools_json_len, false,
            c->limits.max_json_depth, c->scratch, c->limits.scratch_bytes);
        if (rc != COG_OK) return rc;
    }
    return COG_OK;
}

cog_result_t cog_provider_prompt_compiler_new(cog_provider_prompt_compiler_t **out,
    const cog_provider_prompt_template_t *format, const cog_provider_prompt_limits_t *limits)
{
    if (out == NULL) return COG_ERROR_NULL_PTR;
    *out = NULL;
    if (format == NULL || limits == NULL) return COG_ERROR_NULL_PTR;
    if (format->id == NULL || format->id[0] == '\0' || format->revision == 0U ||
        format->provider == NULL || format->render == NULL || limits->max_messages == 0U ||
        limits->max_fragments == 0U || limits->max_tools == 0U || limits->max_json_depth == 0U ||
        limits->max_context_bytes == 0U || limits->max_request_bytes == 0U ||
        limits->scratch_bytes == 0U || limits->scratch_bytes < format->required_scratch_bytes)
        return COG_ERROR_INVALID_ARG;
    if (limits->max_request_bytes > (SIZE_MAX - sizeof(cog_provider_prompt_compiler_t)) / 2U ||
        limits->scratch_bytes > SIZE_MAX - sizeof(cog_provider_prompt_compiler_t) - limits->max_request_bytes * 2U)
        return COG_ERROR_LIMIT;
    cog_provider_prompt_compiler_t *c = calloc(1U, sizeof(*c));
    if (c == NULL) return COG_ERROR_NOMEM;
    c->format = format; c->limits = *limits;
    c->lanes[0] = malloc(limits->max_request_bytes);
    c->lanes[1] = malloc(limits->max_request_bytes);
    c->scratch = malloc(limits->scratch_bytes);
    if (c->lanes[0] == NULL || c->lanes[1] == NULL || c->scratch == NULL) {
        (void)cog_provider_prompt_compiler_free(c); return COG_ERROR_NOMEM;
    }
    *out = c;
    return COG_OK;
}
cog_result_t cog_provider_prompt_compile(cog_provider_prompt_compiler_t *c,
    const cog_provider_vtable_t *selected, const cog_provider_prompt_input_t *input,
    cog_provider_request_t *request)
{
    if (c == NULL || selected == NULL || input == NULL || request == NULL) return COG_ERROR_NULL_PTR;
    cog_result_t rc = validate_input(c, selected, input);
    if (rc != COG_OK) return rc;
    const unsigned lane = c->active ^ 1U;
    cog_provider_request_t candidate = {0};
    rc = c->format->render(input, &c->limits, c->scratch, c->limits.scratch_bytes,
        c->lanes[lane], c->limits.max_request_bytes, &candidate);
    if (rc != COG_OK) return rc;
    rc = validate_output(c, lane, input, &candidate);
    if (rc != COG_OK) return rc;
    c->active = lane;
    *request = candidate;
    return COG_OK;
}
cog_result_t cog_provider_prompt_compiler_free(cog_provider_prompt_compiler_t *c)
{
    if (c != NULL) {
        free(c->scratch); free(c->lanes[0]); free(c->lanes[1]); free(c);
    }
    return COG_OK;
}

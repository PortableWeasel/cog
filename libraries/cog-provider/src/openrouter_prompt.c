/** @file openrouter_prompt.c @brief Pure bounded Chat Completions template. */
#include "prompt_internal.h"
#include <cog/openrouter.h>

typedef struct prompt_writer { prompt_arena_t *arena; } prompt_writer_t;
static void emit(prompt_writer_t *w, const char *s, size_t n)
{
    void *out = prompt_reserve(w->arena, n, 1U);
    if (out != NULL && n != 0U) memcpy(out, s, n);
}
static void literal(prompt_writer_t *w, const char *s) { emit(w, s, strlen(s)); }
static void quoted(prompt_writer_t *w, const char *s)
{
    static const char hex[] = "0123456789abcdef";
    literal(w, "\"");
    for (const unsigned char *p = (const unsigned char *)s; *p != 0U; ++p) {
        unsigned char c = *p;
        if (c == '"' || c == '\\') { char v[2] = {'\\', (char)c}; emit(w, v, 2U); }
        else if (c < 32U) {
            char v[6] = {'\\','u','0','0',hex[c >> 4U],hex[c & 15U]}; emit(w, v, 6U);
        } else { char v = (char)c; emit(w, &v, 1U); }
    }
    literal(w, "\"");
}
static cog_result_t render(const cog_provider_prompt_input_t *input,
    const cog_provider_prompt_limits_t *limits, void *scratch, size_t scratch_capacity,
    void *storage, size_t capacity, cog_provider_request_t *out)
{
    (void)scratch; (void)scratch_capacity;
    prompt_arena_t a = {.data = storage, .capacity = capacity};
    cog_provider_request_t request;
    const size_t prefix = input->prompt.fragment_count == 0U ? 0U : 1U;
    if (prefix > limits->max_messages - input->request.message_count) return COG_ERROR_LIMIT;
    cog_result_t rc = prompt_copy_history(&a, &input->request, prefix, &request);
    if (rc != COG_OK) return rc;
    prompt_writer_t writer = {.arena = &a};
    if (prefix != 0U) {
        static const char *const origins[] = {
            "host", "workspace", "agent", "skill_catalog", "skill", "resource", "tool_guidance"};
        const size_t start = a.used;
        for (size_t i = 0U; i < input->prompt.fragment_count; ++i) {
            const cog_prompt_fragment_t *f = &input->prompt.fragments[i];
            literal(&writer, "[COG "); literal(&writer, origins[(size_t)f->origin]);
            literal(&writer, " source="); quoted(&writer, f->source_id);
            literal(&writer, " scope="); quoted(&writer, f->scope);
            literal(&writer, "]\n"); emit(&writer, f->text, f->text_bytes);
            literal(&writer, "\n[/COG]\n");
        }
        const size_t bytes = a.used - start;
        literal(&writer, "");
        char *zero = prompt_reserve(&a, 1U, 1U);
        if (zero != NULL) *zero = '\0';
        if (a.error != COG_OK) return a.error;
        cog_message_t *messages = (cog_message_t *)(void *)request.messages;
        messages[0] = (cog_message_t){.role = COG_MESSAGE_ROLE_SYSTEM,
            .content = (const char *)a.data + start, .content_len = bytes};
    }
    size_t enabled = 0U;
    for (size_t i = 0U; i < input->prompt.tool_count; ++i)
        if (input->prompt.tools[i].policy != COG_PROMPT_TOOL_DENY) ++enabled;
    if (enabled != 0U) {
        const size_t start = a.used;
        literal(&writer, "[");
        size_t emitted = 0U;
        for (size_t i = 0U; i < input->prompt.tool_count; ++i) {
            if (input->prompt.tools[i].policy == COG_PROMPT_TOOL_DENY) continue;
            const cog_tool_t *tool = input->prompt.tools[i].descriptor;
            if (emitted++ != 0U) literal(&writer, ",");
            literal(&writer, "{\"type\":\"function\",\"function\":{\"name\":");
            quoted(&writer, tool->name);
            if (tool->description != NULL) {
                literal(&writer, ",\"description\":"); quoted(&writer, tool->description);
            }
            literal(&writer, ",\"parameters\":"); literal(&writer, tool->input_schema);
            literal(&writer, "}}");
        }
        literal(&writer, "]");
        request.tools_json_len = a.used - start;
        request.tools_json = (const char *)a.data + start;
        char *zero = prompt_reserve(&a, 1U, 1U);
        if (zero != NULL) *zero = '\0';
    }
    if (a.error != COG_OK) return a.error;
    *out = request;
    return COG_OK;
}
const cog_provider_prompt_template_t cog_openrouter_prompt_template = {
    .id = COG_OPENROUTER_PROMPT_TEMPLATE_ID,
    .revision = COG_OPENROUTER_PROMPT_TEMPLATE_REVISION,
    .provider = &cog_openrouter_provider_vtable,
    .required_scratch_bytes = 1U,
    .render = render
};

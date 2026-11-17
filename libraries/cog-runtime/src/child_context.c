/** @file child_context.c @brief Bounded escaped child SYSTEM provenance. */
#include "child_profile.h"
#include "codec_internal.h"

static void decimal(cli_wire_writer_t *w, uint64_t value)
{
    char digits[20];
    size_t n = sizeof(digits);
    do { digits[--n] = (char)('0' + value % 10U); value /= 10U; } while (value != 0U);
    cli_wire_bytes(w, digits + n, sizeof(digits) - n);
}
static void quoted_number(cli_wire_writer_t *w, uint64_t value)
{
    cli_wire_bytes(w, "\"", 1U); decimal(w, value); cli_wire_bytes(w, "\"", 1U);
}
static void json_string(cli_wire_writer_t *w, const char *text, size_t length)
{
    static const char hex[] = "0123456789abcdef";
    cli_wire_bytes(w, "\"", 1U);
    for (size_t i = 0U; i < length; ++i) {
        unsigned char c = (unsigned char)text[i];
        if (c < 32U) {
            char escaped[] = {'\\', 'u', '0', '0', hex[c >> 4U], hex[c & 15U]};
            cli_wire_bytes(w, escaped, sizeof(escaped));
        } else {
            if (c == '"' || c == '\\') cli_wire_bytes(w, "\\", 1U);
            cli_wire_bytes(w, text + i, 1U);
        }
    }
    cli_wire_bytes(w, "\"", 1U);
}
static void context_write(cli_wire_writer_t *w, const cog_cli_child_context_t *c, size_t sid, size_t call)
{
    static const char prefix[] = "{\"cog_child_context\":1,\"parent_session\":";
    cli_wire_bytes(w, prefix, sizeof(prefix) - 1U); json_string(w, c->parent_session_id, sid);
    cli_wire_bytes(w, ",\"parent_node\":", 15U); quoted_number(w, c->parent_node);
    cli_wire_bytes(w, ",\"parent_call\":", 15U); json_string(w, c->parent_call_id, call);
    cli_wire_bytes(w, ",\"parent_ordinal\":", 18U); decimal(w, c->parent_call_ordinal);
    cli_wire_bytes(w, ",\"runtime\":", 11U); quoted_number(w, c->runtime_incarnation);
    cli_wire_bytes(w, ",\"group\":", 9U); quoted_number(w, c->group_id);
    cli_wire_bytes(w, ",\"agent\":", 9U); quoted_number(w, c->agent_id);
    cli_wire_bytes(w, ",\"attempt\":", 11U); quoted_number(w, c->execution_epoch);
    cli_wire_bytes(w, ",\"configuration\":", 17U); quoted_number(w, c->configuration_generation);
    cli_wire_bytes(w, ",\"members\":[", 12U);
    for (size_t i = 0U; i < c->member_count; ++i) {
        if (i != 0U) cli_wire_bytes(w, ",", 1U);
        quoted_number(w, c->members[i]);
    }
    cli_wire_bytes(w, "]}", 2U);
}
cog_result_t cog_cli_child_context_encode(const cog_cli_child_context_t *context,
    char *output, size_t capacity, size_t *required)
{
    if (context == NULL || required == NULL || (output == NULL && capacity != 0U)) return COG_ERROR_NULL_PTR;
    size_t sid, call;
    if (!cli_wire_cstr(context->parent_session_id, 127U, &sid) || sid == 0U ||
        !cli_wire_cstr(context->parent_call_id, 255U, &call) || call == 0U ||
        context->parent_node == 0U || context->runtime_incarnation == 0U || context->group_id == 0U ||
        context->agent_id == 0U || context->execution_epoch == 0U || context->configuration_generation == 0U ||
        context->parent_call_ordinal > 15U || context->member_count == 0U ||
        context->member_count > 16U || context->members == NULL) return COG_ERROR_INVALID_ARG;
    bool self = false;
    for (size_t i = 0U; i < context->member_count; ++i) {
        if (context->members[i] == 0U || (i != 0U && context->members[i] <= context->members[i - 1U])) return COG_ERROR_INVALID_ARG;
        if (context->members[i] == context->agent_id) self = true;
    }
    if (!self) return COG_ERROR_INVALID_ARG;
    const cli_wire_region_t writes[] = {{output, capacity}, {required, sizeof(*required)}};
    const cli_wire_region_t reads[] = {{context, sizeof(*context)}, {context->parent_session_id, sid + 1U},
        {context->parent_call_id, call + 1U}, {context->members, context->member_count * sizeof(*context->members)}};
    if (!cli_wire_regions(writes, 2U)) return COG_ERROR_INVALID_ARG;
    for (size_t i = 0U; i < 2U; ++i) for (size_t j = 0U; j < 4U; ++j)
        if (!cli_wire_disjoint(writes[i].bytes, writes[i].length, reads[j].bytes, reads[j].length)) return COG_ERROR_INVALID_ARG;
    cli_wire_writer_t w = {NULL, 0U, true}; context_write(&w, context, sid, call);
    if (!w.valid || w.used > 4095U) return COG_ERROR_LIMIT;
    *required = w.used + 1U;
    if (capacity < *required) return COG_ERROR_OVERFLOW;
    w = (cli_wire_writer_t){(unsigned char *)output, 0U, true}; context_write(&w, context, sid, call);
    output[w.used] = '\0';
    return COG_OK;
}

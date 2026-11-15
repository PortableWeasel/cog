/** @file message.c
 * @brief Allocation-free provider-neutral structural message validation.
 */
#include "cog/message.h"
#include <math.h>
#include <stdint.h>
#include <string.h>

/* Reject overlong encodings, surrogates, NUL and codepoints above U+10FFFF.
 * Explicit length bounds every access; protocol JSON/schema stays downstream. */
static bool utf8(const char *text, size_t length)
{
    size_t i = 0U;
    if (text == NULL) return length == 0U;
    while (i < length) {
        const unsigned char first = (unsigned char)text[i++];
        uint32_t code;
        size_t rest;
        uint32_t minimum;
        if (first == 0U) return false;
        if (first < 0x80U) continue;
        if (first >= 0xc2U && first <= 0xdfU) { rest = 1U; code = first & 0x1fU; minimum = 0x80U; }
        else if (first >= 0xe0U && first <= 0xefU) { rest = 2U; code = first & 0x0fU; minimum = 0x800U; }
        else if (first >= 0xf0U && first <= 0xf4U) { rest = 3U; code = first & 0x07U; minimum = 0x10000U; }
        else return false;
        if (rest > length - i) return false;
        for (size_t k = 0U; k < rest; ++k) {
            const unsigned char c = (unsigned char)text[i++];
            if ((c & 0xc0U) != 0x80U) return false;
            code = (code << 6U) | (c & 0x3fU);
        }
        if (code < minimum || code > 0x10ffffU || (code >= 0xd800U && code <= 0xdfffU)) return false;
    }
    return true;
}

static bool optional_string(const char *text)
{
    return text == NULL || utf8(text, strlen(text));
}
static bool nonempty_string(const char *text)
{
    return text != NULL && text[0] != '\0' && utf8(text, strlen(text));
}

cog_result_t cog_message_validate(const cog_message_t *m)
{
    if (m == NULL) return COG_ERROR_NULL_PTR;
    if (m->role < COG_MESSAGE_ROLE_SYSTEM || m->role > COG_MESSAGE_ROLE_TOOL ||
        m->status < COG_MESSAGE_COMPLETE || m->status > COG_MESSAGE_FAILED ||
        !utf8(m->content, m->content_len) || !optional_string(m->name) ||
        (m->tool_calls == NULL && m->tool_call_count != 0U) ||
        (m->opaque == NULL && m->opaque_count != 0U) ||
        m->tool_call_count > SIZE_MAX / sizeof(*m->tool_calls) ||
        m->opaque_count > SIZE_MAX / sizeof(*m->opaque)) return COG_ERROR_INVALID_ARG;
    if (m->role != COG_MESSAGE_ROLE_ASSISTANT && (m->tool_call_count != 0U || m->opaque_count != 0U))
        return COG_ERROR_INVALID_ARG;
    if (m->role == COG_MESSAGE_ROLE_TOOL) {
        if (!nonempty_string(m->tool_call_id)) return COG_ERROR_INVALID_ARG;
    } else if (m->tool_call_id != NULL) return COG_ERROR_INVALID_ARG;
    for (size_t i = 0U; i < m->tool_call_count; ++i) {
        const cog_message_tool_call_t *c = &m->tool_calls[i];
        if (!nonempty_string(c->id) || !nonempty_string(c->name) ||
            c->arguments == NULL || !utf8(c->arguments, c->arguments_len)) return COG_ERROR_INVALID_ARG;
        for (size_t k = 0U; k < i; ++k)
            if (strcmp(c->id, m->tool_calls[k].id) == 0) return COG_ERROR_INVALID_ARG;
    }
    for (size_t i = 0U; i < m->opaque_count; ++i) {
        const cog_message_opaque_t *o = &m->opaque[i];
        if (!nonempty_string(o->namespace_id) || !nonempty_string(o->format) ||
            (o->data == NULL && o->size != 0U)) return COG_ERROR_INVALID_ARG;
    }
    if (!optional_string(m->usage.currency) || (m->usage.cost_known &&
        (!isfinite(m->usage.cost) || m->usage.cost < 0.0 || !nonempty_string(m->usage.currency))))
        return COG_ERROR_INVALID_ARG;
    return COG_OK;
}

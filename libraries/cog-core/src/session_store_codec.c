/** @file session_store_codec.c
 * @brief Bounded explicit-endian persistence records and legacy migration.
 */
#define _POSIX_C_SOURCE 200809L
#include "session_store_codec.h"
#include <ctype.h>
#include <errno.h>
#include <float.h>
#include <math.h>
#include <stdalign.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* IEEE binary64 is part of this version's wire profile, never native ABI. */
_Static_assert(sizeof(double) == sizeof(uint64_t), "v2 requires binary64 double");
_Static_assert(DBL_MANT_DIG == 53 && DBL_MAX_EXP == 1024, "v2 requires IEEE binary64");

typedef struct wire {
    unsigned char *output;
    const unsigned char *input;
    size_t capacity;
    size_t at;
    cog_store_codec_workspace_t *scratch;
    cog_result_t error;
} wire_t;

static void *scratch(cog_store_codec_workspace_t *w, size_t size)
{
    const size_t a = alignof(max_align_t);
    if (w->used > SIZE_MAX - (a - 1U)) return NULL;
    const size_t at = (w->used + a - 1U) / a * a;
    if (at > w->capacity || size > w->capacity - at) return NULL;
    void *p = w->data + at;
    (void)memset(p, 0, size);
    w->used = at + size;
    return p;
}

static void bytes(wire_t *w, void *value, size_t size)
{
    if (w->error != COG_OK) return;
    if (size > w->capacity - w->at) {
        w->error = w->input == NULL ? COG_ERROR_LIMIT : COG_ERROR_PROTOCOL;
        return;
    }
    if (size != 0U) {
        if (w->input != NULL) (void)memcpy(value, w->input + w->at, size);
        else (void)memcpy(w->output + w->at, value, size);
    }
    w->at += size;
}

static void u64(wire_t *w, uint64_t *value)
{
    unsigned char data[8] = {0};
    if (w->input == NULL)
        for (size_t k = 0U; k < 8U; ++k) data[k] = (unsigned char)(*value >> (k * 8U));
    bytes(w, data, sizeof(data));
    if (w->input != NULL && w->error == COG_OK) {
        *value = 0U;
        for (size_t k = 0U; k < 8U; ++k) *value |= (uint64_t)data[k] << (k * 8U);
    }
}
static void boolean(wire_t *w, bool *value)
{
    unsigned char v = *value ? 1U : 0U;
    bytes(w, &v, 1U);
    if (v > 1U) w->error = COG_ERROR_PROTOCOL;
    if (w->input != NULL) *value = v != 0U;
}
static void number(wire_t *w, double *value)
{
    uint64_t bits = 0U;
    if (w->input == NULL) (void)memcpy(&bits, value, sizeof(bits));
    u64(w, &bits);
    if (w->input != NULL) (void)memcpy(value, &bits, sizeof(bits));
}
static void count(wire_t *w, size_t *value)
{
    uint64_t v = (uint64_t)*value;
    u64(w, &v);
    if (v > SIZE_MAX || (w->input != NULL && v > w->capacity - w->at)) w->error = COG_ERROR_PROTOCOL;
    if (w->error == COG_OK) *value = (size_t)v;
}
static void *array(wire_t *w, size_t count_, size_t element)
{
    if (w->error != COG_OK) return NULL;
    if (count_ > SIZE_MAX / element) { w->error = COG_ERROR_LIMIT; return NULL; }
    void *p = scratch(w->scratch, count_ * element);
    if (p == NULL) w->error = COG_ERROR_LIMIT;
    return p;
}

/* Presence is explicit: NULL differs from a present empty byte/string value.
 * A terminator is wire data and checked, never read beyond length. */
static void blob(wire_t *w, const unsigned char **data, size_t *length)
{
    bool present = *data != NULL;
    boolean(w, &present);
    if (!present) {
        if (w->input != NULL) { *data = NULL; *length = 0U; }
        else if (*length != 0U) w->error = COG_ERROR_PROTOCOL;
        return;
    }
    count(w, length);
    if (w->error != COG_OK) return;
    if (*length >= w->capacity - w->at) { w->error = w->input == NULL ? COG_ERROR_LIMIT : COG_ERROR_PROTOCOL; return; }
    if (w->input != NULL) {
        *data = w->input + w->at;
        w->at += *length;
    } else {
        if (*length != 0U) (void)memcpy(w->output + w->at, *data, *length);
        w->at += *length;
    }
    unsigned char zero = 0U;
    bytes(w, &zero, 1U);
    if (zero != 0U) w->error = COG_ERROR_PROTOCOL;
}
static void text(wire_t *w, const char **data, size_t *length)
{
    const unsigned char *p = (const unsigned char *)*data;
    blob(w, &p, length);
    if (w->input != NULL) *data = (const char *)p;
}
static void string(wire_t *w, const char **value)
{
    size_t length = w->input == NULL && *value != NULL ? strlen(*value) : 0U;
    text(w, value, &length);
    if (w->input != NULL && w->error == COG_OK && *value != NULL && memchr(*value, '\0', length) != NULL)
        w->error = COG_ERROR_PROTOCOL;
}

static void usage(wire_t *w, cog_usage_t *u)
{
    boolean(w, &u->tokens_known); u64(w, &u->input_tokens); u64(w, &u->output_tokens);
    boolean(w, &u->reasoning_tokens_known); u64(w, &u->reasoning_tokens);
    boolean(w, &u->cost_known); number(w, &u->cost); string(w, &u->currency);
}
static void message(wire_t *w, cog_message_t *m)
{
    uint64_t role = (uint64_t)m->role, status = (uint64_t)m->status;
    u64(w, &role); u64(w, &status);
    if (role > COG_MESSAGE_ROLE_TOOL || status > COG_MESSAGE_FAILED) { w->error = COG_ERROR_PROTOCOL; return; }
    m->role = (cog_message_role_t)role; m->status = (cog_message_status_t)status;
    text(w, &m->content, &m->content_len); string(w, &m->name); string(w, &m->tool_call_id);
    usage(w, &m->usage);
    count(w, &m->tool_call_count);
    cog_message_tool_call_t *calls = w->input == NULL ? NULL : array(w, m->tool_call_count, sizeof(*calls));
    if (w->input != NULL) m->tool_calls = calls;
    if (w->error != COG_OK) return;
    if (m->tool_call_count != 0U && m->tool_calls == NULL) { w->error = COG_ERROR_PROTOCOL; return; }
    for (size_t k = 0U; k < m->tool_call_count && w->error == COG_OK; ++k) {
        cog_message_tool_call_t c = w->input == NULL ? m->tool_calls[k] : (cog_message_tool_call_t){0};
        string(w, &c.id); string(w, &c.name); text(w, &c.arguments, &c.arguments_len);
        if (w->input != NULL) calls[k] = c;
    }
    count(w, &m->opaque_count);
    cog_message_opaque_t *opaque = w->input == NULL ? NULL : array(w, m->opaque_count, sizeof(*opaque));
    if (w->input != NULL) m->opaque = opaque;
    if (w->error != COG_OK) return;
    if (m->opaque_count != 0U && m->opaque == NULL) { w->error = COG_ERROR_PROTOCOL; return; }
    for (size_t k = 0U; k < m->opaque_count && w->error == COG_OK; ++k) {
        cog_message_opaque_t o = w->input == NULL ? m->opaque[k] : (cog_message_opaque_t){0};
        string(w, &o.namespace_id); string(w, &o.format); blob(w, &o.data, &o.size);
        if (w->input != NULL) opaque[k] = o;
    }
    if (w->error == COG_OK && cog_message_validate(m) != COG_OK) w->error = COG_ERROR_PROTOCOL;
}

static void node(wire_t *w, cog_node_data_t *n)
{
    uint64_t role = (uint64_t)n->role;
    u64(w, &n->node_id); u64(w, &n->parent_id); u64(w, &role);
    if (role > COG_NODE_TOOL_RESULT) { w->error = COG_ERROR_PROTOCOL; return; }
    n->role = (cog_node_role_t)role;
    text(w, &n->content, &n->content_len);
    string(w, &n->tool_name); string(w, &n->tool_args); string(w, &n->tool_result);
    string(w, &n->raw_request); string(w, &n->raw_response);
    uint64_t chunks = (uint64_t)n->chunk_count;
    u64(w, &chunks);
    if (chunks > SIZE_MAX) w->error = COG_ERROR_PROTOCOL;
    n->chunk_count = (size_t)chunks;
    u64(w, &n->timing.request_sent_ms); u64(w, &n->timing.first_token_ms);
    u64(w, &n->timing.last_token_ms); u64(w, &n->timing.duration_ms);
    u64(w, &n->timing.ttft_ms); number(w, &n->timing.tokens_per_second);
    u64(w, &n->cost.input_tokens); u64(w, &n->cost.output_tokens);
    number(w, &n->cost.cost_per_input_token); number(w, &n->cost.cost_per_output_token);
    number(w, &n->cost.total_cost); boolean(w, &n->cost.tokens_known); boolean(w, &n->cost.total_cost_known);
    string(w, &n->model); string(w, &n->provider);
    bool present = n->message != NULL;
    boolean(w, &present);
    if (!present || w->error != COG_OK) return;
    cog_message_t m = w->input == NULL ? *n->message : (cog_message_t){0};
    message(w, &m);
    if (w->input != NULL && w->error == COG_OK) {
        cog_message_t *owned = array(w, 1U, sizeof(*owned));
        if (owned != NULL) { *owned = m; n->message = owned; }
    }
}
static void audit(wire_t *w, cog_tool_audit_record_t *a)
{
    uint64_t previous = (uint64_t)a->previous, next = (uint64_t)a->next;
    u64(w, &a->sequence); u64(w, &previous); u64(w, &next);
    if (previous > COG_TOOL_EXECUTION_ACKNOWLEDGED || next > COG_TOOL_EXECUTION_ACKNOWLEDGED) {
        w->error = COG_ERROR_PROTOCOL; return;
    }
    a->previous = (cog_tool_execution_state_t)previous; a->next = (cog_tool_execution_state_t)next;
    boolean(w, &a->explicit_reconciliation); text(w, &a->evidence, &a->evidence_len);
}
static void ledger(wire_t *w, cog_tool_ledger_record_t *l)
{
    uint64_t state = (uint64_t)l->state;
    u64(w, &l->assistant_node_id); string(w, &l->call_id); u64(w, &state);
    if (state > COG_TOOL_EXECUTION_ACKNOWLEDGED) { w->error = COG_ERROR_PROTOCOL; return; }
    l->state = (cog_tool_execution_state_t)state;
    count(w, &l->audit_count);
    cog_tool_audit_record_t *records = w->input == NULL ? NULL : array(w, l->audit_count, sizeof(*records));
    if (w->input != NULL) l->audit = records;
    if (w->error != COG_OK) return;
    if (l->audit_count != 0U && l->audit == NULL) { w->error = COG_ERROR_PROTOCOL; return; }
    for (size_t k = 0U; k < l->audit_count && w->error == COG_OK; ++k) {
        cog_tool_audit_record_t a = w->input == NULL ? l->audit[k] : (cog_tool_audit_record_t){0};
        audit(w, &a); if (w->input != NULL) records[k] = a;
    }
}
static void snapshot(wire_t *w, cog_session_snapshot_t *s)
{
    unsigned char magic[8] = {'C','O','G','S','N','A','P','2'};
    const unsigned char expected[8] = {'C','O','G','S','N','A','P','2'};
    bytes(w, magic, sizeof(magic));
    if (w->error != COG_OK) return;
    if (memcmp(magic, expected, sizeof(magic)) != 0) { w->error = COG_ERROR_UNSUPPORTED; return; }
    string(w, &s->session_id); string(w, &s->parent_session_id);
    u64(w, &s->active_node_id); u64(w, &s->node_id_high_water); u64(w, &s->audit_high_water);
    boolean(w, &s->inspection_only); count(w, &s->node_count);
    cog_node_data_t *nodes = w->input == NULL ? NULL : array(w, s->node_count, sizeof(*nodes));
    if (w->input != NULL) s->nodes = nodes;
    if (w->error != COG_OK) return;
    if (s->node_count != 0U && s->nodes == NULL) { w->error = COG_ERROR_PROTOCOL; return; }
    for (size_t k = 0U; k < s->node_count && w->error == COG_OK; ++k) {
        cog_node_data_t n = w->input == NULL ? s->nodes[k] : (cog_node_data_t){0};
        node(w, &n); if (w->input != NULL) nodes[k] = n;
    }
    count(w, &s->ledger_count);
    cog_tool_ledger_record_t *records = w->input == NULL ? NULL : array(w, s->ledger_count, sizeof(*records));
    if (w->input != NULL) s->ledger = records;
    if (w->error != COG_OK) return;
    if (s->ledger_count != 0U && s->ledger == NULL) { w->error = COG_ERROR_PROTOCOL; return; }
    for (size_t k = 0U; k < s->ledger_count && w->error == COG_OK; ++k) {
        cog_tool_ledger_record_t l = w->input == NULL ? s->ledger[k] : (cog_tool_ledger_record_t){0};
        ledger(w, &l); if (w->input != NULL) records[k] = l;
    }
}

cog_result_t cog_store_codec_encode(const cog_session_snapshot_t *s,
    unsigned char *output, size_t capacity, size_t *out_size)
{
    if (s == NULL || output == NULL || out_size == NULL) return COG_ERROR_NULL_PTR;
    cog_session_snapshot_t copy = *s;
    wire_t w = {output, NULL, capacity, 0U, NULL, COG_OK};
    snapshot(&w, &copy);
    if (w.error == COG_OK) *out_size = w.at;
    return w.error;
}
cog_result_t cog_store_codec_decode(const unsigned char *input, size_t size,
    cog_store_codec_workspace_t *workspace, cog_session_snapshot_t *out)
{
    if (input == NULL || workspace == NULL || out == NULL || workspace->data == NULL) return COG_ERROR_NULL_PTR;
    cog_session_snapshot_t decoded = {0};
    wire_t w = {NULL, input, size, 0U, workspace, COG_OK};
    snapshot(&w, &decoded);
    if (w.error == COG_OK && w.at != size) return COG_ERROR_PROTOCOL;
    if (w.error == COG_OK) *out = decoded;
    return w.error;
}

/* The v1 writer emitted a deliberately narrow JSON schema: all payload strings
 * are base64, numeric fields plain JSON numbers, fixed nested timing/cost maps.
 * This parser rejects duplicates/unknown fields/escapes and malformed canonical
 * base64 rather than applying permissive repair to authoritative history. */
typedef struct json {
    const unsigned char *at;
    const unsigned char *end;
    cog_store_codec_workspace_t *scratch;
    cog_result_t error;
} json_t;
static void whitespace(json_t *j)
{
    while (j->at != j->end && (*j->at == ' ' || *j->at == '\n' || *j->at == '\r' || *j->at == '\t')) ++j->at;
}
static bool character(json_t *j, unsigned char c)
{
    whitespace(j);
    if (j->at == j->end || *j->at != c) return false;
    ++j->at; return true;
}
static bool span(json_t *j, const unsigned char **out, size_t *size)
{
    if (!character(j, '"')) return false;
    const unsigned char *start = j->at;
    while (j->at != j->end && *j->at != '"') {
        if (*j->at < 32U || *j->at == '\\') return false;
        ++j->at;
    }
    if (j->at == j->end) return false;
    *out = start; *size = (size_t)(j->at - start); ++j->at;
    return true;
}
static bool integer(json_t *j, uint64_t *out)
{
    whitespace(j);
    const unsigned char *start = j->at;
    uint64_t value = 0U;
    while (j->at != j->end && *j->at >= '0' && *j->at <= '9') {
        const unsigned int digit = (unsigned int)(*j->at - '0');
        if (value > (UINT64_MAX - digit) / 10U) return false;
        value = value * 10U + digit; ++j->at;
    }
    if (start == j->at || (j->at - start > 1 && *start == '0')) return false;
    *out = value; return true;
}
static bool decimal(json_t *j, double *out)
{
    whitespace(j);
    const unsigned char *start = j->at;
    if (j->at != j->end && *j->at == '-') ++j->at;
    const unsigned char *whole = j->at;
    while (j->at != j->end && *j->at >= '0' && *j->at <= '9') ++j->at;
    if (whole == j->at || (j->at - whole > 1 && *whole == '0')) return false;
    if (j->at != j->end && *j->at == '.') {
        const unsigned char *fraction = ++j->at;
        while (j->at != j->end && *j->at >= '0' && *j->at <= '9') ++j->at;
        if (fraction == j->at) return false;
    }
    if (j->at != j->end && (*j->at == 'e' || *j->at == 'E')) {
        ++j->at;
        if (j->at != j->end && (*j->at == '+' || *j->at == '-')) ++j->at;
        const unsigned char *exponent = j->at;
        while (j->at != j->end && *j->at >= '0' && *j->at <= '9') ++j->at;
        if (exponent == j->at) return false;
    }
    const size_t size = (size_t)(j->at - start);
    char buf[128];
    if (size >= sizeof(buf)) return false;
    (void)memcpy(buf, start, size); buf[size] = '\0';
    char *end = NULL;
    errno = 0; *out = strtod(buf, &end);
    return errno == 0 && end == buf + size && isfinite(*out);
}
static int base64_value(unsigned char c)
{
    if (c >= 'A' && c <= 'Z') return (int)(c - 'A');
    if (c >= 'a' && c <= 'z') return (int)(c - 'a') + 26;
    if (c >= '0' && c <= '9') return (int)(c - '0') + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}
static bool legacy_string(json_t *j, const char **out, size_t *length)
{
    whitespace(j);
    if ((size_t)(j->end - j->at) >= 4U && memcmp(j->at, "null", 4U) == 0) {
        j->at += 4U; *out = NULL; *length = 0U; return true;
    }
    const unsigned char *encoded = NULL;
    size_t size = 0U;
    if (!span(j, &encoded, &size) || size % 4U != 0U || size / 4U > (SIZE_MAX - 1U) / 3U) return false;
    const size_t maximum = size / 4U * 3U;
    unsigned char *decoded = scratch(j->scratch, maximum + 1U);
    if (decoded == NULL) { j->error = COG_ERROR_LIMIT; return false; }
    size_t at = 0U;
    for (size_t k = 0U; k < size; k += 4U) {
        const int a = base64_value(encoded[k]), b = base64_value(encoded[k + 1U]);
        const bool pad2 = encoded[k + 2U] == '=', pad3 = encoded[k + 3U] == '=';
        const int c = pad2 ? 0 : base64_value(encoded[k + 2U]);
        const int d = pad3 ? 0 : base64_value(encoded[k + 3U]);
        if (a < 0 || b < 0 || c < 0 || d < 0 || (pad2 && !pad3) ||
            ((pad2 || pad3) && k + 4U != size) || (pad2 && (b & 15) != 0) ||
            (pad3 && !pad2 && (c & 3) != 0)) return false;
        decoded[at++] = (unsigned char)((unsigned int)a * 4U + (unsigned int)b / 16U);
        if (!pad2) decoded[at++] = (unsigned char)(((unsigned int)b & 15U) * 16U + (unsigned int)c / 4U);
        if (!pad3) decoded[at++] = (unsigned char)(((unsigned int)c & 3U) * 64U + (unsigned int)d);
    }
    decoded[at] = 0U; *out = (const char *)decoded; *length = at;
    return true;
}
static bool legacy_cstring(json_t *j, const char **out)
{
    size_t length = 0U;
    if (!legacy_string(j, out, &length)) return false;
    if (*out != NULL && memchr(*out, '\0', length) != NULL) {
        j->error = COG_ERROR_UNSUPPORTED; return false;
    }
    return true;
}
static bool key(json_t *j, const char *const *names, size_t count_, size_t *index, uint64_t *seen)
{
    const unsigned char *name;
    size_t length;
    if (!span(j, &name, &length) || !character(j, ':')) return false;
    for (size_t k = 0U; k < count_; ++k) if (strlen(names[k]) == length && memcmp(names[k], name, length) == 0) {
        const uint64_t bit = UINT64_C(1) << k;
        if ((*seen & bit) != 0U) return false;
        *seen |= bit; *index = k; return true;
    }
    return false;
}
static bool timing(json_t *j, cog_timing_t *t)
{
    static const char *const keys[] = {"request_sent_ms", "first_token_ms", "last_token_ms", "duration_ms", "ttft_ms", "tokens_per_second"};
    uint64_t *const values[] = {&t->request_sent_ms, &t->first_token_ms, &t->last_token_ms, &t->duration_ms, &t->ttft_ms};
    uint64_t seen = 0U;
    if (!character(j, '{')) return false;
    do {
        size_t k;
        if (!key(j, keys, 6U, &k, &seen)) return false;
        if (k == 5U) { if (!decimal(j, &t->tokens_per_second)) return false; }
        else if (!integer(j, values[k])) return false;
        if (character(j, '}')) return seen == 63U;
    } while (character(j, ','));
    return false;
}
static bool cost(json_t *j, cog_cost_t *c)
{
    static const char *const keys[] = {"input_tokens", "output_tokens", "cost_per_input_token", "cost_per_output_token", "total_cost"};
    uint64_t seen = 0U;
    if (!character(j, '{')) return false;
    do {
        size_t k;
        if (!key(j, keys, 5U, &k, &seen)) return false;
        if (k == 0U) { if (!integer(j, &c->input_tokens)) return false; }
        else if (k == 1U) { if (!integer(j, &c->output_tokens)) return false; }
        else if (k == 2U) { if (!decimal(j, &c->cost_per_input_token)) return false; }
        else if (k == 3U) { if (!decimal(j, &c->cost_per_output_token)) return false; }
        else if (!decimal(j, &c->total_cost)) return false;
        if (character(j, '}')) return seen == 31U;
    } while (character(j, ','));
    return false;
}
static bool legacy_node(json_t *j, cog_node_data_t *n)
{
    static const char *const keys[] = {"node_id", "parent_id", "role", "content", "tool_name", "tool_args", "tool_result", "raw_request", "raw_response", "chunk_count", "timing", "cost", "model", "provider"};
    uint64_t seen = 0U;
    if (!character(j, '{')) return false;
    do {
        size_t k;
        uint64_t v;
        if (!key(j, keys, 14U, &k, &seen)) return false;
        if (k == 0U) { if (!integer(j, &n->node_id)) return false; }
        else if (k == 1U) { if (!integer(j, &n->parent_id)) return false; }
        else if (k == 2U) { if (!integer(j, &v) || v > COG_NODE_TOOL_RESULT) return false; n->role = (cog_node_role_t)v; }
        else if (k == 3U) { if (!legacy_string(j, &n->content, &n->content_len)) return false; }
        else if (k == 4U) { if (!legacy_cstring(j, &n->tool_name)) return false; }
        else if (k == 5U) { if (!legacy_cstring(j, &n->tool_args)) return false; }
        else if (k == 6U) { if (!legacy_cstring(j, &n->tool_result)) return false; }
        else if (k == 7U) { if (!legacy_cstring(j, &n->raw_request)) return false; }
        else if (k == 8U) { if (!legacy_cstring(j, &n->raw_response)) return false; }
        else if (k == 9U) { if (!integer(j, &v) || v > SIZE_MAX) return false; n->chunk_count = (size_t)v; }
        else if (k == 10U) { if (!timing(j, &n->timing)) return false; }
        else if (k == 11U) { if (!cost(j, &n->cost)) return false; }
        else if (k == 12U) { if (!legacy_cstring(j, &n->model)) return false; }
        else if (!legacy_cstring(j, &n->provider)) return false;
        if (character(j, '}')) {
            whitespace(j);
            return seen == 16383U && j->at == j->end;
        }
    } while (character(j, ','));
    return false;
}

cog_result_t cog_store_codec_decode_v1(const char *id,
    const unsigned char *metadata, size_t metadata_size,
    const unsigned char *input, size_t size, cog_store_load_mode_t mode,
    bool recover_tail, cog_store_codec_workspace_t *workspace,
    cog_session_snapshot_t *out, cog_store_load_report_t *report)
{
    static const char *const keys[] = {"version", "id", "parent_id", "max_nodes", "node_count", "next_node_id", "active_node_id"};
    uint64_t seen = 0U, version = 0U, count_ = 0U, next = 0U, maximum = 0U;
    if (id == NULL || metadata == NULL || input == NULL || workspace == NULL || workspace->data == NULL || out == NULL || report == NULL)
        return COG_ERROR_NULL_PTR;
    if (mode != COG_STORE_LOAD_REPLAY && mode != COG_STORE_LOAD_INSPECT) return COG_ERROR_INVALID_ARG;
    cog_session_snapshot_t s = {0};
    cog_store_load_report_t r = {.source_version = 1U, .migrated_in_memory = true, .replay_safe = true};
    bool closed = false;
    json_t j = {metadata, metadata + metadata_size, workspace, COG_OK};
    if (!character(&j, '{')) return COG_ERROR_PROTOCOL;
    do {
        size_t k;
        if (!key(&j, keys, 7U, &k, &seen)) return COG_ERROR_PROTOCOL;
        bool ok;
        if (k == 0U) ok = integer(&j, &version);
        else if (k == 1U) ok = legacy_cstring(&j, &s.session_id);
        else if (k == 2U) ok = legacy_cstring(&j, &s.parent_session_id);
        else if (k == 3U) ok = integer(&j, &maximum);
        else if (k == 4U) ok = integer(&j, &count_);
        else if (k == 5U) ok = integer(&j, &next);
        else ok = integer(&j, &s.active_node_id);
        if (!ok) return j.error == COG_OK ? COG_ERROR_PROTOCOL : j.error;
        if (character(&j, '}')) { closed = true; break; }
        if (!character(&j, ',')) return COG_ERROR_PROTOCOL;
    } while (j.at != j.end);
    whitespace(&j);
    if (!closed || j.at != j.end || seen != 127U || s.session_id == NULL || strcmp(s.session_id, id) != 0 ||
        next == 0U || count_ > SIZE_MAX || maximum > SIZE_MAX) return COG_ERROR_PROTOCOL;
    if (version != 1U) return COG_ERROR_UNSUPPORTED;
    s.node_id_high_water = next - 1U; s.node_count = (size_t)count_;
    if (s.node_count > SIZE_MAX / sizeof(cog_node_data_t)) return COG_ERROR_LIMIT;
    cog_node_data_t *nodes = scratch(workspace, s.node_count * sizeof(*nodes));
    if (nodes == NULL) return COG_ERROR_LIMIT;
    s.nodes = nodes;
    size_t at = 0U;
    for (size_t k = 0U; k < s.node_count; ++k) {
        const unsigned char *end = memchr(input + at, '\n', size - at);
        if (end == NULL) return COG_ERROR_IO;
        json_t line = {input + at, end, workspace, COG_OK};
        if (!legacy_node(&line, &nodes[k])) return line.error == COG_OK ? COG_ERROR_PROTOCOL : line.error;
        at = (size_t)(end - input) + 1U;
        if (nodes[k].role == COG_NODE_TOOL_CALL || nodes[k].role == COG_NODE_TOOL_RESULT) r.replay_safe = false;
        else {
            const cog_message_t m = {.role = (cog_message_role_t)nodes[k].role,
                .content = nodes[k].content, .content_len = nodes[k].content_len};
            if (cog_message_validate(&m) != COG_OK) return COG_ERROR_UNSUPPORTED;
        }
    }
    if (at != size) {
        if (!recover_tail || memchr(input + at, '\n', size - at) != NULL) return COG_ERROR_IO;
        r.recovered_truncated_tail = true; r.discarded_tail_bytes = size - at;
    }
    if (!r.replay_safe && mode == COG_STORE_LOAD_REPLAY) return COG_ERROR_UNSUPPORTED;
    s.inspection_only = !r.replay_safe || mode == COG_STORE_LOAD_INSPECT;
    *out = s; *report = r;
    return COG_OK;
}

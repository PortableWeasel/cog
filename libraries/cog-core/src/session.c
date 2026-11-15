/** @file session.c
 * @brief Constructor-bounded structured conversation trees and effect ledger.
 * All mutation storage is reserved by cog_session_new. Public snapshots are
 * the persistence boundary; private pointer layouts are never wire records.
 */
#define _POSIX_C_SOURCE 200809L
#include "cog/session.h"
#include "session_internal.h"
#include <stdalign.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_NODES ((size_t)4096)
#define DEFAULT_PAYLOAD ((size_t)16 * 1024U * 1024U)
#define DEFAULT_CALLS ((size_t)16)
#define AUDITS_PER_CALL ((size_t)4)

/* Each allocation is a whole node payload (or parent ID). Free blocks coalesce,
 * so discarding drafts really returns capacity without moving borrowed views. */
typedef union arena_block {
    max_align_t alignment;
    struct { size_t size; bool free; } info;
} arena_block_t;

typedef struct pack {
    unsigned char *base;
    size_t used;
    bool failed;
} pack_t;

static size_t aligned(size_t n)
{
    const size_t a = alignof(max_align_t);
    return n > SIZE_MAX - (a - 1U) ? 0U : (n + a - 1U) / a * a;
}

static void *pack_bytes(pack_t *p, const void *data, size_t size)
{
    size_t at = aligned(p->used);
    void *out = NULL;
    if ((p->used != 0U && at == 0U) || size > SIZE_MAX - at) {
        p->failed = true;
        return NULL;
    }
    if (p->base != NULL) {
        out = p->base + at;
        if (data != NULL && size != 0U) (void)memcpy(out, data, size);
        else if (size != 0U) (void)memset(out, 0, size);
    }
    p->used = at + size;
    return out;
}

static const char *pack_text(pack_t *p, const char *data, size_t len)
{
    char *out;
    if (data == NULL) return NULL;
    if (len == SIZE_MAX) { p->failed = true; return NULL; }
    out = pack_bytes(p, NULL, len + 1U);
    if (out != NULL) { (void)memcpy(out, data, len); out[len] = '\0'; }
    return out;
}

static const char *pack_string(pack_t *p, const char *s)
{
    return s == NULL ? NULL : pack_text(p, s, strlen(s));
}

static void *arena_alloc(cog_session_t *s, size_t bytes)
{
    size_t at = 0U;
    size_t need = aligned(bytes);
    if (need == 0U || need > SIZE_MAX - sizeof(arena_block_t)) return NULL;
    need += sizeof(arena_block_t);
    while (at < s->arena_capacity) {
        arena_block_t *b = (arena_block_t *)(void *)(s->arena + at);
        if (b->info.free && b->info.size >= need) {
            const size_t rest = b->info.size - need;
            if (rest >= sizeof(arena_block_t) + alignof(max_align_t)) {
                arena_block_t *tail = (arena_block_t *)(void *)(s->arena + at + need);
                tail->info.size = rest;
                tail->info.free = true;
                b->info.size = need;
            }
            b->info.free = false;
            return b + 1;
        }
        at += b->info.size;
    }
    return NULL;
}

static void arena_free(cog_session_t *s, void *ptr)
{
    size_t at = 0U;
    if (ptr == NULL) return;
    ((arena_block_t *)ptr - 1)->info.free = true;
    while (at < s->arena_capacity) {
        arena_block_t *b = (arena_block_t *)(void *)(s->arena + at);
        const size_t next = at + b->info.size;
        if (b->info.free && next < s->arena_capacity) {
            arena_block_t *n = (arena_block_t *)(void *)(s->arena + next);
            if (n->info.free) { b->info.size += n->info.size; continue; }
        }
        at += b->info.size;
    }
}

static void copy_message(pack_t *p, const cog_message_t *src, cog_message_t *dst)
{
    cog_message_t m = *src;
    cog_message_tool_call_t *calls;
    cog_message_opaque_t *opaque;
    m.content = pack_text(p, src->content, src->content_len);
    m.name = pack_string(p, src->name);
    m.tool_call_id = pack_string(p, src->tool_call_id);
    m.usage.currency = pack_string(p, src->usage.currency);
    calls = pack_bytes(p, NULL, src->tool_call_count * sizeof(*calls));
    m.tool_calls = src->tool_call_count == 0U ? NULL : calls;
    for (size_t i = 0U; i < src->tool_call_count; ++i) {
        cog_message_tool_call_t c = src->tool_calls[i];
        c.id = pack_string(p, c.id);
        c.name = pack_string(p, c.name);
        c.arguments = pack_text(p, c.arguments, c.arguments_len);
        if (calls != NULL) calls[i] = c;
    }
    opaque = pack_bytes(p, NULL, src->opaque_count * sizeof(*opaque));
    m.opaque = src->opaque_count == 0U ? NULL : opaque;
    for (size_t i = 0U; i < src->opaque_count; ++i) {
        cog_message_opaque_t o = src->opaque[i];
        o.namespace_id = pack_string(p, o.namespace_id);
        o.format = pack_string(p, o.format);
        o.data = o.data == NULL ? NULL : pack_bytes(p, o.data, o.size);
        if (opaque != NULL) opaque[i] = o;
    }
    if (dst != NULL) *dst = m;
}

static bool contains(const cog_session_t *s, const cog_session_node_t *n)
{
    return s != NULL && n != NULL && n->session == s && n->used;
}

static bool on_path(const cog_session_node_t *leaf, const cog_session_node_t *n)
{
    for (; leaf != NULL; leaf = leaf->parent) if (leaf == n) return true;
    return false;
}

static cog_session_node_t *find_node(const cog_session_t *s, uint64_t id)
{
    for (size_t i = 0U; i < s->max_nodes; ++i)
        if (s->nodes[i].used && s->nodes[i].data.node_id == id) return &s->nodes[i];
    return NULL;
}

static cog_tool_ledger_record_t *find_call(const cog_session_t *s,
    const cog_session_node_t *n, const char *id)
{
    for (size_t i = 0U; i < s->ledger_count; ++i)
        if (s->ledger[i].assistant_node_id == n->data.node_id &&
            strcmp(s->ledger[i].call_id, id) == 0) return &s->ledger[i];
    return NULL;
}

static bool final_state(cog_tool_execution_state_t state)
{
    return state == COG_TOOL_EXECUTION_SUCCEEDED || state == COG_TOOL_EXECUTION_FAILED ||
        state == COG_TOOL_EXECUTION_DENIED || state == COG_TOOL_EXECUTION_CANCELLED ||
        state == COG_TOOL_EXECUTION_ACKNOWLEDGED;
}

static cog_session_node_t *result_for(const cog_session_t *s,
    const cog_session_node_t *assistant, const char *id)
{
    for (size_t i = 0U; i < s->max_nodes; ++i) {
        cog_session_node_t *n = &s->nodes[i];
        if (!n->used || n->data.message == NULL ||
            n->message.role != COG_MESSAGE_ROLE_TOOL || n->message.tool_call_id == NULL ||
            strcmp(n->message.tool_call_id, id) != 0) continue;
        /* A result binds to the closest assistant turn, even if IDs repeat. */
        const cog_session_node_t *a = n->parent;
        while (a != NULL && a->data.role != COG_NODE_ASSISTANT) a = a->parent;
        if (a == assistant) return n;
    }
    return NULL;
}

static bool unresolved(const cog_session_t *s)
{
    for (size_t i = 0U; i < s->ledger_count; ++i) {
        const cog_tool_ledger_record_t *l = &s->ledger[i];
        cog_session_node_t *n = find_node(s, l->assistant_node_id);
        if (!final_state(l->state) || result_for(s, n, l->call_id) == NULL) return true;
    }
    return false;
}

static bool indeterminate(const cog_session_t *s)
{
    for (size_t i = 0U; i < s->ledger_count; ++i)
        if (s->ledger[i].state == COG_TOOL_EXECUTION_INDETERMINATE) return true;
    return false;
}

static void link_node(cog_session_t *s, cog_session_node_t *parent,
    cog_session_node_t *n)
{
    n->session = s;
    n->used = true;
    n->parent = parent;
    n->data.parent_id = parent == NULL ? 0U : parent->data.node_id;
    if (parent == NULL) s->root = n;
    else {
        n->prev_sibling = parent->last_child;
        if (parent->last_child != NULL) parent->last_child->next_sibling = n;
        else parent->first_child = n;
        parent->last_child = n;
        ++parent->child_count;
        if (parent->child_count > 1U) {
            for (cog_session_node_t *a = parent; a != NULL; a = a->parent) a->shared = true;
        }
    }
    s->active = n;
    ++s->node_count;
    s->history_count = 0U;
}

static cog_result_t insert(cog_session_t *s, cog_session_node_t *parent,
    const cog_message_t *m, cog_session_node_t **out, bool reconciliation)
{
    pack_t measure = {0};
    cog_session_node_t *n = NULL;
    cog_tool_ledger_record_t *associated = NULL;
    cog_result_t rc = cog_message_validate(m);
    if (rc != COG_OK) return rc;
    if (s->inspection_only || (!reconciliation && indeterminate(s))) return COG_ERROR_INVALID_STATE;
    if (parent != NULL && !contains(s, parent)) return COG_ERROR_NOT_FOUND;
    if (parent == NULL) parent = s->root;
    if (m->tool_call_count > s->max_calls || m->tool_call_count > s->ledger_capacity - s->ledger_count ||
        s->node_count == s->max_nodes || s->next_node_id == UINT64_MAX) return COG_ERROR_LIMIT;
    if (m->tool_call_count > SIZE_MAX / sizeof(cog_message_tool_call_t) ||
        m->opaque_count > SIZE_MAX / sizeof(cog_message_opaque_t)) return COG_ERROR_LIMIT;
    if (unresolved(s) && (m->role != COG_MESSAGE_ROLE_TOOL || parent != s->active))
        return COG_ERROR_INVALID_STATE;
    if (m->role == COG_MESSAGE_ROLE_TOOL) {
        cog_session_node_t *a = parent;
        while (a != NULL && a->data.role != COG_NODE_ASSISTANT) a = a->parent;
        if (a == NULL || m->status != COG_MESSAGE_COMPLETE) return COG_ERROR_INVALID_ARG;
        associated = find_call(s, a, m->tool_call_id);
        if (associated == NULL || result_for(s, a, m->tool_call_id) != NULL) return COG_ERROR_INVALID_ARG;
        if (!reconciliation && associated->state == COG_TOOL_EXECUTION_INDETERMINATE)
            return COG_ERROR_INVALID_STATE;
        if (!reconciliation && associated->state == COG_TOOL_EXECUTION_PROPOSED)
            return COG_ERROR_INVALID_STATE;
        if (a->shared || parent != s->active) return COG_ERROR_INVALID_STATE;
    }
    copy_message(&measure, m, NULL);
    if (measure.failed) return COG_ERROR_LIMIT;
    for (size_t i = 0U; i < s->max_nodes; ++i) if (!s->nodes[i].used) { n = &s->nodes[i]; break; }
    if (n == NULL) return COG_ERROR_LIMIT;
    void *payload = arena_alloc(s, measure.used == 0U ? 1U : measure.used);
    if (payload == NULL) return COG_ERROR_LIMIT;
    (void)memset(n, 0, sizeof(*n));
    pack_t p = {payload, 0U, false};
    copy_message(&p, m, &n->message);
    n->payload = payload;
    n->data.node_id = s->next_node_id++;
    n->data.role = m->role == COG_MESSAGE_ROLE_TOOL ? COG_NODE_TOOL_RESULT : (cog_node_role_t)m->role;
    n->data.content = n->message.content;
    n->data.content_len = n->message.content_len;
    n->data.message = &n->message;
    n->data.tool_name = n->message.name;
    if (m->role == COG_MESSAGE_ROLE_TOOL) {
        n->data.tool_result = n->message.content;
        n->tool_result_len = n->message.content_len;
    }
    n->data.cost.tokens_known = m->usage.tokens_known;
    n->data.cost.input_tokens = m->usage.input_tokens;
    n->data.cost.output_tokens = m->usage.output_tokens;
    n->data.cost.total_cost_known = m->usage.cost_known;
    n->data.cost.total_cost = m->usage.cost;
    link_node(s, parent, n);
    for (size_t i = 0U; i < m->tool_call_count; ++i) {
        const size_t slot = s->ledger_count++;
        s->ledger[slot] = (cog_tool_ledger_record_t){n->data.node_id,
            n->message.tool_calls[i].id, COG_TOOL_EXECUTION_PROPOSED,
            &s->audits[slot * AUDITS_PER_CALL], 0U};
    }
    if (out != NULL) *out = n;
    return COG_OK;
}

cog_result_t cog_session_new(cog_session_t **out, const cog_session_config_t *config)
{
    cog_session_t *s;
    char id[96];
    struct timespec ts;
    if (out == NULL) return COG_ERROR_NULL_PTR;
    *out = NULL;
    if (config != NULL && config->id != NULL && config->id[0] == '\0') return COG_ERROR_INVALID_ARG;
    s = calloc(1U, sizeof(*s));
    if (s == NULL) return COG_ERROR_NOMEM;
    s->max_nodes = config != NULL && config->max_nodes != 0U ? config->max_nodes : DEFAULT_NODES;
    s->max_calls = config != NULL && config->max_tool_calls_per_message != 0U ? config->max_tool_calls_per_message : DEFAULT_CALLS;
    const size_t payload = config != NULL && config->max_payload_bytes != 0U ? config->max_payload_bytes : DEFAULT_PAYLOAD;
    if (s->max_nodes > SIZE_MAX / sizeof(*s->nodes) ||
        s->max_nodes > SIZE_MAX / sizeof(*s->history) || s->max_nodes > SIZE_MAX / sizeof(*s->snapshot_nodes) ||
        s->max_calls > SIZE_MAX / s->max_nodes || payload < sizeof(arena_block_t) + alignof(max_align_t)) {
        free(s); return COG_ERROR_LIMIT;
    }
    s->ledger_capacity = s->max_nodes * s->max_calls;
    if (s->ledger_capacity > SIZE_MAX / AUDITS_PER_CALL / sizeof(*s->audits) ||
        s->ledger_capacity > SIZE_MAX / sizeof(*s->ledger)) { free(s); return COG_ERROR_LIMIT; }
    s->nodes = calloc(s->max_nodes, sizeof(*s->nodes));
    s->history = calloc(s->max_nodes, sizeof(*s->history));
    s->snapshot_nodes = calloc(s->max_nodes, sizeof(*s->snapshot_nodes));
    s->ledger = calloc(s->ledger_capacity, sizeof(*s->ledger));
    s->audits = calloc(s->ledger_capacity * AUDITS_PER_CALL, sizeof(*s->audits));
    s->arena = malloc(payload);
    if (s->nodes == NULL || s->history == NULL || s->snapshot_nodes == NULL || s->ledger == NULL || s->audits == NULL || s->arena == NULL) {
        cog_session_free(s); return COG_ERROR_NOMEM;
    }
    s->arena_capacity = payload / alignof(max_align_t) * alignof(max_align_t);
    ((arena_block_t *)(void *)s->arena)->info.size = s->arena_capacity;
    ((arena_block_t *)(void *)s->arena)->info.free = true;
    if (config != NULL && config->id != NULL) s->id = strdup(config->id);
    else {
        (void)clock_gettime(CLOCK_REALTIME, &ts);
        (void)snprintf(id, sizeof(id), "%lx-%lx-%lx-%llx", (unsigned long)getpid(),
            (unsigned long)ts.tv_sec, (unsigned long)ts.tv_nsec, (unsigned long long)(uintptr_t)s);
        s->id = strdup(id);
    }
    if (s->id == NULL) { cog_session_free(s); return COG_ERROR_NOMEM; }
    s->next_node_id = 1U;
    s->history_capacity = s->max_nodes;
    *out = s;
    return COG_OK;
}

void cog_session_free(cog_session_t *s)
{
    if (s == NULL) return;
    free(s->nodes); free(s->arena); free(s->history); free(s->snapshot_nodes);
    free(s->ledger); free(s->audits); free(s->id); free(s);
}
const char *cog_session_id(const cog_session_t *s) { return s == NULL ? NULL : s->id; }
bool cog_session_is_empty(const cog_session_t *s) { return s == NULL || s->node_count == 0U; }
cog_session_node_t *cog_session_root(const cog_session_t *s) { return s == NULL ? NULL : s->root; }
cog_session_node_t *cog_session_active(const cog_session_t *s) { return s == NULL ? NULL : s->active; }
cog_session_node_t *cog_node_parent(const cog_session_node_t *n) { return n == NULL ? NULL : n->parent; }
cog_session_node_t *cog_node_first_child(const cog_session_node_t *n) { return n == NULL ? NULL : n->first_child; }
cog_session_node_t *cog_node_next_sibling(const cog_session_node_t *n) { return n == NULL ? NULL : n->next_sibling; }
size_t cog_node_child_count(const cog_session_node_t *n) { return n == NULL ? 0U : n->child_count; }
const cog_node_data_t *cog_node_data(const cog_session_node_t *n) { return n == NULL ? NULL : &n->data; }

cog_result_t cog_session_add_message(cog_session_t *s, cog_session_node_t *parent,
    const cog_message_t *m, cog_session_node_t **out)
{
    if (s == NULL || m == NULL) return COG_ERROR_NULL_PTR;
    return insert(s, parent, m, out, false);
}
cog_result_t cog_session_append(cog_session_t *s, const cog_message_t *m, cog_session_node_t **out)
{
    if (s == NULL || m == NULL) return COG_ERROR_NULL_PTR;
    return insert(s, s->active, m, out, false);
}

static cog_result_t legacy(cog_session_t *s, cog_session_node_t *parent,
    const char *name, const char *text, cog_node_role_t role, cog_session_node_t **out)
{
    cog_message_t m = {0};
    cog_session_node_t *n;
    cog_result_t rc;
    if (s == NULL || text == NULL) return COG_ERROR_NULL_PTR;
    if (unresolved(s)) return COG_ERROR_INVALID_STATE;
    m.role = COG_MESSAGE_ROLE_ASSISTANT; m.content = text; m.content_len = strlen(text); m.name = name;
    rc = insert(s, parent, &m, &n, false);
    if (rc != COG_OK) return rc;
    n->data.role = role; n->data.message = NULL;
    n->data.tool_name = n->message.name;
    if (role == COG_NODE_TOOL_CALL) { n->data.tool_args = n->message.content; n->tool_args_len = m.content_len; }
    else { n->data.tool_result = n->message.content; n->tool_result_len = m.content_len; }
    if (out != NULL) *out = n;
    return COG_OK;
}
cog_result_t cog_session_add_tool_call(cog_session_t *s, cog_session_node_t *p,
    const char *name, const char *args, cog_session_node_t **out)
{
    if (name == NULL) return COG_ERROR_NULL_PTR;
    return legacy(s, p, name, args, COG_NODE_TOOL_CALL, out);
}
cog_result_t cog_session_add_tool_result(cog_session_t *s, cog_session_node_t *p,
    const char *text, cog_session_node_t **out)
{
    if (p == NULL) return COG_ERROR_NULL_PTR;
    return legacy(s, p, p->data.tool_name, text, COG_NODE_TOOL_RESULT, out);
}

cog_result_t cog_session_copy_history(const cog_session_t *s, const cog_session_node_t *n,
    cog_message_t *messages, size_t cap, size_t *count)
{
    size_t depth = 0U;
    if (s == NULL || count == NULL || (messages == NULL && cap != 0U)) return COG_ERROR_NULL_PTR;
    if (n == NULL) n = s->active;
    if (n != NULL && !contains(s, n)) return COG_ERROR_INVALID_ARG;
    if (s->inspection_only || unresolved(s)) return COG_ERROR_INVALID_STATE;
    for (const cog_session_node_t *a = n; a != NULL; a = a->parent) {
        if (a->data.message == NULL || a->message.status != COG_MESSAGE_COMPLETE) return COG_ERROR_INVALID_STATE;
        ++depth;
    }
    *count = depth;
    if (depth > cap) return COG_ERROR_OVERFLOW;
    for (const cog_session_node_t *a = n; a != NULL; a = a->parent) messages[--depth] = a->message;
    return COG_OK;
}

cog_result_t cog_session_get_history(const cog_session_t *s, const cog_session_node_t *n,
    const cog_message_t **messages, size_t *count)
{
    cog_result_t rc;
    if (s == NULL || n == NULL || messages == NULL || count == NULL) return COG_ERROR_NULL_PTR;
    rc = cog_session_copy_history(s, n, s->history, s->history_capacity, count);
    if (rc == COG_OK) *messages = s->history;
    return rc;
}

cog_result_t cog_session_fork(cog_session_t *s, cog_session_node_t *n, cog_session_node_t **out)
{
    size_t count;
    if (s == NULL || n == NULL) return COG_ERROR_NULL_PTR;
    if (!contains(s, n)) return COG_ERROR_NOT_FOUND;
    if (cog_session_copy_history(s, n, s->history, s->history_capacity, &count) != COG_OK)
        return COG_ERROR_INVALID_STATE;
    /* Fork names a shared immutable ancestor; children create divergent branches.
     * Never copy an assistant call to a fresh identity and authorize its effect. */
    for (cog_session_node_t *a = n; a != NULL; a = a->parent) a->shared = true;
    s->forked = true;
    if (n->child_count == 0U) s->active = n;
    if (out != NULL) *out = n;
    return COG_OK;
}

static bool legal_transition(cog_tool_execution_state_t previous, cog_tool_execution_state_t next)
{
    return (previous == COG_TOOL_EXECUTION_PROPOSED &&
        (next == COG_TOOL_EXECUTION_STARTED || next == COG_TOOL_EXECUTION_DENIED)) ||
        (previous == COG_TOOL_EXECUTION_STARTED &&
        (next == COG_TOOL_EXECUTION_SUCCEEDED || next == COG_TOOL_EXECUTION_FAILED ||
         next == COG_TOOL_EXECUTION_CANCELLED || next == COG_TOOL_EXECUTION_INDETERMINATE));
}

static cog_result_t audit_add(cog_session_t *s, cog_tool_ledger_record_t *l,
    cog_tool_execution_state_t next, bool explicit_, const char *evidence, size_t length)
{
    if (l->audit_count == AUDITS_PER_CALL || s->audit_high_water == UINT64_MAX) return COG_ERROR_LIMIT;
    cog_tool_audit_record_t *a = (cog_tool_audit_record_t *)l->audit;
    a[l->audit_count++] = (cog_tool_audit_record_t){++s->audit_high_water,
        l->state, next, explicit_, evidence, length};
    l->state = next;
    return COG_OK;
}

cog_result_t cog_session_tool_state(const cog_session_t *s, const cog_session_node_t *n,
    const char *id, cog_tool_execution_state_t *out)
{
    if (s == NULL || n == NULL || id == NULL || out == NULL) return COG_ERROR_NULL_PTR;
    if (!contains(s, n)) return COG_ERROR_INVALID_ARG;
    cog_tool_ledger_record_t *l = find_call(s, n, id);
    if (l == NULL) return COG_ERROR_NOT_FOUND;
    *out = l->state;
    return COG_OK;
}

cog_result_t cog_session_tool_transition(cog_session_t *s, cog_session_node_t *n,
    const char *id, cog_tool_execution_state_t expected, cog_tool_execution_state_t next)
{
    if (s == NULL || n == NULL || id == NULL) return COG_ERROR_NULL_PTR;
    if (!contains(s, n)) return COG_ERROR_INVALID_ARG;
    cog_tool_ledger_record_t *l = find_call(s, n, id);
    if (l == NULL) return COG_ERROR_NOT_FOUND;
    if (s->inspection_only || indeterminate(s) || n->shared || n->message.status != COG_MESSAGE_COMPLETE ||
        !on_path(s->active, n) || l->state != expected || !legal_transition(expected, next))
        return COG_ERROR_INVALID_STATE;
    return audit_add(s, l, next, false, NULL, 0U);
}

cog_result_t cog_session_reconcile_tool(cog_session_t *s, cog_session_node_t *n,
    const char *id, cog_tool_execution_state_t resolution, const cog_message_t *result,
    cog_session_node_t **out)
{
    cog_session_node_t *r;
    cog_result_t rc;
    if (s == NULL || n == NULL || id == NULL || result == NULL) return COG_ERROR_NULL_PTR;
    if (!contains(s, n)) return COG_ERROR_INVALID_ARG;
    cog_tool_ledger_record_t *l = find_call(s, n, id);
    if (l == NULL) return COG_ERROR_NOT_FOUND;
    if (n->shared || !on_path(s->active, n) || l->state != COG_TOOL_EXECUTION_INDETERMINATE)
        return COG_ERROR_INVALID_STATE;
    if ((resolution != COG_TOOL_EXECUTION_SUCCEEDED && resolution != COG_TOOL_EXECUTION_FAILED &&
        resolution != COG_TOOL_EXECUTION_CANCELLED && resolution != COG_TOOL_EXECUTION_ACKNOWLEDGED) ||
        result->role != COG_MESSAGE_ROLE_TOOL || result->tool_call_id == NULL ||
        strcmp(result->tool_call_id, id) != 0 || result->content_len == 0U || result->status != COG_MESSAGE_COMPLETE)
        return COG_ERROR_INVALID_ARG;
    if (l->audit_count == AUDITS_PER_CALL || s->audit_high_water == UINT64_MAX) return COG_ERROR_LIMIT;
    rc = insert(s, s->active, result, &r, true);
    if (rc != COG_OK) return rc;
    rc = audit_add(s, l, resolution, true, r->message.content, r->message.content_len);
    if (out != NULL) *out = r;
    return rc;
}

cog_result_t cog_session_discard_node(cog_session_t *s, cog_session_node_t *n)
{
    if (s == NULL || n == NULL) return COG_ERROR_NULL_PTR;
    if (!contains(s, n)) return COG_ERROR_NOT_FOUND;
    if (n->child_count != 0U || n->shared || n->committed || n->data.message == NULL ||
        n->message.tool_call_count != 0U || n->message.role == COG_MESSAGE_ROLE_TOOL)
        return COG_ERROR_INVALID_STATE;
    cog_session_node_t *p = n->parent;
    if (p == NULL) s->root = NULL;
    else {
        if (p->first_child == n) p->first_child = n->next_sibling;
        if (p->last_child == n) p->last_child = n->prev_sibling;
        --p->child_count;
    }
    if (n->prev_sibling != NULL) n->prev_sibling->next_sibling = n->next_sibling;
    if (n->next_sibling != NULL) n->next_sibling->prev_sibling = n->prev_sibling;
    if (s->active == n) s->active = p;
    arena_free(s, n->payload);
    (void)memset(n, 0, sizeof(*n));
    --s->node_count;
    return COG_OK;
}

cog_result_t cog_session_reset(cog_session_t *s)
{
    if (s == NULL) return COG_ERROR_NULL_PTR;
    if (s->attached || s->forked || s->ledger_count != 0U || s->inspection_only) return COG_ERROR_INVALID_STATE;
    for (size_t i = 0U; i < s->max_nodes; ++i) {
        cog_session_node_t *n = &s->nodes[i];
        if (n->used && (n->committed || n->shared || n->data.message == NULL || n->message.role == COG_MESSAGE_ROLE_TOOL))
            return COG_ERROR_INVALID_STATE;
    }
    for (size_t i = 0U; i < s->max_nodes; ++i) if (s->nodes[i].used) {
        arena_free(s, s->nodes[i].payload); (void)memset(&s->nodes[i], 0, sizeof(s->nodes[i]));
    }
    s->root = NULL; s->active = NULL; s->node_count = 0U; s->history_count = 0U;
    return COG_OK;
}

cog_result_t cog_session_set_timing(cog_session_t *s, cog_session_node_t *n, const cog_timing_t *t)
{
    if (s == NULL || n == NULL || t == NULL) return COG_ERROR_NULL_PTR;
    if (!contains(s, n)) return COG_ERROR_NOT_FOUND;
    if (n->shared || n->committed) return COG_ERROR_INVALID_STATE;
    n->data.timing = *t; return COG_OK;
}
cog_result_t cog_session_set_cost(cog_session_t *s, cog_session_node_t *n, const cog_cost_t *c)
{
    if (s == NULL || n == NULL || c == NULL) return COG_ERROR_NULL_PTR;
    if (!contains(s, n)) return COG_ERROR_NOT_FOUND;
    if (n->shared || n->committed) return COG_ERROR_INVALID_STATE;
    n->data.cost = *c; return COG_OK;
}
double cog_session_cumulative_cost(const cog_session_t *s, const cog_session_node_t *n)
{
    double cost = 0.0;
    if (!contains(s, n)) return 0.0;
    for (; n != NULL; n = n->parent) cost += n->data.cost.total_cost;
    return cost;
}
cog_result_t cog_session_cumulative_timing(const cog_session_t *s, const cog_session_node_t *n, cog_timing_t *out)
{
    cog_timing_t t = {0};
    if (s == NULL || n == NULL || out == NULL) return COG_ERROR_NULL_PTR;
    if (!contains(s, n)) return COG_ERROR_NOT_FOUND;
    for (; n != NULL; n = n->parent) {
        t.request_sent_ms += n->data.timing.request_sent_ms;
        t.first_token_ms += n->data.timing.first_token_ms;
        t.last_token_ms += n->data.timing.last_token_ms;
        t.duration_ms += n->data.timing.duration_ms;
        t.ttft_ms += n->data.timing.ttft_ms;
        t.tokens_per_second += n->data.timing.tokens_per_second;
    }
    *out = t; return COG_OK;
}
const char *cog_session_parent_id(const cog_session_t *s) { return s == NULL ? NULL : s->parent_id; }
cog_result_t cog_session_set_parent(cog_session_t *s, const char *id)
{
    char *copy = NULL;
    if (s == NULL) return COG_ERROR_NULL_PTR;
    if (id != NULL) {
        const size_t len = strlen(id);
        copy = arena_alloc(s, len + 1U);
        if (copy == NULL) return COG_ERROR_LIMIT;
        (void)memcpy(copy, id, len + 1U);
    }
    arena_free(s, s->parent_id); s->parent_id = copy;
    return COG_OK;
}

/* Iterative preorder preserves actual sibling order, including after restores. */
cog_result_t cog_session_snapshot_view(const cog_session_t *s, cog_session_snapshot_t *out)
{
    size_t count = 0U;
    const cog_session_node_t *n;
    if (s == NULL || out == NULL) return COG_ERROR_NULL_PTR;
    n = s->root;
    while (n != NULL) {
        if (count == s->max_nodes) return COG_ERROR_LIMIT;
        s->snapshot_nodes[count++] = n->data;
        if (n->first_child != NULL) n = n->first_child;
        else {
            while (n != NULL && n->next_sibling == NULL) n = n->parent;
            if (n != NULL) n = n->next_sibling;
        }
    }
    cog_session_snapshot_t snapshot = {s->id, s->parent_id,
        s->active == NULL ? 0U : s->active->data.node_id, s->next_node_id - 1U,
        s->audit_high_water, s->snapshot_nodes, count, s->ledger, s->ledger_count, s->inspection_only};
    *out = snapshot;
    return COG_OK;
}

static cog_result_t restore_node(cog_session_t *s, const cog_node_data_t *src)
{
    cog_message_t legacy_message = {0};
    const cog_message_t *m = src->message;
    cog_session_node_t *parent = NULL;
    pack_t measure = {0};
    if (src->node_id == 0U || src->node_id >= s->next_node_id || find_node(s, src->node_id) != NULL ||
        src->role < COG_NODE_SYSTEM || src->role > COG_NODE_TOOL_RESULT ||
        (src->content == NULL && src->content_len != 0U)) return COG_ERROR_PROTOCOL;
    if (src->parent_id == 0U) {
        if (s->root != NULL) return COG_ERROR_PROTOCOL;
    } else {
        parent = find_node(s, src->parent_id);
        if (parent == NULL) return COG_ERROR_PROTOCOL;
    }
    if (m == NULL && src->role <= COG_NODE_ASSISTANT) {
        legacy_message.role = (cog_message_role_t)src->role;
        legacy_message.content = src->content;
        legacy_message.content_len = src->content_len;
        m = &legacy_message;
    }
    if (m != NULL) {
        if (m->tool_call_count > s->max_calls ||
            m->opaque_count > SIZE_MAX / sizeof(cog_message_opaque_t) ||
            m->tool_call_count > SIZE_MAX / sizeof(cog_message_tool_call_t)) return COG_ERROR_LIMIT;
        if (cog_message_validate(m) != COG_OK ||
            src->role != (m->role == COG_MESSAGE_ROLE_TOOL ? COG_NODE_TOOL_RESULT : (cog_node_role_t)m->role))
            return COG_ERROR_PROTOCOL;
        copy_message(&measure, m, NULL);
    } else if (!s->inspection_only) return COG_ERROR_UNSUPPORTED;
    (void)pack_text(&measure, src->content, src->content_len);
    (void)pack_string(&measure, src->tool_name);
    (void)pack_string(&measure, src->tool_args);
    (void)pack_string(&measure, src->tool_result);
    (void)pack_string(&measure, src->raw_request);
    (void)pack_string(&measure, src->raw_response);
    (void)pack_string(&measure, src->model);
    (void)pack_string(&measure, src->provider);
    if (measure.failed) return COG_ERROR_LIMIT;
    void *payload = arena_alloc(s, measure.used == 0U ? 1U : measure.used);
    if (payload == NULL) return COG_ERROR_LIMIT;
    cog_session_node_t *n = &s->nodes[s->node_count];
    pack_t p = {payload, 0U, false};
    n->data = *src;
    n->payload = payload;
    n->committed = true;
    if (m != NULL) {
        copy_message(&p, m, &n->message);
        n->data.message = &n->message;
    }
    n->data.content = pack_text(&p, src->content, src->content_len);
    n->data.tool_name = pack_string(&p, src->tool_name);
    n->data.tool_args = pack_string(&p, src->tool_args);
    n->data.tool_result = pack_string(&p, src->tool_result);
    n->data.raw_request = pack_string(&p, src->raw_request);
    n->data.raw_response = pack_string(&p, src->raw_response);
    n->data.model = pack_string(&p, src->model);
    n->data.provider = pack_string(&p, src->provider);
    n->tool_args_len = src->tool_args == NULL ? 0U : strlen(src->tool_args);
    n->tool_result_len = src->tool_result == NULL ? 0U : strlen(src->tool_result);
    link_node(s, parent, n);
    return COG_OK;
}

static cog_result_t restore_ledger(cog_session_t *s, const cog_session_snapshot_t *snapshot)
{
    size_t required = 0U;
    for (size_t i = 0U; i < s->node_count; ++i) {
        const cog_session_node_t *n = &s->nodes[i];
        if (n->data.message != NULL) {
            if (n->message.tool_call_count > SIZE_MAX - required) return COG_ERROR_LIMIT;
            required += n->message.tool_call_count;
        }
    }
    if (required != snapshot->ledger_count) return COG_ERROR_PROTOCOL;
    if (required > s->ledger_capacity) return COG_ERROR_LIMIT;
    for (size_t i = 0U; i < required; ++i) {
        const cog_tool_ledger_record_t *src = &snapshot->ledger[i];
        cog_session_node_t *n = find_node(s, src->assistant_node_id);
        bool match = false;
        if (src->call_id == NULL || src->call_id[0] == '\0' || n == NULL || n->data.message == NULL ||
            n->message.role != COG_MESSAGE_ROLE_ASSISTANT ||
            src->state < COG_TOOL_EXECUTION_PROPOSED || src->state > COG_TOOL_EXECUTION_ACKNOWLEDGED ||
            (src->audit_count != 0U && src->audit == NULL)) return COG_ERROR_PROTOCOL;
        if (src->audit_count > AUDITS_PER_CALL) return COG_ERROR_LIMIT;
        for (size_t k = 0U; k < n->message.tool_call_count; ++k)
            if (strcmp(n->message.tool_calls[k].id, src->call_id) == 0) match = true;
        if (!match || find_call(s, n, src->call_id) != NULL) return COG_ERROR_PROTOCOL;
        cog_tool_execution_state_t state = COG_TOOL_EXECUTION_PROPOSED;
        for (size_t k = 0U; k < src->audit_count; ++k) {
            const cog_tool_audit_record_t *a = &src->audit[k];
            if (a->sequence == 0U || a->sequence > snapshot->audit_high_water || a->previous != state ||
                (k != 0U && a->sequence <= src->audit[k - 1U].sequence) ||
                (a->evidence == NULL && a->evidence_len != 0U)) return COG_ERROR_PROTOCOL;
            const cog_message_t evidence = {.role = COG_MESSAGE_ROLE_USER,
                .content = a->evidence, .content_len = a->evidence_len};
            if (cog_message_validate(&evidence) != COG_OK) return COG_ERROR_PROTOCOL;
            if (a->explicit_reconciliation) {
                if (state != COG_TOOL_EXECUTION_INDETERMINATE || !final_state(a->next) ||
                    a->next == COG_TOOL_EXECUTION_DENIED || a->evidence_len == 0U)
                    return COG_ERROR_PROTOCOL;
            } else if (!legal_transition(state, a->next)) return COG_ERROR_PROTOCOL;
            for (size_t j = 0U; j < i; ++j)
                for (size_t q = 0U; q < s->ledger[j].audit_count; ++q)
                    if (s->ledger[j].audit[q].sequence == a->sequence) return COG_ERROR_PROTOCOL;
            state = a->next;
        }
        if (state != src->state) return COG_ERROR_PROTOCOL;
        cog_tool_ledger_record_t *dst = &s->ledger[i];
        *dst = *src;
        dst->audit = &s->audits[i * AUDITS_PER_CALL];
        for (size_t k = 0U; k < n->message.tool_call_count; ++k)
            if (strcmp(n->message.tool_calls[k].id, src->call_id) == 0) dst->call_id = n->message.tool_calls[k].id;
        for (size_t k = 0U; k < src->audit_count; ++k) {
            cog_tool_audit_record_t a = src->audit[k];
            if (a.evidence != NULL) {
                if (a.evidence_len == SIZE_MAX) return COG_ERROR_LIMIT;
                char *e = arena_alloc(s, a.evidence_len + 1U);
                if (e == NULL) return COG_ERROR_LIMIT;
                (void)memcpy(e, a.evidence, a.evidence_len); e[a.evidence_len] = '\0'; a.evidence = e;
            }
            s->audits[i * AUDITS_PER_CALL + k] = a;
        }
        ++s->ledger_count;
    }
    /* Every result resolves exactly one call on the same nearest assistant path. */
    for (size_t i = 0U; i < s->node_count; ++i) {
        const cog_session_node_t *r = &s->nodes[i];
        if (r->data.message == NULL || r->message.role != COG_MESSAGE_ROLE_TOOL) continue;
        const cog_session_node_t *a = r->parent;
        while (a != NULL && a->data.role != COG_NODE_ASSISTANT) a = a->parent;
        if (a == NULL || find_call(s, a, r->message.tool_call_id) == NULL ||
            result_for(s, a, r->message.tool_call_id) != r || r->message.status != COG_MESSAGE_COMPLETE)
            return COG_ERROR_PROTOCOL;
    }
    for (size_t i = 0U; i < s->ledger_count; ++i) {
        cog_tool_ledger_record_t *l = &s->ledger[i];
        const cog_session_node_t *a = find_node(s, l->assistant_node_id);
        const cog_session_node_t *r = result_for(s, a, l->call_id);
        if (!final_state(l->state) || r == NULL) {
            if (!on_path(s->active, a)) return COG_ERROR_PROTOCOL;
            for (const cog_session_node_t *descendant = s->active; descendant != a; descendant = descendant->parent)
                if (descendant->data.message == NULL || descendant->message.role != COG_MESSAGE_ROLE_TOOL)
                    return COG_ERROR_PROTOCOL;
        }
        if (final_state(l->state) && r == NULL) return COG_ERROR_PROTOCOL;
        if (r != NULL && (!final_state(l->state) || !on_path(r, a))) return COG_ERROR_PROTOCOL;
    }
    s->audit_high_water = snapshot->audit_high_water;
    for (size_t i = 0U; i < s->ledger_count; ++i)
        if (s->ledger[i].state == COG_TOOL_EXECUTION_STARTED) {
            cog_result_t rc = audit_add(s, &s->ledger[i], COG_TOOL_EXECUTION_INDETERMINATE, false, NULL, 0U);
            if (rc != COG_OK) return rc;
        }
    return COG_OK;
}

cog_result_t cog_session_restore(cog_session_t **out, const cog_session_config_t *config,
    const cog_session_snapshot_t *snapshot)
{
    cog_session_t *s = NULL;
    cog_result_t rc;
    if (out == NULL) return COG_ERROR_NULL_PTR;
    *out = NULL;
    if (snapshot == NULL || snapshot->session_id == NULL) return COG_ERROR_NULL_PTR;
    if (snapshot->session_id[0] == '\0' || (config != NULL && config->id != NULL &&
        strcmp(config->id, snapshot->session_id) != 0)) return COG_ERROR_INVALID_ARG;
    if ((snapshot->node_count != 0U && snapshot->nodes == NULL) ||
        (snapshot->ledger_count != 0U && snapshot->ledger == NULL) ||
        ((snapshot->node_count == 0U) != (snapshot->active_node_id == 0U))) return COG_ERROR_PROTOCOL;
    if (snapshot->node_id_high_water == UINT64_MAX) return COG_ERROR_LIMIT;
    cog_session_config_t cfg = config == NULL ? (cog_session_config_t){0} : *config;
    cfg.id = snapshot->session_id;
    rc = cog_session_new(&s, &cfg);
    if (rc != COG_OK) return rc;
    if (snapshot->node_count > s->max_nodes) { rc = COG_ERROR_LIMIT; goto fail; }
    s->inspection_only = snapshot->inspection_only;
    s->next_node_id = snapshot->node_id_high_water + 1U;
    rc = cog_session_set_parent(s, snapshot->parent_session_id);
    if (rc != COG_OK) goto fail;
    for (size_t i = 0U; i < snapshot->node_count; ++i) {
        rc = restore_node(s, &snapshot->nodes[i]);
        if (rc != COG_OK) goto fail;
    }
    s->active = find_node(s, snapshot->active_node_id);
    if (snapshot->node_count != 0U && (s->active == NULL || s->active->child_count != 0U)) {
        rc = COG_ERROR_PROTOCOL; goto fail;
    }
    rc = restore_ledger(s, snapshot);
    if (rc != COG_OK) goto fail;
    *out = s;
    return COG_OK;
fail:
    cog_session_free(s);
    return rc;
}

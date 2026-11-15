/** @file config.c
 * @brief Constructor-reserved aggregate configuration pools.
 * A fixed string arena and constructor-reserved admission bitmap keep borrowed
 * addresses stable across operations on other transactions. Nodes use stable indices,
 * never recursion.
 */
#include <assert.h>
#include <cog/config.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#define NONE SIZE_MAX

typedef struct node {
    size_t owner, parent, first, last, next;
    char *name, *value;
    bool used;
} node_t;
typedef struct validator {
    char *prefix;
    cog_config_validator_fn fn;
} validator_t;
struct cog_config_transaction {
    cog_config_tree_t *tree;
    size_t owner, root;
    bool active;
};
struct cog_config_tree {
    cog_config_limits_t limits;
    node_t *nodes;
    cog_config_txn_t *txns;
    validator_t *validators;
    size_t *map;
    char *banks[2];
    unsigned char *occupied, *admission;
    size_t used, handles, validator_count, root;
};
static bool valid(const char *p, bool empty)
{
    size_t n = strlen(p);
    return n == 0U ? empty : p[0] != '.' && p[n - 1U] != '.' && strstr(p, "..") == NULL;
}
static size_t child(const cog_config_tree_t *t, size_t parent, const char *s,
                    size_t len)
{
    size_t i;
    for (i = t->nodes[parent].first; i != NONE; i = t->nodes[i].next)
        if (strlen(t->nodes[i].name) == len && memcmp(t->nodes[i].name, s, len) == 0)
            return i;
    return NONE;
}
static size_t walk(const cog_config_tree_t *t, size_t root, const char *p)
{
    size_t cur = root;
    while (*p != '\0') {
        const char *dot = strchr(p, '.');
        size_t len = dot != NULL ? (size_t)(dot - p) : strlen(p);
        cur = child(t, cur, p, len);
        if (cur == NONE || dot == NULL)
            return cur;
        p = dot + 1;
    }
    return cur;
}
static size_t take(cog_config_tree_t *t, size_t owner)
{
    size_t i;
    for (i = 0U; i < t->limits.max_nodes; ++i)
        if (!t->nodes[i].used) {
            node_t n = {0};
            n.used = true;
            n.owner = owner;
            n.parent = NONE;
            n.first = NONE;
            n.last = NONE;
            n.next = NONE;
            t->nodes[i] = n;
            ++t->used;
            return i;
        }
    assert(false); /* node admission invariant */
    return NONE;   /* admission checked before mutation */
}
static char *copy(char *bank, size_t *pos, const char *s, size_t len)
{
    char *out = bank + *pos;
    memcpy(out, s, len);
    out[len] = '\0';
    *pos += len + 1U;
    return out;
}
static size_t live_strings(const cog_config_tree_t *t)
{
    size_t i, n = 0U;
    for (i = 0U; i < t->limits.max_nodes; ++i)
        if (t->nodes[i].used) {
            if (t->nodes[i].name != NULL)
                n += strlen(t->nodes[i].name) + 1U;
            if (t->nodes[i].value != NULL)
                n += strlen(t->nodes[i].value) + 1U;
        }
    for (i = 0U; i < t->validator_count; ++i)
        n += strlen(t->validators[i].prefix) + 1U;
    return n;
}
/* Trial allocations change only the scratch bitmap. Publishing it is atomic. */
static size_t reserve(cog_config_tree_t *t, size_t len)
{
    size_t i, run = 0U;
    for (i = 0U; i < t->limits.string_bytes; ++i) {
        run = t->admission[i] == 0U ? run + 1U : 0U;
        if (run == len) {
            size_t start = i + 1U - len;
            memset(t->admission + start, 1, len);
            return start;
        }
    }
    return NONE;
}
static void release_string(cog_config_tree_t *t, const char *s)
{
    if (s != NULL)
        memset(t->occupied + (size_t)(s - t->banks[0]), 0, strlen(s) + 1U);
}
static void discard(cog_config_tree_t *t, size_t owner)
{
    size_t i;
    for (i = 0U; i < t->limits.max_nodes; ++i)
        if (t->nodes[i].used && t->nodes[i].owner == owner) {
            release_string(t, t->nodes[i].name);
            release_string(t, t->nodes[i].value);
            t->nodes[i].used = false;
            --t->used;
        }
}
static cog_result_t snapshot(cog_config_tree_t *t, size_t owner, size_t *root)
{
    size_t i, count = 0U, bytes = 0U;
    for (i = 0U; i < t->limits.max_nodes; ++i)
        if (t->nodes[i].used && t->nodes[i].owner == 0U) {
            ++count;
            if (t->nodes[i].name != NULL)
                bytes += strlen(t->nodes[i].name) + 1U;
            if (t->nodes[i].value != NULL)
                bytes += strlen(t->nodes[i].value) + 1U;
        }
    if (count > t->limits.max_nodes - t->used ||
        bytes > t->limits.string_bytes - live_strings(t))
        return COG_ERROR_LIMIT;
    memcpy(t->admission, t->occupied, t->limits.string_bytes);
    /* Store planned name/value offsets per source node in reserved empty nodes.
     * Failure rolls back temporary nodes without touching published strings. */
    for (i = 0U; i < t->limits.max_nodes; ++i)
        if (t->nodes[i].used && t->nodes[i].owner == 0U) {
            node_t src = t->nodes[i];
            size_t j = take(t, owner), pos;
            t->map[i] = j;
            t->nodes[j] = src;
            t->nodes[j].owner = owner;
            t->nodes[j].name = NULL;
            t->nodes[j].value = NULL;
            if (src.name != NULL) {
                pos = reserve(t, strlen(src.name) + 1U);
                if (pos == NONE)
                    goto fail;
                t->nodes[j].name = t->banks[0] + pos;
            }
            if (src.value != NULL) {
                pos = reserve(t, strlen(src.value) + 1U);
                if (pos == NONE)
                    goto fail;
                t->nodes[j].value = t->banks[0] + pos;
            }
        }
    memcpy(t->occupied, t->admission, t->limits.string_bytes);
    for (i = 0U; i < t->limits.max_nodes; ++i)
        if (t->nodes[i].used && t->nodes[i].owner == 0U) {
            node_t *d = &t->nodes[t->map[i]], *src = &t->nodes[i];
            if (src->name != NULL)
                memcpy(d->name, src->name, strlen(src->name) + 1U);
            if (src->value != NULL)
                memcpy(d->value, src->value, strlen(src->value) + 1U);
        }
    for (i = 0U; i < t->limits.max_nodes; ++i)
        if (t->nodes[i].used && t->nodes[i].owner == owner) {
            node_t *n = &t->nodes[i];
            if (n->parent != NONE)
                n->parent = t->map[n->parent];
            if (n->first != NONE)
                n->first = t->map[n->first];
            if (n->last != NONE)
                n->last = t->map[n->last];
            if (n->next != NONE)
                n->next = t->map[n->next];
        }
    *root = t->map[t->root];
    return COG_OK;
fail:
    for (i = 0U; i < t->limits.max_nodes; ++i)
        if (t->nodes[i].used && t->nodes[i].owner == owner) {
            t->nodes[i].used = false;
            --t->used;
        }
    return COG_ERROR_LIMIT;
}
cog_result_t cog_config_tree_new_ex(cog_config_tree_t **out,
                                    const cog_config_limits_t *limits)
{
    cog_config_limits_t l = {8192U, 8U * 1024U * 1024U, 4096U, 16U};
    cog_config_tree_t *t;
    size_t total = sizeof(*t), pieces[6], i;
    if (out == NULL)
        return COG_ERROR_NULL_PTR;
    *out = NULL;
    if (limits != NULL) {
        if (limits->max_nodes != 0U)
            l.max_nodes = limits->max_nodes;
        if (limits->string_bytes != 0U)
            l.string_bytes = limits->string_bytes;
        if (limits->max_transactions != 0U)
            l.max_transactions = limits->max_transactions;
        if (limits->max_validators != 0U)
            l.max_validators = limits->max_validators;
    }
    if (l.max_nodes > SIZE_MAX / sizeof(node_t) ||
        l.max_nodes > SIZE_MAX / sizeof(size_t) ||
        l.max_transactions > SIZE_MAX / sizeof(cog_config_txn_t) ||
        l.max_transactions == SIZE_MAX ||
        l.max_validators > SIZE_MAX / sizeof(validator_t) ||
        l.string_bytes > SIZE_MAX / 4U)
        return COG_ERROR_INVALID_ARG;
    pieces[0] = l.max_nodes * sizeof(node_t);
    pieces[1] = l.max_nodes * sizeof(size_t);
    pieces[2] = l.max_transactions * sizeof(cog_config_txn_t);
    pieces[3] = l.max_validators * sizeof(validator_t);
    pieces[4] = l.string_bytes * 2U;
    pieces[5] = l.string_bytes * 2U;
    for (i = 0U; i < 6U; ++i) {
        if (pieces[i] > SIZE_MAX - total)
            return COG_ERROR_INVALID_ARG;
        total += pieces[i];
    }
    t = calloc(1U, sizeof(*t));
    if (t == NULL)
        return COG_ERROR_NOMEM;
    t->limits = l;
    t->nodes = calloc(l.max_nodes, sizeof(*t->nodes));
    t->map = calloc(l.max_nodes, sizeof(*t->map));
    t->txns = calloc(l.max_transactions, sizeof(*t->txns));
    t->validators = calloc(l.max_validators, sizeof(*t->validators));
    t->banks[0] = malloc(l.string_bytes);
    t->banks[1] = malloc(l.string_bytes);
    t->occupied = calloc(l.string_bytes, 1U);
    t->admission = malloc(l.string_bytes);
    if (t->nodes == NULL || t->map == NULL || t->txns == NULL ||
        t->validators == NULL || t->banks[0] == NULL || t->banks[1] == NULL ||
        t->occupied == NULL || t->admission == NULL) {
        cog_config_tree_free(t);
        return COG_ERROR_NOMEM;
    }
    t->root = take(t, 0U);
    *out = t;
    return COG_OK;
}
cog_result_t cog_config_tree_new(cog_config_tree_t **out)
{
    return cog_config_tree_new_ex(out, NULL);
}
void cog_config_tree_free(cog_config_tree_t *t)
{
    if (t == NULL)
        return;
    free(t->nodes);
    free(t->map);
    free(t->txns);
    free(t->validators);
    free(t->banks[0]);
    free(t->banks[1]);
    free(t->occupied);
    free(t->admission);
    free(t);
}
cog_result_t cog_config_begin(cog_config_tree_t *t, cog_config_txn_t **out)
{
    cog_config_txn_t *txn;
    cog_result_t rc;
    if (out == NULL)
        return COG_ERROR_NULL_PTR;
    *out = NULL;
    if (t == NULL)
        return COG_ERROR_NULL_PTR;
    if (t->handles == t->limits.max_transactions)
        return COG_ERROR_LIMIT;
    txn = &t->txns[t->handles];
    txn->owner = t->handles + 1U;
    rc = snapshot(t, txn->owner, &txn->root);
    if (rc != COG_OK)
        return rc;
    txn->tree = t;
    txn->active = true;
    ++t->handles;
    *out = txn;
    return COG_OK;
}
void cog_config_rollback(cog_config_txn_t *txn)
{
    if (txn != NULL && txn->active) {
        discard(txn->tree, txn->owner);
        txn->active = false;
    }
}
cog_result_t cog_config_commit(cog_config_txn_t *txn)
{
    size_t i;
    cog_config_tree_t *t;
    if (txn == NULL)
        return COG_ERROR_NULL_PTR;
    if (!txn->active)
        return COG_ERROR_INVALID_STATE;
    t = txn->tree;
    for (i = 0U; i < t->validator_count; ++i) {
        validator_t *v = &t->validators[i];
        if (walk(t, txn->root, v->prefix) == NONE)
            continue;
        if (v->fn(txn, v->prefix) != COG_OK) {
            discard(t, txn->owner);
            if (snapshot(t, txn->owner, &txn->root) != COG_OK) {
                txn->active = false;
                return COG_ERROR_LIMIT;
            }
            return COG_ERROR_INVALID_ARG;
        }
    }
    discard(t, 0U);
    t->root = txn->root;
    for (i = 0U; i < t->limits.max_nodes; ++i)
        if (t->nodes[i].used && t->nodes[i].owner == txn->owner)
            t->nodes[i].owner = 0U;
    txn->active = false;
    return COG_OK;
}
cog_result_t cog_config_get(const cog_config_txn_t *txn, const char *path,
                            const char **out)
{
    size_t i;
    if (out == NULL)
        return COG_ERROR_NULL_PTR;
    *out = NULL;
    if (txn == NULL || path == NULL)
        return COG_ERROR_NULL_PTR;
    if (!valid(path, false))
        return COG_ERROR_INVALID_ARG;
    if (!txn->active)
        return COG_ERROR_NOT_FOUND;
    i = walk(txn->tree, txn->root, path);
    if (i == NONE || txn->tree->nodes[i].value == NULL)
        return COG_ERROR_NOT_FOUND;
    *out = txn->tree->nodes[i].value;
    return COG_OK;
}
cog_result_t cog_config_set(cog_config_txn_t *txn, const char *path, const char *value)
{
    cog_config_tree_t *t;
    size_t cur, need = 0U, bytes, old = 0U, pos, k = 0U, valpos;
    const char *p;
    char *bank;
    if (txn == NULL || path == NULL || value == NULL)
        return COG_ERROR_NULL_PTR;
    if (!txn->active)
        return COG_ERROR_INVALID_STATE;
    if (!valid(path, false))
        return COG_ERROR_INVALID_ARG;
    t = txn->tree;
    cur = txn->root;
    p = path;
    bytes = strlen(value) + 1U;
    while (*p != '\0') {
        const char *dot = strchr(p, '.');
        size_t len = dot != NULL ? (size_t)(dot - p) : strlen(p);
        cur = cur != NONE ? child(t, cur, p, len) : NONE;
        if (cur == NONE) {
            ++need;
            if (len + 1U > SIZE_MAX - bytes)
                return COG_ERROR_LIMIT;
            bytes += len + 1U;
        }
        if (dot == NULL)
            break;
        p = dot + 1;
    }
    if (cur != NONE && t->nodes[cur].value != NULL)
        old = strlen(t->nodes[cur].value) + 1U;
    if (need > t->limits.max_nodes - t->used ||
        bytes > t->limits.string_bytes - (live_strings(t) - old))
        return COG_ERROR_LIMIT;
    /* Preserve aliased input before reusing the replaced value's bytes. */
    if (strlen(path) + 1U > t->limits.string_bytes ||
        strlen(value) + 1U > t->limits.string_bytes - (strlen(path) + 1U))
        return COG_ERROR_LIMIT;
    bank = t->banks[1];
    memcpy(bank, path, strlen(path) + 1U);
    value = copy(bank, &(size_t){strlen(path) + 1U}, value, strlen(value));
    path = bank;
    memcpy(t->admission, t->occupied, t->limits.string_bytes);
    if (cur != NONE && old != 0U)
        memset(t->admission + (size_t)(t->nodes[cur].value - t->banks[0]), 0, old);
    valpos = reserve(t, strlen(value) + 1U);
    if (valpos == NONE)
        return COG_ERROR_LIMIT;
    cur = txn->root;
    p = path;
    while (*p != '\0') {
        const char *dot = strchr(p, '.');
        size_t len = dot != NULL ? (size_t)(dot - p) : strlen(p);
        cur = cur != NONE ? child(t, cur, p, len) : NONE;
        if (cur == NONE) {
            pos = reserve(t, len + 1U);
            if (pos == NONE)
                return COG_ERROR_LIMIT;
            t->map[k++] = pos;
        }
        if (dot == NULL)
            break;
        p = dot + 1;
    }
    memcpy(t->occupied, t->admission, t->limits.string_bytes);
    bank = t->banks[0];
    cur = txn->root;
    p = path;
    k = 0U;
    while (*p != '\0') {
        const char *dot = strchr(p, '.');
        size_t len = dot != NULL ? (size_t)(dot - p) : strlen(p);
        size_t j = child(t, cur, p, len);
        if (j == NONE) {
            j = take(t, txn->owner);
            t->nodes[j].parent = cur;
            pos = t->map[k++];
            t->nodes[j].name = copy(bank, &pos, p, len);
            if (t->nodes[cur].last == NONE)
                t->nodes[cur].first = j;
            else
                t->nodes[t->nodes[cur].last].next = j;
            t->nodes[cur].last = j;
        }
        cur = j;
        if (dot == NULL)
            break;
        p = dot + 1;
    }
    pos = valpos;
    t->nodes[cur].value = copy(bank, &pos, value, strlen(value));
    return COG_OK;
}
static void unlink_node(cog_config_tree_t *t, size_t i)
{
    size_t parent = t->nodes[i].parent, j, prev = NONE;
    for (j = t->nodes[parent].first; j != i; j = t->nodes[j].next)
        prev = j;
    if (prev == NONE)
        t->nodes[parent].first = t->nodes[i].next;
    else
        t->nodes[prev].next = t->nodes[i].next;
    if (t->nodes[parent].last == i)
        t->nodes[parent].last = prev;
}
cog_result_t cog_config_delete(cog_config_txn_t *txn, const char *path)
{
    cog_config_tree_t *t;
    size_t i, parent, j;
    if (txn == NULL || path == NULL)
        return COG_ERROR_NULL_PTR;
    if (!txn->active)
        return COG_ERROR_INVALID_STATE;
    if (!valid(path, false))
        return COG_ERROR_INVALID_ARG;
    t = txn->tree;
    i = walk(t, txn->root, path);
    if (i == NONE)
        return COG_ERROR_NOT_FOUND;
    parent = t->nodes[i].parent;
    unlink_node(t, i);
    /* Mark membership before removing nodes, so arbitrary pool index order is safe. */
    for (j = 0U; j < t->limits.max_nodes; ++j) {
        size_t k = j;
        t->map[j] = 0U;
        if (!t->nodes[j].used || t->nodes[j].owner != txn->owner)
            continue;
        while (k != NONE && k != i)
            k = t->nodes[k].parent;
        if (k == i)
            t->map[j] = 1U;
    }
    for (j = 0U; j < t->limits.max_nodes; ++j)
        if (t->map[j] != 0U) {
            release_string(t, t->nodes[j].name);
            release_string(t, t->nodes[j].value);
            t->nodes[j].used = false;
            --t->used;
        }
    while (parent != txn->root && t->nodes[parent].first == NONE &&
           t->nodes[parent].value == NULL) {
        size_t next = t->nodes[parent].parent;
        unlink_node(t, parent);
        release_string(t, t->nodes[parent].name);
        t->nodes[parent].used = false;
        --t->used;
        parent = next;
    }
    return COG_OK;
}
cog_result_t cog_config_list(const cog_config_txn_t *txn, const char *path,
                             const char **keys, size_t cap, size_t *count)
{
    size_t i, n = 0U, root;
    cog_config_tree_t *t;
    if (txn == NULL || path == NULL || count == NULL || (cap != 0U && keys == NULL))
        return COG_ERROR_NULL_PTR;
    if (!valid(path, true))
        return COG_ERROR_INVALID_ARG;
    if (!txn->active)
        return COG_ERROR_NOT_FOUND;
    t = txn->tree;
    root = walk(t, txn->root, path);
    if (root == NONE)
        return COG_ERROR_NOT_FOUND;
    for (i = t->nodes[root].first; i != NONE; i = t->nodes[i].next)
        ++n;
    *count = n;
    if (cap != 0U && n > cap)
        return COG_ERROR_OVERFLOW;
    if (cap != 0U) {
        n = 0U;
        for (i = t->nodes[root].first; i != NONE; i = t->nodes[i].next)
            keys[n++] = t->nodes[i].name;
    }
    return COG_OK;
}
cog_result_t cog_config_register_validator(cog_config_tree_t *t, const char *prefix,
                                           cog_config_validator_fn fn)
{
    char *bank;
    size_t len;
    if (t == NULL || prefix == NULL || fn == NULL)
        return COG_ERROR_NULL_PTR;
    if (!valid(prefix, true))
        return COG_ERROR_INVALID_ARG;
    if (t->validator_count == t->limits.max_validators)
        return COG_ERROR_OVERFLOW;
    len = strlen(prefix) + 1U;
    if (len > t->limits.string_bytes - live_strings(t))
        return COG_ERROR_LIMIT;
    memcpy(t->admission, t->occupied, t->limits.string_bytes);
    {
        size_t pos = reserve(t, len);
        if (pos == NONE)
            return COG_ERROR_LIMIT;
        bank = t->banks[0];
        t->validators[t->validator_count].prefix = copy(bank, &pos, prefix, len - 1U);
    }
    memcpy(t->occupied, t->admission, t->limits.string_bytes);
    t->validators[t->validator_count++].fn = fn;
    return COG_OK;
}

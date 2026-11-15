/**
 * @file config.c
 * @brief Implementation of the hierarchical configuration tree declared
 *        in cog/config.h.
 *
 * Ticket: df4d0fa (Stage 12.3c — Implement config.c)
 *
 * Data structure
 * ==============
 *
 * A configuration is a tree of nodes:
 *
 *   struct cog_config_node {
 *       struct cog_config_node *parent;
 *       char                  *name;     // NULL only for the tree root
 *       char                  *value;    // NULL when the node carries none
 *       struct cog_config_node **children;
 *       size_t                child_count;
 *       size_t                child_cap;
 *   };
 *
 * Any node may carry both a value and children.  Paths are resolved by
 * splitting on '.' and walking child arrays (a linear scan per level).
 *
 * A cog_config_tree_t owns the running tree plus a fixed-capacity
 * validator table (16 slots; registration never grows it).  Every
 * transaction is an independent deep copy of the running tree ("candidate")
 * taken by cog_config_begin; set/delete mutate only the candidate.
 *
 * Commit is copy-on-commit: validators run against the candidate, and on
 * success the candidate replaces the running tree by pointer swap.  Nothing
 * in the running tree is ever mutated in place, so a reader holding an
 * older transaction keeps seeing the pre-commit configuration (hot-reload
 * isolation) and a mid-commit failure cannot leave a partial configuration
 * visible.
 *
 * Transaction lifecycle
 * =====================
 *
 * A transaction struct is allocated by cog_config_begin and retained by
 * its tree until cog_config_tree_free (the struct itself is never
 * recycled, so a stale handle can never alias a new transaction).  Commit
 * and rollback release the candidate and mark the transaction FINALIZED;
 * a finalized transaction reports COG_ERROR_INVALID_STATE to mutations
 * and commit, and COG_ERROR_NOT_FOUND to reads.  A validator rejection
 * does NOT finalize the transaction: the staged edits are discarded, the
 * candidate is re-derived as a clean copy of the running tree, and the
 * caller may stage new edits and retry.
 *
 * There is intentionally no diff/reload API here: config.h does not
 * declare one.  Hot reload is signalled application-side with
 * COG_EVENT_CONFIG_CHANGED (cog/event.h) after a successful commit; this
 * unit provides the snapshot isolation and atomic visibility that make
 * that reload safe.
 */
#define _POSIX_C_SOURCE 200809L

#include "cog/config.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/** Fixed validator-table capacity per tree (never grows at runtime). */
#define COG_CONFIG_MAX_VALIDATORS ((size_t)16U)

/* ------------------------------------------------------------------ */
/* Node                                                                */
/* ------------------------------------------------------------------ */

/** A node in the configuration tree; defined privately in this unit. */
typedef struct cog_config_node {
    struct cog_config_node *parent;
    char *name;                /**< NULL only for the tree root. */
    char *value;               /**< NULL when the node carries no value. */
    struct cog_config_node **children;
    size_t child_count;
    size_t child_cap;
} cog_config_node_t;

/** Create a node; @p name may be NULL (tree root). */
static cog_config_node_t *node_new_n(const char *name, size_t name_len)
{
    cog_config_node_t *n;

    n = (cog_config_node_t *)calloc(1U, sizeof *n);
    if (n == NULL) {
        return NULL;
    }
    if (name != NULL) {
        n->name = (char *)malloc(name_len + 1U);
        if (n->name == NULL) {
            free(n);
            return NULL;
        }
        (void)memcpy(n->name, name, name_len);
        n->name[name_len] = '\0';
    }
    return n;
}

/** Free a node and all of its descendants. */
static void node_free(cog_config_node_t *n)
{
    size_t i;

    if (n == NULL) {
        return;
    }
    for (i = 0U; i < n->child_count; ++i) {
        node_free(n->children[i]);
    }
    free(n->children);
    free(n->name);
    free(n->value);
    free(n);
}

/** Append @p child to @p parent, growing the children array as needed. */
static cog_result_t node_add_child(cog_config_node_t *parent,
                                   cog_config_node_t *child)
{
    if (parent->child_count == parent->child_cap) {
        size_t new_cap = parent->child_cap != 0U ? parent->child_cap * 2U : 4U;
        cog_config_node_t **nc;

        nc = (cog_config_node_t **)realloc(parent->children,
                                           new_cap * sizeof *nc);
        if (nc == NULL) {
            return COG_ERROR_NOMEM;
        }
        parent->children = nc;
        parent->child_cap = new_cap;
    }
    parent->children[parent->child_count] = child;
    parent->child_count++;
    child->parent = parent;
    return COG_OK;
}

/** Look up a child by name slice; returns NULL when absent. */
static cog_config_node_t *node_find_child_n(const cog_config_node_t *cur,
                                            const char *name, size_t name_len)
{
    size_t i;

    for (i = 0U; i < cur->child_count; ++i) {
        cog_config_node_t *c = cur->children[i];

        if (strlen(c->name) == name_len &&
            memcmp(c->name, name, name_len) == 0) {
            return c;
        }
    }
    return NULL;
}

/** Remove @p child from @p parent's children array (no free). */
static void node_remove_child(cog_config_node_t *parent,
                              cog_config_node_t *child)
{
    size_t i;

    for (i = 0U; i < parent->child_count; ++i) {
        if (parent->children[i] == child) {
            size_t j;

            for (j = i + 1U; j < parent->child_count; ++j) {
                parent->children[j - 1U] = parent->children[j];
            }
            parent->child_count--;
            child->parent = NULL;
            return;
        }
    }
}

/** Deep-copy a node and its whole subtree; NULL on allocation failure. */
static cog_config_node_t *node_copy(const cog_config_node_t *src)
{
    cog_config_node_t *d;
    size_t i;

    d = node_new_n(src->name, src->name != NULL ? strlen(src->name) : 0U);
    if (d == NULL) {
        return NULL;
    }
    if (src->value != NULL) {
        d->value = strdup(src->value);
        if (d->value == NULL) {
            node_free(d);
            return NULL;
        }
    }
    for (i = 0U; i < src->child_count; ++i) {
        cog_config_node_t *c = node_copy(src->children[i]);

        if (c == NULL) {
            node_free(d);
            return NULL;
        }
        if (node_add_child(d, c) != COG_OK) {
            node_free(c);
            node_free(d);
            return NULL;
        }
    }
    return d;
}

/** Remove empty, valueless nodes walking upward from @p node. */
static void prune_empty(cog_config_node_t *node)
{
    while (node != NULL && node->parent != NULL &&
           node->child_count == 0U && node->value == NULL) {
        cog_config_node_t *p = node->parent;

        node_remove_child(p, node);
        node_free(node);
        node = p;
    }
}

/* ------------------------------------------------------------------ */
/* Path handling                                                       */
/* ------------------------------------------------------------------ */

/**
 * @brief Validate a dotted path against the declared path grammar.
 *
 * @param[in] allow_empty When true, "" is valid (tree root).
 * @return true when the path conforms.
 */
static bool path_valid(const char *path, bool allow_empty)
{
    size_t len;

    if (path == NULL) {
        return false;
    }
    len = strlen(path);
    if (len == 0U) {
        return allow_empty;
    }
    if (path[0] == '.' || path[len - 1U] == '.') {
        return false; /* empty leading/trailing segment */
    }
    if (strstr(path, "..") != NULL) {
        return false; /* empty interior segment */
    }
    return true;
}

/**
 * @brief Split the next dot-free segment off a dotted path.
 *
 * The caller has already validated the full path, so only the terminal
 * empty path can occur here; it is reported as invalid defensively.
 *
 * @param[in]  path    Remaining path text.
 * @param[out] seg     Start of the segment (into @p path).
 * @param[out] seg_len Length of the segment.
 * @param[out] rest    NULL if this is the last segment, else the text
 *                     after the separating '.'.
 * @return COG_OK, or COG_ERROR_INVALID_ARG on an empty path/segment.
 */
static cog_result_t path_split_segment(const char *path, const char **seg,
                                       size_t *seg_len, const char **rest)
{
    const char *dot;

    *seg = path;
    *rest = NULL;
    if (path[0] == '\0') {
        return COG_ERROR_INVALID_ARG;
    }
    dot = strchr(path, '.');
    if (dot == NULL) {
        *seg_len = strlen(path);
        return COG_OK;
    }
    if (dot == path || dot[1] == '\0') {
        return COG_ERROR_INVALID_ARG; /* empty segment */
    }
    *seg_len = (size_t)(dot - path);
    *rest = dot + 1;
    return COG_OK;
}

/**
 * @brief Walk a (validated) dotted path from @p base without creating
 *        nodes.  "" returns @p base itself.
 *
 * @param[out] out Receives the node found, or NULL.
 * @return COG_OK, COG_ERROR_NOT_FOUND (missing segment), or
 *         COG_ERROR_INVALID_ARG (malformed path — defensive).
 */
static cog_result_t node_walk(const cog_config_node_t *base,
                              const char *path,
                              const cog_config_node_t **out)
{
    const cog_config_node_t *cur = base;
    const char *p = path;

    *out = NULL;
    if (p[0] == '\0') {
        *out = cur;
        return COG_OK;
    }
    for (;;) {
        const char *seg;
        const char *rest;
        size_t seg_len;
        cog_result_t r = path_split_segment(p, &seg, &seg_len, &rest);

        if (r != COG_OK) {
            return r;
        }
        cur = node_find_child_n(cur, seg, seg_len);
        if (cur == NULL) {
            return COG_ERROR_NOT_FOUND;
        }
        if (rest == NULL) {
            *out = cur;
            return COG_OK;
        }
        p = rest;
    }
}

/* ------------------------------------------------------------------ */
/* Candidate mutations                                                 */
/* ------------------------------------------------------------------ */

/**
 * @brief Set @p value at @p path under @p cur, creating intermediate
 *        nodes.  On error, nodes created along the failed branch are
 *        removed again, so a failing set never leaves partial nodes.
 */
static cog_result_t set_rec(cog_config_node_t *cur, const char *path,
                            const char *value)
{
    const char *seg;
    const char *rest;
    size_t seg_len;
    cog_config_node_t *child;
    cog_result_t r;
    bool created;

    r = path_split_segment(path, &seg, &seg_len, &rest);
    if (r != COG_OK) {
        return r;
    }

    child = node_find_child_n(cur, seg, seg_len);
    created = false;
    if (child == NULL) {
        child = node_new_n(seg, seg_len);
        if (child == NULL) {
            return COG_ERROR_NOMEM;
        }
        r = node_add_child(cur, child);
        if (r != COG_OK) {
            node_free(child);
            return r;
        }
        created = true;
    }

    if (rest == NULL) {
        char *nv = strdup(value);

        if (nv == NULL) {
            r = COG_ERROR_NOMEM;
        } else {
            free(child->value);
            child->value = nv;
            r = COG_OK;
        }
    } else {
        r = set_rec(child, rest, value);
    }

    if (r != COG_OK && created) {
        node_remove_child(cur, child);
        node_free(child);
    }
    return r;
}

/**
 * @brief Delete the node at @p path under @p cur, then prune empty
 *        valueless ancestors (VyOS-style: deleting the last leaf of a
 *        subtree removes the subtree).
 */
static cog_result_t delete_rec(cog_config_node_t *cur, const char *path)
{
    const char *seg;
    const char *rest;
    size_t seg_len;
    cog_config_node_t *child;
    cog_result_t r;

    r = path_split_segment(path, &seg, &seg_len, &rest);
    if (r != COG_OK) {
        return r;
    }

    child = node_find_child_n(cur, seg, seg_len);
    if (child == NULL) {
        return COG_ERROR_NOT_FOUND;
    }

    if (rest == NULL) {
        node_remove_child(cur, child);
        node_free(child);
        prune_empty(cur);
        return COG_OK;
    }
    return delete_rec(child, rest);
}

/* ------------------------------------------------------------------ */
/* Tree and transaction                                                */
/* ------------------------------------------------------------------ */

/** Transaction state: active candidates are mutable; finalized are not. */
typedef enum cog_config_txn_state {
    COG_CONFIG_TXN_ACTIVE = 0,    /**< Candidate usable. */
    COG_CONFIG_TXN_FINALIZED = 1, /**< Committed or rolled back. */
} cog_config_txn_state_t;

/**
 * @brief Opaque transaction layout (definition of @c cog_config_txn_t).
 *
 * The struct is retained by its tree until cog_config_tree_free: it is
 * never recycled, so a stale handle can never alias a different
 * transaction and "already finalized" is detectable deterministically.
 */
struct cog_config_transaction {
    cog_config_txn_state_t state;          /**< ACTIVE or FINALIZED. */
    struct cog_config_tree *tree;          /**< Owning tree. */
    cog_config_node_t *candidate;          /**< Candidate root; NULL when finalized. */
    struct cog_config_transaction *next;   /**< Tree-owned list linkage. */
};

/**
 * @brief Opaque tree layout (definition of @c cog_config_tree_t).
 *
 * The validator table has fixed capacity @c COG_CONFIG_MAX_VALIDATORS and
 * never grows: registration fills the next free slot.  All transactions
 * ever begun on this tree stay linked in @c txns until tree_free.
 */
struct cog_config_tree {
    cog_config_node_t *running;            /**< Committed configuration. */
    struct cog_config_validator_slot {
        char *prefix;                      /**< Owned copy of the prefix. */
        cog_config_validator_fn fn;        /**< Validator callback. */
    } validators[COG_CONFIG_MAX_VALIDATORS];
    size_t validator_count;                /**< Number of registered validators. */
    struct cog_config_transaction *txns;   /**< All begun transactions. */
};

/* ------------------------------------------------------------------ */
/* Lifecycle                                                            */
/* ------------------------------------------------------------------ */

cog_result_t cog_config_tree_new(cog_config_tree_t **out)
{
    struct cog_config_tree *tree;

    if (out == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    *out = NULL;

    tree = (struct cog_config_tree *)calloc(1U, sizeof *tree);
    if (tree == NULL) {
        return COG_ERROR_NOMEM;
    }
    tree->running = node_new_n(NULL, 0U);
    if (tree->running == NULL) {
        free(tree);
        return COG_ERROR_NOMEM;
    }
    *out = tree;
    return COG_OK;
}

void cog_config_tree_free(cog_config_tree_t *tree)
{
    struct cog_config_transaction *t;
    size_t i;

    if (tree == NULL) {
        return;
    }
    node_free(tree->running);
    for (i = 0U; i < tree->validator_count; ++i) {
        free(tree->validators[i].prefix);
    }
    t = tree->txns;
    while (t != NULL) {
        struct cog_config_transaction *next = t->next;

        node_free(t->candidate);
        free(t);
        t = next;
    }
    free(tree);
}

/* ------------------------------------------------------------------ */
/* Transactions                                                         */
/* ------------------------------------------------------------------ */

cog_result_t cog_config_begin(cog_config_tree_t *tree,
                               cog_config_txn_t **out_txn)
{
    struct cog_config_transaction *t;

    if (tree == NULL || out_txn == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    *out_txn = NULL;

    t = (struct cog_config_transaction *)calloc(1U, sizeof *t);
    if (t == NULL) {
        return COG_ERROR_NOMEM;
    }
    t->candidate = node_copy(tree->running);
    if (t->candidate == NULL) {
        free(t);
        return COG_ERROR_NOMEM;
    }
    t->state = COG_CONFIG_TXN_ACTIVE;
    t->tree = tree;
    t->next = tree->txns;
    tree->txns = t;
    *out_txn = t;
    return COG_OK;
}

cog_result_t cog_config_commit(cog_config_txn_t *txn)
{
    struct cog_config_transaction *t = txn;
    struct cog_config_tree *tree;
    cog_config_node_t *old_running;
    size_t i;

    if (t == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    if (t->state != COG_CONFIG_TXN_ACTIVE) {
        return COG_ERROR_INVALID_STATE; /* already committed or rolled back */
    }
    tree = t->tree;

    /* Run validators in registration order against the candidate. */
    for (i = 0U; i < tree->validator_count; ++i) {
        const cog_config_node_t *sub = NULL;
        cog_result_t r = node_walk(t->candidate,
                                   tree->validators[i].prefix, &sub);

        if (r == COG_ERROR_NOT_FOUND) {
            continue; /* prefix absent from the candidate: not fired */
        }
        if (r != COG_OK) {
            return r; /* defensive; prefixes are validated at registration */
        }
        if (tree->validators[i].fn(txn, tree->validators[i].prefix) != COG_OK) {
            /* Rejection: discard staged edits, re-derive a clean
             * candidate from the unchanged running tree, and keep the
             * transaction usable for a further attempt. */
            node_free(t->candidate);
            t->candidate = node_copy(tree->running);
            if (t->candidate == NULL) {
                /* Cannot keep a usable transaction without a candidate;
                 * fail closed. */
                t->state = COG_CONFIG_TXN_FINALIZED;
                return COG_ERROR_NOMEM;
            }
            return COG_ERROR_INVALID_ARG;
        }
    }

    /* All validators passed: promote the candidate atomically.  The
     * candidate is a complete deep copy, so no allocation happens after
     * this point and the swap cannot fail. */
    old_running = tree->running;
    tree->running = t->candidate;
    t->candidate = NULL;
    node_free(old_running);
    t->state = COG_CONFIG_TXN_FINALIZED;
    return COG_OK;
}

void cog_config_rollback(cog_config_txn_t *txn)
{
    struct cog_config_transaction *t = txn;

    if (t == NULL) {
        return;
    }
    if (t->state == COG_CONFIG_TXN_ACTIVE) {
        node_free(t->candidate);
        t->candidate = NULL;
        t->state = COG_CONFIG_TXN_FINALIZED;
    }
    /* Rolling back an already-finalized transaction is a no-op. */
}

/* ------------------------------------------------------------------ */
/* Path-based access                                                    */
/* ------------------------------------------------------------------ */

cog_result_t cog_config_get(const cog_config_txn_t *txn,
                             const char *path,
                             const char **out_value)
{
    const struct cog_config_transaction *t = txn;
    const cog_config_node_t *node = NULL;
    cog_result_t r;

    if (t == NULL || path == NULL || out_value == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    *out_value = NULL;
    if (!path_valid(path, false)) {
        return COG_ERROR_INVALID_ARG;
    }
    if (t->state != COG_CONFIG_TXN_ACTIVE) {
        return COG_ERROR_NOT_FOUND; /* candidate no longer exists */
    }
    r = node_walk(t->candidate, path, &node);
    if (r != COG_OK) {
        return r;
    }
    if (node->value == NULL) {
        return COG_ERROR_NOT_FOUND; /* container without a value */
    }
    *out_value = node->value;
    return COG_OK;
}

cog_result_t cog_config_set(cog_config_txn_t *txn,
                             const char *path,
                             const char *value)
{
    struct cog_config_transaction *t = txn;

    if (t == NULL || path == NULL || value == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    if (!path_valid(path, false)) {
        return COG_ERROR_INVALID_ARG;
    }
    if (t->state != COG_CONFIG_TXN_ACTIVE) {
        return COG_ERROR_INVALID_STATE;
    }
    return set_rec(t->candidate, path, value);
}

cog_result_t cog_config_delete(cog_config_txn_t *txn,
                                const char *path)
{
    struct cog_config_transaction *t = txn;

    if (t == NULL || path == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    if (!path_valid(path, false)) {
        return COG_ERROR_INVALID_ARG;
    }
    if (t->state != COG_CONFIG_TXN_ACTIVE) {
        return COG_ERROR_INVALID_STATE;
    }
    return delete_rec(t->candidate, path);
}

cog_result_t cog_config_list(const cog_config_txn_t *txn,
                              const char *path,
                              const char **out_keys,
                              size_t max_keys,
                              size_t *out_count)
{
    const struct cog_config_transaction *t = txn;
    const cog_config_node_t *node = NULL;
    cog_result_t r;
    size_t i;

    if (t == NULL || path == NULL || out_count == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    *out_count = 0U;
    if (!path_valid(path, true)) {
        return COG_ERROR_INVALID_ARG;
    }
    if (max_keys > 0U && out_keys == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    if (t->state != COG_CONFIG_TXN_ACTIVE) {
        return COG_ERROR_NOT_FOUND;
    }

    /* "" lists the root's children. */
    r = node_walk(t->candidate, path, &node);
    if (r != COG_OK) {
        return r;
    }

    if (node->child_count > max_keys && max_keys > 0U) {
        *out_count = node->child_count;
        return COG_ERROR_OVERFLOW; /* no truncation, fail loudly */
    }
    if (max_keys > 0U) {
        for (i = 0U; i < node->child_count; ++i) {
            out_keys[i] = node->children[i]->name;
        }
    }
    *out_count = node->child_count;
    return COG_OK;
}

/* ------------------------------------------------------------------ */
/* Validators                                                           */
/* ------------------------------------------------------------------ */

cog_result_t cog_config_register_validator(cog_config_tree_t *tree,
                                            const char *prefix,
                                            cog_config_validator_fn validator)
{
    char *copy;

    if (tree == NULL || prefix == NULL || validator == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    if (!path_valid(prefix, true)) {
        return COG_ERROR_INVALID_ARG;
    }
    if (tree->validator_count >= COG_CONFIG_MAX_VALIDATORS) {
        return COG_ERROR_OVERFLOW;
    }
    copy = strdup(prefix);
    if (copy == NULL) {
        return COG_ERROR_NOMEM;
    }
    tree->validators[tree->validator_count].prefix = copy;
    tree->validators[tree->validator_count].fn = validator;
    tree->validator_count++;
    return COG_OK;
}

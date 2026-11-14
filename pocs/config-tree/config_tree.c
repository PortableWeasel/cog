/**
 * @file config_tree.c
 * @brief Implementation of the config-tree PoC (see config_tree.h).
 *
 * Data structure
 * ==============
 *
 * A configuration is a tree of nodes:
 *
 *   struct cog_cfg_node {
 *       struct cog_cfg_node *parent;
 *       char               *name;    // NULL only for the root
 *       char               *value;  // NULL when the node carries no value
 *       struct cog_cfg_node **children;
 *       size_t              child_count;
 *       size_t              child_cap;
 *   };
 *
 * A leaf is simply a node with children == NULL; any node may carry both
 * children and a value (the root never carries a value).
 *
 * A cog_cfg_t owns three trees:
 *
 *   running   -- what the rest of the system reads.
 *   candidate -- staged set/delete operations, invisible to readers.
 *   saved     -- the previous running tree, i.e. the rollback point.
 *
 * Commit is *copy-on-commit*: the candidate is deep-copied into a fresh
 * running tree and the old running tree is demoted to the rollback slot.
 * This makes commit atomic by construction -- nothing in the running tree
 * is mutated in place, so a mid-commit failure can never leave a partial
 * configuration visible. The trade-off is an O(n) copy per commit; a
 * production implementation would switch to copy-on-write or a persistent
 * tree (see README.md).
 */

#include "config_tree.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Node                                                                */
/* ------------------------------------------------------------------ */

struct cog_cfg_node {
    struct cog_cfg_node *parent;
    char *name;               /* NULL only for the tree root */
    char *value;              /* NULL when the node carries no value */
    struct cog_cfg_node **children;
    size_t child_count;
    size_t child_cap;
    int dirty;                /* changed since last commit/rollback */
};

/* ------------------------------------------------------------------ */
/* Node helpers                                                       */
/* ------------------------------------------------------------------ */

/** malloc + copy, our own strdup so we stay POSIX-independent. */
static char *cfg_strdup(const char *s)
{
    if (s == NULL) {
        return NULL;
    }
    size_t len = strlen(s);
    char *d = (char *)malloc(len + 1U);
    if (d == NULL) {
        return NULL;
    }
    memcpy(d, s, len + 1U);
    return d;
}

/** Create a node; @p name may be NULL (root). @p name_len is its length. */
static struct cog_cfg_node *node_new_n(const char *name, size_t name_len)
{
    struct cog_cfg_node *n = (struct cog_cfg_node *)calloc(1U, sizeof *n);
    if (n == NULL) {
        return NULL;
    }
    if (name != NULL) {
        n->name = (char *)malloc(name_len + 1U);
        if (n->name == NULL) {
            free(n);
            return NULL;
        }
        memcpy(n->name, name, name_len);
        n->name[name_len] = '\0';
    }
    return n;
}

/** Convenience wrapper: create a node from a NUL-terminated name. */
static struct cog_cfg_node *node_new(const char *name)
{
    return node_new_n(name, name != NULL ? strlen(name) : 0U);
}

/** Free a node and all of its descendants. */
static void node_free(struct cog_cfg_node *n)
{
    if (n == NULL) {
        return;
    }
    for (size_t i = 0U; i < n->child_count; i++) {
        node_free(n->children[i]);
    }
    free(n->children);
    free(n->name);
    free(n->value);
    free(n);
}

/** Append @p child to @p parent, growing the children array as needed. */
static int node_add_child(struct cog_cfg_node *parent, struct cog_cfg_node *child)
{
    if (parent->child_count == parent->child_cap) {
        size_t new_cap = parent->child_cap != 0U ? parent->child_cap * 2U : 4U;
        struct cog_cfg_node **nc =
            (struct cog_cfg_node **)realloc(parent->children, new_cap * sizeof *nc);
        if (nc == NULL) {
            return COG_CFG_ENOMEM;
        }
        parent->children = nc;
        parent->child_cap = new_cap;
    }
    parent->children[parent->child_count] = child;
    parent->child_count++;
    child->parent = parent;
    return COG_CFG_OK;
}

/** Look up a child by name slice; returns NULL when absent. */
static struct cog_cfg_node *node_find_child_n(const struct cog_cfg_node *cur,
                                              const char *name, size_t name_len)
{
    for (size_t i = 0U; i < cur->child_count; i++) {
        struct cog_cfg_node *c = cur->children[i];
        if (strlen(c->name) == name_len && memcmp(c->name, name, name_len) == 0) {
            return c;
        }
    }
    return NULL;
}

/** Remove @p child from @p parent's children array (no free). */
static void node_remove_child(struct cog_cfg_node *parent, struct cog_cfg_node *child)
{
    for (size_t i = 0U; i < parent->child_count; i++) {
        if (parent->children[i] == child) {
            for (size_t j = i + 1U; j < parent->child_count; j++) {
                parent->children[j - 1U] = parent->children[j];
            }
            parent->child_count--;
            child->parent = NULL;
            return;
        }
    }
}

/** Mark a node and every ancestor as changed (validator scoping). */
static void node_mark_dirty(struct cog_cfg_node *n)
{
    while (n != NULL && !n->dirty) {
        n->dirty = 1;
        n = n->parent;
    }
}

/**
 * Deep-copy a node and its whole subtree.
 * Returns NULL on allocation failure (caller owns no partial result).
 * The copy is always clean (dirty == 0): fresh candidates derived from a
 * running tree start unchanged.
 */
static struct cog_cfg_node *node_copy(const struct cog_cfg_node *src)
{
    struct cog_cfg_node *d = node_new_n(src->name, src->name != NULL ? strlen(src->name) : 0U);
    if (d == NULL) {
        return NULL;
    }
    if (src->value != NULL) {
        d->value = cfg_strdup(src->value);
        if (d->value == NULL) {
            node_free(d);
            return NULL;
        }
    }
    for (size_t i = 0U; i < src->child_count; i++) {
        struct cog_cfg_node *c = node_copy(src->children[i]);
        if (c == NULL) {
            node_free(d);
            return NULL;
        }
        if (node_add_child(d, c) != COG_CFG_OK) {
            node_free(c);
            node_free(d);
            return NULL;
        }
    }
    return d;
}

/* ------------------------------------------------------------------ */
/* Path handling                                                      */
/* ------------------------------------------------------------------ */

/**
 * Split the next dot-free segment off a dotted path.
 *
 * @param[in]  path    Remaining path text.
 * @param[out] seg     Start of the segment (into @p path).
 * @param[out] seg_len Length of the segment.
 * @param[out] rest    NULL if this is the last segment, else the text
 *                     after the separating '.'.
 * @return 0 on success, 1 if @p path is empty, -1 on a malformed path
 *         (empty segment: leading/trailing dot or "..").
 */
static int path_split_segment(const char *path, const char **seg,
                              size_t *seg_len, const char **rest)
{
    *seg = path;
    if (path[0] == '\0') {
        return 1; /* empty path */
    }
    const char *dot = strchr(path, '.');
    if (dot == path) {
        return -1; /* empty segment at the start */
    }
    if (dot == NULL) {
        *seg = path;
        *seg_len = strlen(path);
        *rest = NULL;
        return 0;
    }
    if (dot[1] == '\0') {
        return -1; /* trailing dot */
    }
    *seg = path;
    *seg_len = (size_t)(dot - path);
    *rest = dot + 1;
    return 0;
}

/**
 * Non-creating walk of a dotted path from @p base.
 *
 * @param[in]  base    Starting node.
 * @param[in]  path    Dotted path; "" returns @p base itself.
 * @param[out] status  COG_CFG_OK on success, COG_CFG_ENOTFOUND if a
 *                     segment is missing, COG_CFG_EINVAL if malformed.
 * @return The node found, or NULL.
 */
static const struct cog_cfg_node *node_walk(const struct cog_cfg_node *base,
                                            const char *path, int *status)
{
    *status = COG_CFG_OK;
    const struct cog_cfg_node *cur = base;
    const char *p = path;
    if (p == NULL || p[0] == '\0') {
        return cur;
    }
    for (;;) {
        const char *seg = NULL;
        const char *rest = NULL;
        size_t seg_len = 0U;
        int rc = path_split_segment(p, &seg, &seg_len, &rest);
        if (rc != 0) {
            *status = COG_CFG_EINVAL;
            return NULL;
        }
        struct cog_cfg_node *child = node_find_child_n(cur, seg, seg_len);
        if (child == NULL) {
            *status = COG_CFG_ENOTFOUND;
            return NULL;
        }
        if (rest == NULL) {
            return child;
        }
        cur = child;
        p = rest;
    }
}

/* ------------------------------------------------------------------ */
/* Mutations on a tree (used on the candidate)                        */
/* ------------------------------------------------------------------ */

/**
 * Set @p value at @p path under @p cur, creating intermediate nodes.
 * On error, any nodes created along the failed branch are removed again
 * (each recursion level cleans up the child it created), so a failing
 * set() never leaves partial nodes behind.
 */
static int set_rec(struct cog_cfg_node *cur, const char *path, const char *value)
{
    const char *seg = NULL;
    const char *rest = NULL;
    size_t seg_len = 0U;
    int rc = path_split_segment(path, &seg, &seg_len, &rest);
    if (rc != 0) {
        return COG_CFG_EINVAL;
    }

    struct cog_cfg_node *child = node_find_child_n(cur, seg, seg_len);
    int created = 0;
    if (child == NULL) {
        child = node_new_n(seg, seg_len);
        if (child == NULL) {
            return COG_CFG_ENOMEM;
        }
        int arc = node_add_child(cur, child);
        if (arc != COG_CFG_OK) {
            node_free(child);
            return arc;
        }
        created = 1;
    }

    int rc2;
    if (rest == NULL) {
        char *nv = cfg_strdup(value);
        if (nv == NULL) {
            rc2 = COG_CFG_ENOMEM;
        } else {
            free(child->value);
            child->value = nv;
            node_mark_dirty(child);
            rc2 = COG_CFG_OK;
        }
    } else {
        rc2 = set_rec(child, rest, value);
    }

    if (rc2 != COG_CFG_OK && created) {
        node_remove_child(cur, child);
        node_free(child);
    }
    return rc2;
}

/** Remove empty, valueless nodes walking upward from @p node. */
static void prune_empty(struct cog_cfg_node *node)
{
    while (node != NULL && node->parent != NULL &&
           node->child_count == 0U && node->value == NULL) {
        struct cog_cfg_node *p = node->parent;
        node_remove_child(p, node);
        node_free(node);
        node = p;
    }
}

/**
 * Delete the node at @p path under @p cur, then prune empty ancestors
 * (VyOS-style: deleting the last leaf of a subtree removes the subtree).
 */
static int delete_rec(struct cog_cfg_node *cur, const char *path)
{
    const char *seg = NULL;
    const char *rest = NULL;
    size_t seg_len = 0U;
    int rc = path_split_segment(path, &seg, &seg_len, &rest);
    if (rc != 0) {
        return COG_CFG_EINVAL;
    }

    struct cog_cfg_node *child = node_find_child_n(cur, seg, seg_len);
    if (child == NULL) {
        return COG_CFG_ENOTFOUND;
    }

    if (rest == NULL) {
        node_mark_dirty(cur); /* the parent's subtree changed */
        node_remove_child(cur, child);
        node_free(child);
        prune_empty(cur);
        return COG_CFG_OK;
    }
    return delete_rec(child, rest);
}

/* ------------------------------------------------------------------ */
/* cog_cfg_t internals                                                 */
/* ------------------------------------------------------------------ */

struct cog_cfg {
    struct cog_cfg_node *running;   /* what the system reads            */
    struct cog_cfg_node *candidate; /* session tree: mirrors running
                                       plus staged set/delete edits     */
    struct cog_cfg_node *saved;     /* previous running tree (rollback)  */
    struct cfg_validator *validators;
    size_t validator_count;
    size_t validator_cap;
    int dirty;                      /* candidate differs from running   */
};

struct cfg_validator {
    char *path;
    cog_cfg_validator_fn fn;
    void *userdata;
};

/* The struct only holds pointers and size_t fields, so its alignment is
 * never stricter than malloc's guarantee (max_align_t). This lets callers
 * use plain malloc(cog_cfg_sizeof()) as caller-owned storage. */
_Static_assert(_Alignof(struct cog_cfg) <= _Alignof(max_align_t),
               "cog_cfg_t alignment exceeds malloc guarantee");

size_t cog_cfg_sizeof(void)
{
    return sizeof(struct cog_cfg);
}

/** Lazily ensure the candidate tree exists (it may be NULL after OOM). */
static struct cog_cfg_node *cfg_candidate(cog_cfg_t *cfg)
{
    if (cfg->candidate == NULL) {
        cfg->candidate = node_new(NULL);
    }
    return cfg->candidate;
}

/**
 * Replace the candidate with a clean copy of @p base. Allocation-atomic:
 * on failure the old candidate is kept and the object stays dirty, so a
 * failed commit/rollback can be retried. Resets the object dirty flag.
 */
static int cfg_candidate_from(cog_cfg_t *cfg, const struct cog_cfg_node *base)
{
    struct cog_cfg_node *nc = node_copy(base);
    if (nc == NULL) {
        return COG_CFG_ENOMEM;
    }
    node_free(cfg->candidate);
    cfg->candidate = nc;
    cfg->dirty = 0;
    return COG_CFG_OK;
}

/** Validate a dotted path without creating anything. */
static int cfg_path_valid(const char *path)
{
    const char *p = path;
    if (p == NULL || p[0] == '\0') {
        return COG_CFG_OK; /* "" is valid for whole-tree validators */
    }
    for (;;) {
        const char *seg = NULL;
        const char *rest = NULL;
        size_t seg_len = 0U;
        int rc = path_split_segment(p, &seg, &seg_len, &rest);
        if (rc != 0) {
            return COG_CFG_EINVAL;
        }
        if (rest == NULL) {
            return COG_CFG_OK;
        }
        p = rest;
    }
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

int cog_cfg_init(cog_cfg_t *cfg)
{
    if (cfg == NULL) {
        return COG_CFG_EINVAL;
    }
    memset(cfg, 0, sizeof *cfg);
    cfg->running = node_new(NULL);
    cfg->candidate = node_new(NULL);
    cfg->saved = node_new(NULL); /* initial known-good: the empty config */
    if (cfg->running == NULL || cfg->candidate == NULL || cfg->saved == NULL) {
        cog_cfg_fini(cfg);
        return COG_CFG_ENOMEM;
    }
    return COG_CFG_OK;
}

void cog_cfg_fini(cog_cfg_t *cfg)
{
    if (cfg == NULL) {
        return;
    }
    node_free(cfg->running);
    node_free(cfg->candidate);
    node_free(cfg->saved);
    for (size_t i = 0U; i < cfg->validator_count; i++) {
        free(cfg->validators[i].path);
    }
    free(cfg->validators);
    memset(cfg, 0, sizeof *cfg);
}

/* ------------------------------------------------------------------ */
/* Candidate operations                                                */
/* ------------------------------------------------------------------ */

int cog_cfg_set(cog_cfg_t *cfg, const char *path, const char *value)
{
    if (cfg == NULL || path == NULL || value == NULL) {
        return COG_CFG_EINVAL;
    }
    /* Validate the whole path up front so a malformed path is always
     * EINVAL regardless of which segments already exist. */
    if (cfg_path_valid(path) != COG_CFG_OK) {
        return COG_CFG_EINVAL;
    }
    struct cog_cfg_node *cand = cfg_candidate(cfg);
    if (cand == NULL) {
        return COG_CFG_ENOMEM;
    }
    int rc = set_rec(cand, path, value);
    if (rc == COG_CFG_OK) {
        cfg->dirty = 1;
    }
    return rc;
}

int cog_cfg_delete(cog_cfg_t *cfg, const char *path)
{
    if (cfg == NULL || path == NULL) {
        return COG_CFG_EINVAL;
    }
    if (cfg_path_valid(path) != COG_CFG_OK) {
        return COG_CFG_EINVAL;
    }
    struct cog_cfg_node *cand = cfg_candidate(cfg);
    if (cand == NULL) {
        return COG_CFG_ENOMEM;
    }
    int rc = delete_rec(cand, path);
    if (rc == COG_CFG_OK) {
        cfg->dirty = 1;
    }
    return rc;
}

int cog_cfg_candidate_get(const cog_cfg_t *cfg, const char *path,
                          char *buf, size_t bufsize)
{
    if (cfg == NULL || path == NULL) {
        return COG_CFG_EINVAL;
    }
    if (buf == NULL || bufsize == 0U) {
        return COG_CFG_EINVAL;
    }
    if (cfg_path_valid(path) != COG_CFG_OK) {
        return COG_CFG_EINVAL;
    }
    const struct cog_cfg_node *cand = cfg->candidate;
    if (cand == NULL) {
        return COG_CFG_ENOTFOUND;
    }
    int status = COG_CFG_OK;
    const struct cog_cfg_node *n = node_walk(cand, path, &status);
    if (status != COG_CFG_OK) {
        return status;
    }
    if (n->value == NULL) {
        return COG_CFG_ENOTFOUND;
    }
    size_t len = strlen(n->value);
    if (len + 1U > bufsize) {
        return COG_CFG_EBUFSMALL;
    }
    memcpy(buf, n->value, len + 1U);
    return COG_CFG_OK;
}

/* ------------------------------------------------------------------ */
/* Running reads                                                       */
/* ------------------------------------------------------------------ */

int cog_cfg_get(const cog_cfg_t *cfg, const char *path,
                char *buf, size_t bufsize)
{
    if (cfg == NULL || path == NULL) {
        return COG_CFG_EINVAL;
    }
    if (buf == NULL || bufsize == 0U) {
        return COG_CFG_EINVAL;
    }
    if (cfg_path_valid(path) != COG_CFG_OK) {
        return COG_CFG_EINVAL;
    }
    int status = COG_CFG_OK;
    const struct cog_cfg_node *n = node_walk(cfg->running, path, &status);
    if (status != COG_CFG_OK) {
        return status;
    }
    if (n->value == NULL) {
        return COG_CFG_ENOTFOUND;
    }
    size_t len = strlen(n->value);
    if (len + 1U > bufsize) {
        return COG_CFG_EBUFSMALL;
    }
    memcpy(buf, n->value, len + 1U);
    return COG_CFG_OK;
}

/* ------------------------------------------------------------------ */
/* Validators                                                          */
/* ------------------------------------------------------------------ */

int cog_cfg_register_validator(cog_cfg_t *cfg, const char *path,
                               cog_cfg_validator_fn fn, void *userdata)
{
    if (cfg == NULL || path == NULL || fn == NULL) {
        return COG_CFG_EINVAL;
    }
    if (cfg_path_valid(path) != COG_CFG_OK) {
        return COG_CFG_EINVAL;
    }

    if (cfg->validator_count == cfg->validator_cap) {
        size_t new_cap = cfg->validator_cap != 0U ? cfg->validator_cap * 2U : 4U;
        struct cfg_validator *nv =
            (struct cfg_validator *)realloc(cfg->validators, new_cap * sizeof *nv);
        if (nv == NULL) {
            return COG_CFG_ENOMEM;
        }
        cfg->validators = nv;
        cfg->validator_cap = new_cap;
    }

    char *pcopy = cfg_strdup(path);
    if (pcopy == NULL) {
        return COG_CFG_ENOMEM;
    }
    cfg->validators[cfg->validator_count].path = pcopy;
    cfg->validators[cfg->validator_count].fn = fn;
    cfg->validators[cfg->validator_count].userdata = userdata;
    cfg->validator_count++;
    return COG_CFG_OK;
}

/* ------------------------------------------------------------------ */
/* Commit / rollback                                                   */
/* ------------------------------------------------------------------ */

int cog_cfg_commit(cog_cfg_t *cfg)
{
    if (cfg == NULL) {
        return COG_CFG_EINVAL;
    }
    const struct cog_cfg_node *cand = cfg->candidate;
    if (cand == NULL || cand->child_count == 0U || !cfg->dirty) {
        return COG_CFG_OK; /* nothing staged: successful no-op */
    }

    /* 1. Run every registered validator against the candidate. The
     *    candidate is a session copy of the running tree plus staged
     *    edits; a validator fires only when its registered subtree is
     *    dirty (changed since the last commit/rollback), so untouched
     *    subtrees are not re-validated. A subtree that was deleted
     *    outright no longer exists and is skipped. */
    for (size_t i = 0U; i < cfg->validator_count; i++) {
        const struct cfg_validator *v = &cfg->validators[i];
        int status = COG_CFG_OK;
        const struct cog_cfg_node *sub = node_walk(cand, v->path, &status);
        if (status == COG_CFG_EINVAL) {
            return COG_CFG_EINVAL; /* unreachable: path checked at registration */
        }
        if (sub == NULL || !sub->dirty) {
            continue; /* subtree deleted or unchanged: skip */
        }
        if (v->fn(sub, v->path, v->userdata) != 0) {
            /* Reject the staged changes outright; the session restarts
             * from the (unchanged) running tree. */
            (void)cfg_candidate_from(cfg, cfg->running);
            return COG_CFG_EVALIDATE;
        }
    }

    /* 2. Pre-allocate the fresh candidate (a clean copy that mirrors the
     *    newly installed running tree) before any live pointer moves, so
     *    a mid-commit allocation failure still leaves everything
     *    untouched and retryable. */
    struct cog_cfg_node *new_candidate = node_copy(cand);
    if (new_candidate == NULL) {
        return COG_CFG_ENOMEM; /* candidate preserved; caller may retry */
    }

    /* 3. Swap: the validated candidate is promoted to running; the old
     *    running tree becomes the rollback point. */
    node_free(cfg->saved);
    cfg->saved = cfg->running;
    cfg->running = (struct cog_cfg_node *)cand;
    cfg->candidate = new_candidate;
    cfg->dirty = 0;
    return COG_CFG_OK;
}

int cog_cfg_rollback(cog_cfg_t *cfg)
{
    if (cfg == NULL) {
        return COG_CFG_EINVAL;
    }
    if (cfg->saved == NULL) {
        return COG_CFG_ENOROLLBACK;
    }
    /* Pre-allocate the session candidate for the restored tree so a
     * failure leaves the current running tree untouched. */
    struct cog_cfg_node *nc = node_copy(cfg->saved);
    if (nc == NULL) {
        return COG_CFG_ENOMEM;
    }
    node_free(cfg->running);
    cfg->running = cfg->saved;
    cfg->saved = NULL; /* single-slot rollback: consumed */
    node_free(cfg->candidate);
    cfg->candidate = nc;
    cfg->dirty = 0;
    return COG_CFG_OK;
}

/* ------------------------------------------------------------------ */
/* Display                                                             */
/* ------------------------------------------------------------------ */

static void show_rec(const struct cog_cfg_node *n, const char *path, FILE *out)
{
    for (size_t i = 0U; i < n->child_count; i++) {
        const struct cog_cfg_node *c = n->children[i];
        size_t plen = strlen(path);
        size_t clen = strlen(c->name);
        size_t need = (plen > 0U ? plen + 1U : 0U) + clen + 1U;
        char *full = (char *)malloc(need);
        if (full == NULL) {
            continue;
        }
        if (plen > 0U) {
            memcpy(full, path, plen);
            full[plen] = '.';
            memcpy(full + plen + 1U, c->name, clen + 1U);
        } else {
            memcpy(full, c->name, clen + 1U);
        }
        if (c->value != NULL) {
            fprintf(out, "%s = %s\n", full, c->value);
        }
        show_rec(c, full, out);
        free(full);
    }
}

static void show_tree(const struct cog_cfg_node *root, FILE *out)
{
    if (root == NULL || out == NULL) {
        return;
    }
    show_rec(root, "", out);
}

void cog_cfg_show(const cog_cfg_t *cfg, FILE *out)
{
    if (cfg == NULL) {
        return;
    }
    show_tree(cfg->running, out);
}

void cog_cfg_show_candidate(const cog_cfg_t *cfg, FILE *out)
{
    if (cfg == NULL) {
        return;
    }
    show_tree(cfg->candidate, out);
}

/* ------------------------------------------------------------------ */
/* Node inspection                                                     */
/* ------------------------------------------------------------------ */

const char *cog_cfg_node_name(const cog_cfg_node_t *node)
{
    if (node == NULL || node->name == NULL) {
        return "";
    }
    return node->name;
}

const char *cog_cfg_node_value(const cog_cfg_node_t *node)
{
    if (node == NULL) {
        return NULL;
    }
    return node->value;
}

size_t cog_cfg_node_child_count(const cog_cfg_node_t *node)
{
    if (node == NULL) {
        return 0U;
    }
    return node->child_count;
}

const cog_cfg_node_t *cog_cfg_node_child(const cog_cfg_node_t *node, size_t idx)
{
    if (node == NULL || idx >= node->child_count) {
        return NULL;
    }
    return node->children[idx];
}

int cog_cfg_node_get_value(const cog_cfg_node_t *root, const char *path,
                           char *buf, size_t bufsize)
{
    if (root == NULL || path == NULL) {
        return COG_CFG_EINVAL;
    }
    if (buf == NULL || bufsize == 0U) {
        return COG_CFG_EINVAL;
    }
    if (cfg_path_valid(path) != COG_CFG_OK) {
        return COG_CFG_EINVAL;
    }
    int status = COG_CFG_OK;
    const struct cog_cfg_node *n = node_walk(root, path, &status);
    if (status != COG_CFG_OK) {
        return status;
    }
    if (n->value == NULL) {
        return COG_CFG_ENOTFOUND;
    }
    size_t len = strlen(n->value);
    if (len + 1U > bufsize) {
        return COG_CFG_EBUFSMALL;
    }
    memcpy(buf, n->value, len + 1U);
    return COG_CFG_OK;
}

/* ------------------------------------------------------------------ */
/* Utilities                                                           */
/* ------------------------------------------------------------------ */

const char *cog_cfg_strerror(int code)
{
    switch (code) {
    case COG_CFG_OK:
        return "success";
    case COG_CFG_EINVAL:
        return "invalid argument";
    case COG_CFG_ENOMEM:
        return "out of memory";
    case COG_CFG_ENOTFOUND:
        return "path not found";
    case COG_CFG_EVALIDATE:
        return "configuration failed validation";
    case COG_CFG_ENOROLLBACK:
        return "no previous configuration to roll back to";
    case COG_CFG_EBUFSMALL:
        return "buffer too small for value";
    default:
        return "unknown error";
    }
}

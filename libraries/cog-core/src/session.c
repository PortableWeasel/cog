/**
 * @file session.c
 * @brief Implementation of the hierarchical conversation tree declared in
 *        cog/session.h.
 *
 * Ticket: 0f055f5 (Stage 12.1a — Implement session.c)
 *
 * Design notes (the full executable contract is recorded in
 * libraries/cog-core/contracts/session/CONTRACT.md):
 *
 * - A session owns every node and every string it stores.  All content,
 *   tool, raw-data and metadata strings are deep-copied on insertion and
 *   released by cog_session_free / cog_session_reset / discard / prune.
 *   No hidden state exists outside the session struct.
 * - The tree is a plain parent/child/sibling structure.  The active node
 *   is the most recently added node; cog_session_append extends it.
 * - cog_session_fork deep-copies the fork-point node (data only, no
 *   children) and inserts the copy as the next sibling of the source, so
 *   the new branch shares all ancestors by reference while owning its own
 *   children.  Forking the root is a no-op that returns the root itself:
 *   the root is already the shared ancestor of every branch.
 * - cog_session_get_history returns a pointer into a per-session cache
 *   that is rebuilt on demand.  The cache is an implementation detail and
 *   is updated through the const-qualified session pointer.
 * - max_nodes is a soft cap: after each insertion the oldest leaf nodes
 *   (smallest node_id, excluding the root and the active node) are pruned
 *   until the count fits.  If only the root/active remain, the cap is
 *   exceeded rather than corrupting the tree.  A prunable-leaf counter
 *   avoids scanning the tree on chains where no pruning can occur.
 */
#define _POSIX_C_SOURCE 200809L

#include "cog/session.h"

#include "session_internal.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/** Default node cap when cog_session_config_t.max_nodes is zero. */
#define COG_SESSION_DEFAULT_MAX_NODES ((size_t)1024)

/* ------------------------------------------------------------------ */
/* Internal helpers                                                    */
/* ------------------------------------------------------------------ */

/** Deep-copy a NUL-terminated string; NULL stays NULL. */
static char *dup_str(const char *s)
{
    if (s == NULL) {
        return NULL;
    }
    return strdup(s);
}

/**
 * Deep-copy @p len bytes of @p s plus a NUL terminator.  NULL stays NULL.
 * Embedded NUL bytes are payload (content is length-prefixed).
 */
static char *dup_bytes(const char *s, size_t len)
{
    char *copy;

    if (s == NULL) {
        return NULL;
    }
    copy = malloc(len + 1U);
    if (copy == NULL) {
        return NULL;
    }
    if (len > 0U) {
        (void)memcpy(copy, s, len);
    }
    copy[len] = '\0';
    return copy;
}

/** Release every string owned by a node (does not free the node itself). */
static void node_free_strings(struct cog_session_node *n)
{
    free((void *)n->data.content);
    free((void *)n->data.tool_name);
    free((void *)n->data.tool_args);
    free((void *)n->data.tool_result);
    free((void *)n->data.raw_request);
    free((void *)n->data.raw_response);
    free((void *)n->data.model);
    free((void *)n->data.provider);
    n->data.content = NULL;
    n->data.tool_name = NULL;
    n->data.tool_args = NULL;
    n->data.tool_result = NULL;
    n->data.raw_request = NULL;
    n->data.raw_response = NULL;
    n->data.model = NULL;
    n->data.provider = NULL;
}

/** Free a subtree in post-order (children first). */
static void node_free_deep(struct cog_session_node *n)
{
    struct cog_session_node *child = n->first_child;

    while (child != NULL) {
        struct cog_session_node *next = child->next_sibling;
        node_free_deep(child);
        child = next;
    }
    node_free_strings(n);
    free(n);
}

/** True if @p n is a leaf that max_nodes pruning may remove. */
static bool node_is_prunable(const cog_session_t *s,
                             const struct cog_session_node *n)
{
    return n != NULL && n != s->root && n != s->active &&
           n->child_count == 0U;
}

/** Keep the prunable-leaf counter exact after a local tree/active change. */
static void session_sync_prunable(cog_session_t *s,
                                  struct cog_session_node *n)
{
    const bool prunable = node_is_prunable(s, n);

    if (n == NULL || prunable == n->prunable_counted) {
        return;
    }
    if (prunable) {
        s->prunable_leaf_count++;
        n->prunable_counted = true;
    } else {
        s->prunable_leaf_count--;
        n->prunable_counted = false;
    }
}

/** O(1) ownership check; a node belongs to exactly one session. */
static bool session_contains(const cog_session_t *s,
                             const cog_session_node_t *n)
{
    return s != NULL && n != NULL && n->session == s;
}

/** Drop the cached history array contents (the buffer itself is kept). */
static void session_invalidate_history(cog_session_t *s)
{
    s->history_count = 0U;
}

/**
 * Link @p n as the last child of @p parent (or as the root when @p parent
 * is NULL), advance the active node, and bump the node count.
 */
static void node_link(cog_session_t *s, struct cog_session_node *parent,
                      struct cog_session_node *n)
{
    struct cog_session_node *old_active = s->active;

    n->session = s;
    n->parent = parent;
    n->data.parent_id = (parent != NULL) ? parent->data.node_id : 0U;
    if (parent == NULL) {
        s->root = n;
    } else {
        n->prev_sibling = parent->last_child;
        if (parent->last_child != NULL) {
            parent->last_child->next_sibling = n;
        } else {
            parent->first_child = n;
        }
        parent->last_child = n;
        parent->child_count++;
    }
    s->node_count++;
    s->active = n;
    session_sync_prunable(s, parent);
    session_sync_prunable(s, old_active);
    session_sync_prunable(s, n);
    session_invalidate_history(s);
}

/**
 * Link @p n immediately after @p anchor (same parent).  Used by fork to
 * insert the branch copy as the next sibling of the fork point.
 */
static void node_link_after(cog_session_t *s,
                            struct cog_session_node *anchor,
                            struct cog_session_node *n)
{
    struct cog_session_node *p = anchor->parent;
    struct cog_session_node *old_active = s->active;

    n->session = s;
    n->parent = p;
    n->data.parent_id = p->data.node_id;
    n->prev_sibling = anchor;
    n->next_sibling = anchor->next_sibling;
    if (anchor->next_sibling != NULL) {
        anchor->next_sibling->prev_sibling = n;
    }
    anchor->next_sibling = n;
    if (p->last_child == anchor) {
        p->last_child = n;
    }
    p->child_count++;
    s->node_count++;
    s->active = n;
    session_sync_prunable(s, p);
    session_sync_prunable(s, old_active);
    session_sync_prunable(s, n);
    session_invalidate_history(s);
}

/** Unlink @p n from the tree (does not free it). */
static void node_unlink(cog_session_t *s, struct cog_session_node *n)
{
    struct cog_session_node *p = n->parent;

    if (n->prunable_counted) {
        s->prunable_leaf_count--;
        n->prunable_counted = false;
    }
    if (p == NULL) {
        s->root = NULL;
    } else {
        if (p->first_child == n) {
            p->first_child = n->next_sibling;
        }
        if (p->last_child == n) {
            p->last_child = n->prev_sibling;
        }
        if (n->prev_sibling != NULL) {
            n->prev_sibling->next_sibling = n->next_sibling;
        }
        if (n->next_sibling != NULL) {
            n->next_sibling->prev_sibling = n->prev_sibling;
        }
        p->child_count--;
    }
    n->session = NULL;
    n->parent = NULL;
    n->prev_sibling = NULL;
    n->next_sibling = NULL;
    s->node_count--;
    session_sync_prunable(s, p);
    session_invalidate_history(s);
}

/** Record the oldest (smallest node_id) leaf excluding root and active. */
static void find_oldest_leaf(struct cog_session_node *n,
                             const cog_session_t *s,
                             struct cog_session_node **out,
                             uint64_t *out_id)
{
    struct cog_session_node *child;

    if (n == NULL) {
        return;
    }
    if (n->child_count == 0U && n != s->root && n != s->active) {
        if (*out == NULL || n->data.node_id < *out_id) {
            *out = n;
            *out_id = n->data.node_id;
        }
    }
    for (child = n->first_child; child != NULL; child = child->next_sibling) {
        find_oldest_leaf(child, s, out, out_id);
    }
}

/**
 * Enforce the max_nodes soft cap: repeatedly prune the oldest leaf that is
 * neither the root nor the active node until the count fits.
 */
static void session_prune(cog_session_t *s)
{
    while (s->node_count > s->max_nodes && s->prunable_leaf_count > 0U) {
        struct cog_session_node *oldest = NULL;
        uint64_t oldest_id = 0U;

        find_oldest_leaf(s->root, s, &oldest, &oldest_id);
        if (oldest == NULL) {
            break;
        }
        node_unlink(s, oldest);
        node_free_strings(oldest);
        free(oldest);
    }
}

/** Fill a node's data fields from a chat message. */
static cog_result_t node_init_from_message(struct cog_session_node *n,
                                           const cog_message_t *msg)
{
    switch (msg->role) {
    case COG_MESSAGE_ROLE_SYSTEM:
        n->data.role = COG_NODE_SYSTEM;
        break;
    case COG_MESSAGE_ROLE_USER:
        n->data.role = COG_NODE_USER;
        break;
    case COG_MESSAGE_ROLE_ASSISTANT:
        n->data.role = COG_NODE_ASSISTANT;
        break;
    case COG_MESSAGE_ROLE_TOOL:
        /* A TOOL message is a tool result; name identifies the tool call. */
        n->data.role = COG_NODE_TOOL_RESULT;
        n->data.tool_name = dup_str(msg->name);
        n->data.tool_result = dup_bytes(msg->content, msg->content_len);
        n->tool_result_len = msg->content_len;
        if ((msg->name != NULL && n->data.tool_name == NULL) ||
            (msg->content != NULL && n->data.tool_result == NULL)) {
            return COG_ERROR_NOMEM;
        }
        return COG_OK;
    default:
        return COG_ERROR_INVALID_ARG;
    }
    n->data.content = dup_bytes(msg->content, msg->content_len);
    n->data.content_len = msg->content_len;
    if (msg->content != NULL && n->data.content == NULL) {
        return COG_ERROR_NOMEM;
    }
    return COG_OK;
}

/** Generate a process-unique session id (time + address of the session). */
static void session_generate_id(cog_session_t *s)
{
    char buf[64];
    uint64_t t = (uint64_t)time(NULL);
    uintptr_t p = (uintptr_t)s;

    (void)snprintf(buf, sizeof(buf), "sess-%016llx-%016llx",
                   (unsigned long long)t, (unsigned long long)p);
    s->id = strdup(buf);
}

/** Map a node to the chat message it contributes to the history. */
static cog_message_t node_to_message(const struct cog_session_node *n)
{
    cog_message_t m = {0};

    switch (n->data.role) {
    case COG_NODE_SYSTEM:
        m.role = COG_MESSAGE_ROLE_SYSTEM;
        m.content = n->data.content;
        m.content_len = n->data.content_len;
        break;
    case COG_NODE_USER:
        m.role = COG_MESSAGE_ROLE_USER;
        m.content = n->data.content;
        m.content_len = n->data.content_len;
        break;
    case COG_NODE_ASSISTANT:
        m.role = COG_MESSAGE_ROLE_ASSISTANT;
        m.content = n->data.content;
        m.content_len = n->data.content_len;
        break;
    case COG_NODE_TOOL_CALL:
        /* Tool identity is preserved via name; args become the payload. */
        m.role = COG_MESSAGE_ROLE_TOOL;
        m.content = n->data.tool_args;
        m.content_len = n->tool_args_len;
        m.name = n->data.tool_name;
        break;
    case COG_NODE_TOOL_RESULT:
        m.role = COG_MESSAGE_ROLE_TOOL;
        m.content = n->data.tool_result;
        m.content_len = n->tool_result_len;
        m.name = n->data.tool_name;
        break;
    default:
        /* Unreachable: roles are validated at node creation. */
        break;
    }
    return m;
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

cog_result_t cog_session_new(cog_session_t **out,
                             const cog_session_config_t *config)
{
    cog_session_t *s;

    if (out == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    *out = NULL;

    s = calloc(1, sizeof(*s));
    if (s == NULL) {
        return COG_ERROR_NOMEM;
    }
    s->max_nodes = (config != NULL && config->max_nodes != 0U)
                       ? config->max_nodes
                       : COG_SESSION_DEFAULT_MAX_NODES;
    if (config != NULL && config->id != NULL) {
        s->id = strdup(config->id);
        if (s->id == NULL) {
            free(s);
            return COG_ERROR_NOMEM;
        }
    } else {
        session_generate_id(s);
        if (s->id == NULL) {
            free(s);
            return COG_ERROR_NOMEM;
        }
    }
    s->next_node_id = 1U;
    *out = s;
    return COG_OK;
}

void cog_session_free(cog_session_t *session)
{
    if (session == NULL) {
        return;
    }
    if (session->root != NULL) {
        node_free_deep(session->root);
    }
    free(session->history);
    free(session->id);
    free(session->parent_id);
    free(session);
}

const char *cog_session_id(const cog_session_t *session)
{
    return (session != NULL) ? session->id : NULL;
}

cog_result_t cog_session_reset(cog_session_t *session)
{
    if (session == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    if (session->attached) {
        return COG_ERROR_INVALID_STATE;
    }
    if (session->root != NULL) {
        node_free_deep(session->root);
        session->root = NULL;
    }
    session->active = NULL;
    session->node_count = 0U;
    session->prunable_leaf_count = 0U;
    session->next_node_id = 1U;
    session_invalidate_history(session);
    return COG_OK;
}

bool cog_session_is_empty(const cog_session_t *session)
{
    return (session == NULL) || (session->node_count == 0U);
}

/* ------------------------------------------------------------------ */
/* Tree navigation                                                     */
/* ------------------------------------------------------------------ */

cog_session_node_t *cog_session_root(const cog_session_t *session)
{
    return (session != NULL) ? session->root : NULL;
}

cog_session_node_t *cog_session_active(const cog_session_t *session)
{
    return (session != NULL) ? session->active : NULL;
}

cog_session_node_t *cog_node_parent(const cog_session_node_t *node)
{
    return (node != NULL) ? node->parent : NULL;
}

cog_session_node_t *cog_node_first_child(const cog_session_node_t *node)
{
    return (node != NULL) ? node->first_child : NULL;
}

cog_session_node_t *cog_node_next_sibling(const cog_session_node_t *node)
{
    return (node != NULL) ? node->next_sibling : NULL;
}

size_t cog_node_child_count(const cog_session_node_t *node)
{
    return (node != NULL) ? node->child_count : 0U;
}

/* ------------------------------------------------------------------ */
/* Node data access                                                    */
/* ------------------------------------------------------------------ */

const cog_node_data_t *cog_node_data(const cog_session_node_t *node)
{
    return (node != NULL) ? &node->data : NULL;
}

/* ------------------------------------------------------------------ */
/* Tree mutation                                                       */
/* ------------------------------------------------------------------ */

cog_result_t cog_session_add_message(cog_session_t *session,
                                     cog_session_node_t *parent,
                                     const cog_message_t *msg,
                                     cog_session_node_t **out_node)
{
    struct cog_session_node *n;
    cog_result_t rc;

    if (session == NULL || msg == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    if (parent != NULL && !session_contains(session, parent)) {
        return COG_ERROR_NOT_FOUND;
    }

    n = calloc(1, sizeof(*n));
    if (n == NULL) {
        return COG_ERROR_NOMEM;
    }
    n->data.node_id = session->next_node_id++;

    rc = node_init_from_message(n, msg);
    if (rc != COG_OK) {
        free(n);
        return rc;
    }

    if (parent == NULL) {
        parent = session->root; /* still NULL when the session is empty */
    }
    node_link(session, parent, n);
    session_prune(session);
    if (out_node != NULL) {
        *out_node = n;
    }
    return COG_OK;
}

cog_result_t cog_session_append(cog_session_t *session,
                                const cog_message_t *msg,
                                cog_session_node_t **out_node)
{
    if (session == NULL || msg == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    return cog_session_add_message(session, session->active, msg, out_node);
}

cog_result_t cog_session_add_tool_call(cog_session_t *session,
                                       cog_session_node_t *parent,
                                       const char *tool_name,
                                       const char *tool_args,
                                       cog_session_node_t **out_node)
{
    struct cog_session_node *n;

    if (session == NULL || tool_name == NULL || tool_args == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    if (parent != NULL && !session_contains(session, parent)) {
        return COG_ERROR_NOT_FOUND;
    }

    n = calloc(1, sizeof(*n));
    if (n == NULL) {
        return COG_ERROR_NOMEM;
    }
    n->data.node_id = session->next_node_id++;
    n->data.role = COG_NODE_TOOL_CALL;
    n->data.tool_name = strdup(tool_name);
    n->tool_args_len = strlen(tool_args);
    n->data.tool_args = dup_bytes(tool_args, n->tool_args_len);
    if (n->data.tool_name == NULL || n->data.tool_args == NULL) {
        node_free_strings(n);
        free(n);
        return COG_ERROR_NOMEM;
    }

    if (parent == NULL) {
        parent = session->root;
    }
    node_link(session, parent, n);
    session_prune(session);
    if (out_node != NULL) {
        *out_node = n;
    }
    return COG_OK;
}

cog_result_t cog_session_add_tool_result(cog_session_t *session,
                                         cog_session_node_t *parent,
                                         const char *tool_result,
                                         cog_session_node_t **out_node)
{
    struct cog_session_node *n;

    if (session == NULL || parent == NULL || tool_result == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    if (!session_contains(session, parent)) {
        return COG_ERROR_NOT_FOUND;
    }

    n = calloc(1, sizeof(*n));
    if (n == NULL) {
        return COG_ERROR_NOMEM;
    }
    n->data.node_id = session->next_node_id++;
    n->data.role = COG_NODE_TOOL_RESULT;
    n->tool_result_len = strlen(tool_result);
    n->data.tool_result = dup_bytes(tool_result, n->tool_result_len);
    /* Inherit the parent tool-call node's tool name so history messages
     * retain tool identity.  Deep-copied under the same ownership rules
     * as every other node string (owned by the node, released by
     * node_free_strings).  When the parent carries no tool name (e.g. it
     * is a plain message node), the result node's tool_name stays NULL. */
    n->data.tool_name = dup_str(parent->data.tool_name);
    if (n->data.tool_result == NULL ||
        (parent->data.tool_name != NULL && n->data.tool_name == NULL)) {
        node_free_strings(n);
        free(n);
        return COG_ERROR_NOMEM;
    }

    node_link(session, parent, n);
    session_prune(session);
    if (out_node != NULL) {
        *out_node = n;
    }
    return COG_OK;
}

cog_result_t cog_session_fork(cog_session_t *session,
                              cog_session_node_t *node,
                              cog_session_node_t **out_node)
{
    struct cog_session_node *src = node;
    struct cog_session_node *copy;

    if (session == NULL || node == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    if (!session_contains(session, node)) {
        return COG_ERROR_NOT_FOUND;
    }
    if (src->parent == NULL) {
        /* The root is already the shared ancestor of every branch. */
        if (out_node != NULL) {
            *out_node = node;
        }
        return COG_OK;
    }

    copy = calloc(1, sizeof(*copy));
    if (copy == NULL) {
        return COG_ERROR_NOMEM;
    }
    copy->data = src->data; /* copy scalars and pointers first */
    copy->data.node_id = session->next_node_id++;
    copy->data.parent_id = src->data.parent_id;
    copy->tool_args_len = src->tool_args_len;
    copy->tool_result_len = src->tool_result_len;
    /* Null out the borrowed pointers, then deep-copy each owned string. */
    copy->data.content = NULL;
    copy->data.tool_name = NULL;
    copy->data.tool_args = NULL;
    copy->data.tool_result = NULL;
    copy->data.raw_request = NULL;
    copy->data.raw_response = NULL;
    copy->data.model = NULL;
    copy->data.provider = NULL;
    copy->data.content = dup_bytes(src->data.content, src->data.content_len);
    copy->data.tool_name = dup_str(src->data.tool_name);
    copy->data.tool_args = dup_bytes(src->data.tool_args,
                                     src->tool_args_len);
    copy->data.tool_result = dup_bytes(src->data.tool_result,
                                       src->tool_result_len);
    copy->data.raw_request = dup_str(src->data.raw_request);
    copy->data.raw_response = dup_str(src->data.raw_response);
    copy->data.model = dup_str(src->data.model);
    copy->data.provider = dup_str(src->data.provider);
    if ((src->data.content != NULL && copy->data.content == NULL) ||
        (src->data.tool_name != NULL && copy->data.tool_name == NULL) ||
        (src->data.tool_args != NULL && copy->data.tool_args == NULL) ||
        (src->data.tool_result != NULL && copy->data.tool_result == NULL) ||
        (src->data.raw_request != NULL && copy->data.raw_request == NULL) ||
        (src->data.raw_response != NULL && copy->data.raw_response == NULL) ||
        (src->data.model != NULL && copy->data.model == NULL) ||
        (src->data.provider != NULL && copy->data.provider == NULL)) {
        node_free_strings(copy);
        free(copy);
        return COG_ERROR_NOMEM;
    }

    node_link_after(session, src, copy);
    session_prune(session);
    if (out_node != NULL) {
        *out_node = copy;
    }
    return COG_OK;
}

/* ------------------------------------------------------------------ */
/* History / context building                                          */
/* ------------------------------------------------------------------ */

cog_result_t cog_session_get_history(const cog_session_t *session,
                                     const cog_session_node_t *node,
                                     const cog_message_t **out_msgs,
                                     size_t *out_count)
{
    cog_session_t *s = (cog_session_t *)session; /* internal cache update */
    const struct cog_session_node *n;
    size_t depth = 0U;
    size_t i;

    if (session == NULL || node == NULL || out_msgs == NULL ||
        out_count == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    if (!session_contains(session, node)) {
        return COG_ERROR_NOT_FOUND;
    }

    for (n = node; n != NULL; n = n->parent) {
        depth++;
    }

    if (depth > s->history_capacity) {
        cog_message_t *nbuf =
            realloc(s->history, depth * sizeof(*nbuf));
        if (nbuf == NULL) {
            return COG_ERROR_NOMEM;
        }
        s->history = nbuf;
        s->history_capacity = depth;
    }

    i = depth;
    for (n = node; n != NULL; n = n->parent) {
        i--;
        s->history[i] = node_to_message(n);
    }
    s->history_count = depth;
    *out_msgs = s->history;
    *out_count = depth;
    return COG_OK;
}

/* ------------------------------------------------------------------ */
/* Timing and billing                                                  */
/* ------------------------------------------------------------------ */

cog_result_t cog_session_set_timing(cog_session_t *session,
                                    cog_session_node_t *node,
                                    const cog_timing_t *timing)
{
    if (session == NULL || node == NULL || timing == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    if (!session_contains(session, node)) {
        return COG_ERROR_NOT_FOUND;
    }
    node->data.timing = *timing;
    return COG_OK;
}

cog_result_t cog_session_set_cost(cog_session_t *session,
                                  cog_session_node_t *node,
                                  const cog_cost_t *cost)
{
    if (session == NULL || node == NULL || cost == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    if (!session_contains(session, node)) {
        return COG_ERROR_NOT_FOUND;
    }
    node->data.cost = *cost;
    return COG_OK;
}

double cog_session_cumulative_cost(const cog_session_t *session,
                                   const cog_session_node_t *node)
{
    const struct cog_session_node *n;
    double total = 0.0;

    if (session == NULL || node == NULL) {
        return 0.0;
    }
    if (!session_contains(session, node)) {
        return 0.0;
    }
    for (n = node; n != NULL; n = n->parent) {
        total += n->data.cost.total_cost;
    }
    return total;
}

cog_result_t cog_session_cumulative_timing(const cog_session_t *session,
                                           const cog_session_node_t *node,
                                           cog_timing_t *out)
{
    const struct cog_session_node *n;
    cog_timing_t acc = {0};

    if (session == NULL || node == NULL || out == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    if (!session_contains(session, node)) {
        return COG_ERROR_NOT_FOUND;
    }
    for (n = node; n != NULL; n = n->parent) {
        acc.request_sent_ms += n->data.timing.request_sent_ms;
        acc.first_token_ms += n->data.timing.first_token_ms;
        acc.last_token_ms += n->data.timing.last_token_ms;
        acc.duration_ms += n->data.timing.duration_ms;
        acc.ttft_ms += n->data.timing.ttft_ms;
        acc.tokens_per_second += n->data.timing.tokens_per_second;
    }
    *out = acc;
    return COG_OK;
}

/* ------------------------------------------------------------------ */
/* Parent session                                                      */
/* ------------------------------------------------------------------ */

const char *cog_session_parent_id(const cog_session_t *session)
{
    return (session != NULL) ? session->parent_id : NULL;
}

cog_result_t cog_session_set_parent(cog_session_t *session,
                                    const char *parent_id)
{
    char *copy = NULL;

    if (session == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    if (parent_id != NULL) {
        copy = strdup(parent_id);
        if (copy == NULL) {
            return COG_ERROR_NOMEM;
        }
    }
    free(session->parent_id);
    session->parent_id = copy;
    return COG_OK;
}

/* ------------------------------------------------------------------ */
/* Node mutation (cancellation support)                                */
/* ------------------------------------------------------------------ */

cog_result_t cog_session_discard_node(cog_session_t *session,
                                      cog_session_node_t *node)
{
    struct cog_session_node *parent;

    if (session == NULL || node == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    if (node->child_count > 0U) {
        return COG_ERROR_INVALID_STATE;
    }
    if (!session_contains(session, node)) {
        return COG_ERROR_NOT_FOUND;
    }

    parent = node->parent;
    node_unlink(session, node);
    if (session->active == node) {
        session->active = parent;
        session_sync_prunable(session, parent);
    }
    node_free_strings(node);
    free(node);
    return COG_OK;
}

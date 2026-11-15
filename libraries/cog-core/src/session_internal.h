/**
 * @file session_internal.h
 * @brief Private shared layout for cog-core/src/session.c and core.c.
 *
 * This header is internal to cog-core.  It is not installed, not included
 * by any public header, and must not be used by consumers.  It exists
 * solely to give a single definition to the @c struct cog_session layout
 * (and the @c struct cog_session_node layout it depends on) that is shared
 * between session.c and core.c.
 *
 * Background: core.c needs to set and clear the session's private
 * `attached` flag (so cog_session_reset refuses while a core owns the
 * session).  The public cog_session_t is opaque and there is no public
 * attach API.  Previously session.c and core.c each defined the struct
 * privately, which risked the two layouts drifting apart.  This header is
 * the single source of truth: both translation units include it, so no
 * mirrored layout can remain.
 *
 * The `attached` field is set only by core.c's attach/detach logic
 * (cog_core_new / cog_core_free) and read by session.c's
 * cog_session_reset.
 */
#ifndef COG_SESSION_INTERNAL_H
#define COG_SESSION_INTERNAL_H

#include "cog/session.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @brief Private conversation node layout (definition of the opaque
 *        @c cog_session_node_t).
 *
 * A node owns every string in its @c data view; the session owns the
 * node.  See session.c for the full ownership and pruning rules.
 */
struct cog_session_node {
    cog_node_data_t data;          /* public read-only view; owns strings */
    cog_session_t *session;        /* owning session: O(1) containment */
    struct cog_session_node *parent;
    struct cog_session_node *first_child;
    struct cog_session_node *last_child;
    struct cog_session_node *prev_sibling;
    struct cog_session_node *next_sibling;
    size_t child_count;
    size_t tool_args_len;          /* byte length, including embedded NULs */
    size_t tool_result_len;        /* byte length, including embedded NULs */
    cog_message_t message;
    void *payload;
    bool used;
    bool shared;
    bool committed;
    bool prunable_counted;         /* counted in session.prunable_leaf_count */
};

/**
 * @brief Private session layout (definition of the opaque @c cog_session_t).
 *
 * The `attached` flag is set only by core.c (cog_core_new /
 * cog_core_free) and read by session.c's cog_session_reset.  All other
 * fields are owned and maintained by session.c.
 */
struct cog_session {
    char *id;
    char *parent_id;
    size_t max_nodes;
    size_t node_count;
    uint64_t next_node_id;
    struct cog_session_node *root;
    struct cog_session_node *active;
    size_t prunable_leaf_count;    /* leaves other than root and active */
    bool attached;                 /* set only by core.c attach/detach */
    cog_message_t *history;        /* owned cache for cog_session_get_history */
    size_t history_count;
    size_t history_capacity;
    struct cog_session_node *nodes;
    unsigned char *arena;
    size_t arena_capacity;
    size_t max_calls;
    size_t ledger_capacity;
    size_t ledger_count;
    cog_tool_ledger_record_t *ledger;
    cog_tool_audit_record_t *audits;
    cog_node_data_t *snapshot_nodes;
    uint64_t audit_high_water;
    bool inspection_only;
    bool forked;

};

/* ------------------------------------------------------------------ */
/* Compile-time regression guard                                       */
/* ------------------------------------------------------------------ */

/* Pin the field order so an accidental reordering of the shared layout
 * fails the build instead of silently moving the `attached` flag.  These
 * ordering assertions are platform-independent. */
_Static_assert(offsetof(struct cog_session, attached) >
                   offsetof(struct cog_session, prunable_leaf_count),
               "session_internal: attached must follow prunable_leaf_count");
_Static_assert(offsetof(struct cog_session, history) >
                   offsetof(struct cog_session, attached),
               "session_internal: history must follow attached");

/* On the primary 64-bit build platform the `attached` flag sits at a
 * known absolute offset; pin it so a layout change is caught at compile
 * time rather than silently shifting the flag. */
#if UINTPTR_MAX == UINT64_MAX
_Static_assert(offsetof(struct cog_session, attached) == 64U,
               "session_internal: attached offset regression (64-bit)");
#endif

#endif /* COG_SESSION_INTERNAL_H */

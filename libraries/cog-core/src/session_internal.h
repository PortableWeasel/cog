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
    cog_tool_job_record_t *jobs;
    cog_tool_output_t *job_outputs;
    char *job_storage;
    size_t job_count, max_tool_jobs, max_tool_job_output_bytes;
    void *launch_payload; /* Reserved while an intent still needs its result. */
    size_t launch_capacity;
    cog_session_context_t *contexts;
    cog_session_context_binding_t *context_bindings;
    unsigned char *context_data;
    size_t context_count, context_binding_count;
    size_t max_contexts, max_context_bindings, max_context_bytes, context_bytes;
    uint64_t context_high_water, context_binding_high_water, selected_context_binding_id;

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

/** Logical persistence bookkeeping; source is constructor-owned mutable storage.
 * Called only after successful/uncertain publication, never changes view bytes.
 * Marks every occupied slot, including slots beyond holes left by draft discard. */
void cog_session_mark_committed(const cog_session_t *session);

/** Allocation-free validation of exact public snapshot records. No normalization,
 * allocation, mutation, disk or activation. Shared by restore and bounded store
 * admission/diff checks. Return OK, NULL_PTR, PROTOCOL, LIMIT identity exhaustion
 * or unsupported unsafe legacy tools. inspection_only restricts, never grants.
 */
cog_result_t cog_session_snapshot_validate(const cog_session_snapshot_t *snapshot);

#endif /* COG_SESSION_INTERNAL_H */

/** @file execution_internal.h
 * @brief O4 executor implementation layout; not another execution interface.
 * One owner thread. cli.c owns allocation/disposal, execution.c owns drive,
 * authority and frozen-copy state; app_turn/tools/sessions/reload use the named
 * helpers below. No bodies, callbacks, configuration internals or wire authority.
 */
#ifndef COG_CLI_EXECUTION_INTERNAL_H
#define COG_CLI_EXECUTION_INTERNAL_H
#include "execution.h"
#include "delegation.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Driver ownership and durable authority are independent axes: freezing an idle
 * source does not restart/end its core, and guarded STAGED is not active. */
typedef enum cli_drive_phase {
    CLI_DRIVE_UNBEGUN = 0, CLI_DRIVE_BEGUN, CLI_DRIVE_DRAINING, CLI_DRIVE_DONE
} cli_drive_phase_t;
typedef enum cli_execution_authority {
    CLI_AUTH_NORMAL = 0, CLI_AUTH_STAGED, CLI_AUTH_SOURCE_FROZEN,
    CLI_AUTH_SOURCE_RELEASED, CLI_AUTH_TARGET_GUARDED
} cli_execution_authority_t;
/** Saved continuation AFTER the visible mutation, BEFORE its next effect.
 * Values select existing static K handlers, never arbitrary function pointers. */
typedef enum cli_after_publication {
    CLI_AFTER_NONE = 0, CLI_AFTER_USER_SAVE, CLI_AFTER_TEXT,
    CLI_AFTER_ASSISTANT_SAVE, CLI_AFTER_TOOL_SAVE, CLI_AFTER_POLICY,
    CLI_AFTER_CONTROL
} cli_after_publication_t;

/** App-owned record, allocated only by common resolved app construction.
 * app_internal.h stores an opaque pointer; NULL keeps standalone M behavior.
 * All arrays/bytes reserved with the app, distinct from mutable live text/history.
 */
typedef struct cli_execution_state {
    cog_cli_execution_kind_t kind;
    cli_drive_phase_t drive;
    cli_execution_authority_t authority;
    bool in_step, drive_ended, activation_accepted, session_borrowed;
    cog_result_t primary;
    cog_cli_delegation_context_t *delegation; /**< App owns; tool handles borrow. */
    uint64_t absolute_deadline_ms;
    uint32_t tool_mask; /**< Opaque configuration getter copy, not ALLOW. */
    char parent_session_id[128];
    char *task, *provenance; /**< Owned constructor copies, no root input queue. */
    size_t task_bytes, provenance_bytes;
    cog_store_owner_t expected_owner, next_owner;
    cog_store_checkpoint_t checkpoint; /**< Exact saved/loaded token, never latest. */
    bool checkpoint_saved, source_released, target_guard_acquired;
    cog_cli_display_snapshot_t frozen;
    cog_cli_display_item_t *frozen_items;
    char *frozen_text;
    size_t frozen_item_capacity, frozen_text_capacity;
    uint64_t mutation_serial, pending_serial, committed_serial;
    uint64_t committed_generation, committed_sequence;
    bool publication_pending, newer_view_dirty, publication_event_queued;
    cli_after_publication_t after_publication;
    cog_state_t resume_state;
    uint64_t prompt_serial, retired_prompt_serial;
    cog_cli_policy_view_t policy;
    char policy_session[128], policy_tool[256];
    bool policy_pending, policy_retirement_pending;
    /* Existing app->reload_attempts/current/future fields remain authoritative;
       STAGED imports the logical count including failed attempts into those. */
} cli_execution_state_t;

/** @brief Common cli.c constructor after exactly one configuration resolution.
 * @param[out] out NULL on failure. @param[in,out] admission Consumes/NULLs its
 * configuration on EVERY return, matching execution_new. Other data copied or
 * exact local_session borrowed. No source reread, nested app_new, CAS or fork.
 * @return Existing execution_new construction errors; no task before initial
 * child SYSTEM checkpoint, no restored proposal before context attachment.
 * cli.c constructs resources; execution.c validates the chosen admission mode.
 * Allocate/mark headless session ownership BEFORE installing a local borrow;
 * partial unwind never falls through standalone owned-session destruction.
 */
cog_result_t cli_app_construct_resolved(cog_cli_app_t **out,
    cog_cli_execution_admission_t *admission);
/** @brief cli.c consuming resource disposal AFTER checked close preconditions.
 * @param[in,out] app Consumed/NULL on return; NULL pointee OK.
 * @param[in] primary First error wins. @return primary/first cleanup/NULL_PTR.
 * Destroy toolkit handles then delegation while app lives; core must not drive;
 * provider consuming disconnect precedes matching secret/config free. Never free
 * borrowed registry session. Net/observer remain caller-slot owned in that order.
 * execution_close validates live/drain/borrow conditions before invoking this.
 */
cog_result_t cli_app_dispose(cog_cli_app_t **app, cog_result_t primary);
/** @brief execution.c stage visible state and select publication continuation.
 * @param[in,out] app Owner outside publication recursion. @param[in] after
 * Existing completed mutation. @param[in] resume Existing static K state.
 * @param[out] next CLI_PUBLICATION if headless, resume otherwise.
 * @return OK (including standalone no-barrier); NULL_PTR; INVALID_STATE;
 * LIMIT exhausted serial/copy bound. Capture full actual snapshot into dedicated
 * reserved storage before returning; never overwrite an outstanding frozen view.
 * If a prior copy is outstanding, drain marks newer_view_dirty and preserves
 * its bytes/continuation instead of replacing it. No provider/tool/input admission,
 * direct output or canonical replay append. Handler propagates errors honestly.
 */
cog_result_t cli_execution_barrier(cog_cli_app_t *app,
    cli_after_publication_t after, cog_state_t resume, cog_state_t *next);
/** @brief app_turn.c one bounded cleanup unit, shared by manual and run drain.
 * @param[in,out] app Owner after core drive_end; @param[out] done Actual retained
 * provider/tool/unstarted-result cleanup ended, no claim effects undone.
 * @return OK/AGAIN/first error. At most one provider/tool poll and one known
 * unstarted-call closure; no blocking loop, model send, wait or allocation.
 */
cog_result_t cli_turn_drain_one(cog_cli_app_t *app, bool *done);
/** @brief app_tools.c resolve enabled ORIGINAL static table and fixed user bind.
 * @param[in] app Admitted role/profile. @param[in] name Literal tool name.
 * @param[out] table Original workspace/delegation table, not copied descriptor.
 * @param[out] user Optional: NULL asks schema-only resolution before context
 * attachment; otherwise fixed workspace/installed-context borrow is required.
 * @return OK; NULL_PTR; NOT_FOUND disabled/unknown; INVALID_STATE requested
 * live userdata before attachment. No allocation/registration or policy grant.
 * Schema-only lookup filters role/mask before provider preflight, uses original
 * static descriptor metadata and never calls a tool. Actual binding enforces
 * the SAME role/mask again; global policy/guard remain checked before STARTED.
 */
cog_result_t cli_tools_binding(const cog_cli_app_t *app, const char *name,
    const cog_tool_table_t **table, void **user);
/** @brief app_sessions.c prepare the exact child/STAGED session without root UX.
 * @param[in,out] app Partial owner. @param[in] admission Validated mode.
 * @return Existing G/L errors. LOCAL uses supplied borrow, REMOTE refuses any
 * existing child identity, STAGED exact INSPECT token without guard/save/input.
 * Child initial SYSTEM+checkpoint precede task once. No source read/re-resolution.
 */
cog_result_t cli_sessions_admit(cog_cli_app_t *app,
    const cog_cli_execution_admission_t *admission);

#ifdef __cplusplus
}
#endif
#endif

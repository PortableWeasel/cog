/**
 * @file delegation.h
 * @brief Private static application tools and retained group/message execution.
 *
 * One owner thread. Reuses toolkit async six-operation lifecycle and CGA1 registry,
 * never constructs a substitute core agent or invokes a nested model loop.
 */
#ifndef COG_CLI_DELEGATION_H
#define COG_CLI_DELEGATION_H
#include <cog_cli/cli.h>
#include <cog/agent.h>
#include <cog/tool.h>
typedef struct cog_cli_host_runtime cog_cli_host_runtime_t;
typedef struct cog_cli_worker_host cog_cli_worker_host_t;
typedef struct cog_cli_delegation_context cog_cli_delegation_context_t;

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Constructor for one app's bounded coordinator/worker tool bridge.
 * @param[out] out NULL on failure, match free.
 * @param[in,out] app Borrowed ordinary executor; outlives tool handles/context.
 * @param[in,out] coordinator Local coordinator or NULL.
 * @param[in,out] worker Worker host or NULL; EXACTLY one bridge owner non-NULL.
 * @param[in] agent Child identity,0 primary; @param[in] group Current child group,
 * zero for primary. Child group members/placement are trusted admitted state.
 * @param[in] mailbox_entries Finite count,default8,max64; @param[in] mailbox_bytes
 * Copied text total,default65536,max1MiB; fixed constructor arrays, no growth.
 * @return OK; NULL_PTR; INVALID_ARG; LIMIT; NOMEM. No model/tool/task side effect.
 * Static context tag selects local/worker adapter; no mutable registry or host
 * callback registration. No credentials or resolver authority is retained here.
 */
cog_result_t cog_cli_delegation_context_new(cog_cli_delegation_context_t **out,
    cog_cli_app_t *app, cog_cli_host_runtime_t *coordinator,
    cog_cli_worker_host_t *worker, cog_agent_id_t agent, uint64_t group,
    size_t mailbox_entries, size_t mailbox_bytes);
/** @brief Consume bridge only after every borrowing tool handle is destroyed.
 * @param[in,out] context NULL is OK. @return OK; INVALID_STATE live tool borrow.
 * Does not release core registry or accepted task ownership behind caller. */
cog_result_t cog_cli_delegation_context_free(cog_cli_delegation_context_t *context);

/** Static strict schemas with additionalProperties=false, all six async ops:
 * delegate: {"tasks":[{"task":STRING},...]} required1..16 tasks (host child count
 * may be lower), no placement/config/policy/path/model fields. Primary only.
 * send_message: {"agent_id":INTEGER,"message":STRING}, positive admitted sibling
 * ID in same group/parent only. receive_messages: {} copies current bounded
 * mailbox once, no wait/new model turn. Child only; no recursive delegate.
 * Text bounds use argument/mailbox constructor capacities; JSON depth bounded.
 * @note Private table REFERENCE composition, not flattened descriptor copies:
 * app uses static const pointers {&cog_workspace_tools,&cog_cli_delegation_tools},
 * iterates declared schemas and selects the SAME original descriptor plus fixed
 * workspace/delegation user binding. No private I vtable access, duplicated schema
 * literals or runtime registration. Advertise only enabled descriptors/masks.
 */
extern const cog_tool_table_t cog_cli_delegation_tools;

/** Execution invariants for the static tool table:
 * - new_ validates/copies strict args and reserves finite slots/credits/output,
 *   no task execution. start runs once only AFTER parent durable STARTED save.
 * - delegate reserves whole group's slots/credit before effects, pins resolved
 *   parent generation/child permission/deadline, uses agent_spawn/copy_config and
 *   exact local borrow or authenticated worker control. Existing agent_spawn is
 *   single-record: if construction of member n fails, already-created prefix
 *   records remain owned in a nonexecuting rollback state. Admit cancel for that
 *   prefix, release its queued intent leases, apply/copy/ack terminal failure and
 *   release normally. Group start gate stays false throughout; NO child executes
 *   on reservation failure. No fake batch primitive or synchronous rollback loop.
 *   Later failures AFTER successful group start may have partial effects,
 *   explicitly reported; group execution is not an atomic filesystem transaction.
 * - Accepted child attempts remain one-shot through local borrow return, terminal
 *   copy/ack and parent TOOL-result save. Lease/receipt retirement never restarts.
 * - poll/result copies actual retained child statuses/results in submitted order,
 *   not callback completion order. No fabricated COMPLETED updates or history.
 * - cancel freezes group and requests actual child cancel/drain/reap. Deadline
 *   min never extends on retransmit. Parent accepted terminal wins arbitration;
 *   child factual ledger/result remains immutable even if cancel wins delivery.
 * - Worker death/lost owner/parent crash cannot prove descendants/effects stopped.
 *   Report INDETERMINATE + side_effects_possible, identify child IDs/attempts and
 *   retain durable recovery-only state. No substitute task/failover/retry.
 * - send OK means one bounded copied mailbox message, not execution/approval.
 *   Full mailbox AGAIN accepts nothing and bounded retry uses SAME wire identity.
 *   receive is an identified at-most-once tool call with cached output; result
 *   copy/save failure retains handle/data and cannot drain mailbox a second time.
 * - Every final result follows existing parent TOOL+terminal ledger durable save
 *   before model continuation; failed save retains immutable result/handle. Core
 *   local ack, remote receipt and parent durable-result ACK are distinct events.
 * No runtime allocation outside explicit new_ or named bounded child constructors.
 */

#ifdef __cplusplus
}
#endif
#endif

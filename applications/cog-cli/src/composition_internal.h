/** @file composition_internal.h
 * @brief O4 concrete coordinator, worker, delegation and handoff ownership.
 * Private implementation only. Header dependencies are one-way:
 * app_internal -> opaque execution pointer; execution_internal -> N contracts;
 * host_internal -> execution_internal; this header -> host_internal/handoff.
 * Source files may include this header, libraries and O6 never include it.
 */
#ifndef COG_CLI_COMPOSITION_INTERNAL_H
#define COG_CLI_COMPOSITION_INTERNAL_H
#include "host_internal.h"
#include "handoff.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum cli_group_phase {
    CLI_GROUP_FREE = 0, CLI_GROUP_GRANTED, CLI_GROUP_ADMITTING,
    CLI_GROUP_RUNNING, CLI_GROUP_ROLLBACK, CLI_GROUP_RESULT, CLI_GROUP_SAVED
} cli_group_phase_t;
/** Already authenticated request, byte views borrowed only through reserve.
 * No model-selected placement/config/policy; owner must compare this entire
 * tuple and task bytes for duplicate reserve/start, never IDs alone. */
typedef struct cli_group_request {
    cog_store_owner_t owner;
    const char *parent_session, *call_id;
    uint64_t parent_node, pinned_configuration, deadline_ms;
    uint16_t ordinal;
    const char *const *tasks;
    const size_t *task_lengths;
    size_t count;
} cli_group_request_t;
typedef struct cli_group_member {
    bool present, terminal, effects_uncertain;
    cog_agent_id_t agent;
    uint64_t attempt;
    size_t slot;
    cog_agent_status_t status;
    cog_result_t outcome; /**< Absent has negative failure, no fabricated ID. */
} cli_group_member_t;
/** Reservation consumes whole group child slots/request/output capacity before
 * parent STARTED. Every output/string arena is fixed, no realloc on result save.
 * gate_open stays false throughout prefix admission/rollback. */
typedef struct cli_group_record {
    cli_group_phase_t phase;
    uint64_t id, reservation, request_wire, deadline_ms, credit;
    cog_store_owner_t parent_owner;
    char parent_session[128], call_id[256];
    uint64_t parent_node, pinned_configuration;
    uint16_t ordinal;
    cog_store_checkpoint_t started_checkpoint, result_checkpoint;
    bool gate_open, started, credit_charged, result_acknowledged;
    size_t submitted, admitted, rollback_cursor;
    size_t result_payload_cap; /**< Checked whole escaped output proof. */
    cli_group_member_t members[CLI_HOST_MAX_CHILDREN];
    size_t task_offsets[CLI_HOST_MAX_CHILDREN], task_lengths[CLI_HOST_MAX_CHILDREN];
    cli_host_bytes_t tasks, output;
    cog_result_t aggregate;
} cli_group_record_t;

/** Stable owner independent of executing app/store replacement. root_options,
 * credential callbacks and net_config input pointers are NOT retained. Paths/CA
 * strings have owned constructor copies; diagnostics/signal_fd are borrows. */
struct cog_cli_host_runtime {
    cog_cli_host_runtime_config_t fixed; /**< Rebound owned path/template pointers;
       root_options=NULL after new, no resolver/caller-source authority. */
    cog_net_config_t net_template;
    char *ui_path, *worker_path, *executable, *ca_bundle;
    char *canonical_workspace, *canonical_store;
    int workspace_directory_fd, store_directory_fd; /**< Owned pins; -1 if absent. */
    cli_host_reservations_t bounds;
    cog_result_t primary;
    bool begun, done, shutdown, in_step, admission_frozen, coordinator_failed;
    bool control_drive_ended; /**< Set after the sole drive_end call on a done
       core (including fatal return). Thereafter never step/end/restart that
       core; executor/native cleanup continues independently until runtime done. */
    uint64_t now_ms, shutdown_deadline_ms, runtime_incarnation;
    uint64_t next_client_lease, next_group, next_reservation, next_nonce;
    uint64_t request_credit_used, request_credit_held;
    uint32_t reload_attempts_used; /**< GLOBAL includes failed/local-only tries. */
    size_t round_robin, placement_cursor, primary_slot;
    cog_worker_id_t primary_worker; /**< Both0 when primary is local. */
    cog_peer_id_t primary_peer;
    cog_store_owner_t primary_owner;
    char primary_session[128];
    cog_session_t *control_session; /**< Owned registry/control tree, NOT root history. */
    cog_core_t *control_core; /**< Owns UI listener and its accepted endpoints. */
    cog_ipc_endpoint_t *ui_alias; /**< Owned before core_new success, borrowed after. */
    bool ui_transferred;
    cog_store_t *catalog; /**< Independently owned; outlives orch/handoff. */
    cog_orchestrator_t *orchestrator; /**< Sole worker fabric reader. */
    cog_cli_handoff_t *handoff;
    cog_cli_projection_t *projection;
    cog_cli_configuration_t *current_mirror, *future_mirror; /**< Owned copies of
       at most two logical generations; no extra live provider or resolver. */
    uint64_t active_configuration, published_configuration;
    cli_client_record_t *clients;
    cli_command_record_t *commands;
    cli_worker_record_t *workers;
    cli_executor_slot_t *executors;
    cli_group_record_t *groups; /**< Count<=children, each holds>=1 reserved slot. */
    size_t group_capacity;
    cli_control_lane_t *lanes; /**< Fixed keyed pool, includes both directions. */
    cli_host_bytes_t ingress, decode, projection_scratch;
};

/** Actual owned worker process. No second coordinator registry, UI client or
 * independent authoritative mailbox. Native CGW management has independent
 * reserved send bytes from application lanes and progresses during backpressure.
 */
struct cog_cli_worker_host {
    cog_cli_worker_startup_t fixed; /**< Rebound socket/template owned copies. */
    cog_net_config_t net_template;
    char *socket_path, *ca_bundle;
    cog_ipc_endpoint_t *parent;
    cog_ipc_peer_info_t parent_proof;
    bool authenticated, startup_ready, in_step, shutdown, done;
    bool stepped; /**< False at construction; set before first valid outer step.
       Invalid/reentrant calls do not set it. Close permits !stepped or done,
       never a connected-but-live owner merely because all executor slots idle. */
    cog_result_t primary;
    uint64_t runtime_incarnation, now_ms, shutdown_deadline_ms;
    uint64_t next_heartbeat_ms, startup_deadline_ms;
    /**< Checked monotonic native schedule, separate from shutdown/executor clocks.
       Startup deadline fixed before first service, never extended by duplicates
       or partial bootstrap. Heartbeat deadline advances only when its reserved
       native frame is accepted for transport; backpressure preserves that frame. */
    cog_agent_id_t opened_agent_high_watermark;
    size_t round_robin, executor_capacity;
    cli_executor_slot_t *executors;
    cli_control_lane_t *lanes;
    cli_secret_exchange_t startup_profile;
    cog_cli_configuration_t *startup_configuration;
    cog_cli_bootstrap_probe_t probe;
    /* Parent→worker may carry CGB1. Entire incoming frame/decode arena is
       secret-capable and wiped after copy, on failure and explicit disposal.
       Only validated NONsecret payloads may enter generic lane/handle caches. */
    unsigned char *receive_secret, *decode_secret;
    size_t receive_capacity, decode_capacity;
    cli_host_bytes_t native_control[3]; /**< HELLO, heartbeat, shutdown-ACK reserves. */
    bool native_pending[3];
};

typedef enum cli_delegation_operation {
    CLI_DELEGATE = 1, CLI_SEND_MESSAGE, CLI_RECEIVE_MESSAGES
} cli_delegation_operation_t;
typedef struct cli_delegation_handle cli_delegation_handle_t;
/** App owns installed context; toolkit handles borrow until destroy. Exactly one
 * coordinator/worker pointer, never a callback registry. Worker requests relay
 * to coordinator; its mailbox_entries/bytes are local capture bounds only. */
struct cog_cli_delegation_context {
    cog_cli_app_t *app;
    cog_cli_host_runtime_t *coordinator;
    cog_cli_worker_host_t *worker;
    cog_agent_id_t agent;
    uint64_t group;
    size_t mailbox_entries, mailbox_bytes, live_handles;
    cli_delegation_handle_t *prepared; /**< Current K serial call, not an owner. */
};
/** Explicit async new_ allocation: immutable validated args, full output/schema
 * pools before PREPARE. No task in new_; start follows grant+durable STARTED.
 * Actual toolkit-owned guard borrow stays alive until destroy; cancellation and
 * result retries reuse these buffers/wires rather than dequeue/reexecute. */
struct cli_delegation_handle {
    cog_cli_delegation_context_t *context;
    cli_delegation_operation_t operation;
    cog_tool_async_config_t fixed;
    cli_group_request_t request;
    char parent_session[128], call_id[256];
    const char *tasks[CLI_HOST_MAX_CHILDREN];
    size_t task_lengths[CLI_HOST_MAX_CHILDREN];
    cli_host_bytes_t arguments, output, codec, pending, reply;
    uint64_t reservation, group, request_wire, deadline_ms;
    cog_agent_id_t destination;
    const char *message; /**< Borrow into this handle's copied argument arena. */
    size_t message_length;
    bool prepared, started, cancelled, done, output_cached, result_saved;
    cog_tool_output_t result;
};

typedef enum cli_handoff_phase {
    CLI_HANDOFF_IDLE = 0, CLI_HANDOFF_QUIESCE, CLI_HANDOFF_PREPARE,
    CLI_HANDOFF_RELEASE, CLI_HANDOFF_CAS, CLI_HANDOFF_GUARD,
    CLI_HANDOFF_PUBLISH, CLI_HANDOFF_ACTIVATE, CLI_HANDOFF_CLEANUP,
    CLI_HANDOFF_FROZEN
} cli_handoff_phase_t;
/** Exactly one move, owned by runtime; all three input owners are borrows.
 * Every phase bit advances only after its actual operation/ACK, not send-OK.
 * Failed source never thaws, even before CAS; next epoch never guessed latest.
 */
struct cog_cli_handoff {
    cog_cli_host_runtime_t *runtime;
    cog_orchestrator_t *orchestrator;
    cog_store_t *catalog;
    uint32_t timeout_ms;
    cli_handoff_phase_t phase;
    cog_result_t primary;
    cog_worker_id_t source_worker, target_worker;
    cog_peer_id_t source_peer, target_peer;
    cog_store_owner_t expected, next;
    cog_store_checkpoint_t checkpoint;
    uint64_t identity, deadline_ms;
    uint32_t reload_attempts_used;
    size_t source_slot, target_slot;
    bool checkpoint_saved, prepared, source_released, cas_committed;
    bool guard_acquired, route_published, activation_acked, abort_requested;
    cli_host_bytes_t pending, reply; /**< CGH1/nonsecret control copies. */
};

/** @brief host_runtime.c reserve complete group before parent STARTED.
 * @param[in,out] runtime Coordinator. @param[in] request Authenticated current
 * primary/call/generation/deadline, borrowed through call and copied on OK.
 * @param[out] reservation Nonzero on OK only. @return OK exact duplicate grant;
 * NULL_PTR; INVALID_STATE stale/frozen; LIMIT capacity/credit/output; PROTOCOL
 * conflicting duplicate. Reservation uses only existing constructor storage.
 * Full reservation/output escaping proof before mutation; no child execution.
 */
cog_result_t cli_runtime_reserve(cog_cli_host_runtime_t *runtime,
    const cli_group_request_t *request, uint64_t *reservation);
/** @brief host_runtime.c start an exact granted group AFTER durable STARTED.
 * @param[in,out] runtime Owner. @param[in] reservation Exact grant.
 * @param[in] checkpoint Actual parent's saved STARTED token.
 * @return OK/AGAIN bounded prefix admission; NULL_PTR; INVALID_STATE/PROTOCOL;
 * explicit constructor/driver failure. Keep gate false until every member was
 * admitted; prefix failure schedules nonexecuting rollback, never synchronous
 * cleanup loop. After start, credit stays charged even if usage is unknown.
 */
cog_result_t cli_runtime_start(cog_cli_host_runtime_t *runtime,
    uint64_t reservation, cog_store_checkpoint_t checkpoint);
/** @brief host_runtime.c release held grant or cancel actual started group.
 * @return OK idempotent; NULL_PTR; NOT_FOUND stale unknown reservation.
 * Refund ONLY known-unstarted hold, otherwise retain credit and actual cleanup.
 * No deadline extension, task restart, guard release or invented terminal.
 */
cog_result_t cli_runtime_release(cog_cli_host_runtime_t *runtime,
    uint64_t reservation);
/** @brief host_runtime.c authoritative SEND/RECEIVE mailbox operation.
 * @param[in,out] runtime Coordinator. @param[in,out] handle Identified admitted
 * child operation; source/group from trusted context, never wire sender text.
 * @return OK progress/result copied once; AGAIN full queue/pending accepts none;
 * NULL_PTR; NOT_FOUND/INVALID_STATE foreign/stale; LIMIT whole capture cannot fit.
 * No allocation/new turn, pointer to worker-local mailbox, repeated dequeue or
 * prefix truncation. Retain immutable reply until consumed under MAILBOX lane.
 */
cog_result_t cli_runtime_mailbox_step(cog_cli_host_runtime_t *runtime,
    cli_delegation_handle_t *handle);
/** @brief delegation.c observe exact parent's TOOL+terminal-ledger save.
 * @param[in,out] context Owner app bridge, retained handle/result.
 * @param[in] node/ordinal Exact original parent call; @param[in] checkpoint
 * Actual successful durable result token. @return OK exact duplicate; AGAIN
 * copied-send pending; NULL_PTR; INVALID_STATE mismatch; driver error.
 * Local group marks result durable; worker queues exact DELEGATE_RESULT_ACK.
 * No CGA apply/local ACK, second mailbox dequeue, tool effect or model send.
 * Retain handle/result until the required copied notification can be retired.
 */
cog_result_t cli_delegation_saved(cog_cli_delegation_context_t *context,
    uint64_t node, uint16_t ordinal, cog_store_checkpoint_t checkpoint);
/** @brief worker_host.c one copied application-lane send/receive bridge for
 * delegation.c, using already authenticated current parent and slot identity.
 * @param[in,out] worker Owner; @param[in,out] handle Retained async request.
 * @return OK progress; AGAIN no accepted new frame; NULL_PTR; INVALID_STATE;
 * driver/protocol error. Dedicated immutable wire retained across AGAIN/retry.
 * No socket read outside worker_host_step, new model turn, allocation or wait.
 */
cog_result_t cli_worker_delegation_step(cog_cli_worker_host_t *worker,
    cli_delegation_handle_t *handle);

#ifdef __cplusplus
}
#endif
#endif

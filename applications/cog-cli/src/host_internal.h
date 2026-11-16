/** @file host_internal.h
 * @brief O4 bounded host record layout shared by coordinator and worker bodies.
 * All pointers below name constructor arrays/arenas or explicit named admission
 * constructors. No vector, realloc, refcount, external callbacks or raw event
 * payload survives handler return. This is private storage, not a new protocol.
 */
#ifndef COG_CLI_HOST_INTERNAL_H
#define COG_CLI_HOST_INTERNAL_H
#include "host_runtime.h"
#include "worker_host.h"
#include "execution_internal.h"
#include <cog/event.h>

#ifdef __cplusplus
extern "C" {
#endif
#define CLI_HOST_MAX_WORKERS 8U
#define CLI_HOST_MAX_CHILDREN 16U
#define CLI_HOST_MAX_CLIENTS 16U
#define CLI_HOST_MAX_EXECUTORS 18U /* primary + possible staged +16 children */
#define CLI_HOST_LANES 10U
#define CLI_HOST_SESSION_BYTES 128U
#define CLI_HOST_CALL_BYTES 256U

/** Explicit arena/slice, stable until owner close; length never exceeds capacity.
 * Secret arenas have a separate type/owner below and NEVER enter this record. */
typedef struct cli_host_bytes { unsigned char *data; size_t capacity, length; } cli_host_bytes_t;
typedef enum cli_host_fabric { CLI_FABRIC_UI = 1, CLI_FABRIC_WORKER } cli_host_fabric_t;
/** Full CGL sender stream key, including fabric and lane. Fixed copied session. */
typedef struct cli_stream_key {
    cli_host_fabric_t fabric;
    uint64_t runtime, sender;
    cog_worker_id_t worker;
    cog_peer_id_t owner_peer; /**< Immutable attempt placement;0 for UI logical lease.
       UI reconnect current transport lives in client_record, NOT this dedup key. */
    cog_agent_id_t agent;
    uint64_t attempt;
    cog_cli_control_lane_t lane;
    char session[CLI_HOST_SESSION_BYTES];
} cli_stream_key_t;
/** One key's stop/wait state. Incoming and outgoing keys have DISTINCT slots.
 * pending/final_original/final_reply are constructor slices, NEVER secret data.
 * Final reply and original must survive until exact consume/successor proof.
 * After retirement old IDs uniformly stale-reject; no byte-conflict claim remains.
 */
/** Retained request/reply sends may retry only after this finite interval once
 * the exact native binding accepted a copied frame. One-way frames never retry
 * after copied acceptance. This is transport eligibility, not semantic ACK or
 * an extension of any operation/native deadline. */
#define CLI_CONTROL_RETRY_MS 1000U
typedef struct cli_control_lane {
    cli_stream_key_t key;
    bool bound, pending_valid, final_valid;
    uint64_t pending_wire, final_answered_wire, final_reply_wire, retired_through;
    uint64_t provisional_wire, provisional_prefix;
    uint64_t next_wire, reserved_cleanup_first, reserved_cleanup_count;
    cli_host_bytes_t pending, provisional_reply, final_original, final_reply;
    uint64_t deadline_ms; /**< Min only; duplicate never extends. */
    uint64_t copied_wire, next_send_ms; /**< Transport-owned exact outer wire and
       independent earliest retransmit time. New wire is immediately eligible;
       AGAIN accepts nothing and changes neither value. No receipt/authority
       advancement. HANDOFF uses the addressed slot outbound lane; delegation
       handle pending frames use that executor outbound stream. See scheduling
       and retirement rules in REAL_CLIENT_CONTROL_SEND_OWNERSHIP.md. */
} cli_control_lane_t;
/** Dedicated PROFILE lane. Re-encode exact pinned generation for duplicate
 * comparison using its ORIGINAL probe snapshot below, not a worker-global
 * current probe or a later store read. NEVER retain secret originals in
 * cli_control_lane/command/events.
 * Only pending secret frame and scratch are reserved here; wipe ENTIRE capacity
 * on retirement/abandonment/free. IPC TX/RX fabric also has wipe_buffers=true.
 */
typedef struct cli_secret_exchange {
    cli_stream_key_t key;
    cog_cli_bootstrap_probe_t probe; /**< Owned by-value original session/owner/
       checkpoint snapshot, including all128 session bytes (no borrowed text).
       Copied/validated with this exchange before publication, then immutable
       through pending/ACK and live-generation duplicate comparison. Independent
       of worker/global probes and other exchanges. Retire with this exchange
       before releasing its source-generation borrow; never reread latest G or
       repurpose an old wire with a new probe. No secret bytes/extra allocation. */
    unsigned char *body, *frame, *scratch;
    size_t body_capacity, frame_capacity, scratch_capacity;
    size_t body_length, frame_length;
    uint64_t wire, replied_wire, retired_through, generation;
    uint64_t copied_wire, next_send_ms; /**< Same copied-send eligibility as a
       generic lane, but PROFILE bytes stay in their dedicated secret arenas.
       Exact re-armed failure ACK preserves this schedule across stale retries. */
    uint32_t reload_attempts_used;
    uint8_t purpose;
    bool pending, acknowledged;
    const cog_cli_configuration_t *source; /**< Borrow exact owned generation. */
} cli_secret_exchange_t;

/** Pure checked reservation result, no allocation/effects. Per-lane ceilings
 * derive from actual nested schema: no multiplying every lane by max projection.
 * Outbound+inbound per-key slices counted separately; PROFILE uses secret arenas.
 */
typedef struct cli_host_reservations {
    size_t executors, clients, children, workers, command_records;
    size_t display_items, display_bytes, projection_bytes;
    size_t lane_frame_bytes[CLI_HOST_LANES];
    size_t streams, nonsecret_bytes, secret_bytes;
    size_t status_bytes, result_bytes, task_bytes, mailbox_entries, mailbox_bytes;
} cli_host_reservations_t;

/** Dedicated non-effect HELLO receipt, one per bounded logical client. Not an
 * ordinary command record/consumption proof. Three constructor slices<=512 each
 * retain exact HELLO original, final result and final ACK-of-ACK reply. Input
 * ACK has no payload: exact bound lease/runtime/session/ack_wire/reply_to are its
 * full correlation. Initial nonce remains available for lost-discovery retry;
 * known reconnect uses this record's logical lease, never steals another role.
 * Existing COMMAND/CANCEL receipts remain separately owned during this exchange.
 * Known active request/ACK lookup precedes per-HELLO retired watermark checks. */
typedef struct cli_client_hello_record {
    bool pending, final_valid, acknowledged;
    uint64_t initial_nonce, request_sender, request_wire, result_wire;
    uint64_t ack_wire, ack_result_wire, retired_through;
    cli_host_bytes_t original, reply, ack_reply;
} cli_client_hello_record_t;
/** UI identity stays separate from actual link proof; no pointer into a core
 * event survives its handler. A reconnect binds only an existing unexpired lease.
 */
typedef struct cli_client_record {
    bool used, connected, controller, awaiting_resync;
    cog_peer_id_t peer;
    cog_ipc_peer_info_t proof;
    uint64_t logical_lease, controller_until_ms, receipts_until_ms;
    cli_client_hello_record_t hello; /**< Independent discovery/reconnect receipt. */
    uint64_t rendered_generation, rendered_sequence, queued_sequence, gap_last;
    cli_host_bytes_t send, resync;
} cli_client_record_t;
typedef enum cli_command_phase {
    CLI_COMMAND_FREE = 0, CLI_COMMAND_COPIED, CLI_COMMAND_FORWARDED, CLI_COMMAND_FINAL
} cli_command_phase_t;
/** UI receipt lifetime ONLY. Forwarded execution owns a separate copy below;
 * lease expiry may dispose this record without revoking/repeating an effect. */
typedef struct cli_command_record {
    cli_command_phase_t phase;
    uint64_t lease, request_wire, result_wire;
    uint64_t observed_generation, observed_sequence, nonce;
    cog_store_owner_t owner;
    cog_result_t result;
    cli_host_bytes_t original, decoded_storage, reply;
} cli_command_record_t;
/** Independent COMMAND/CANCEL operation, never aliases client receipt bytes.
 * Carries original lease/request for read-only result lookup after role loss.
 * send OK commits authorization once; send AGAIN must recheck current lease.
 */
typedef struct cli_forwarded_operation {
    bool used, forward_committed, final;
    uint64_t lease, request_wire, executor_wire, executor_serial;
    cog_store_owner_t owner;
    cog_result_t result;
    cli_host_bytes_t original, decoded_storage, reply;
} cli_forwarded_operation_t;

typedef struct cli_mailbox_entry {
    bool used;
    cog_agent_id_t source;
    size_t offset, length;
} cli_mailbox_entry_t;
/** Coordinator owns authoritative sibling mailbox; no worker-local second queue.
 * Admission proves one WHOLE item fits receive capture after worst-case escaping.
 * Cached identified poll response owns removed prefix until durable tool result.
 */
typedef struct cli_mailbox {
    cli_mailbox_entry_t *entries;
    unsigned char *text;
    size_t entry_capacity, text_capacity, head, count, text_used;
    uint64_t poll_wire;
    bool reply_pending, more_pending;
    cli_host_bytes_t reply;
} cli_mailbox_t;

typedef enum cli_executor_phase {
    CLI_SLOT_FREE = 0, CLI_SLOT_RESERVED, CLI_SLOT_PROFILE, CLI_SLOT_PREPARED,
    CLI_SLOT_STARTED, CLI_SLOT_DRAINING, CLI_SLOT_RETAINED, CLI_SLOT_DISPOSING
} cli_executor_phase_t;
/** Reusable PHYSICAL slot, immutable admitted logical attempt. Never restart
 * because a session borrow was returned. Reuse only AFTER result/lease cleanup;
 * allocate a fresh logical agent ID, not a renewed execution of the old record.
 */
typedef struct cli_executor_slot {
    cli_executor_phase_t phase;
    cog_cli_execution_kind_t kind;
    cog_agent_id_t agent;
    uint64_t attempt, group, runtime, configuration_generation;
    char session[CLI_HOST_SESSION_BYTES], parent_session[CLI_HOST_SESSION_BYTES];
    uint64_t parent_node;
    uint16_t parent_ordinal;
    char parent_call[CLI_HOST_CALL_BYTES];
    cog_agent_id_t members[CLI_HOST_MAX_CHILDREN];
    size_t member_count;
    cog_worker_id_t worker;
    cog_peer_id_t owner_peer; /**< Actual authenticated placement;0 local. */
    cog_ipc_peer_info_t proof; /**< Copied native facts, NOT worker identity. */
    cog_store_owner_t owner;
    cog_cli_app_t *app; /**< Owned LOCAL/worker app;NULL remote coordinator shadow. */
    cog_net_ctx_t *net; /**< Owned slot transport, outlives app/provider/config. */
    cog_cli_observer_t *observer; /**< Owned slot; outlives net. */
    cog_cli_configuration_t *pending_profile; /**< Owned until transfer/free. */
    cog_session_t *borrowed_session; /**< Exact local registry lease, never owned. */
    bool borrow_live, started, cancel_latched, effects_uncertain;
    bool first_admission_sent; /**< First profile/CANCEL send OK, NOT mere queue intent. */
    bool local_done, terminal_copied, terminal_acked, parent_result_saved;
    uint64_t deadline_ms, drain_deadline_ms;
    cog_result_t primary;
    /* Factual local execution and parent's semantic prefix are separate. */
    cog_agent_status_t factual_status, accepted_status;
    cog_result_t factual_outcome, accepted_outcome;
    uint64_t accepted_sequence, pending_update_wire, pending_update_sequence;
    cog_cli_control_receipt_t receipt;
    cli_host_bytes_t task, status, result, update;
    cli_forwarded_operation_t command, cancel;
    cli_control_lane_t *inbound, *outbound; /**< Each10 constructor metadata slots;
       PROFILE generic byte capacities0, dedicated profile exchange only. */
    cli_secret_exchange_t profile;
    cli_mailbox_t mailbox; /**< Used only in coordinator child records. */
} cli_executor_slot_t;
/** Native metadata is READ ONLY copied from orchestrator; no invented heartbeat
 * count or PID. Reserved slots are coordinator-local placement accounting. */
typedef struct cli_worker_record {
    bool used, profile_ready;
    cog_worker_id_t id;
    cog_core_info_t native;
    size_t reserved_slots;
    cog_agent_id_t first_admission_high_watermark;
    cli_secret_exchange_t startup_profile;
} cli_worker_record_t;

/** @brief host_runtime.c pure capacity plan, before any publication/fork.
 * @param[in] config Valid host inputs. @param[in] resolved Effective J options,
 * no source values or authority callbacks retained. @param[out] plan Commit only
 * on OK. @return OK; NULL_PTR; INVALID_ARG unsupported bounds; LIMIT arithmetic/
 * impossible full projection/group/mailbox/resync/final-control reservation.
 * Checked add/multiply for all products, whole frame overhead and output escape
 * bounds. Does not allocate, open directories, construct providers or call J.
 */
cog_result_t cli_host_plan(const cog_cli_host_runtime_config_t *config,
    const cog_cli_options_t *resolved, cli_host_reservations_t *plan);
/** @brief host_runtime.c process one protected UI/agent event on owner thread.
 * @param[in,out] core Stable coordinator core. @param[in] event Borrowed only
 * for this call. @return Existing static handler result, self transition.
 * Copy any deferred intent/config/frame/native proof into reserved owned records
 * BEFORE return. Refuse full ordinary admission via reserved reply capacity;
 * do not store raw event/arena pointer or invent a protected peer proof.
 */
cog_handler_result_t cli_host_dispatch(cog_core_t *core, const cog_event_t *event);

#ifdef __cplusplus
}
#endif
#endif

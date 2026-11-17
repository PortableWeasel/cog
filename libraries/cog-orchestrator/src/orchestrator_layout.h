/** @file orchestrator_layout.h
 * @brief O3 bounded private process/event/route ownership BEFORE bodies.
 * One owner thread. All arrays/string/frame arenas reserved at new; endpoint
 * accept is the existing explicitly bounded constructor exception in service.
 * No global mutable state, application CGL headers, runtime realloc/strdup,
 * implicit task replay, endpoint-layout access, foreign reap or new public API.
 * This header is not included by the old body until O3 source migration removes
 * its obsolete local struct definitions. Legacy test-hook header stays separate.
 */
#ifndef COG_ORCHESTRATOR_LAYOUT_H
#define COG_ORCHESTRATOR_LAYOUT_H
#include "orchestrator_wire_internal.h"
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif
#define ORCH_CONTROL_TRANSITIONS 3U
#define ORCH_NUMBER_BYTES 21U
#define ORCH_WORKER_ARGV_ENTRIES 17U /*16 argv strings incl optional capsule bound +NULL */
#define ORCH_RESERVED_EXECUTOR_SLOTS UINT64_C(2)

typedef enum orch_native_state {
    ORCH_NATIVE_NONE = 0, ORCH_NATIVE_OWNED, ORCH_NATIVE_REAPED, ORCH_NATIVE_LOST
} orch_native_state_t;
typedef enum orch_link_state {
    ORCH_LINK_FREE = 0, ORCH_LINK_HANDSHAKE, ORCH_LINK_PROVED,
    ORCH_LINK_READY, ORCH_LINK_DRAINING
} orch_link_state_t;
typedef enum orch_control_slot_kind {
    ORCH_CONTROL_READY = 0, ORCH_CONTROL_DISCONNECTED, ORCH_CONTROL_FINAL
} orch_control_slot_kind_t;
/** Payload pointers NEVER point into endpoints/events after handler return.
 * These fields name stable constructor-owned arenas, not growable buffers. */
typedef struct orch_bytes { unsigned char *data; size_t capacity, length; } orch_bytes_t;
/** Control events have independent metadata slots and a separate FIFO of refs.
 * READY may remain unused if child dies before HELLO; FINAL is exactly one of
 * REAPED/OWNERSHIP_LOST. Native terminal does not retroactively fabricate READY.
 */
typedef struct orch_control_slot {
    bool reserved, pending, acknowledged;
    cog_orchestrator_event_t event; /**< data=NULL,data_len=0, copied proof. */
} orch_control_slot_t;
typedef struct orch_control_ref { size_t worker; orch_control_slot_kind_t transition; } orch_control_ref_t;
/** Ring payload offset/length may wrap; capture/copy uses at most two segments.
 * Aggregate message bytes counts these queued payloads, not endpoint/staging
 * arenas which are separately included in the constructor reservation plan. */
typedef struct orch_message_slot {
    cog_orchestrator_event_t event; /**< data is reconstructed into caller buffer. */
    size_t offset, length, worker;
} orch_message_slot_t;
/** Accepted but not yet trusted link is NOT a managed worker. Slots bound total
 * accepted connections. After one handshake publication unit, cache peer_info
 * IMMEDIATELY before further service, independent of event/byte queue pressure.
 * Proof is fixed and remains readable after physical EOF until logical teardown.
 */
typedef struct orch_link {
    orch_link_state_t state;
    cog_ipc_endpoint_t *endpoint; /**< Owned accepted constructor result. */
    cog_peer_id_t peer;
    cog_ipc_peer_info_t proof;
    bool proof_valid, eof, staged_frame_valid;
    size_t worker; /**< SIZE_MAX until exact kernel+HELLO bind; not connect order. */
    uint64_t handshake_deadline_ms;
    orch_bytes_t frame; /**< One reserved ingress frame. Blocked ordinary head
       stays here, pausing ONLY this peer's receive; native timers/reap continue.
       All bytes wipe on abandonment/free, no extra message event identity yet. */
} orch_link_t;
/** Exactly one published fork reservation. PID is written only by successful
 * native fork and never changed by HELLO/heartbeat/registration/replacement.
 * Both native-state and bound-once flag prevent PID/slot/peer identity reuse.
 */
typedef struct orch_worker {
    bool used, bound_once, disconnected, ready, has_heartbeat;
    cog_worker_id_t id;
    pid_t native_pid;
    uint64_t expected_uid;
    orch_native_state_t native_state;
    int wait_status;
    bool wait_status_known;
    size_t link; /**< SIZE_MAX when absent; no fresh link after bound_once loss. */
    cog_peer_id_t peer;
    cog_ipc_peer_info_t proof; /**< Immutable admitted by-value copy. */
    cog_health_t health;
    size_t active_subagents, capacity; /**< Child counts only; never C+2. */
    uint64_t ready_at_ms, last_heartbeat_ms, startup_deadline_ms;
    bool stop_requested, shutdown_queued, shutdown_acked, term_sent, kill_sent;
    uint64_t stop_deadline_ms, terminate_deadline_ms;
    bool replacement_pending;
    char *binary_path; /**< Constructor slice, copied at spawn before fork. */
    size_t binary_capacity;
    char incarnation_arg[ORCH_NUMBER_BYTES], number_arg[ORCH_NUMBER_BYTES];
    char parent_arg[ORCH_NUMBER_BYTES], executors_arg[ORCH_NUMBER_BYTES];
    char frame_arg[ORCH_NUMBER_BYTES], instruction_arg[ORCH_NUMBER_BYTES];
    char *argv[ORCH_WORKER_ARGV_ENTRIES]; /**< No task/config/credential strings. */
    orch_control_slot_t control[ORCH_CONTROL_TRANSITIONS];
    unsigned char shutdown_frame[ORCH_CGW1_BYTES];
    size_t shutdown_length;
} orch_worker_t;
/** Advisory route persists for affected-session diagnosis until exact-owner
 * forget; socket loss never clears a newer route or silently releases G guard.
 * No CGH transaction engine lives here: O4 owns phase/save/CAS/guard/activation.
 */
typedef struct orch_route {
    bool used, frozen;
    char session[ORCH_SESSION_BYTES];
    cog_peer_id_t peer; /**<0 is explicitly trusted local guarded executor. */
    cog_worker_id_t worker; /**< Both0 local, exact old worker retained remote. */
    cog_store_owner_t owner;
    cog_store_checkpoint_t checkpoint;
} orch_route_t;
/** Pure checked constructor plan. Native executable path slices are sized for
 * the supported host's prevalidated absolute exec path bound; no service copy
 * can enlarge them. Plan includes socket path NUL and allocation alignment;
 * sum/product overflow is INVALID_ARG, allocation failure later is NOMEM.
 * Worker argv physical-executor mapping is fixed before body,
 * as uint64_t(C)+2, independently of size_t host width. This is an argv
 * scalar, NOT an orchestrator allocation count. Any worker converting it to
 * size_t must validate its own supported range first. Generic C<=UINT32_MAX
 * remains valid; installed host constrains C<=16 before spawning its binary.
 */
typedef struct orch_reservations {
    size_t workers, links, routes, control_entries, message_entries;
    size_t message_bytes, frame_bytes, binary_bytes, payload_bytes, total_bytes;
    uint64_t worker_executor_slots; /**< Physical C+2, maximum4294967297. */
} orch_reservations_t;

struct cog_orchestrator {
    cog_orchestrator_config_t config; /**< socket_path rebound to owned copy;
       store remains explicit borrow until AFTER this owner free. */
    cog_ipc_endpoint_t *listener;
    char *socket_path;
    orch_reservations_t bounds;
    orch_worker_t *workers;
    orch_link_t *links;
    orch_route_t *routes;
    size_t worker_count, link_count, route_count;
    orch_control_ref_t *control_fifo;
    size_t control_head, control_count;
    orch_message_slot_t *messages;
    unsigned char *message_data;
    size_t message_head, message_count, byte_head, bytes_used;
    uint64_t incarnation, last_worker_number, last_event_id;
    uint64_t mandatory_event_ids; /**< HELD count, not preassigned ordering IDs.
       Ordinary capture leaves this many fresh IDs; spawn reserves3 more before
       fork; captured control consumes one. Zero-wrap is never representable. */
    uint64_t last_control_ack, last_message_ack; /**< Latest duplicate PER CLASS. */
    uint64_t now_ms, shutdown_deadline_ms;
    bool clock_set, shutting_down, shutdown_done, admission_frozen, in_service;
    cog_result_t primary;
    size_t round_robin_worker, round_robin_link;
    uint32_t spawn_timeout_ms, ack_timeout_ms;
    bool clock_overridden; /**< Existing deterministic test hook only. */
};

/** @brief orchestrator.c pure checked configuration/reservation plan.
 * @param[in] config Caller view. @param[out] plan Commit on OK only.
 * @return OK; NULL_PTR; INVALID_ARG bounds/products/unsupported native sizes.
 * Resolve documented defaults, prove3*workers control entries/ID hold, every
 * metadata/payload/exec slice byte product, message frame<=class byte bound.
 * Normalize child C=default_core_capacity or4, reject C>UINT32_MAX before
 * converting to uint64_t, then checked wide add2. Never allocate C+2 executor
 * contexts here. Installed binary support is not a generic plan restriction.
 * No allocation, signal/disposition changes, source lookup or native child.
 */
cog_result_t orch_reservation_plan(const cog_orchestrator_config_t *config,
    orch_reservations_t *plan);
/** @brief orchestrator.c exact-PID WNOHANG check, used before EVERY signal.
 * @param[in,out] orch Owner. @param[in,out] worker Published native reservation.
 * @return OK live/reaped; AGAIN transient interruption; IO permanently lost or
 * native error. REAPED is final; ECHILD sets LOST forever and queues its reserved
 * final event with IO, never signals/rebinds that PID. No waitpid(-1)/blocking.
 */
cog_result_t orch_native_check(cog_orchestrator_t *orch, orch_worker_t *worker);
/** @brief orchestrator.c one owned signal after the mandatory exact-PID check.
 * @param[in,out] orch Owner. @param[in,out] worker Exact reservation.
 * @param[in] signal_number Only declared TERM/KILL stage chosen by owner.
 * @return OK including retained already-reaped; AGAIN check interrupted; IO.
 * At most one native check plus one kill, no reaper/disposition installation.
 * LOST forbids signal even if numeric PID now names an unrelated live process.
 */
cog_result_t orch_native_signal(cog_orchestrator_t *orch, orch_worker_t *worker,
    int signal_number);
/** @brief orchestrator.c one link unit including immediate first-proof barrier.
 * @param[in,out] orch Owner; @param[in,out] link Reserved accepted slot.
 * @param[in] now_ms Shared checked clock; @param[out] deadline_ms Earliest timer.
 * @return OK/AGAIN; retained driver error. Uses private IPC one-unit/readiness
 * functions without endpoint-layout access. Capture proof immediately on
 * handshake publication BEFORE another service/recv/TX/EOF unit, regardless of
 * ordinary queue space. Never admits a message/READY under a guessed proof.
 */
cog_result_t orch_link_step(cog_orchestrator_t *orch, orch_link_t *link,
    uint64_t now_ms, uint64_t *deadline_ms);
/** @brief orchestrator.c publish one pre-reserved control transition.
 * @param[in,out] orch Owner. @param[in,out] worker Exact record.
 * @param[in] transition One of3 fixed slots; @param[in] outcome Actual known code.
 * @return OK exact already-published duplicate; INVALID_STATE impossible phase;
 * no NOMEM/backpressure once reservation exists. Assign next fresh event ID
 * from held budget, append independent control FIFO, copy immutable native proof.
 * No CGA terminal, durable save, descendant-fencing or guard effect is implied.
 */
cog_result_t orch_control_capture(cog_orchestrator_t *orch, orch_worker_t *worker,
    orch_control_slot_kind_t transition, cog_result_t outcome);

#ifdef __cplusplus
}
#endif
#endif

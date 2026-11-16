/**
 * @file orchestrator.h
 * @brief Process orchestrator: spawn, monitor, and place subagents.
 *
 * Lives only in the main core process.  Owns the IPC listener socket,
 * tracks auxiliary core health, and decides where to place new
 * subagent workloads.
 *
 * Threading model: the orchestrator runs on the main core's event
 * loop thread.  Health checks and placement decisions happen inside
 * the cog_core_run() loop via IPC events.
 */
#ifndef COG_ORCHESTRATOR_H
#define COG_ORCHESTRATOR_H

#include <cog/ipc.h>
#include <cog/result.h>
#include <cog/session_store.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Opaque orchestrator context.
 *
 * Created with cog_orchestrator_new and destroyed with
 * cog_orchestrator_free.
 */
typedef struct cog_orchestrator cog_orchestrator_t;

/** @brief Managed process reservation, distinct from a transport peer ID.
 * Both fields are nonzero. Incarnation is constructor-owned and fresh for this
 * orchestrator lifetime; number is monotonic and never reused, including after
 * reap. Zero initialization is invalid. Equality requires BOTH fields.
 * A worker may exist before any socket is connected/authenticated. Never cast
 * this identity to cog_peer_id_t or use a wire PID as managed ownership. */
typedef struct cog_worker_id {
    uint64_t incarnation;
    uint64_t number;
} cog_worker_id_t;

/**
 * @brief Health status of a core process.
 */
typedef enum cog_health {
    COG_HEALTH_UNKNOWN = 0,  /**< No heartbeat received yet. */
    COG_HEALTH_HEALTHY,      /**< Responding to heartbeats. */
    COG_HEALTH_DEGRADED,     /**< Missed one heartbeat. */
    COG_HEALTH_DEAD,         /**< Missed threshold heartbeats. */
} cog_health_t;

/**
 * @brief Information about a managed core process.
 */
typedef struct cog_core_info {
    cog_peer_id_t peer_id;        /**< ACTUAL control-fabric connection ID; zero
       until authenticated ready, never a managed worker number. */
    int pid;                      /**< Immutable native PID returned by owned
       spawn; no HELLO/message/task field may replace it. Never signal after reap. */
    cog_health_t health;          /**< Current health status. */
    size_t active_subagents;      /**< Number of running subagents. */
    size_t capacity;              /**< Maximum subagent capacity. */
    uint64_t last_heartbeat_ms;   /**< Timestamp of last heartbeat. */
    cog_worker_id_t worker_id;    /**< Managed lifecycle identity from spawn. */
    bool reaped;                 /**< Native owned child reaped, not merely a
       disconnected/unhealthy socket or proof all descendant effects stopped. */
    bool ownership_lost;          /**< ECHILD/out-of-profile reap loss: permanently
       forbids signalling this PID. Driver failure, never a new execution grant. */
} cog_core_info_t;

/**
 * @brief Configuration for the orchestrator.
 */
typedef struct cog_orchestrator_config {
    /** IPC listener socket path.  The orchestrator binds this. */
    const char *socket_path;

    /** Maximum number of auxiliary cores to manage. */
    size_t max_aux_cores;

    /** Health check interval in milliseconds. */
    uint32_t health_interval_ms;

    /** Missed heartbeats before marking a core as dead. */
    uint32_t unhealthy_threshold;

    /** Maximum subagents per core (for placement decisions). */
    size_t default_core_capacity;

    /** If true, automatically spawn a replacement when a core dies. */
    bool auto_replace;
    size_t service_quantum; /**< Constructor-bounded work/step; zero defaults 16. */
    size_t max_sessions; /**< Reserved owner/route records; zero defaults 64. */
    uint32_t shutdown_timeout_ms; /**< Per-stop grace; zero defaults 1000. */
    cog_store_t *store; /**< Borrowed same-host require_owner store for durable
       migration; optional only for non-migrating fixture/process-only use. */
    size_t message_events; /**< Reserved application-message events; zero=16. */
    size_t message_bytes; /**< Reserved copied message bytes; zero=4 MiB. */
    size_t max_frame_bytes; /**< Control/application frame ceiling; zero=1 MiB,
       <=UINT32_MAX and <=message_bytes. No truncation or runtime growth. */
} cog_orchestrator_config_t;

/** @brief Independent bounded delivery classes; control progress cannot depend
 * on freeing an ordinary message slot. Each class preserves capture order. */
typedef enum cog_orchestrator_event_class {
    COG_ORCHESTRATOR_CONTROL_EVENTS = 1,
    COG_ORCHESTRATOR_MESSAGE_EVENTS
} cog_orchestrator_event_class_t;
/** @brief Control transitions have fixed per-worker reservations. */
typedef enum cog_orchestrator_event_kind {
    COG_ORCHESTRATOR_READY = 1,
    COG_ORCHESTRATOR_DISCONNECTED,
    COG_ORCHESTRATOR_REAPED,
    COG_ORCHESTRATOR_MESSAGE,
    COG_ORCHESTRATOR_OWNERSHIP_LOST
} cog_orchestrator_event_kind_t;
/** @brief Copied event metadata, no borrowed endpoint/native handle. */
typedef struct cog_orchestrator_event {
    uint64_t event_id; /**< Nonzero, never reused within orchestrator incarnation.
       Ordinary capture/spawn must preserve outstanding mandatory control IDs. */
    cog_orchestrator_event_kind_t kind;
    cog_worker_id_t worker_id;
    cog_peer_id_t peer_id; /**< Actual bound fabric peer, or0 if never bound. */
    cog_ipc_peer_info_t peer_info; /**< Cached kernel snapshot if peer_id!=0. */
    cog_result_t outcome;
    const void *data; /**< Application frame in caller storage, NULL for control. */
    size_t data_len;
} cog_orchestrator_event_t;

/* ── Lifecycle ──────────────────────────────────────────────────── */

/**
 * @brief Create a new orchestrator.
 *
 * @param[out] out    Uninitialised pointer; set to the orchestrator.
 * @param[in]  config Configuration; must be non-NULL.
 * @return COG_OK on success.
 * @return COG_ERROR_NULL_PTR if @p out or @p config is NULL.
 * @return COG_ERROR_INVALID_ARG if @p config->socket_path is NULL.
 * @return COG_ERROR_NOMEM if allocation fails.
 * @return IO if fresh nonzero incarnation entropy/setup fails (no PID/time
 * fallback); UNSUPPORTED without kernel peer identity or supported child-reap
 * profile. Validate SIGCHLD is neither SIG_IGN nor SA_NOCLDWAIT before fork.
 * @pre For every owned child lifetime the host leaves that disposition stable
 * and gives this context exclusive reap ownership of its exact native PIDs.
 * No foreign waitpid(-1)/handler reaper may consume them. Other host children
 * remain independent; this context never changes SIGCHLD or installs a reaper.
 *
 * Threading: not thread-safe.
 */
cog_result_t cog_orchestrator_new(cog_orchestrator_t **out,
                                   const cog_orchestrator_config_t *config);

/**
 * @brief Destroy an orchestrator.
 *
 * Releases constructor resources after asynchronous shutdown has completed.
 * @pre request_shutdown/service reports all owned children reaped or permanently
 * disowned with explicit ownership_lost/IO. No implicit
 * blocking peer wait or abandoned child is permitted; store outlives orch.
 *
 * @param[in,out] orch Orchestrator to destroy; may be NULL (no-op).
 *
 * Threading: not thread-safe.
 */
void cog_orchestrator_free(cog_orchestrator_t *orch);

/* ── Process management ─────────────────────────────────────────── */

/**
 * @brief Spawn an auxiliary core process.
 *
 * Reserves a managed worker identity and forks/execs a trusted binary without
 * waiting for connection. Fixed startup arguments include the control socket
 * and exact managed identity, never tasks/credentials. service binds an actual
 * socket only after kernel peer PID/UID matches this live UNREAPED owned child
 * and HELLO echoes the exact managed incarnation/number. Connection order and
 * user-provided PID text grant nothing. HEALTHY/placement require validated
 * readiness; no session task is admitted on UNKNOWN readiness.
 *
 * A late connection/HELLO from a reaped worker cannot bind its replacement even
 * if an OS PID is recycled. Endpoint/role references close on retirement; worker
 * IDs never reuse. Pre-auth stop/kill uses the owned reservation's native PID.
 *
 * @param[in,out] orch       Orchestrator.
 * @param[in]     binary_path Path to the cog core binary.
 * @param[out]    out_worker_id Managed process reservation, unchanged on failure.
 * @return COG_OK on success.
 * @return COG_ERROR_NULL_PTR if @p orch, @p binary_path, or
 *         @p out_worker_id is NULL.
 * @return COG_ERROR_OVERFLOW if max_aux_cores is reached.
 * @return LIMIT if managed identity space is exhausted, checked before fork.
 * Also LIMIT before fork if event identity space cannot reserve this worker's
 * three mandatory control transitions in addition to existing reservations.
 * @return IO for known prepublication fork/exec-setup failure. Exec failure after
 * successful publication is reported through service/health/reap, never by
 * silently discarding an already returned owned worker. No blocking waitpid.
 *
 * Threading: not thread-safe.
 */
cog_result_t cog_orchestrator_spawn(cog_orchestrator_t *orch,
                                     const char *binary_path,
                                     cog_worker_id_t *out_worker_id);

/**
 * @brief Gracefully stop an auxiliary core.
 *
 * Admits a nonblocking shutdown intent/deadline for this managed owned worker,
 * including before authentication/connection. service
 * drives acknowledgement, KILL if needed, and nonblocking reap; do not assume
 * the process is gone merely because stop returned OK.
 *
 * @param[in,out] orch    Orchestrator.
 * @param[in]     worker_id Exact managed reservation, not transport identity.
 * @return COG_OK on success.
 * @return COG_ERROR_NULL_PTR if @p orch is NULL.
 * @return COG_ERROR_NOT_FOUND if the peer does not exist.
 *
 * Threading: not thread-safe.
 */
cog_result_t cog_orchestrator_stop(cog_orchestrator_t *orch,
                                    cog_worker_id_t worker_id);

/**
 * @brief Force kill an auxiliary core.
 *
 * Sends SIGKILL only to the immutable still-owned, UNREAPED native child. Use
 * when graceful stop fails. Repeated kill of an already reaped reservation is
 * OK while that record is retained and never signals its old numeric PID.
 * Retired/foreign IDs return NOT_FOUND, never select a reused slot. Before EVERY
 * signal, exact-PID waitpid(WNOHANG) confirms ownership. ECHILD permanently
 * disowns the record and returns IO with no signal. With the supported exclusive
 * reap/disposition lifetime precondition, an exit between check and signal stays
 * an unreaped zombie and cannot recycle PID; no guarantee is made against a
 * foreign thread violating that precondition. No waitpid(-1)/blocking reap.
 *
 * @param[in,out] orch    Orchestrator.
 * @param[in]     worker_id Exact managed reservation, even before connection.
 * @return COG_OK on success.
 * @return COG_ERROR_NULL_PTR if @p orch is NULL.
 * @return COG_ERROR_NOT_FOUND if the peer does not exist.
 *
 * Threading: not thread-safe.
 */
cog_result_t cog_orchestrator_kill(cog_orchestrator_t *orch,
                                    cog_worker_id_t worker_id);

/* ── Health ─────────────────────────────────────────────────────── */

/**
 * @brief Query the health of a peer.
 *
 * @param[in] orch    Orchestrator; may be NULL.
 * @param[in] worker_id Managed reservation to query, including UNKNOWN startup.
 * @return Current health status, or COG_HEALTH_UNKNOWN if @p orch
 *         is NULL or the peer is unknown.
 *
 * Threading: not thread-safe.
 */
cog_health_t cog_orchestrator_health(const cog_orchestrator_t *orch,
                                      cog_worker_id_t worker_id);

/**
 * @brief Retire a bounded managed record after its process/event/route lifetime.
 * @param[in,out] orch Owner thread. @param[in] worker_id Exact retained ID.
 * @return OK; NULL_PTR; NOT_FOUND; INVALID_STATE while child still owned/live,
 * pending copied events/unacknowledged messages or routes remain. Lost ownership
 * is a permanent reported failure; it never permits old-PID signalling/rebind.
 * @post OK frees only the bounded slot; its incarnation/number is never reused.
 * Old stop/kill/health/query cannot target a later worker occupying that slot.
 * No task/effect replay, guard release or semantic agent ack is implied.
 */
cog_result_t cog_orchestrator_release_worker(cog_orchestrator_t *orch,
                                              cog_worker_id_t worker_id);

/**
 * @brief Queue one copied private application frame to an authenticated worker.
 * @param[in,out] orch Single owner of worker fabric.
 * @param[in] worker_id Exact HEALTHY/READY reservation.
 * @param[in] message Bounded caller frame, copied on OK.
 * @return OK exactly once; AGAIN full queue accepts nothing; NULL_PTR;
 * NOT_FOUND; INVALID_STATE not authenticated/ready; LIMIT above max_frame_bytes.
 * No allocation/wait; service flushes partial bytes. App verifies its own role,
 * permission, session/attempt/config protocol; process readiness is not a guard.
 */
cog_result_t cog_orchestrator_send(cog_orchestrator_t *orch,
                                    cog_worker_id_t worker_id,
                                    const cog_ipc_message_t *message);

/**
 * @brief Copy oldest retained event in one class without consuming it.
 * @param[in] orch Owner. @param[in] event_class Control or application messages.
 * @param[out] event Metadata; data points only into caller storage on OK.
 * @param[out] storage Payload bytes; NULL iff capacity=0. No NUL is appended.
 * @param[in] capacity Writable bytes. @param[out] required Exact payload size.
 * @return OK; AGAIN empty class; NULL_PTR; INVALID_ARG; OVERFLOW changes only
 * required and retains event/bytes. Repeated copies safe; no allocation.
 * @note Constructor reserves three independent control transitions per worker
 * (READY/DISCONNECTED/REAPED-or-OWNERSHIP_LOST), separate from message storage.
 * A full message class pauses that peer's application receive, never owner
 * timer/reap/native shutdown progress. FIFO head-of-line pressure can delay
 * incoming heartbeat/acks and is not proof of process death; wire shutdown still
 * has the owned-PID TERM/KILL deadline path. Control may overtake app delivery; app must
 * resolve already-retained final updates/notifications before projecting a
 * fenced failure. Reap/socket loss never overwrites an accepted agent terminal.
 * Kernel/worker proof is copied at ingress and survives endpoint EOF until ack.
 * Identity capacity is reserved as well as storage: ordinary event capture
 * cannot consume IDs needed for outstanding READY/DISCONNECTED/final transitions.
 * LIMIT occurs before new ordinary capture/fork, never wraps or steals a final
 * reservation. Control transitions consume their reserved identities; unused
 * reservations release only when that worker becomes terminal. Fatal ordinary
 * admission still permits native cleanup and publication of reserved final state.
 */
cog_result_t cog_orchestrator_copy_event(const cog_orchestrator_t *orch,
                                          cog_orchestrator_event_class_t event_class,
                                          cog_orchestrator_event_t *event,
                                          void *storage, size_t capacity,
                                          size_t *required);
/**
 * @brief Acknowledge one independently copied/processed retained event.
 * @param[in,out] orch Owner. @param[in] event_id Exact retained event identity.
 * @return OK (latest duplicate ack IN EACH CLASS idempotent); NULL_PTR;
 * NOT_FOUND stale/retired;
 * INVALID_STATE not the oldest retained event in its class. No mutation on error.
 * No native endpoint buffer is borrowed after return. This frees transport event
 * storage ONLY, never acknowledges/applies a CGA1 update or durable tool result.
 */
cog_result_t cog_orchestrator_ack_event(cog_orchestrator_t *orch, uint64_t event_id);

/**
 * @brief List all managed cores.
 *
 * @param[in]  orch      Orchestrator.
 * @param[out] out_infos Caller-provided array for core info.
 * @param[in]  max_infos Maximum entries in @p out_infos.
 * @param[out] out_count Receives the actual number of cores.
 * @return COG_OK on success.
 * @return COG_ERROR_NULL_PTR if @p orch or @p out_count is NULL.
 *
 * Threading: not thread-safe.
 */
cog_result_t cog_orchestrator_list(const cog_orchestrator_t *orch,
                                    cog_core_info_t *out_infos,
                                    size_t max_infos,
                                    size_t *out_count);

/**
 * @brief Record a heartbeat from an auxiliary core.
 *
 * Called when a heartbeat message is received via IPC.
 *
 * @param[in,out] orch             Orchestrator.
 * @param[in]     peer_id          Core sending the heartbeat.
 * @param[in]     active_subagents Number of active subagents.
 * @return COG_OK on success.
 * @return COG_ERROR_NULL_PTR if @p orch is NULL.
 * @return COG_ERROR_NOT_FOUND if the peer is unknown.
 *
 * Threading: not thread-safe.
 */
cog_result_t cog_orchestrator_heartbeat(cog_orchestrator_t *orch,
                                         cog_peer_id_t peer_id,
                                         size_t active_subagents);

/* ── Placement ──────────────────────────────────────────────────── */

/**
 * @brief Choose a core for a new subagent.
 *
 * Uses health and capacity to select the best core.  Returns
 * COG_PEER_ID_INVALID (0) if the subagent should run locally in
 * the main core.
 * Returns only an authenticated READY ACTUAL control-fabric peer ID, never a
 * managed worker identity. Parent agent placement/CGA1/CGH1 and owner routes
 * continue using actual peers scoped by this orchestrator's fabric. Placement
 * is advisory capacity selection, not admission/permission/guard ownership.
 *
 * @param[in] orch           Orchestrator; may be NULL.
 * @param[in] estimated_load Estimated resource usage (0 = light).
 * @return Peer id of the chosen core, or COG_PEER_ID_INVALID for local.
 *
 * Threading: not thread-safe.
 */
cog_peer_id_t cog_orchestrator_place(const cog_orchestrator_t *orch,
                                      size_t estimated_load);

/* ── Shutdown ───────────────────────────────────────────────────── */

/**
 * @brief Gracefully shut down all managed cores.
 *
 * Legacy blocking compatibility helper, excluded from responsive core hooks.
 * New code uses request_shutdown/service. Sends shutdown commands and waits
 * up to the timeout.  Cores that don't respond are force-killed.
 *
 * @param[in,out] orch       Orchestrator.
 * @param[in]     timeout_ms Maximum time to wait for graceful shutdown.
 * @return COG_OK on success.
 * @return COG_ERROR_NULL_PTR if @p orch is NULL.
 * @return COG_ERROR_TIMEOUT if not all cores acknowledged in time.
 *
 * Threading: not thread-safe.
 */
cog_result_t cog_orchestrator_shutdown(cog_orchestrator_t *orch,
                                        uint32_t timeout_ms);

/* ── Session tracking ───────────────────────────────────────────── */

/**
 * @brief List sessions running on a specific peer.
 *
 * Used during crash recovery to find affected subagents.
 *
 * @param[in]  orch         Orchestrator.
 * @param[in]  peer_id      Peer to query.
 * @param[out] out_sessions Caller-provided array for session ids.
 * @param[in]  max_sessions Maximum entries in @p out_sessions.
 * @param[out] out_count    Receives the actual number of sessions.
 * @return COG_OK on success.
 * @return COG_ERROR_NULL_PTR if @p orch, @p out_sessions, or
 *         @p out_count is NULL.
 * @return COG_ERROR_NOT_FOUND if the peer is unknown.
 *
 * Threading: not thread-safe.
 */
cog_result_t cog_orchestrator_sessions(const cog_orchestrator_t *orch,
                                        cog_peer_id_t peer_id,
                                        const char **out_sessions,
                                        size_t max_sessions,
                                        size_t *out_count);

/**
 * @brief Advance bounded child connection/heartbeat/shutdown/reap work, no wait.
 * @param[in,out] orch Owner thread. @param[in] now_ms Nondecreasing clock.
 * @param[out] out_shutdown_done True iff requested shutdown has disposed all
 * owned lifetimes (reaped or explicit permanent ownership_lost/IO); false before
 * shutdown. Lost ownership is NEVER successful cleanup/proof descendants stopped.
 * @param[out] out_deadline_ms Earliest absolute timer or UINT64_MAX.
 * @return OK; AGAIN no progress; NULL_PTR; INVALID_ARG clock; IO/PROTOCOL/LIMIT/
 * NOMEM/UNSUPPORTED driver failure. Failure retains owned state; later service
 * still advances shutdown/reap while admission is frozen, reporting first error.
 * ownership_lost remains observable in its control event and retained core_info
 * even when an earlier error determines the return. IO is returned for lost
 * ownership only if no earlier primary error exists; done never means success.
 * @note Bounded by constructor service_quantum even with continuous traffic.
 * No blocking handshake/waitpid or foreign-PID signals. Dead owner sessions
 * stay failed/frozen; auto_replace creates idle capacity, never replays tools.
 * At most one actual transport binds each managed worker. Disconnect ends that
 * attempt: no new peer ID may reattach immutable owner_peer. UI reconnect is a
 * separate fabric. Accepted endpoint construction is explicit and peer-bounded;
 * NOMEM is a checked driver failure, never table/buffer growth.
 */
cog_result_t cog_orchestrator_service(cog_orchestrator_t *orch, uint64_t now_ms,
                                       bool *out_shutdown_done,
                                       uint64_t *out_deadline_ms);
/**
 * @brief Admit bounded all-child shutdown, then service until done.
 * @param[in,out] orch Owner. @param[in] now_ms Clock.
 * @param[in] timeout_ms Finite graceful deadline before owned-child KILL/reap.
 * @return OK; NULL_PTR; INVALID_ARG. Idempotent, never extends earlier deadline.
 * @note No peer wait; service reports TIMEOUT only with out_shutdown_done=true
 * after all owned children are reaped. Lost ownership remains explicitly visible
 * and reports IO with done if it is the first error, once no owned lifetime
 * remains; it never authorizes failover or effect replay. An earlier primary
 * driver error remains the return rather than being replaced by cleanup IO.
 * Other errors retain ownership for explicit cleanup and do not make free safe.
 */
cog_result_t cog_orchestrator_request_shutdown(cog_orchestrator_t *orch,
                                                uint64_t now_ms,
                                                uint32_t timeout_ms);
/**
 * @brief Publish an advisory route only for the durably admitted active owner.
 * @param[in,out] orch Owner with borrowed shared store configured.
 * @param[in] session_id Copied UTF-8 ID <=127 bytes.
 * @param[in] peer_id Healthy authenticated destination owning its live guard;
 * zero explicitly denotes the trusted local coordinator executor, never a
 * remote/unknown peer. The app must validate the local guarded READY directly.
 * @param[in] owner Durable process-incarnation tuple, not socket identity.
 * @param[in] checkpoint Exact validated shared-store generation loaded by target.
 * @return OK (identical publication idempotent); NULL_PTR; LIMIT reserved routes;
 * INVALID_STATE stale/conflicting epoch/peer; NOT_FOUND peer; IO/INDETERMINATE
 * store state; UNSUPPORTED no fenced store/profile. Failure changes no route.
 * @note Checks owner_read against claimed tuple/checkpoint; route is NOT an
 * execution lease. The app validates target READY under that guard before
 * publication. Target remains staged (no new save/effects) between acquiring
 * its guard and publishing this initial checkpoint route; only then starts
 * work. Registration never overwrites a newer epoch. No remote-host or
 * active-handle migration; quiescent same-host shared-store handoff only.
 */
cog_result_t cog_orchestrator_publish_owner(cog_orchestrator_t *orch,
                                            const char *session_id,
                                            cog_peer_id_t peer_id,
                                            cog_store_owner_t owner,
                                            cog_store_checkpoint_t checkpoint);
/**
 * @brief Query advisory routed ownership; durable guard alone grants execution.
 * @param[in] orch Owner. @param[in] session_id Registered ID.
 * @param[out] peer_id Socket peer. @param[out] owner Epoch/incarnation.
 * @param[out] checkpoint Validated immutable generation.
 * @return OK; NULL_PTR; NOT_FOUND. Outputs unchanged on failure; no allocation.
 */
cog_result_t cog_orchestrator_session_owner(const cog_orchestrator_t *orch,
                                            const char *session_id,
                                            cog_peer_id_t *peer_id,
                                            cog_store_owner_t *owner,
                                            cog_store_checkpoint_t *checkpoint);
/**
 * @brief Remove an advisory route only if its exact owner is still current.
 * @param[in,out] orch Owner. @param[in] session_id ID.
 * @param[in] expected Current owner tuple, prevents stale disconnect teardown.
 * @return OK; NULL_PTR; NOT_FOUND; INVALID_STATE stale tuple. Does not release
 * a durable owner guard or authorize failover; remaining data/ledger persist.
 */
cog_result_t cog_orchestrator_forget_owner(cog_orchestrator_t *orch,
                                           const char *session_id,
                                           cog_store_owner_t expected);

/** @brief Bounded same-host handoff envelope; no native handles are transferred. */
typedef enum cog_handoff_kind {
    COG_HANDOFF_QUIESCE = 1,
    COG_HANDOFF_PREPARED,
    COG_HANDOFF_COMMIT,
    COG_HANDOFF_ABORT,
    COG_HANDOFF_FAILED,
    COG_HANDOFF_CHECKPOINTED, /**< Source froze and saved exact generation. */
    COG_HANDOFF_READY, /**< Target owns committed guard, still staged. */
    COG_HANDOFF_ACTIVATE /**< Coordinator published route; permit fresh input. */
} cog_handoff_kind_t;
/** @brief Every ACK is correlated to exact old/new owner and checkpoint tuples. */
typedef struct cog_handoff_message {
    cog_handoff_kind_t kind;
    const char *session_id; /**< UTF-8 <=127 bytes; exact configured shared store. */
    cog_peer_id_t source_peer; /**< Actual worker peer or trusted local0. */
    cog_peer_id_t target_peer; /**< Actual worker peer or trusted local0;
       source/target differ and cannot both be0. No live/active-child handoff. */
    cog_store_owner_t expected;
    cog_store_owner_t next;
    cog_store_checkpoint_t checkpoint; /**< QUIESCE requires0 (not saved yet).
       CHECKPOINTED/PREPARED/COMMIT/READY/ACTIVATE require exact nonzero source
       checkpoint. ABORT/FAILED echo the current phase:0 only before source
       checkpoint is known. No invented sentinel selects a latest generation. */
} cog_handoff_message_t;
/**
 * @brief Encode CGH1 correlation metadata, never a live session/provider/tool.
 * @param[in] message Complete view. @param[out] output Bytes; NULL iff capacity=0.
 * @param[in] capacity Bytes. @param[out] required Exact wire size.
 * @return OK; OVERFLOW changes required only; NULL_PTR; INVALID_ARG.
 * @note CGH1 magic, kind u8, session length u8+UTF-8 (no NUL), source/target peer,
 * expected epoch/instance, next epoch/instance, checkpoint generation as u64
 * big-endian in that order. Shared store authority is fixed/verified at peer
 * startup; no path or remote transfer is selected by an untrusted envelope.
 * Expected/next owner tuples are nonzero with strictly increasing epoch. A
 * coordinator admits only one handoff per session/next tuple; IDs never reused.
 * QUIESCE carries checkpoint0 because guarded source save creates a fresh exact
 * generation. CHECKPOINTED pins that generation; every later phase echoes it.
 * Source remains guarded until target PREPARED; then source releases guard and
 * coordinator CASes expected->next at that EXACT checkpoint. COMMIT permits
 * target acquire only. READY proves guarded/still-staged target; publish_owner
 * precedes ACTIVATE, which alone permits new work. Retries are exact phase/tuple
 * duplicates, not a second save/CAS. No route is an execution lease.
 * Pre-CAS abort can resume source ONLY if its original guard remains owned and
 * durable tuple/checkpoint still match; after guard release, reacquire requires
 * an explicit fresh owner epoch. After CAS/uncertain commit, never restore old
 * epoch/thaw source. Failed target remains frozen; explicit recovery/reclaim
 * advances to another fresh epoch under the same checkpoint/guard rules.
 */
cog_result_t cog_orchestrator_handoff_encode(const cog_handoff_message_t *message,
                                             void *output, size_t capacity,
                                             size_t *required);
/**
 * @brief Decode CGH1 to caller storage without route or ownership side effects.
 * @param[in] input Bytes. @param[in] input_len Exact length.
 * @param[out] message Metadata. @param[out] session_id Copied ID+NUL.
 * @param[in] capacity ID bytes. @param[out] required Exact ID bytes.
 * @return OK; OVERFLOW changes required only; NULL_PTR; UNSUPPORTED version/kind;
 * PROTOCOL bad UTF-8/length/zero tuple/wrapped epoch/trailing bytes. Caller
 * validates transport peer, store authority and all ACK correlation fields.
 * No allocation, activation, rollback, or native pointer survives input.
 */
cog_result_t cog_orchestrator_handoff_decode(const void *input, size_t input_len,
                                             cog_handoff_message_t *message,
                                             char *session_id, size_t capacity,
                                             size_t *required);

#ifdef __cplusplus
}
#endif

#endif /* COG_ORCHESTRATOR_H */

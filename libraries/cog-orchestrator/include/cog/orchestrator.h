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
    cog_peer_id_t peer_id;        /**< IPC peer identifier. */
    int pid;                      /**< OS process id. */
    cog_health_t health;          /**< Current health status. */
    size_t active_subagents;      /**< Number of running subagents. */
    size_t capacity;              /**< Maximum subagent capacity. */
    uint64_t last_heartbeat_ms;   /**< Timestamp of last heartbeat. */
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
} cog_orchestrator_config_t;

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
 *
 * Threading: not thread-safe.
 */
cog_result_t cog_orchestrator_new(cog_orchestrator_t **out,
                                   const cog_orchestrator_config_t *config);

/**
 * @brief Destroy an orchestrator.
 *
 * Stops all managed cores and releases resources.
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
 * Forks and execs the given binary with the IPC socket path as an
 * argument.  The aux core connects back to the orchestrator.
 *
 * @param[in,out] orch       Orchestrator.
 * @param[in]     binary_path Path to the cog core binary.
 * @param[out]    out_peer_id Receives the peer id of the new core.
 * @return COG_OK on success.
 * @return COG_ERROR_NULL_PTR if @p orch, @p binary_path, or
 *         @p out_peer_id is NULL.
 * @return COG_ERROR_OVERFLOW if max_aux_cores is reached.
 * @return COG_ERROR_IO if fork/exec fails.
 *
 * Threading: not thread-safe.
 */
cog_result_t cog_orchestrator_spawn(cog_orchestrator_t *orch,
                                     const char *binary_path,
                                     cog_peer_id_t *out_peer_id);

/**
 * @brief Gracefully stop an auxiliary core.
 *
 * Sends a shutdown command and waits for acknowledgment.
 *
 * @param[in,out] orch    Orchestrator.
 * @param[in]     peer_id Core to stop.
 * @return COG_OK on success.
 * @return COG_ERROR_NULL_PTR if @p orch is NULL.
 * @return COG_ERROR_NOT_FOUND if the peer does not exist.
 *
 * Threading: not thread-safe.
 */
cog_result_t cog_orchestrator_stop(cog_orchestrator_t *orch,
                                    cog_peer_id_t peer_id);

/**
 * @brief Force kill an auxiliary core.
 *
 * Sends SIGKILL.  Use when graceful stop fails.
 *
 * @param[in,out] orch    Orchestrator.
 * @param[in]     peer_id Core to kill.
 * @return COG_OK on success.
 * @return COG_ERROR_NULL_PTR if @p orch is NULL.
 * @return COG_ERROR_NOT_FOUND if the peer does not exist.
 *
 * Threading: not thread-safe.
 */
cog_result_t cog_orchestrator_kill(cog_orchestrator_t *orch,
                                    cog_peer_id_t peer_id);

/* ── Health ─────────────────────────────────────────────────────── */

/**
 * @brief Query the health of a peer.
 *
 * @param[in] orch    Orchestrator; may be NULL.
 * @param[in] peer_id Peer to query.
 * @return Current health status, or COG_HEALTH_UNKNOWN if @p orch
 *         is NULL or the peer is unknown.
 *
 * Threading: not thread-safe.
 */
cog_health_t cog_orchestrator_health(const cog_orchestrator_t *orch,
                                      cog_peer_id_t peer_id);

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
 * Sends shutdown commands to all aux cores and waits for acknowledgments
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

#ifdef __cplusplus
}
#endif

#endif /* COG_ORCHESTRATOR_H */
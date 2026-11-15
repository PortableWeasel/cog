/**
 * @file orchestrator_internal.h
 * @brief Private shared layout for libraries/cog-orchestrator/src/orchestrator.c.
 *
 * This header is internal to cog-orchestrator.  It is not installed and
 * must not be used by consumers.  It exists to give the white-box unit
 * and integration tests a way to drive the orchestrator deterministically:
 *
 *   - accept pending auxiliary-core connections (the public contract has
 *     no accept entry point; the main core's run loop drives the
 *     orchestrator through these hooks),
 *   - poll/reap/recv heartbeats and control messages,
 *   - register and unregister sessions for crash-recovery tracking,
 *   - override the monotonic clock so health degradation is testable
 *     without sleeping.
 *
 * The public contract in <cog/orchestrator.h> is unchanged; these are
 * private hooks, mirroring how cog-core exposes agent_internal.h to its
 * white-box tests.
 */
#ifndef COG_ORCHESTRATOR_INTERNAL_H
#define COG_ORCHESTRATOR_INTERNAL_H

#include "cog/orchestrator.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Drive the orchestrator: reap dead children, accept pending
 *        connections, and process incoming control messages.
 *
 * This is the hook the main core's run loop calls each iteration.  It:
 *   - refreshes the cached monotonic clock,
 *   - reaps any exited auxiliary-core children (waitpid WNOHANG),
 *   - accepts any pending listener connections and registers them as
 *     managed cores,
 *   - drains readable messages from each managed core's endpoint
 *     (heartbeats, session registrations, shutdown acknowledgements),
 *   - auto-replaces dead cores when @c auto_replace is enabled.
 *
 * @param[in,out] orch       Orchestrator.
 * @param[in]     timeout_ms Poll timeout for the listener and per-core
 *                           endpoints (0 = non-blocking).
 * @return COG_OK on success.
 * @return COG_ERROR_NULL_PTR if @p orch is NULL.
 *
 * Threading: not thread-safe.
 */
cog_result_t cog_orchestrator_poll(cog_orchestrator_t *orch, int timeout_ms);

/**
 * @brief Accept a single pending auxiliary-core connection.
 *
 * Registers the new connection as a managed core with health UNKNOWN and
 * the default capacity.  The peer's OS pid is read via SO_PEERCRED.
 *
 * @param[in,out] orch        Orchestrator.
 * @param[out]    out_peer_id Optional; receives the accepted peer id.
 * @return COG_OK on success.
 * @return COG_ERROR_NULL_PTR if @p orch is NULL.
 * @return COG_ERROR_AGAIN if no connection is pending.
 * @return COG_ERROR_OVERFLOW if max_aux_cores is reached.
 * @return COG_ERROR_IO if accept fails.
 *
 * Threading: not thread-safe.
 */
cog_result_t cog_orchestrator_accept(cog_orchestrator_t *orch,
                                     cog_peer_id_t *out_peer_id);

/**
 * @brief Register a session as hosted on a peer.
 *
 * Used during crash recovery to find affected subagents.  The session id
 * is copied.
 *
 * @param[in,out] orch       Orchestrator.
 * @param[in]     peer_id    Peer hosting the session.
 * @param[in]     session_id Session identifier; copied.
 * @return COG_OK on success.
 * @return COG_ERROR_NULL_PTR if @p orch or @p session_id is NULL.
 * @return COG_ERROR_NOT_FOUND if the peer is unknown.
 * @return COG_ERROR_OVERFLOW if the peer's session table is full.
 *
 * Threading: not thread-safe.
 */
cog_result_t cog_orchestrator_register_session(cog_orchestrator_t *orch,
                                               cog_peer_id_t peer_id,
                                               const char *session_id);

/**
 * @brief Remove a session from a peer's tracking table.
 *
 * @param[in,out] orch       Orchestrator.
 * @param[in]     peer_id    Peer that hosted the session.
 * @param[in]     session_id Session to remove.
 * @return COG_OK on success.
 * @return COG_ERROR_NULL_PTR if @p orch or @p session_id is NULL.
 * @return COG_ERROR_NOT_FOUND if the peer is unknown or the session is
 *         not registered.
 *
 * Threading: not thread-safe.
 */
cog_result_t cog_orchestrator_unregister_session(cog_orchestrator_t *orch,
                                                 cog_peer_id_t peer_id,
                                                 const char *session_id);

/**
 * @brief Override the orchestrator's monotonic clock (test hook).
 *
 * @param[in,out] orch   Orchestrator.
 * @param[in]     now_ms Monotonic time in milliseconds.
 *
 * Threading: not thread-safe.
 */
void cog_orchestrator_set_now(cog_orchestrator_t *orch, uint64_t now_ms);

/**
 * @brief Override the spawn connection timeout (test hook).
 *
 * @param[in,out] orch      Orchestrator.
 * @param[in]     timeout_ms Timeout in milliseconds.
 *
 * Threading: not thread-safe.
 */
void cog_orchestrator_set_spawn_timeout(cog_orchestrator_t *orch,
                                        uint32_t timeout_ms);

/**
 * @brief Override the shutdown-acknowledgement timeout (test hook).
 *
 * @param[in,out] orch      Orchestrator.
 * @param[in]     timeout_ms Timeout in milliseconds.
 *
 * Threading: not thread-safe.
 */
void cog_orchestrator_set_ack_timeout(cog_orchestrator_t *orch,
                                      uint32_t timeout_ms);

/**
 * @brief Return the orchestrator's current monotonic clock value.
 *
 * @param[in] orch Orchestrator; may be NULL.
 * @return Current monotonic time in milliseconds, or 0 if @p orch is NULL.
 *
 * Threading: not thread-safe.
 */
uint64_t cog_orchestrator_now(const cog_orchestrator_t *orch);

#ifdef __cplusplus
}
#endif

#endif /* COG_ORCHESTRATOR_INTERNAL_H */

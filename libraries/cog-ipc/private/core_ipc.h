/** @file core_ipc.h
 * @brief Private cog-ipc/core integration query, never installed.
 * Owned by cog-ipc; no endpoint layout crosses the library boundary.
 */
#ifndef COG_PRIVATE_CORE_IPC_H
#define COG_PRIVATE_CORE_IPC_H
#include <cog/ipc.h>
/** 0 unbound, 1 listener, 2 connected or connecting. No mutation. */
int cog_ipc_internal_role(const cog_ipc_endpoint_t *endpoint);
/** Single bounded framing/connect unit for the core's aggregate quantum. */
cog_result_t cog_ipc_internal_service_one(cog_ipc_endpoint_t *endpoint, uint64_t now_ms,
                                          uint64_t *deadline);
/** Refuse one excess incoming connection without constructing an endpoint. */
cog_result_t cog_ipc_internal_refuse(cog_ipc_endpoint_t *listener);
/** Query whether one endpoint has work that can advance without waiting.
 * Read-only: no service, queue mutation, credential capture or allocation.
 * Listener checks only acceptance readiness, not its independently owned peers.
 * Complete retained RX and latched terminal failure are immediately actionable.
 * Handshake interest matches its next native unit; established links request
 * writable readiness only with queued TX, never just because a socket is writable.
 * Readable/poll-error readiness is considered only for eligible receive work.
 * A caller separately combines handshake/service deadlines and event/arena
 * backpressure; this query grants neither proof nor permission to consume RX.
 * Returns OK and writes ready (possibly false); NULL_PTR/INVALID_STATE unbound/
 * IO leave ready unchanged; AGAIN on transient poll interruption leaves ready
 * unchanged and requires a later bounded retry. Only zero-time poll is allowed.
 */
cog_result_t cog_ipc_internal_ready(const cog_ipc_endpoint_t *endpoint, bool *ready);
#endif

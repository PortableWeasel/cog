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
#endif

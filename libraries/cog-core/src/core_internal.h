/** @file core_internal.h
 * @brief C11-only private driver, peer-cache and reservation layout.
 * Owned only by core.c. Not installed; no public/IPC endpoint layout crosses
 * this boundary. Static helpers in core.c own transitions; no mutable globals.
 */
#ifndef COG_PRIVATE_CORE_INTERNAL_H
#define COG_PRIVATE_CORE_INTERNAL_H
#include <cog/core.h>
#include "agent_internal.h"
#include <stdatomic.h>

typedef enum core_driver_kind {
    CORE_DRIVER_NONE = 0,
    CORE_DRIVER_RUN,
    CORE_DRIVER_MANUAL
} core_driver_kind_t;
/** One unified engine, with run-only waiting outside bounded slices. */
typedef struct core_drive_state {
    core_driver_kind_t kind;
    bool inside_slice; /**< Forbid callback/reentrant manual driver operations. */
    bool done; /**< Stop/fatal latched, still owned until matching end. */
    bool service_due;
    cog_result_t result; /**< First work error, or OK for ordinary stop. */
    uint64_t deadline;
    size_t dispatched_since_service;
} core_drive_state_t;
/** One exact endpoint lifetime. Copy this whole record when compacting slots.
 * Proof is cached immediately after handshake publication, before any further
 * service, independent of event/arena capacity. It survives EOF and drive_end;
 * removal clears the record only after copied event descriptors own their proof.
 * Endpoint ownership has transferred: callers cannot disconnect/relisten behind
 * the core. The numeric ID alone never authenticates a replacement connection.
 */
typedef struct core_peer {
    cog_ipc_endpoint_t *endpoint;
    cog_peer_id_t id;
    cog_ipc_peer_info_t proof;
    bool proof_valid;
    bool connected_queued;
} core_peer_t;

typedef struct core_pending_owner {
    char id[128];
    size_t count;
    bool leased; /**< An injected copy reserves this owner and one restore slot. */
} core_pending_owner_t;
typedef struct core_pending_item {
    size_t length;
} core_pending_item_t;
typedef struct core_slice {
    size_t offset, length, charge;
} core_slice_t;
struct cog_core {
    cog_session_t *session;
    const cog_dispatch_entry_t *dispatch_table;
    size_t dispatch_table_size;
    const cog_tool_table_t *tool_table;
    cog_event_queue_t *event_queue;
    cog_event_queue_t *ipc_events;
    bool prefer_ipc;
    core_drive_state_t drive;
    size_t step_event_limit;
    bool clock_seen;
    uint64_t last_now;
    cog_wait_fn wait_fn;
    void *wait_arg;
    cog_service_fn service_fn;
    void *service_arg;
    cog_idle_fn idle_fn;
    void *idle_arg;
    size_t service_interval;
    uint32_t max_idle_wait_ms;
    void *user;
    cog_ipc_endpoint_t *ipc_endpoint;
    cog_peer_id_t local_peer_id;
    int ipc_is_listener;
    core_peer_t *ipc_clients;
    core_peer_t ipc_attached;
    bool require_peer_credentials;
    size_t ipc_client_count, ipc_client_capacity, ipc_cursor, ipc_quantum;
    size_t ipc_max_frame;
    unsigned char *ipc_arena;
    size_t ipc_arena_capacity, ipc_arena_head, ipc_arena_tail, ipc_arena_used;
    core_slice_t *ipc_slices;
    size_t ipc_slice_capacity, ipc_slice_head, ipc_slice_count;
    cog_provider_t *provider;
    cog_obs_t *obs;
    cog_store_t *store;
    cog_state_t state;
    _Atomic bool running, stop_requested;
    core_pending_owner_t *pending_owners;
    core_pending_item_t *pending_items;
    char *pending_text;
    size_t pending_owner_capacity, pending_per_owner, pending_bytes;
    char *injected_payload;
    size_t injected_owner, injected_length;
    bool injected;
    cog_agent_registry_t agents;
};
#endif

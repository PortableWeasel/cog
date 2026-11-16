/** @file ipc_internal.h
 * @brief Private endpoint layout and lifecycle for authenticated bounded IPC.
 * Not installed; ipc.c owns all mutations. No global state or runtime growth.
 */
#ifndef COG_PRIVATE_IPC_INTERNAL_H
#define COG_PRIVATE_IPC_INTERNAL_H
#include <cog/ipc.h>
#include <sys/un.h>

/* Native capture: once per connection after native connect success, before
 * publishing handshake readiness; accepted sockets capture before publication.
 * A failed/unsupported capture is retained, not replaced with HELLO/caller PID.
 * Generic unauthenticated IPC remains usable; peer_info fails closed.
 * Constructor explicitly starts native_peer_result at AGAIN, never calloc OK.
 * No credential-query syscall occurs in the public metadata query.
 * Query requires live socket and published handshake; retained facts do not
 * resurrect a disconnected link. Core owns any already-admitted proof copy.
 *
 * Queue retirement: when wipe_buffers is true, volatile byte stores clear the
 * full reserved slot (including prefix/tail) after complete TX flush or successful
 * RX copy, then clear its descriptor. OVERFLOW/AGAIN never retires a slot.
 * Physical failure abandons all TX and only incomplete RX scratch (none when
 * rx_count equals receive_frames; that slot is a complete retained head). Complete RX
 * slots and the immutable native snapshot survive until explicit retirement.
 * Explicit disconnect/free/unwind clears all allocated payload reservations.
 * Caller-owned message buffers remain caller-owned; no implicit wipe is added.
 */
#define IPC_DEFAULT_FRAME ((size_t)1048576)
#define IPC_ID_BYTES 8U
#define IPC_SESSION_BYTES 128U
#define IPC_IO_BYTES ((size_t)65536)

typedef struct ipc_frame {
    size_t length;
    size_t cursor;
} ipc_frame_t;
typedef struct ipc_route {
    char id[IPC_SESSION_BYTES];
    cog_peer_id_t peer;
} ipc_route_t;
struct cog_ipc_endpoint {
    int fd;
    int listener_role;
    int connecting;
    int connected;
    int connect_pending;
    int clock_set;
    uint64_t last_now;
    uint64_t handshake_deadline;
    cog_result_t failure;
    cog_peer_id_t peer_id;
    cog_peer_id_t next_peer;
    /* Kernel snapshot captured once after native connection establishment.
       result is AGAIN until attempted, then OK/IO/UNSUPPORTED; never authority
       from identity bytes or caller data. info.peer_id is filled on query. */
    cog_ipc_peer_info_t native_peer;
    cog_result_t native_peer_result;
    cog_ipc_endpoint_config_t cfg;
    ipc_frame_t *rx;
    ipc_frame_t *tx;
    unsigned char *rx_data;
    unsigned char *tx_data;
    ipc_route_t *routes;
    size_t route_count;
    size_t rx_head, rx_count, tx_head, tx_count;
    unsigned char prefix[4];
    size_t prefix_used;
    size_t rx_partial;
    int rx_length_known;
    unsigned char identity[IPC_ID_BYTES];
    size_t identity_used;
    unsigned int direction;
    cog_ipc_endpoint_t *parent, *next, *previous, *peers, *service_peer;
    char path[sizeof(((struct sockaddr_un *)0)->sun_path)];
};

#endif

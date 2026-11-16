/**
 * @file ipc.h
 * @brief IPC fabric transport layer over Unix domain sockets.
 *
 * cog-ipc provides connection-oriented message transport between Cog
 * processes (UI, main core, auxiliary cores).  Each endpoint wraps a
 * Unix domain socket.  A listener endpoint accepts incoming connections;
 * a connected endpoint sends and receives framed messages.
 *
 * All contexts are caller-allocated and released through matching
 * destructors.  The library never allocates hidden buffers: message
 * payloads live in caller-provided @c cog_ipc_message_t buffers.
 *
 * Threading model: a single @c cog_ipc_endpoint_t instance is not
 * thread-safe.  One thread at a time may operate on an endpoint.  Distinct
 * endpoints may be used by distinct threads without locking.
 *
 * Nonblocking model: every socket is nonblocking. Send copies a whole frame
 * into a constructor-reserved queue; receive consumes only complete retained
 * frames. endpoint_service advances bounded partial I/O and identity handshake
 * work without waiting. Callers explicitly drive service and handle AGAIN.
 * Only endpoint_poll permits the documented legacy positive/indefinite wait;
 * responsive service hooks and the core use no positive poll waits.
 */
#ifndef COG_IPC_H
#define COG_IPC_H

#include <cog/result.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Invalid peer identifier.
 */
#define COG_PEER_ID_INVALID ((cog_peer_id_t)0)

/**
 * @brief Remote peer identifier.
 *
 * Identifies a connected peer within its owning listener/fabric. Accept
 * assigns monotonically increasing nonzero IDs, never reused during that
 * listener lifetime; connect learns the same ID through its handshake.
 * Independent fabrics may assign the same numeric ID. Cross-fabric consumers
 * must use the composite (owning endpoint/fabric, peer_id), never the number
 * alone. Exhausted listener identity space rejects further accepts with LIMIT.
 */
typedef uint64_t cog_peer_id_t;

/**
 * @brief IPC message buffer.
 *
 * The caller owns the @c data buffer.  Use @c cog_ipc_message_new to
 * allocate the buffer and @c cog_ipc_message_free to release it.  The
 * @c len field is updated by @c cog_ipc_endpoint_recv to reflect the
 * number of payload bytes received.
 */
typedef struct cog_ipc_message {
    uint8_t *data;       /**< Payload bytes; caller-owned. */
    size_t len;          /**< Number of valid payload bytes. */
    size_t capacity;     /**< Total allocated bytes in @c data. */
} cog_ipc_message_t;

/**
 * @brief Poll event bits.
 */
typedef enum cog_ipc_poll_event {
    COG_IPC_POLL_IN  = 0x01, /**< Readable data or pending connection. */
    COG_IPC_POLL_OUT = 0x02, /**< Ready for writing. */
    COG_IPC_POLL_ERR = 0x04, /**< Error condition. */
} cog_ipc_poll_event_t;

/**
 * @brief Opaque IPC endpoint.
 *
 * Created with @c cog_ipc_endpoint_new and destroyed with
 * @c cog_ipc_endpoint_free.  An endpoint may be a listener, a connected
 * client, or an accepted connection.
 */
typedef struct cog_ipc_endpoint cog_ipc_endpoint_t;

/** @brief Kernel-origin identity of one established Unix connection.
 * peer_id identifies the link within its listener, not these native credentials.
 * Linux SO_PEERCRED values are the kernel connection-establishment snapshot
 * (socketpair creation for socketpairs), never HELLO/user PID or a refreshed
 * effective-credential/process-liveness query. */
typedef struct cog_ipc_peer_info {
    cog_peer_id_t peer_id;
    uint64_t process_id; /**< Nonzero native peer PID, diagnostic/auth input only. */
    uint64_t user_id; /**< Native UID at establishment; zero is a valid UID. */
    uint64_t group_id; /**< Native GID at establishment. */
} cog_ipc_peer_info_t;

/**
 * @brief Allocate an IPC message buffer.
 *
 * @param[out] msg      Uninitialised message descriptor; set on success.
 * @param[in]  capacity Number of bytes to allocate for @c msg->data.
 *                     Must be greater than zero.
 * @return COG_OK on success.
 * @return COG_ERROR_NULL_PTR if @p msg is NULL.
 * @return COG_ERROR_INVALID_ARG if @p capacity is zero.
 * @return COG_ERROR_NOMEM if allocation fails.
 *
 * @post On success, @p msg->data points to @p capacity bytes, @p msg->len
 *       is zero, and @p msg->capacity is @p capacity.
 * @post On failure, @p msg is zeroed and no resources are held.
 *
 * Threading: not thread-safe.
 */
cog_result_t cog_ipc_message_new(cog_ipc_message_t *msg, size_t capacity);

/**
 * @brief Release an IPC message buffer.
 *
 * @param[in,out] msg Message to release; may be NULL (no-op).
 *
 * @post The buffer owned by @p msg is released and @p msg is zeroed.
 *
 * Threading: not thread-safe.
 */
cog_result_t cog_ipc_message_free(cog_ipc_message_t *msg);

/** @brief Constructor-reserved nonblocking framing limits; zero selects defaults. */
typedef struct cog_ipc_endpoint_config {
    size_t max_frame_bytes; /**< Default 1 MiB; oversized peer frame is LIMIT. */
    size_t receive_frames; /**< Default 8 complete+partial retained frames. */
    size_t send_frames; /**< Default 8 copied output frames. */
    size_t service_quantum; /**< Default 16 bounded read/write/frame units. */
    size_t max_routes; /**< Default 64 copied session routes; ID max 127 bytes. */
    uint32_t connect_timeout_ms; /**< Default 1000; asynchronous handshake timer. */
} cog_ipc_endpoint_config_t;
/**
 * @brief Reserve bounded framing/routing storage; all sockets are nonblocking.
 * @param[out] out Owned unbound endpoint, NULL on failure.
 * @param[in] config NULL selects documented defaults; checked size arithmetic.
 * @return OK; NULL_PTR; INVALID_ARG; NOMEM. No socket/peer activity on failure.
 * @note Owner thread. Accepted endpoints copy listener limits and use explicit
 * constructor allocation released by endpoint_free. No runtime buffer growth.
 */
cog_result_t cog_ipc_endpoint_new_ex(cog_ipc_endpoint_t **out,
                                     const cog_ipc_endpoint_config_t *config);
/**
 * @brief Advance bounded partial framing/connect/output work without waiting.
 * @param[in,out] endpoint Bound/connecting endpoint (listener accepts separately).
 * @param[in] now_ms Nondecreasing caller monotonic clock.
 * @param[out] out_deadline_ms Absolute next timer or UINT64_MAX.
 * @return OK; AGAIN no progress; NULL_PTR; INVALID_ARG clock; IO/PROTOCOL/LIMIT
 * terminal peer failure. Partial prefixes and payloads stay endpoint-owned;
 * no read_full/send_full loops wait for peer. Full input queue pauses that peer,
 * not unrelated endpoints; an over-limit declared frame fails before allocation.
 * @note Owner thread, no positive poll wait. Core calls this at fairness steps.
 */
cog_result_t cog_ipc_endpoint_service(cog_ipc_endpoint_t *endpoint,
                                      uint64_t now_ms, uint64_t *out_deadline_ms);
/**
 * @brief Allocate a nonblocking endpoint; equivalent to new_ex(out,NULL).
 *
 * @param[out] out Uninitialised pointer; set to the new endpoint on success.
 * @return COG_OK on success.
 * @return COG_ERROR_NULL_PTR if @p out is NULL.
 * @return COG_ERROR_NOMEM if allocation fails.
 *
 * @post On success, @p *out is a valid, unbound endpoint and the caller
 *       owns it.
 * @post On failure, @p *out is NULL and no resources are held.
 *
 * Threading: not thread-safe.
 */
cog_result_t cog_ipc_endpoint_new(cog_ipc_endpoint_t **out);

/**
 * @brief Destroy an IPC endpoint.
 *
 * If the endpoint is still connected or listening, it is disconnected
 * before being freed.
 *
 * @param[in,out] endpoint Endpoint to destroy; if NULL, no resources are
 *                         held and @c COG_ERROR_NULL_PTR is returned.
 * @return COG_OK on success.
 * @return COG_ERROR_NULL_PTR if @p endpoint is NULL.
 *
 * @post All storage owned by @p endpoint is released.  @p endpoint must
 *       not be used after this call.
 *
 * Threading: not thread-safe.  No other thread may use the endpoint
 * during or after destruction.
 */
cog_result_t cog_ipc_endpoint_free(cog_ipc_endpoint_t *endpoint);

/**
 * @brief Bind and listen on a Unix domain socket path.
 *
 * @param[in,out] endpoint Unbound endpoint created with
 *                         @c cog_ipc_endpoint_new.
 * @param[in]     path     Filesystem path for the socket.  Must be
 *                         non-NULL and suitable as a Unix domain socket
 *                         address.
 * @return COG_OK on success.
 * @return COG_ERROR_NULL_PTR if @p endpoint or @p path is NULL.
 * @return COG_ERROR_INVALID_STATE if @p endpoint is already bound,
 *         connected, or listening.
 * @return COG_ERROR_IO if the socket cannot be created, bound, or set
 *         to listen.
 *
 * @pre @p endpoint has been created with @c cog_ipc_endpoint_new and is
 *      not already associated with a socket.
 *
 * @post On success, @p endpoint is a listening endpoint and incoming
 *       connections can be accepted with @c cog_ipc_endpoint_accept.
 *
 * Threading: not thread-safe.
 */
cog_result_t cog_ipc_endpoint_listen(cog_ipc_endpoint_t *endpoint,
                                     const char *path);

/**
 * @brief Accept an incoming connection on a listening endpoint.
 *
 * @param[in,out] listener Listening endpoint.
 * @param[out]    client   Uninitialised pointer; set to a new connected
 *                         endpoint on success.
 * @param[out]    peer_id  Optional; if non-NULL, receives the identifier
 *                         assigned to the remote peer.
 * @return COG_OK on success.
 * @return COG_ERROR_NULL_PTR if @p listener or @p client is NULL.
 * @return COG_ERROR_INVALID_STATE if @p listener is not listening.
 * @return COG_ERROR_AGAIN if no connection is pending; never waits for a peer.
 * @return COG_ERROR_IO if accept fails; NOMEM if the explicit accepted-endpoint
 * constructor cannot reserve its inherited bounded framing workspaces.
 * @note Accepted endpoints may link intrusively into their listener's peer list;
 * acceptance adds no separate growing listener peer-table allocation. Caller
 * bounds admitted accepted endpoints and frees each one explicitly.
 *
 * @pre @p listener is listening.
 *
 * @post On success, @p *client is a connected endpoint owned by the caller
 *       and may be used with @c cog_ipc_endpoint_send,
 *       @c cog_ipc_endpoint_recv, and @c cog_ipc_endpoint_poll.
 *
 * Threading: not thread-safe.
 */
cog_result_t cog_ipc_endpoint_accept(cog_ipc_endpoint_t *listener,
                                     cog_ipc_endpoint_t **client,
                                     cog_peer_id_t *peer_id);

/**
 * @brief Connect to a listening Unix domain socket.
 *
 * @param[in,out] endpoint Unbound endpoint created with
 *                         @c cog_ipc_endpoint_new.
 * @param[in]     path     Path of the listener's socket.
 * @param[out]    peer_id  Optional; if non-NULL, receives the identifier
 *                         assigned to the local end of the connection.
 * @return COG_OK if connected immediately; AGAIN while one retained
 * nonblocking connect/identity handshake is pending (peer_id unchanged). Drive
 * endpoint_service, then get_peer_id; do not call connect again on that socket.
 * @return COG_ERROR_NULL_PTR if @p endpoint or @p path is NULL.
 * @return COG_ERROR_INVALID_STATE if @p endpoint is already associated
 *         with a socket.
 * @return COG_ERROR_IO if the connection cannot be established.
 *
 * @pre @p endpoint has been created with @c cog_ipc_endpoint_new and is
 *      not already associated with a socket.
 *
 * @post On success, @p endpoint is connected and may be used with
 *       @c cog_ipc_endpoint_send, @c cog_ipc_endpoint_recv, and
 *       @c cog_ipc_endpoint_poll.
 *
 * Threading: not thread-safe.
 */
cog_result_t cog_ipc_endpoint_connect(cog_ipc_endpoint_t *endpoint,
                                      const char *path,
                                      cog_peer_id_t *peer_id);

/**
 * @brief Send a framed message.
 *
 * @param[in,out] endpoint Connected endpoint.
 * @param[in]     msg      Message to send.  @p msg->data must be non-NULL
 *                         and @p msg->len must not exceed
 *                         @p msg->capacity.
 * @return COG_OK on success.
 * @return COG_ERROR_NULL_PTR if @p endpoint or @p msg is NULL.
 * @return COG_ERROR_INVALID_ARG if @p msg->data is NULL or
 *         @p msg->len > @p msg->capacity.
 * @return COG_ERROR_INVALID_STATE if @p endpoint is not connected.
 * @return COG_ERROR_IO if the send fails.
 * @return COG_ERROR_AGAIN if the constructor output queue is temporarily full;
 * no part of this new message is accepted. LIMIT if it exceeds max_frame_bytes.
 *
 * @pre @p endpoint is connected.
 *
 * @post OK copies one whole frame into reserved output storage exactly once;
 * caller may release msg immediately. endpoint_service flushes partial writes
 * from an internal cursor. Retrying send after OK would enqueue a second frame;
 * retry only AGAIN, which accepted nothing. No blocking peer I/O or growth.
 *
 * Threading: not thread-safe.
 */
cog_result_t cog_ipc_endpoint_send(cog_ipc_endpoint_t *endpoint,
                                   const cog_ipc_message_t *msg);

/**
 * @brief Receive a framed message.
 *
 * @param[in,out] endpoint Connected endpoint.
 * @param[in,out] msg      Message buffer.  On input, @p msg->data must
 *                         be non-NULL and @p msg->capacity must be large
 *                         enough to hold the next frame.  On output,
 *                         @p msg->len is set to the number of payload
 *                         bytes received.
 * @return COG_OK on success.
 * @return COG_ERROR_NULL_PTR if @p endpoint or @p msg is NULL.
 * @return COG_ERROR_INVALID_ARG if @p msg->data is NULL.
 * @return COG_ERROR_INVALID_STATE if @p endpoint is not connected.
 * @return COG_ERROR_IO if the receive fails or the remote closed the
 *         connection.
 * @return COG_ERROR_AGAIN if no complete retained frame is available. This
 * call never waits or drives peer I/O; service retains partial frame prefixes.
 * @return COG_ERROR_OVERFLOW if the next frame is larger than
 *         @p msg->capacity; only msg->len changes to required bytes. Metadata
 * and destination bytes remain unchanged; the SAME complete frame stays
 * retained across service for a larger-buffer retry.
 *
 * @pre @p endpoint is connected.
 *
 * @post On success, @p msg->len contains the received payload length.
 *
 * Threading: not thread-safe.
 */
cog_result_t cog_ipc_endpoint_recv(cog_ipc_endpoint_t *endpoint,
                                   cog_ipc_message_t *msg);

/**
 * @brief Poll an endpoint for I/O readiness.
 *
 * @param[in,out] endpoint    Endpoint to poll.
 * @param[in]     events      Bitmask of @c cog_ipc_poll_event_t values.
 * @param[in]     timeout_ms  Timeout in milliseconds; core service uses zero.
 * Legacy positive/indefinite waits are excluded from responsive service hooks.  A negative value
 *                            waits indefinitely; zero returns immediately.
 * @param[out]    revents     Optional; if non-NULL, receives the bitmask
 *                            of events that are ready.
 * @return COG_OK on success (one or more requested events are ready).
 * @return COG_ERROR_NULL_PTR if @p endpoint is NULL.
 * @return COG_ERROR_INVALID_ARG if @p events is zero.
 * @return COG_ERROR_AGAIN if the timeout expires before any requested
 *         event becomes ready.
 * @return COG_ERROR_IO if the poll itself fails.
 *
 * @pre @p endpoint is bound, listening, or connected.
 *
 * @post If @p revents is non-NULL, it contains the ready event bitmask.
 *
 * Threading: not thread-safe.
 */
cog_result_t cog_ipc_endpoint_poll(cog_ipc_endpoint_t *endpoint,
                                   int events,
                                   int timeout_ms,
                                   int *revents);

/**
 * @brief Close a connected or listening endpoint.
 *
 * @param[in,out] endpoint Endpoint to disconnect.
 * @return COG_OK on success.
 * @return COG_ERROR_NULL_PTR if @p endpoint is NULL.
 * @return COG_ERROR_INVALID_STATE if @p endpoint is not associated with
 *         a socket.
 *
 * @post The socket is closed and the endpoint returns to the unbound
 *       state.  It may be reused with @c cog_ipc_endpoint_listen or
 *       @c cog_ipc_endpoint_connect.
 *
 * Threading: not thread-safe.
 */
cog_result_t cog_ipc_endpoint_disconnect(cog_ipc_endpoint_t *endpoint);

/**
 * @brief Return the peer identifier associated with an endpoint.
 *
 * @param[in]  endpoint Endpoint to inspect.
 * @param[out] peer_id  Receives the peer identifier.
 * @return COG_OK on success.
 * @return COG_ERROR_NULL_PTR if @p endpoint or @p peer_id is NULL.
 * @return COG_ERROR_INVALID_STATE if @p endpoint has no peer identifier
 *         (e.g., it is unbound or listening).
 *
 * Threading: not thread-safe.
 */
cog_result_t cog_ipc_endpoint_get_peer_id(const cog_ipc_endpoint_t *endpoint,
                                          cog_peer_id_t *peer_id);

/**
 * @brief Query kernel credentials for an exact current connection without I/O.
 * @param[in] endpoint Listener/fabric, or one connected endpoint.
 * @param[in] peer_id Nonzero assigned link ID. Listener resolves that accepted
 * connection; connected endpoint requires its published matching link ID.
 * @param[out] info Copied metadata, unchanged on failure.
 * @return OK; NULL_PTR; INVALID_ARG zero; NOT_FOUND unknown/disconnected link;
 * AGAIN before handshake; INVALID_STATE unbound; IO credential-query failure;
 * UNSUPPORTED without authoritative native peer credentials. No allocation,
 * peer wait, send/recv or ownership transfer. Owner thread.
 * @note PID/UID alone does not prove a current worker attempt: apps also check
 * immutable unreaped spawn ownership and exact trusted bootstrap incarnation.
 * A reaped PID is never signalled/reused as authority; old socket/HELLO cannot
 * bind a replacement worker. Identity is scoped by endpoint/fabric as usual.
 */
cog_result_t cog_ipc_endpoint_peer_info(const cog_ipc_endpoint_t *endpoint,
                                         cog_peer_id_t peer_id,
                                         cog_ipc_peer_info_t *info);

/**
 * @brief Queue one copied frame to an exact accepted peer without a route.
 * @param[in,out] listener Owning listener/fabric, one owner thread.
 * @param[in] peer_id Current nonzero connection identity in this fabric.
 * @param[in] message Complete caller-owned frame.
 * @return OK copies exactly once; AGAIN full output queue accepts nothing;
 * NULL_PTR; INVALID_ARG; NOT_FOUND unknown/disconnected peer; INVALID_STATE
 * nonlistener/incomplete handshake; LIMIT above constructor frame bound.
 * No allocation/wait; existing endpoint_service cursor flushes partial bytes.
 * @note Grants no role/session/execution authority. Authenticate/bind before
 * private control/bootstrap. IDs never alias after disconnect in this listener
 * lifetime. Retry only AGAIN, never a send already accepted with OK.
 */
cog_result_t cog_ipc_send_to_peer(cog_ipc_endpoint_t *listener,
                                   cog_peer_id_t peer_id,
                                   const cog_ipc_message_t *message);

/* ── Broadcast ──────────────────────────────────────────────────── */

/**
 * @brief Send a message to all connected peers.
 *
 * @param[in,out] endpoint Endpoint to send from (must be a listener).
 * @param[in]     msg      Message to broadcast.
 * Admission is transactional across the current connected-peer set: reserve
 * every copied output entry before accepting any. AGAIN if any queue is full
 * accepts nothing; LIMIT for oversized payload. OK accepts one per peer, then
 * bounded endpoint_service flushes them. Do not repeat an accepted broadcast
 * after a later peer failure; per-peer effect/epoch IDs still deduplicate control.
 * @return COG_OK on success.
 * @return COG_ERROR_NULL_PTR if @p endpoint or @p msg is NULL.
 * @return COG_ERROR_INVALID_STATE if @p endpoint is not a listener.
 *
 * Threading: not thread-safe.
 */
cog_result_t cog_ipc_broadcast(cog_ipc_endpoint_t *endpoint,
                                const cog_ipc_message_t *msg);

/* ── Session routing ────────────────────────────────────────────── */

/**
 * @brief Register a session-to-peer mapping.
 *
 * Allows messages to be routed by session id rather than peer id.
 * Used for subagent-to-subagent and subagent-to-parent messaging.
 *
 * @param[in,out] endpoint   Endpoint (must be a listener).
 * @param[in]     session_id Session identifier; copied.
 * @param[in]     peer_id    Peer that hosts this session.
 * @return COG_OK on success.
 * @return COG_ERROR_NULL_PTR if @p endpoint or @p session_id is NULL.
 * @return COG_ERROR_INVALID_STATE if @p endpoint is not a listener.
 * @return COG_ERROR_OVERFLOW if the session table is full.
 *
 * Threading: not thread-safe.
 */
cog_result_t cog_ipc_register_session(cog_ipc_endpoint_t *endpoint,
                                       const char *session_id,
                                       cog_peer_id_t peer_id);

/**
 * @brief Send a message to the peer hosting a specific session.
 *
 * @param[in,out] endpoint   Endpoint to send from.
 * @param[in]     session_id Target session identifier.
 * @param[in]     msg        Message to send.
 * @return COG_OK on success.
 * @return COG_ERROR_NULL_PTR if @p endpoint, @p session_id, or @p msg
 *         is NULL.
 * @return COG_ERROR_NOT_FOUND if no peer hosts the given session.
 *
 * Threading: not thread-safe.
 */
cog_result_t cog_ipc_send_to_session(cog_ipc_endpoint_t *endpoint,
                                      const char *session_id,
                                      const cog_ipc_message_t *msg);

/**
 * @brief Remove a session-to-peer mapping.
 *
 * @param[in,out] endpoint   Endpoint.
 * @param[in]     session_id Session to unregister.
 * @return COG_OK on success.
 * @return COG_ERROR_NULL_PTR if @p endpoint or @p session_id is NULL.
 * @return COG_ERROR_NOT_FOUND if the session is not registered.
 *
 * Threading: not thread-safe.
 */
cog_result_t cog_ipc_unregister_session(cog_ipc_endpoint_t *endpoint,
                                         const char *session_id);

/** @brief Bounded application-owned per-session replay ring, no socket ownership. */
typedef struct cog_ipc_replay cog_ipc_replay_t;
/** @brief Versioned reconnect envelope; CURSOR is client acknowledgement/request. */
typedef enum cog_ipc_replay_kind {
    COG_IPC_REPLAY_DATA = 1,
    COG_IPC_REPLAY_GAP,
    COG_IPC_REPLAY_CURSOR
} cog_ipc_replay_kind_t;
/** @brief Replay envelope; input pointers borrowed, copied outputs caller-owned. */
typedef struct cog_ipc_replay_record {
    cog_ipc_replay_kind_t kind;
    const char *session_id;
    uint64_t generation; /**< Nonzero stream lifetime; never reused across restart. */
    uint64_t sequence; /**< DATA ID; CURSOR last rendered ID; GAP oldest-1. */
    uint64_t first_sequence; /**< Current retained window; empty: first=last=0. */
    uint64_t last_sequence;
    uint16_t type; /**< Application payload type; zero for GAP/CURSOR. */
    const void *payload;
    size_t payload_len;
} cog_ipc_replay_record_t;
/**
 * @brief Reserve one replay ring; never allocates while appending/copying.
 * @param[out] out Owned ring, NULL on failure; match replay_free.
 * @param[in] session_id Nonempty UTF-8 ID, copied, maximum 127 bytes.
 * @param[in] generation Host-selected nonzero lifetime ID, not reused after
 * restart; clients deduplicate by (session,generation,sequence), never sequence alone.
 * @param[in] max_entries Nonzero retained record bound.
 * @param[in] max_bytes Nonzero total copied payload bound.
 * @return OK; NULL_PTR; INVALID_ARG for bounds/arithmetic/ID; NOMEM.
 * @note One owner thread, no implicit dependency/global state or network I/O.
 */
cog_result_t cog_ipc_replay_new(cog_ipc_replay_t **out, const char *session_id,
                                uint64_t generation, size_t max_entries,
                                size_t max_bytes);
/** @brief Release constructor storage. @param[in,out] replay NULL is a no-op.
 * @note Owner thread; caller copies all views before release. */
void cog_ipc_replay_free(cog_ipc_replay_t *replay);
/**
 * @brief Copy one record and assign monotonic sequence starting at 1.
 * @param[in,out] replay Ring. @param[in] type Nonzero application type.
 * @param[in] payload Bytes; NULL iff payload_len=0. @param[in] payload_len Bytes.
 * @param[out] out_sequence Assigned sequence, unchanged on failure.
 * @return OK; NULL_PTR; INVALID_ARG; LIMIT if a single record exceeds max_bytes
 * or sequence would wrap. Failure changes nothing. On success evicts only as
 * many oldest records as needed; the window exposes a detectable replay gap.
 * @note No effects are executed by replay; dropping display records never
 * deletes durable session/ledger data or authorizes tool/request replay.
 */
cog_result_t cog_ipc_replay_append(cog_ipc_replay_t *replay, uint16_t type,
                                   const void *payload, size_t payload_len,
                                   uint64_t *out_sequence);
/**
 * @brief Copy next DATA or explicit GAP after a client cursor, no consumption.
 * @param[in] replay Ring. @param[in] generation Client's last known lifetime.
 * @param[in] after_sequence Last rendered DATA ID (0 for fresh matching lifetime).
 * @param[out] out_record Metadata unchanged except on OK; pointers use storage.
 * @param[out] storage Caller bytes for session ID+NUL and payload; NULL iff capacity=0.
 * @param[in] capacity Available bytes. @param[out] out_required Exact required bytes.
 * @return OK for DATA/GAP; AGAIN at current tail (required=0); OVERFLOW changes
 * required only; NULL_PTR; INVALID_ARG for cursor beyond matching generation tail.
 * Mismatched generation or evicted prefix returns GAP with current window,
 * never silently resumes as if data were contiguous. Copying does not mutate a
 * shared cursor; caller advances only after successful render/ack. No allocation.
 * @note Owner thread. GAP requires explicit full resynchronization/reload before
 * rendering further deltas; it never triggers provider/tool re-execution.
 */
cog_result_t cog_ipc_replay_copy_after(const cog_ipc_replay_t *replay,
                                       uint64_t generation, uint64_t after_sequence,
                                       cog_ipc_replay_record_t *out_record,
                                       void *storage, size_t capacity,
                                       size_t *out_required);
/**
 * @brief Encode a replay record for endpoint_send; never send native structs.
 * @param[in] record Valid record, strings/bytes borrowed until return.
 * @param[out] output Wire bytes; NULL iff capacity=0. @param[in] capacity Bytes.
 * @param[out] out_required Exact encoded size on OK/OVERFLOW.
 * @return OK; OVERFLOW (no writes); NULL_PTR; INVALID_ARG; LIMIT for uint32 length.
 * @note Version 1: magic CGR1, kind u8, generation/sequence/first/last u64,
 * type u16, session length/payload length u32, then exact session/payload bytes;
 * all integers big-endian, no padding/native pointers or session NUL on wire.
 * DATA requires 1<=first<=sequence<=last and nonzero type. GAP has no
 * payload/type, sequence=first-1 for nonempty window; empty GAP uses all three
 * sequence/first/last=0 (including generation mismatch before first append).
 * CURSOR has no payload/type, first=last=0 and sequence=last rendered ID.
 * Session ID nonempty UTF-8 <=127 bytes; generation nonzero for every kind.
 * Owner thread; no allocation, credentials or implicit transport activity.
 */
cog_result_t cog_ipc_replay_encode(const cog_ipc_replay_record_t *record,
                                   void *output, size_t capacity,
                                   size_t *out_required);
/**
 * @brief Decode one complete replay envelope transactionally into caller storage.
 * @param[in] input Wire bytes. @param[in] input_len Exact frame length.
 * @param[out] out_record Metadata. @param[out] storage Owned copied payload/ID.
 * @param[in] capacity Bytes. @param[out] out_required Exact storage on OK/OVERFLOW.
 * @return OK; OVERFLOW (required only); NULL_PTR; UNSUPPORTED version/kind;
 * PROTOCOL for invalid bounds/UTF-8/trailing bytes. Failure writes no outputs
 * except required on OVERFLOW. No borrowed input survives; no allocation.
 */
cog_result_t cog_ipc_replay_decode(const void *input, size_t input_len,
                                   cog_ipc_replay_record_t *out_record,
                                   void *storage, size_t capacity,
                                   size_t *out_required);

#ifdef __cplusplus
}
#endif

#endif /* COG_IPC_H */

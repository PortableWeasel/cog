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
 * Identifies a connected peer within the local process.  The value is
 * assigned by @c cog_ipc_endpoint_accept or @c cog_ipc_endpoint_connect
 * and is unique among peers of the calling process for the lifetime of
 * the connection.
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

/**
 * @brief Allocate a new IPC endpoint.
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
 * @param[in,out] endpoint Endpoint to destroy; may be NULL (no-op).
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
 * @return COG_ERROR_AGAIN if no connection is pending and the listener
 *         is non-blocking.
 * @return COG_ERROR_IO if accept fails.
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
 * @return COG_OK on success.
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
 * @return COG_ERROR_AGAIN if the operation would block and the endpoint
 *         is non-blocking.
 *
 * @pre @p endpoint is connected.
 *
 * @post On success, the message payload has been queued or transmitted.
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
 * @return COG_ERROR_AGAIN if no complete frame is available and the
 *         endpoint is non-blocking.
 * @return COG_ERROR_OVERFLOW if the next frame is larger than
 *         @p msg->capacity.
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
 * @param[in]     timeout_ms  Timeout in milliseconds.  A negative value
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

/* ── Broadcast ──────────────────────────────────────────────────── */

/**
 * @brief Send a message to all connected peers.
 *
 * @param[in,out] endpoint Endpoint to send from (must be a listener).
 * @param[in]     msg      Message to broadcast.
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

#ifdef __cplusplus
}
#endif

#endif /* COG_IPC_H */

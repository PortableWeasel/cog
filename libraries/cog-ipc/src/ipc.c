/**
 * @file ipc.c
 * @brief IPC fabric transport layer over Unix domain sockets.
 *
 * Implements the public contract declared in <cog/ipc.h>.  Each endpoint
 * wraps a Unix domain socket.  A listener endpoint accepts incoming
 * connections and tracks them for broadcast and session routing; a
 * connected endpoint sends and receives length-prefixed framed messages.
 *
 * Framing (matching the IPC fabric PoC):
 *
 *   +----------------------------+
 *   | payload length (uint32be)  |
 *   +----------------------------+
 *   | payload bytes              |
 *   +----------------------------+
 *
 * The length field is big-endian and capped at COG_IPC_MAX_PAYLOAD
 * (65535 bytes).  A zero length is a valid empty frame.
 *
 * Ownership model:
 *   - The caller owns every endpoint and message buffer.
 *   - A listener owns its peer table (pointers to accepted endpoints) and
 *     its session table.  Accepted endpoints keep a back-pointer to the
 *     listener so that disconnecting them removes them from the table.
 *   - The file descriptor of an accepted connection is owned by the
 *     accepted endpoint; the listener's peer table holds only a pointer.
 *   - When a listener is freed or disconnected it detaches its accepted
 *     endpoints (clears their back-pointer) without closing their fds.
 *
 * Threading: a single endpoint is not thread-safe.  Distinct endpoints
 * may be used by distinct threads without locking.
 */

#define _POSIX_C_SOURCE 200809L

#include <cog/ipc.h>

#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

/** Size of the frame header, in bytes. */
#define COG_IPC_HEADER_SIZE 4U

/** Maximum payload size in a single frame. */
#define COG_IPC_MAX_PAYLOAD ((size_t)UINT16_MAX)

/** Maximum number of pending connections for listen(2). */
#define COG_IPC_LISTEN_BACKLOG 8

/** Maximum number of session-to-peer mappings a listener can hold. */
#define COG_IPC_MAX_SESSIONS 64U

/** Maximum length of a session identifier, including the NUL terminator. */
#define COG_IPC_SESSION_ID_MAX 128U

/** Maximum length of a stored socket path, including the NUL terminator. */
#define COG_IPC_PATH_MAX 128U

/** Initial capacity of a listener's peer table. */
#define COG_IPC_PEERS_INITIAL 8U

/** A session-to-peer mapping held by a listener. */
struct ipc_session {
    char session_id[COG_IPC_SESSION_ID_MAX];
    cog_peer_id_t peer_id;
};

/**
 * @brief Opaque IPC endpoint.
 *
 * An endpoint is one of:
 *   - unbound (fd == -1),
 *   - listening (is_listener != 0),
 *   - connected (is_connected != 0), either via connect() or accept().
 *
 * A listener additionally owns a peer table (pointers to accepted
 * endpoints) and a session table.  An accepted endpoint holds a
 * back-pointer to its listener so it can detach itself on disconnect.
 */
struct cog_ipc_endpoint {
    int fd;                    /**< Socket fd, or -1 when unbound. */
    int is_listener;           /**< Non-zero when listening. */
    int is_connected;          /**< Non-zero when connected. */
    cog_peer_id_t peer_id;     /**< Assigned peer id, or COG_PEER_ID_INVALID. */
    cog_peer_id_t next_peer_id; /**< Next peer id to assign (listener only). */

    struct cog_ipc_endpoint *listener; /**< Owning listener, if accepted. */

    struct cog_ipc_endpoint **peers;   /**< Accepted endpoints (listener only). */
    size_t peer_count;                 /**< Number of accepted endpoints. */
    size_t peer_capacity;              /**< Allocated slots in @c peers. */

    struct ipc_session sessions[COG_IPC_MAX_SESSIONS]; /**< Session table. */
    size_t session_count;              /**< Number of registered sessions. */

    char path[COG_IPC_PATH_MAX];       /**< Bound socket path (listener only). */
};

/* ------------------------------------------------------------------ */
/* Framing helpers                                                     */
/* ------------------------------------------------------------------ */

/**
 * @brief Encode a payload length into a 4-byte big-endian header.
 *
 * @param[out] header 4-byte buffer to receive the encoded length.
 * @param[in]  len    Payload length; must be <= COG_IPC_MAX_PAYLOAD.
 */
static void
ipc_frame_encode_header(uint8_t header[COG_IPC_HEADER_SIZE], uint32_t len)
{
    header[0] = (uint8_t)((len >> 24) & 0xFFU);
    header[1] = (uint8_t)((len >> 16) & 0xFFU);
    header[2] = (uint8_t)((len >> 8) & 0xFFU);
    header[3] = (uint8_t)(len & 0xFFU);
}

/**
 * @brief Decode a 4-byte big-endian header into a host payload length.
 *
 * @param[in] header 4-byte buffer containing the encoded length.
 * @return Decoded payload length.
 */
static uint32_t
ipc_frame_decode_header(const uint8_t header[COG_IPC_HEADER_SIZE])
{
    uint32_t len = 0U;

    len |= ((uint32_t)header[0]) << 24;
    len |= ((uint32_t)header[1]) << 16;
    len |= ((uint32_t)header[2]) << 8;
    len |= ((uint32_t)header[3]);

    return len;
}

/**
 * @brief Read exactly @p n bytes from a file descriptor.
 *
 * Loops until @p n bytes are read, the peer closes the connection, or an
 * error occurs.  Handles partial reads and EINTR.
 *
 * @param[in]  fd  File descriptor to read from.
 * @param[out] buf Buffer to read into.
 * @param[in]  n   Number of bytes to read.
 * @return Number of bytes read, or -1 on error.  A value less than @p n
 *         indicates the peer closed the connection.
 */
static ssize_t
ipc_read_exact(int fd, void *buf, size_t n)
{
    size_t total = 0U;
    uint8_t *cursor = buf;

    while (total < n) {
        ssize_t rc = read(fd, cursor + total, n - total);

        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (rc == 0) {
            break;
        }
        total += (size_t)rc;
    }

    return (ssize_t)total;
}

/**
 * @brief Write exactly @p n bytes to a file descriptor.
 *
 * Loops until all bytes are written or an unrecoverable error occurs.
 * Handles partial writes and EINTR.
 *
 * @param[in] fd  File descriptor to write to.
 * @param[in] buf Buffer to write from.
 * @param[in] n   Number of bytes to write.
 * @return 0 on success, -1 on error, -2 if the write would block.
 */
static int
ipc_write_exact(int fd, const void *buf, size_t n)
{
    size_t total = 0U;
    const uint8_t *cursor = buf;

    while (total < n) {
        ssize_t rc = write(fd, cursor + total, n - total);

        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
#if EAGAIN != EWOULDBLOCK
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
#else
            if (errno == EAGAIN) {
#endif
                return -2;
            }
            return -1;
        }
        total += (size_t)rc;
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* Listener peer-table helpers                                         */
/* ------------------------------------------------------------------ */

/**
 * @brief Add an accepted endpoint to a listener's peer table.
 *
 * @param[in,out] listener Listener owning the table.
 * @param[in]     client   Accepted endpoint to add.
 * @return 0 on success, -1 on allocation failure.
 */
static int
ipc_peer_add(cog_ipc_endpoint_t *listener, cog_ipc_endpoint_t *client)
{
    if (listener->peer_count >= listener->peer_capacity) {
        size_t new_capacity = listener->peer_capacity == 0U
                                  ? COG_IPC_PEERS_INITIAL
                                  : listener->peer_capacity * 2U;
        cog_ipc_endpoint_t **new_peers =
            realloc(listener->peers, new_capacity * sizeof(*new_peers));

        if (new_peers == NULL) {
            return -1;
        }
        listener->peers = new_peers;
        listener->peer_capacity = new_capacity;
    }

    listener->peers[listener->peer_count] = client;
    listener->peer_count++;
    return 0;
}

/**
 * @brief Remove an accepted endpoint from a listener's peer table.
 *
 * @param[in,out] listener Listener owning the table.
 * @param[in]     client   Accepted endpoint to remove.
 */
static void
ipc_peer_remove(cog_ipc_endpoint_t *listener, cog_ipc_endpoint_t *client)
{
    size_t i;

    for (i = 0U; i < listener->peer_count; i++) {
        if (listener->peers[i] == client) {
            if (i < listener->peer_count - 1U) {
                (void)memmove(&listener->peers[i], &listener->peers[i + 1U],
                              (listener->peer_count - i - 1U) *
                                  sizeof(listener->peers[0]));
            }
            listener->peer_count--;
            return;
        }
    }
}

/**
 * @brief Find an accepted endpoint by peer id.
 *
 * @param[in] listener Listener owning the table.
 * @param[in] peer_id  Peer id to find.
 * @return The matching endpoint, or NULL if not found.
 */
static cog_ipc_endpoint_t *
ipc_peer_find_by_id(cog_ipc_endpoint_t *listener, cog_peer_id_t peer_id)
{
    size_t i;

    for (i = 0U; i < listener->peer_count; i++) {
        if (listener->peers[i]->peer_id == peer_id) {
            return listener->peers[i];
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Message buffers                                                     */
/* ------------------------------------------------------------------ */

cog_result_t
cog_ipc_message_new(cog_ipc_message_t *msg, size_t capacity)
{
    uint8_t *data;

    if (msg == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    if (capacity == 0U) {
        msg->data = NULL;
        msg->len = 0U;
        msg->capacity = 0U;
        return COG_ERROR_INVALID_ARG;
    }

    data = malloc(capacity);
    if (data == NULL) {
        msg->data = NULL;
        msg->len = 0U;
        msg->capacity = 0U;
        return COG_ERROR_NOMEM;
    }

    msg->data = data;
    msg->len = 0U;
    msg->capacity = capacity;
    return COG_OK;
}

cog_result_t
cog_ipc_message_free(cog_ipc_message_t *msg)
{
    if (msg == NULL) {
        return COG_OK;
    }

    free(msg->data);
    msg->data = NULL;
    msg->len = 0U;
    msg->capacity = 0U;
    return COG_OK;
}

/* ------------------------------------------------------------------ */
/* Endpoint lifecycle                                                  */
/* ------------------------------------------------------------------ */

cog_result_t
cog_ipc_endpoint_new(cog_ipc_endpoint_t **out)
{
    cog_ipc_endpoint_t *endpoint;

    if (out == NULL) {
        return COG_ERROR_NULL_PTR;
    }

    endpoint = calloc(1U, sizeof(*endpoint));
    if (endpoint == NULL) {
        *out = NULL;
        return COG_ERROR_NOMEM;
    }

    endpoint->fd = -1;
    endpoint->peer_id = COG_PEER_ID_INVALID;
    *out = endpoint;
    return COG_OK;
}

cog_result_t
cog_ipc_endpoint_free(cog_ipc_endpoint_t *endpoint)
{
    if (endpoint == NULL) {
        return COG_ERROR_NULL_PTR;
    }

    if (endpoint->fd >= 0) {
        (void)cog_ipc_endpoint_disconnect(endpoint);
    }

    free(endpoint->peers);
    free(endpoint);
    return COG_OK;
}

cog_result_t
cog_ipc_endpoint_disconnect(cog_ipc_endpoint_t *endpoint)
{
    size_t i;

    if (endpoint == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    if (endpoint->fd < 0) {
        return COG_ERROR_INVALID_STATE;
    }

    if (endpoint->is_listener) {
        /* Detach all accepted endpoints so they become independent. */
        for (i = 0U; i < endpoint->peer_count; i++) {
            endpoint->peers[i]->listener = NULL;
        }
        endpoint->peer_count = 0U;
        endpoint->session_count = 0U;

        if (endpoint->path[0] != '\0') {
            (void)unlink(endpoint->path);
            endpoint->path[0] = '\0';
        }
    } else if (endpoint->listener != NULL) {
        /* Remove this accepted endpoint from its listener's table. */
        ipc_peer_remove(endpoint->listener, endpoint);
        endpoint->listener = NULL;
    }

    (void)close(endpoint->fd);
    endpoint->fd = -1;
    endpoint->is_listener = 0;
    endpoint->is_connected = 0;
    endpoint->peer_id = COG_PEER_ID_INVALID;
    return COG_OK;
}

cog_result_t
cog_ipc_endpoint_listen(cog_ipc_endpoint_t *endpoint, const char *path)
{
    int fd;
    struct sockaddr_un addr;
    size_t path_len;

    if (endpoint == NULL || path == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    if (endpoint->fd >= 0) {
        return COG_ERROR_INVALID_STATE;
    }

    path_len = strlen(path);
    if (path_len >= sizeof(addr.sun_path)) {
        return COG_ERROR_IO;
    }

    (void)unlink(path);

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return COG_ERROR_IO;
    }

    (void)memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    (void)memcpy(addr.sun_path, path, path_len);

    if (bind(fd, (const struct sockaddr *)&addr, sizeof(addr)) < 0) {
        (void)close(fd);
        return COG_ERROR_IO;
    }
    if (listen(fd, COG_IPC_LISTEN_BACKLOG) < 0) {
        (void)close(fd);
        (void)unlink(path);
        return COG_ERROR_IO;
    }

    endpoint->fd = fd;
    endpoint->is_listener = 1;
    endpoint->next_peer_id = 1U;
    (void)strncpy(endpoint->path, path, sizeof(endpoint->path) - 1U);
    endpoint->path[sizeof(endpoint->path) - 1U] = '\0';
    return COG_OK;
}

cog_result_t
cog_ipc_endpoint_accept(cog_ipc_endpoint_t *listener,
                        cog_ipc_endpoint_t **client,
                        cog_peer_id_t *peer_id)
{
    int fd;
    cog_ipc_endpoint_t *c;

    if (listener == NULL || client == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    if (!listener->is_listener || listener->fd < 0) {
        return COG_ERROR_INVALID_STATE;
    }

    fd = accept(listener->fd, NULL, NULL);
    if (fd < 0) {
#if EAGAIN != EWOULDBLOCK
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
#else
        if (errno == EAGAIN) {
#endif
            return COG_ERROR_AGAIN;
        }
        return COG_ERROR_IO;
    }

    c = calloc(1U, sizeof(*c));
    if (c == NULL) {
        (void)close(fd);
        return COG_ERROR_NOMEM;
    }

    c->fd = fd;
    c->is_connected = 1;
    c->peer_id = listener->next_peer_id;
    listener->next_peer_id++;
    c->listener = listener;

    if (ipc_peer_add(listener, c) < 0) {
        (void)close(fd);
        free(c);
        return COG_ERROR_NOMEM;
    }

    *client = c;
    if (peer_id != NULL) {
        *peer_id = c->peer_id;
    }
    return COG_OK;
}

cog_result_t
cog_ipc_endpoint_connect(cog_ipc_endpoint_t *endpoint,
                         const char *path,
                         cog_peer_id_t *peer_id)
{
    int fd;
    struct sockaddr_un addr;
    size_t path_len;

    if (endpoint == NULL || path == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    if (endpoint->fd >= 0) {
        return COG_ERROR_INVALID_STATE;
    }

    path_len = strlen(path);
    if (path_len >= sizeof(addr.sun_path)) {
        return COG_ERROR_IO;
    }

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return COG_ERROR_IO;
    }

    (void)memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    (void)memcpy(addr.sun_path, path, path_len);

    if (connect(fd, (const struct sockaddr *)&addr, sizeof(addr)) < 0) {
        (void)close(fd);
        return COG_ERROR_IO;
    }

    endpoint->fd = fd;
    endpoint->is_connected = 1;
    endpoint->peer_id = 1U;
    if (peer_id != NULL) {
        *peer_id = endpoint->peer_id;
    }
    return COG_OK;
}

/* ------------------------------------------------------------------ */
/* Send / receive                                                      */
/* ------------------------------------------------------------------ */

cog_result_t
cog_ipc_endpoint_send(cog_ipc_endpoint_t *endpoint,
                      const cog_ipc_message_t *msg)
{
    uint8_t header[COG_IPC_HEADER_SIZE];
    int rc;

    if (endpoint == NULL || msg == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    if (msg->data == NULL || msg->len > msg->capacity) {
        return COG_ERROR_INVALID_ARG;
    }
    if (msg->len > COG_IPC_MAX_PAYLOAD) {
        return COG_ERROR_INVALID_ARG;
    }
    if (endpoint->fd < 0 || !endpoint->is_connected) {
        return COG_ERROR_INVALID_STATE;
    }

    ipc_frame_encode_header(header, (uint32_t)msg->len);

    rc = ipc_write_exact(endpoint->fd, header, COG_IPC_HEADER_SIZE);
    if (rc < 0) {
        return rc == -2 ? COG_ERROR_AGAIN : COG_ERROR_IO;
    }
    if (msg->len > 0U) {
        rc = ipc_write_exact(endpoint->fd, msg->data, msg->len);
        if (rc < 0) {
            return rc == -2 ? COG_ERROR_AGAIN : COG_ERROR_IO;
        }
    }
    return COG_OK;
}

cog_result_t
cog_ipc_endpoint_recv(cog_ipc_endpoint_t *endpoint, cog_ipc_message_t *msg)
{
    uint8_t header[COG_IPC_HEADER_SIZE];
    uint32_t payload_len;
    ssize_t rc;

    if (endpoint == NULL || msg == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    if (msg->data == NULL) {
        return COG_ERROR_INVALID_ARG;
    }
    if (endpoint->fd < 0 || !endpoint->is_connected) {
        return COG_ERROR_INVALID_STATE;
    }

    rc = ipc_read_exact(endpoint->fd, header, COG_IPC_HEADER_SIZE);
    if (rc < 0) {
        (void)cog_ipc_endpoint_disconnect(endpoint);
        return COG_ERROR_IO;
    }
    if (rc != (ssize_t)COG_IPC_HEADER_SIZE) {
        /* Peer closed mid-header. */
        (void)cog_ipc_endpoint_disconnect(endpoint);
        return COG_ERROR_IO;
    }

    payload_len = ipc_frame_decode_header(header);
    if (payload_len > COG_IPC_MAX_PAYLOAD ||
        (size_t)payload_len > msg->capacity) {
        /* Frame too large for the caller's buffer (or malformed).  Close
         * the connection to avoid desynchronising the stream. */
        (void)cog_ipc_endpoint_disconnect(endpoint);
        return COG_ERROR_OVERFLOW;
    }

    if (payload_len > 0U) {
        rc = ipc_read_exact(endpoint->fd, msg->data, (size_t)payload_len);
        if (rc < 0 || (size_t)rc != (size_t)payload_len) {
            (void)cog_ipc_endpoint_disconnect(endpoint);
            return COG_ERROR_IO;
        }
    }

    msg->len = (size_t)payload_len;
    return COG_OK;
}

/* ------------------------------------------------------------------ */
/* Poll                                                               */
/* ------------------------------------------------------------------ */

cog_result_t
cog_ipc_endpoint_poll(cog_ipc_endpoint_t *endpoint,
                      int events,
                      int timeout_ms,
                      int *revents)
{
    struct pollfd pfd;
    int rc;
    int out = 0;

    if (endpoint == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    if (events == 0) {
        return COG_ERROR_INVALID_ARG;
    }
    if (endpoint->fd < 0) {
        return COG_ERROR_INVALID_STATE;
    }

    pfd.fd = endpoint->fd;
    pfd.events = 0;
    if ((events & COG_IPC_POLL_IN) != 0) {
        pfd.events = (short)(pfd.events | POLLIN);
    }
    if ((events & COG_IPC_POLL_OUT) != 0) {
        pfd.events = (short)(pfd.events | POLLOUT);
    }
    if ((events & COG_IPC_POLL_ERR) != 0) {
        pfd.events = (short)(pfd.events | POLLERR);
    }
    pfd.revents = 0;

    rc = poll(&pfd, 1, timeout_ms);
    if (rc < 0) {
        if (errno == EINTR) {
            return COG_ERROR_AGAIN;
        }
        return COG_ERROR_IO;
    }
    if (rc == 0) {
        return COG_ERROR_AGAIN;
    }

    if ((pfd.revents & POLLIN) != 0) {
        out |= COG_IPC_POLL_IN;
    }
    if ((pfd.revents & POLLOUT) != 0) {
        out |= COG_IPC_POLL_OUT;
    }
    if ((pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
        out |= COG_IPC_POLL_ERR;
    }

    if (revents != NULL) {
        *revents = out;
    }
    return COG_OK;
}

cog_result_t
cog_ipc_endpoint_get_peer_id(const cog_ipc_endpoint_t *endpoint,
                             cog_peer_id_t *peer_id)
{
    if (endpoint == NULL || peer_id == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    if (endpoint->peer_id == COG_PEER_ID_INVALID) {
        return COG_ERROR_INVALID_STATE;
    }

    *peer_id = endpoint->peer_id;
    return COG_OK;
}

/* ------------------------------------------------------------------ */
/* Broadcast                                                           */
/* ------------------------------------------------------------------ */

cog_result_t
cog_ipc_broadcast(cog_ipc_endpoint_t *endpoint, const cog_ipc_message_t *msg)
{
    size_t i;

    if (endpoint == NULL || msg == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    if (!endpoint->is_listener) {
        return COG_ERROR_INVALID_STATE;
    }
    if (msg->data == NULL || msg->len > msg->capacity) {
        return COG_ERROR_INVALID_ARG;
    }
    if (msg->len > COG_IPC_MAX_PAYLOAD) {
        return COG_ERROR_INVALID_ARG;
    }

    /* Best-effort: send to every live peer, dropping dead ones. */
    i = 0U;
    while (i < endpoint->peer_count) {
        cog_ipc_endpoint_t *c = endpoint->peers[i];

        if (cog_ipc_endpoint_send(c, msg) == COG_OK) {
            i++;
        } else {
            ipc_peer_remove(endpoint, c);
        }
    }

    return COG_OK;
}

/* ------------------------------------------------------------------ */
/* Session routing                                                     */
/* ------------------------------------------------------------------ */

cog_result_t
cog_ipc_register_session(cog_ipc_endpoint_t *endpoint,
                         const char *session_id,
                         cog_peer_id_t peer_id)
{
    size_t i;
    size_t len;

    if (endpoint == NULL || session_id == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    if (!endpoint->is_listener) {
        return COG_ERROR_INVALID_STATE;
    }

    len = strlen(session_id);
    if (len >= COG_IPC_SESSION_ID_MAX) {
        return COG_ERROR_INVALID_ARG;
    }

    /* Re-registering an existing id updates its peer mapping. */
    for (i = 0U; i < endpoint->session_count; i++) {
        if (strcmp(endpoint->sessions[i].session_id, session_id) == 0) {
            endpoint->sessions[i].peer_id = peer_id;
            return COG_OK;
        }
    }

    if (endpoint->session_count >= COG_IPC_MAX_SESSIONS) {
        return COG_ERROR_OVERFLOW;
    }

    (void)strncpy(endpoint->sessions[endpoint->session_count].session_id,
                  session_id, COG_IPC_SESSION_ID_MAX - 1U);
    endpoint->sessions[endpoint->session_count]
        .session_id[COG_IPC_SESSION_ID_MAX - 1U] = '\0';
    endpoint->sessions[endpoint->session_count].peer_id = peer_id;
    endpoint->session_count++;
    return COG_OK;
}

cog_result_t
cog_ipc_send_to_session(cog_ipc_endpoint_t *endpoint,
                        const char *session_id,
                        const cog_ipc_message_t *msg)
{
    size_t i;
    cog_peer_id_t peer_id = COG_PEER_ID_INVALID;
    cog_ipc_endpoint_t *c;

    if (endpoint == NULL || session_id == NULL || msg == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    if (msg->data == NULL || msg->len > msg->capacity) {
        return COG_ERROR_INVALID_ARG;
    }
    if (msg->len > COG_IPC_MAX_PAYLOAD) {
        return COG_ERROR_INVALID_ARG;
    }

    for (i = 0U; i < endpoint->session_count; i++) {
        if (strcmp(endpoint->sessions[i].session_id, session_id) == 0) {
            peer_id = endpoint->sessions[i].peer_id;
            break;
        }
    }
    if (peer_id == COG_PEER_ID_INVALID) {
        return COG_ERROR_NOT_FOUND;
    }

    c = ipc_peer_find_by_id(endpoint, peer_id);
    if (c == NULL) {
        return COG_ERROR_IO;
    }

    return cog_ipc_endpoint_send(c, msg);
}

cog_result_t
cog_ipc_unregister_session(cog_ipc_endpoint_t *endpoint,
                           const char *session_id)
{
    size_t i;

    if (endpoint == NULL || session_id == NULL) {
        return COG_ERROR_NULL_PTR;
    }

    for (i = 0U; i < endpoint->session_count; i++) {
        if (strcmp(endpoint->sessions[i].session_id, session_id) == 0) {
            if (i < endpoint->session_count - 1U) {
                (void)memmove(&endpoint->sessions[i],
                              &endpoint->sessions[i + 1U],
                              (endpoint->session_count - i - 1U) *
                                  sizeof(endpoint->sessions[0]));
            }
            endpoint->session_count--;
            return COG_OK;
        }
    }

    return COG_ERROR_NOT_FOUND;
}

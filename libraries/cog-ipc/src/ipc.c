/** @file ipc.c
 * @brief Constructor-bounded nonblocking Unix stream framing.
 * Wire format: listener assigns an eight-byte big-endian connection identity,
 * followed in each direction by four-byte big-endian length/payload frames.
 * Every service unit performs at most one syscall or frame publication.
 */
#define _GNU_SOURCE
#include <cog/ipc.h>
#include "../private/core_ipc.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

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

static size_t ipc_stride(const cog_ipc_endpoint_t *ep)
{
    return ep->cfg.max_frame_bytes + 4U;
}
static uint32_t ipc_length(const unsigned char *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) |
           (uint32_t)p[3];
}
static void ipc_header(unsigned char *p, uint32_t n)
{
    p[0] = (unsigned char)(n >> 24);
    p[1] = (unsigned char)(n >> 16);
    p[2] = (unsigned char)(n >> 8);
    p[3] = (unsigned char)n;
}
static int ipc_nonblock(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0 ||
        fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) {
        return -1;
    }
    return 0;
}
static int ipc_transient(void)
{
#if EAGAIN != EWOULDBLOCK
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
#else
    return errno == EAGAIN || errno == EINTR;
#endif
}
static void ipc_unlink_peer(cog_ipc_endpoint_t *ep)
{
    cog_ipc_endpoint_t *parent = ep->parent;
    if (parent == NULL) {
        return;
    }
    if (ep->previous != NULL) {
        ep->previous->next = ep->next;
    } else {
        parent->peers = ep->next;
    }
    if (ep->next != NULL) {
        ep->next->previous = ep->previous;
    }
    if (parent->service_peer == ep) {
        parent->service_peer = ep->next;
    }
    ep->parent = NULL;
    ep->next = NULL;
    ep->previous = NULL;
}
static cog_result_t ipc_fail(cog_ipc_endpoint_t *ep, cog_result_t rc)
{
    if (ep->fd >= 0) {
        (void)close(ep->fd);
        ep->fd = -1;
    }
    /* Keep identity and retained complete frames until disconnect/free. */
    ep->failure = rc;
    ep->connecting = 0;
    ep->connect_pending = 0;
    return rc;
}

cog_result_t cog_ipc_message_new(cog_ipc_message_t *msg, size_t capacity)
{
    if (msg == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    memset(msg, 0, sizeof(*msg));
    if (capacity == 0U) {
        return COG_ERROR_INVALID_ARG;
    }
    msg->data = malloc(capacity);
    if (msg->data == NULL) {
        return COG_ERROR_NOMEM;
    }
    msg->capacity = capacity;
    return COG_OK;
}
cog_result_t cog_ipc_message_free(cog_ipc_message_t *msg)
{
    if (msg == NULL) {
        return COG_OK;
    }
    free(msg->data);
    memset(msg, 0, sizeof(*msg));
    return COG_OK;
}
cog_result_t cog_ipc_endpoint_new_ex(cog_ipc_endpoint_t **out,
                                     const cog_ipc_endpoint_config_t *config)
{
    cog_ipc_endpoint_config_t cfg = {0};
    cog_ipc_endpoint_t *ep;
    size_t stride;
    if (out == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    *out = NULL;
    if (config != NULL) {
        cfg = *config;
    }
    if (cfg.max_frame_bytes == 0U) {
        cfg.max_frame_bytes = IPC_DEFAULT_FRAME;
    }
    if (cfg.receive_frames == 0U) {
        cfg.receive_frames = 8U;
    }
    if (cfg.send_frames == 0U) {
        cfg.send_frames = 8U;
    }
    if (cfg.service_quantum == 0U) {
        cfg.service_quantum = 16U;
    }
    if (cfg.max_routes == 0U) {
        cfg.max_routes = 64U;
    }
    if (cfg.connect_timeout_ms == 0U) {
        cfg.connect_timeout_ms = 1000U;
    }
    if (cfg.max_frame_bytes > UINT32_MAX || cfg.max_frame_bytes > SIZE_MAX - 4U) {
        return COG_ERROR_INVALID_ARG;
    }
    stride = cfg.max_frame_bytes + 4U;
    if (cfg.receive_frames > SIZE_MAX / stride || cfg.send_frames > SIZE_MAX / stride ||
        cfg.receive_frames > SIZE_MAX / sizeof(ipc_frame_t) ||
        cfg.send_frames > SIZE_MAX / sizeof(ipc_frame_t) ||
        cfg.max_routes > SIZE_MAX / sizeof(ipc_route_t)) {
        return COG_ERROR_INVALID_ARG;
    }
    ep = calloc(1U, sizeof(*ep));
    if (ep == NULL) {
        return COG_ERROR_NOMEM;
    }
    ep->fd = -1;
    ep->cfg = cfg;
    ep->rx = calloc(cfg.receive_frames, sizeof(*ep->rx));
    ep->tx = calloc(cfg.send_frames, sizeof(*ep->tx));
    ep->rx_data = malloc(cfg.receive_frames * stride);
    ep->tx_data = malloc(cfg.send_frames * stride);
    ep->routes = calloc(cfg.max_routes, sizeof(*ep->routes));
    if (ep->rx == NULL || ep->tx == NULL || ep->rx_data == NULL ||
        ep->tx_data == NULL || ep->routes == NULL) {
        (void)cog_ipc_endpoint_free(ep);
        return COG_ERROR_NOMEM;
    }
    *out = ep;
    return COG_OK;
}
cog_result_t cog_ipc_endpoint_new(cog_ipc_endpoint_t **out)
{
    return cog_ipc_endpoint_new_ex(out, NULL);
}
cog_result_t cog_ipc_endpoint_disconnect(cog_ipc_endpoint_t *ep)
{
    cog_ipc_endpoint_t *p;
    if (ep == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    if (ep->fd < 0 && ep->failure == COG_OK) {
        return COG_ERROR_INVALID_STATE;
    }
    ipc_unlink_peer(ep);
    for (p = ep->peers; p != NULL;) {
        cog_ipc_endpoint_t *next = p->next;
        p->parent = NULL;
        p->next = NULL;
        p->previous = NULL;
        p = next;
    }
    if (ep->listener_role && ep->path[0] != '\0') {
        (void)unlink(ep->path);
    }
    if (ep->fd >= 0) {
        (void)close(ep->fd);
    }
    ep->fd = -1;
    ep->listener_role = 0;
    ep->connected = 0;
    ep->connecting = 0;
    ep->connect_pending = 0;
    ep->clock_set = 0;
    ep->failure = COG_OK;
    ep->peer_id = 0U;
    ep->peers = NULL;
    ep->service_peer = NULL;
    ep->route_count = 0U;
    ep->rx_head = 0U;
    ep->rx_count = 0U;
    ep->tx_head = 0U;
    ep->tx_count = 0U;
    ep->prefix_used = 0U;
    ep->rx_partial = 0U;
    ep->rx_length_known = 0;
    ep->identity_used = 0U;
    ep->path[0] = '\0';
    return COG_OK;
}
cog_result_t cog_ipc_endpoint_free(cog_ipc_endpoint_t *ep)
{
    if (ep == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    if (ep->fd >= 0 || ep->failure != COG_OK) {
        (void)cog_ipc_endpoint_disconnect(ep);
    }
    free(ep->rx);
    free(ep->tx);
    free(ep->rx_data);
    free(ep->tx_data);
    free(ep->routes);
    free(ep);
    return COG_OK;
}
static cog_result_t ipc_address(struct sockaddr_un *address, const char *path)
{
    size_t n = strlen(path);
    if (n == 0U || n >= sizeof(address->sun_path)) {
        return COG_ERROR_IO;
    }
    memset(address, 0, sizeof(*address));
    address->sun_family = AF_UNIX;
    memcpy(address->sun_path, path, n + 1U);
    return COG_OK;
}
cog_result_t cog_ipc_endpoint_listen(cog_ipc_endpoint_t *ep, const char *path)
{
    struct sockaddr_un address;
    int fd;
    cog_result_t rc;
    if (ep == NULL || path == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    if (ep->fd >= 0 || ep->failure != COG_OK) {
        return COG_ERROR_INVALID_STATE;
    }
    rc = ipc_address(&address, path);
    if (rc != COG_OK) {
        return rc;
    }
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return COG_ERROR_IO;
    }
    if (ipc_nonblock(fd) < 0 ||
        bind(fd, (const struct sockaddr *)&address, sizeof(address)) < 0) {
        (void)close(fd);
        return COG_ERROR_IO;
    }
    if (listen(fd, 16) < 0) {
        (void)close(fd);
        (void)unlink(path);
        return COG_ERROR_IO;
    }
    ep->fd = fd;
    ep->listener_role = 1;
    ep->next_peer = 1U;
    memcpy(ep->path, path, strlen(path) + 1U);
    return COG_OK;
}
cog_result_t cog_ipc_endpoint_accept(cog_ipc_endpoint_t *listener,
                                     cog_ipc_endpoint_t **client,
                                     cog_peer_id_t *peer_id)
{
    int fd;
    cog_ipc_endpoint_t *ep;
    cog_result_t rc;
    size_t i;
    if (listener == NULL || client == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    *client = NULL;
    if (!listener->listener_role || listener->fd < 0) {
        return COG_ERROR_INVALID_STATE;
    }
    if (listener->next_peer == 0U) {
        return COG_ERROR_LIMIT;
    }
    fd = accept(listener->fd, NULL, NULL);
    if (fd < 0) {
        return ipc_transient() ? COG_ERROR_AGAIN : COG_ERROR_IO;
    }
    if (ipc_nonblock(fd) < 0) {
        (void)close(fd);
        return COG_ERROR_IO;
    }
    rc = cog_ipc_endpoint_new_ex(&ep, &listener->cfg);
    if (rc != COG_OK) {
        (void)close(fd);
        return rc;
    }
    ep->fd = fd;
    ep->connected = 1;
    ep->peer_id = listener->next_peer++;
    for (i = 0U; i < IPC_ID_BYTES; ++i) {
        ep->identity[i] = (unsigned char)(ep->peer_id >> ((7U - i) * 8U));
    }
    ep->parent = listener;
    ep->next = listener->peers;
    if (ep->next != NULL) {
        ep->next->previous = ep;
    }
    listener->peers = ep;
    *client = ep;
    if (peer_id != NULL) {
        *peer_id = ep->peer_id;
    }
    return COG_OK;
}
cog_result_t cog_ipc_endpoint_connect(cog_ipc_endpoint_t *ep, const char *path,
                                      cog_peer_id_t *peer_id)
{
    struct sockaddr_un address;
    int fd, result;
    cog_result_t rc;
    (void)peer_id;
    if (ep == NULL || path == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    if (ep->fd >= 0 || ep->failure != COG_OK) {
        return COG_ERROR_INVALID_STATE;
    }
    rc = ipc_address(&address, path);
    if (rc != COG_OK) {
        return rc;
    }
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return COG_ERROR_IO;
    }
    if (ipc_nonblock(fd) < 0) {
        (void)close(fd);
        return COG_ERROR_IO;
    }
    result = connect(fd, (const struct sockaddr *)&address, sizeof(address));
    if (result < 0 && errno != EINPROGRESS) {
        (void)close(fd);
        return COG_ERROR_IO;
    }
    ep->fd = fd;
    ep->connecting = 1;
    ep->connect_pending = result < 0;
    return COG_ERROR_AGAIN;
}
static cog_result_t ipc_handshake(cog_ipc_endpoint_t *ep)
{
    ssize_t n;
    if (ep->connect_pending) {
        struct pollfd pfd = {ep->fd, POLLOUT, 0};
        int error = 0;
        socklen_t length = sizeof(error);
        int ready = poll(&pfd, 1U, 0);
        if (ready < 0) {
            return ipc_transient() ? COG_ERROR_AGAIN : ipc_fail(ep, COG_ERROR_IO);
        }
        if (ready == 0) {
            return COG_ERROR_AGAIN;
        }
        if (getsockopt(ep->fd, SOL_SOCKET, SO_ERROR, &error, &length) < 0 ||
            error != 0) {
            return ipc_fail(ep, COG_ERROR_IO);
        }
        ep->connect_pending = 0;
        return COG_OK;
    }
    if (ep->connecting) {
        n = recv(ep->fd, ep->identity + ep->identity_used,
                 IPC_ID_BYTES - ep->identity_used, 0);
    } else {
        n = send(ep->fd, ep->identity + ep->identity_used,
                 IPC_ID_BYTES - ep->identity_used, MSG_NOSIGNAL);
    }
    if (n < 0) {
        return ipc_transient() ? COG_ERROR_AGAIN : ipc_fail(ep, COG_ERROR_IO);
    }
    if (n == 0) {
        return ipc_fail(ep, COG_ERROR_IO);
    }
    ep->identity_used += (size_t)n;
    if (ep->connecting && ep->identity_used == IPC_ID_BYTES) {
        size_t i;
        for (i = 0U; i < IPC_ID_BYTES; ++i) {
            ep->peer_id = (ep->peer_id << 8) | ep->identity[i];
        }
        if (ep->peer_id == 0U) {
            return ipc_fail(ep, COG_ERROR_PROTOCOL);
        }
        ep->connecting = 0;
        ep->connected = 1;
    }
    return COG_OK;
}
static cog_result_t ipc_read_unit(cog_ipc_endpoint_t *ep)
{
    size_t slot, wanted;
    ssize_t n;
    unsigned char *destination;
    if (ep->rx_count == ep->cfg.receive_frames) {
        return COG_ERROR_AGAIN;
    }
    slot = (ep->rx_head + ep->rx_count) % ep->cfg.receive_frames;
    if (!ep->rx_length_known) {
        n = recv(ep->fd, ep->prefix + ep->prefix_used, 4U - ep->prefix_used, 0);
        if (n < 0) {
            return ipc_transient() ? COG_ERROR_AGAIN : ipc_fail(ep, COG_ERROR_IO);
        }
        if (n == 0) {
            return ipc_fail(ep,
                            ep->prefix_used != 0U ? COG_ERROR_PROTOCOL : COG_ERROR_IO);
        }
        ep->prefix_used += (size_t)n;
        if (ep->prefix_used != 4U) {
            return COG_OK;
        }
        ep->rx[slot].length = ipc_length(ep->prefix);
        if (ep->rx[slot].length > ep->cfg.max_frame_bytes) {
            return ipc_fail(ep, COG_ERROR_LIMIT);
        }
        ep->rx_length_known = 1;
    }
    if (ep->rx_partial == ep->rx[slot].length) {
        ++ep->rx_count;
        ep->prefix_used = 0U;
        ep->rx_partial = 0U;
        ep->rx_length_known = 0;
        return COG_OK;
    }
    /* Prefix read and payload read occupy distinct service units. */
    if (ep->prefix_used != 0U) {
        ep->prefix_used = 0U;
        return COG_OK;
    }
    wanted = ep->rx[slot].length - ep->rx_partial;
    if (wanted > IPC_IO_BYTES) {
        wanted = IPC_IO_BYTES;
    }
    destination = ep->rx_data + slot * ipc_stride(ep) + ep->rx_partial;
    n = recv(ep->fd, destination, wanted, 0);
    if (n < 0) {
        return ipc_transient() ? COG_ERROR_AGAIN : ipc_fail(ep, COG_ERROR_IO);
    }
    if (n == 0) {
        return ipc_fail(ep, COG_ERROR_PROTOCOL);
    }
    ep->rx_partial += (size_t)n;
    return COG_OK;
}
static cog_result_t ipc_write_unit(cog_ipc_endpoint_t *ep)
{
    ipc_frame_t *frame;
    size_t wanted;
    ssize_t n;
    if (ep->tx_count == 0U) {
        return COG_ERROR_AGAIN;
    }
    frame = &ep->tx[ep->tx_head];
    wanted = frame->length + 4U - frame->cursor;
    if (wanted > IPC_IO_BYTES) {
        wanted = IPC_IO_BYTES;
    }
    n = send(ep->fd, ep->tx_data + ep->tx_head * ipc_stride(ep) + frame->cursor, wanted,
             MSG_NOSIGNAL);
    if (n < 0) {
        return ipc_transient() ? COG_ERROR_AGAIN : ipc_fail(ep, COG_ERROR_IO);
    }
    if (n == 0) {
        return ipc_fail(ep, COG_ERROR_IO);
    }
    frame->cursor += (size_t)n;
    if (frame->cursor == frame->length + 4U) {
        ep->tx_head = (ep->tx_head + 1U) % ep->cfg.send_frames;
        --ep->tx_count;
    }
    return COG_OK;
}
static cog_result_t ipc_one_unit(cog_ipc_endpoint_t *ep, uint64_t now,
                                 uint64_t *deadline)
{
    cog_result_t rc;
    *deadline = UINT64_MAX;
    if (ep->clock_set && now < ep->last_now) {
        return COG_ERROR_INVALID_ARG;
    }
    ep->last_now = now;
    if (!ep->clock_set) {
        ep->clock_set = 1;
        ep->handshake_deadline = now > UINT64_MAX - ep->cfg.connect_timeout_ms
                                     ? UINT64_MAX
                                     : now + ep->cfg.connect_timeout_ms;
    }
    if (ep->failure != COG_OK) {
        return ep->failure;
    }
    if (ep->fd < 0) {
        return COG_ERROR_INVALID_STATE;
    }
    if (ep->identity_used < IPC_ID_BYTES) {
        *deadline = ep->handshake_deadline;
        if (now >= ep->handshake_deadline) {
            return ipc_fail(ep, COG_ERROR_IO);
        }
        rc = ipc_handshake(ep);
        if (ep->identity_used == IPC_ID_BYTES) {
            *deadline = UINT64_MAX;
        }
        return rc;
    }
    ++ep->direction;
    return (ep->direction & 1U) != 0U ? ipc_read_unit(ep) : ipc_write_unit(ep);
}
cog_result_t cog_ipc_endpoint_service(cog_ipc_endpoint_t *ep, uint64_t now_ms,
                                      uint64_t *out_deadline_ms)
{
    size_t i;
    int progress = 0;
    uint64_t minimum = UINT64_MAX;
    if (ep == NULL || out_deadline_ms == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    *out_deadline_ms = UINT64_MAX;
    if (ep->clock_set && now_ms < ep->last_now) {
        return COG_ERROR_INVALID_ARG;
    }
    ep->last_now = now_ms;
    ep->clock_set = 1;
    if (ep->failure != COG_OK) {
        return ep->failure;
    }
    if (ep->fd < 0) {
        return COG_ERROR_INVALID_STATE;
    }
    /* The first service clock anchors a pending identity timer. */
    if (!ep->listener_role && ep->handshake_deadline == 0U) {
        ep->handshake_deadline = now_ms > UINT64_MAX - ep->cfg.connect_timeout_ms
                                     ? UINT64_MAX
                                     : now_ms + ep->cfg.connect_timeout_ms;
    }
    for (i = 0U; i < ep->cfg.service_quantum; ++i) {
        cog_result_t rc;
        uint64_t deadline;
        cog_ipc_endpoint_t *target = ep;
        if (ep->listener_role) {
            if (ep->peers == NULL) {
                break;
            }
            target = ep->service_peer != NULL ? ep->service_peer : ep->peers;
            ep->service_peer = target->next != NULL ? target->next : ep->peers;
            /* Peer failures are reported through their own endpoint so a
             * slow/broken peer cannot fail the listener or unrelated peers. */
        }
        rc = ipc_one_unit(target, now_ms, &deadline);
        if (deadline < minimum) {
            minimum = deadline;
        }
        if (rc == COG_OK) {
            progress = 1;
        } else if (rc != COG_ERROR_AGAIN && !ep->listener_role) {
            return rc;
        }
    }
    /* Pending handshakes omitted by this quantum require immediate rotation. */
    if (ep->listener_role && ep->peers != NULL) {
        minimum = now_ms;
    }
    *out_deadline_ms = minimum;
    return progress ? COG_OK : COG_ERROR_AGAIN;
}
cog_result_t cog_ipc_endpoint_send(cog_ipc_endpoint_t *ep, const cog_ipc_message_t *msg)
{
    size_t slot;
    unsigned char *destination;
    if (ep == NULL || msg == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    if (msg->data == NULL || msg->len > msg->capacity) {
        return COG_ERROR_INVALID_ARG;
    }
    if (ep->failure != COG_OK) {
        return ep->failure;
    }
    if (!ep->connected || ep->fd < 0) {
        return COG_ERROR_INVALID_STATE;
    }
    if (msg->len > ep->cfg.max_frame_bytes) {
        return COG_ERROR_LIMIT;
    }
    if (ep->tx_count == ep->cfg.send_frames) {
        return COG_ERROR_AGAIN;
    }
    slot = (ep->tx_head + ep->tx_count) % ep->cfg.send_frames;
    destination = ep->tx_data + slot * ipc_stride(ep);
    ipc_header(destination, (uint32_t)msg->len);
    memcpy(destination + 4U, msg->data, msg->len);
    ep->tx[slot].length = msg->len;
    ep->tx[slot].cursor = 0U;
    ++ep->tx_count;
    return COG_OK;
}
cog_result_t cog_ipc_endpoint_recv(cog_ipc_endpoint_t *ep, cog_ipc_message_t *msg)
{
    size_t length;
    if (ep == NULL || msg == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    if (msg->data == NULL) {
        return COG_ERROR_INVALID_ARG;
    }
    if (!ep->connected) {
        return ep->failure != COG_OK ? ep->failure : COG_ERROR_INVALID_STATE;
    }
    if (ep->rx_count == 0U) {
        return ep->failure != COG_OK ? ep->failure : COG_ERROR_AGAIN;
    }
    length = ep->rx[ep->rx_head].length;
    msg->len = length;
    if (length > msg->capacity) {
        return COG_ERROR_OVERFLOW;
    }
    memcpy(msg->data, ep->rx_data + ep->rx_head * ipc_stride(ep), length);
    ep->rx_head = (ep->rx_head + 1U) % ep->cfg.receive_frames;
    --ep->rx_count;
    return COG_OK;
}
cog_result_t cog_ipc_endpoint_get_peer_id(const cog_ipc_endpoint_t *ep,
                                          cog_peer_id_t *peer_id)
{
    if (ep == NULL || peer_id == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    if (ep->peer_id == 0U) {
        return COG_ERROR_INVALID_STATE;
    }
    *peer_id = ep->peer_id;
    return COG_OK;
}
cog_result_t cog_ipc_endpoint_poll(cog_ipc_endpoint_t *ep, int events, int timeout_ms,
                                   int *revents)
{
    struct pollfd pfd;
    int rc, output = 0;
    if (ep == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    if (events == 0 ||
        (events & ~(COG_IPC_POLL_IN | COG_IPC_POLL_OUT | COG_IPC_POLL_ERR)) != 0) {
        return COG_ERROR_INVALID_ARG;
    }
    if (revents != NULL) {
        *revents = 0;
    }
    if (ep->failure != COG_OK) {
        if (revents != NULL) {
            *revents = COG_IPC_POLL_ERR;
        }
        return COG_OK;
    }
    if (ep->fd < 0) {
        return COG_ERROR_INVALID_STATE;
    }
    if ((events & COG_IPC_POLL_IN) != 0 && ep->rx_count != 0U) {
        output |= COG_IPC_POLL_IN;
        timeout_ms = 0;
    }
    pfd.fd = ep->fd;
    pfd.events = 0;
    pfd.revents = 0;
    if ((events & COG_IPC_POLL_IN) != 0) {
        pfd.events |= POLLIN;
    }
    if ((events & COG_IPC_POLL_OUT) != 0) {
        pfd.events |= POLLOUT;
    }
    rc = poll(&pfd, 1U, timeout_ms);
    if (rc < 0) {
        return ipc_transient() ? COG_ERROR_AGAIN : COG_ERROR_IO;
    }
    if ((pfd.revents & POLLIN) != 0) {
        output |= COG_IPC_POLL_IN;
    }
    if ((pfd.revents & POLLOUT) != 0) {
        output |= COG_IPC_POLL_OUT;
    }
    if ((pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
        output |= COG_IPC_POLL_ERR;
    }
    if (revents != NULL) {
        *revents = output;
    }
    return output != 0 ? COG_OK : COG_ERROR_AGAIN;
}
cog_result_t cog_ipc_broadcast(cog_ipc_endpoint_t *ep, const cog_ipc_message_t *msg)
{
    cog_ipc_endpoint_t *p;
    if (ep == NULL || msg == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    if (!ep->listener_role || ep->fd < 0) {
        return COG_ERROR_INVALID_STATE;
    }
    if (msg->data == NULL || msg->len > msg->capacity) {
        return COG_ERROR_INVALID_ARG;
    }
    if (msg->len > ep->cfg.max_frame_bytes) {
        return COG_ERROR_LIMIT;
    }
    for (p = ep->peers; p != NULL; p = p->next) {
        if (p->failure != COG_OK) {
            continue;
        }
        if (p->tx_count == p->cfg.send_frames) {
            return COG_ERROR_AGAIN;
        }
    }
    for (p = ep->peers; p != NULL; p = p->next) {
        if (p->failure == COG_OK) {
            (void)cog_ipc_endpoint_send(p, msg);
        }
    }
    return COG_OK;
}
cog_result_t cog_ipc_register_session(cog_ipc_endpoint_t *ep, const char *session_id,
                                      cog_peer_id_t peer_id)
{
    size_t i, n;
    if (ep == NULL || session_id == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    if (!ep->listener_role || ep->fd < 0) {
        return COG_ERROR_INVALID_STATE;
    }
    n = strnlen(session_id, IPC_SESSION_BYTES);
    if (n == 0U || n == IPC_SESSION_BYTES || peer_id == 0U) {
        return COG_ERROR_INVALID_ARG;
    }
    for (i = 0U; i < ep->route_count; ++i) {
        if (strcmp(ep->routes[i].id, session_id) == 0) {
            ep->routes[i].peer = peer_id;
            return COG_OK;
        }
    }
    if (ep->route_count == ep->cfg.max_routes) {
        return COG_ERROR_OVERFLOW;
    }
    memcpy(ep->routes[ep->route_count].id, session_id, n + 1U);
    ep->routes[ep->route_count++].peer = peer_id;
    return COG_OK;
}
cog_result_t cog_ipc_unregister_session(cog_ipc_endpoint_t *ep, const char *session_id)
{
    size_t i;
    if (ep == NULL || session_id == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    for (i = 0U; i < ep->route_count; ++i) {
        if (strcmp(ep->routes[i].id, session_id) == 0) {
            ep->routes[i] = ep->routes[--ep->route_count];
            return COG_OK;
        }
    }
    return COG_ERROR_NOT_FOUND;
}
cog_result_t cog_ipc_send_to_session(cog_ipc_endpoint_t *ep, const char *session_id,
                                     const cog_ipc_message_t *msg)
{
    size_t i;
    cog_ipc_endpoint_t *p;
    if (ep == NULL || session_id == NULL || msg == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    for (i = 0U; i < ep->route_count; ++i) {
        if (strcmp(ep->routes[i].id, session_id) != 0) {
            continue;
        }
        for (p = ep->peers; p != NULL; p = p->next) {
            if (p->peer_id == ep->routes[i].peer) {
                return cog_ipc_endpoint_send(p, msg);
            }
        }
        return COG_ERROR_IO;
    }
    return COG_ERROR_NOT_FOUND;
}

int cog_ipc_internal_role(const cog_ipc_endpoint_t *endpoint)
{
    if (endpoint == NULL || (endpoint->fd < 0 && endpoint->failure == COG_OK)) {
        return 0;
    }
    return endpoint->listener_role ? 1 : 2;
}

cog_result_t cog_ipc_internal_service_one(cog_ipc_endpoint_t *endpoint, uint64_t now_ms,
                                          uint64_t *deadline)
{
    if (endpoint == NULL || deadline == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    return ipc_one_unit(endpoint, now_ms, deadline);
}
cog_result_t cog_ipc_internal_refuse(cog_ipc_endpoint_t *listener)
{
    int fd;
    if (listener == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    if (!listener->listener_role || listener->fd < 0) {
        return COG_ERROR_INVALID_STATE;
    }
    fd = accept(listener->fd, NULL, NULL);
    if (fd < 0) {
        return ipc_transient() ? COG_ERROR_AGAIN : COG_ERROR_IO;
    }
    (void)close(fd);
    return COG_OK;
}

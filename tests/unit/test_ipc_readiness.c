/** @file test_ipc_readiness.c
 * @brief Read-only actual-socket readiness, buffered publication and error oracles.
 */
#define _GNU_SOURCE
#include <cog/ipc.h>
#include "../../libraries/cog-ipc/src/ipc_internal.h"
#include "../../libraries/cog-ipc/private/core_ipc.h"
#include <assert.h>
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int __real_poll(struct pollfd *fds, nfds_t count, int timeout);
int __wrap_poll(struct pollfd *fds, nfds_t count, int timeout);
int __real_connect(int fd, const struct sockaddr *address, socklen_t length);
int __wrap_connect(int fd, const struct sockaddr *address, socklen_t length);
ssize_t __real_send(int fd, const void *data, size_t length, int flags);
ssize_t __wrap_send(int fd, const void *data, size_t length, int flags);
static bool pending_connect;
static size_t send_ceiling;
int __wrap_connect(int fd, const struct sockaddr *address, socklen_t length)
{
    int rc = __real_connect(fd, address, length);
    if (rc == 0 && pending_connect) { errno = EINPROGRESS; return -1; }
    return rc;
}
ssize_t __wrap_send(int fd, const void *data, size_t length, int flags)
{
    if (send_ceiling != 0U && length > send_ceiling) length = send_ceiling;
    return __real_send(fd, data, length, flags);
}
static int poll_error;
static unsigned polls;
int __wrap_poll(struct pollfd *fds, nfds_t count, int timeout)
{
    ++polls;
    assert(timeout == 0);
    if (poll_error != 0) { errno = poll_error; return -1; }
    return __real_poll(fds, count, timeout);
}
static cog_result_t one(cog_ipc_endpoint_t *ep)
{
    uint64_t deadline;
    return cog_ipc_internal_service_one(ep, 1U, &deadline);
}
static void expect_at(cog_ipc_endpoint_t *ep, bool expected, bool uses_poll, int line)
{
    cog_ipc_endpoint_t before; memcpy(&before, ep, sizeof(before));
    unsigned char rx[136], tx[136];
    ipc_frame_t rxf[2], txf[2];
    memcpy(rx, ep->rx_data, sizeof(rx)); memcpy(tx, ep->tx_data, sizeof(tx));
    memcpy(rxf, ep->rx, sizeof(rxf)); memcpy(txf, ep->tx, sizeof(txf));
    unsigned prior = polls;
    bool actual = !expected;
    assert(cog_ipc_internal_ready(ep, &actual) == COG_OK);
    if (actual != expected || polls - prior != (uses_poll ? 1U : 0U))
        (void)fprintf(stderr, "readiness line %d: ready=%d expected=%d polls=%u expected=%d\n",
                      line, (int)actual, (int)expected, polls-prior, (int)uses_poll);
    assert(actual == expected && polls - prior == (uses_poll ? 1U : 0U));
    assert(memcmp(&before, ep, sizeof(before)) == 0);
    assert(memcmp(rx, ep->rx_data, sizeof(rx)) == 0);
    assert(memcmp(tx, ep->tx_data, sizeof(tx)) == 0);
    assert(memcmp(rxf, ep->rx, sizeof(rxf)) == 0);
    assert(memcmp(txf, ep->tx, sizeof(txf)) == 0);
}
#define expect(ep, value, poll_used) expect_at(ep, value, poll_used, __LINE__)
static void until_read_progress(cog_ipc_endpoint_t *ep)
{
    cog_result_t rc = one(ep);
    assert(rc == COG_OK || rc == COG_ERROR_AGAIN);
    if (rc == COG_ERROR_AGAIN) assert(one(ep) == COG_OK);
}
/* Independent critic: a queued frame alone is not kernel writability. A
 * saturated actual socket must allow a bounded host wait, then become ready
 * when the receiver frees capacity. Query never services either direction. */
static void blocked_transmit(void)
{
    cog_ipc_endpoint_config_t cfg = {.max_frame_bytes = 65536U, .receive_frames = 1U,
        .send_frames = 1U, .service_quantum = 1U};
    cog_ipc_endpoint_t *listener = NULL, *client = NULL, *accepted = NULL;
    char path[] = "/tmp/cog-ready-pressure-XXXXXX";
    int temporary = mkstemp(path);
    assert(temporary >= 0 && close(temporary) == 0 && unlink(path) == 0);
    assert(cog_ipc_endpoint_new_ex(&listener, &cfg) == COG_OK);
    assert(cog_ipc_endpoint_new_ex(&client, &cfg) == COG_OK);
    assert(cog_ipc_endpoint_listen(listener, path) == COG_OK);
    assert(cog_ipc_endpoint_connect(client, path, NULL) == COG_ERROR_AGAIN);
    assert(cog_ipc_endpoint_accept(listener, &accepted, NULL) == COG_OK);
    for (unsigned i = 0U; i < 20U &&
         (client->identity_used < IPC_ID_BYTES || accepted->identity_used < IPC_ID_BYTES); ++i) {
        assert(one(accepted) == COG_OK);
        assert(one(client) == COG_OK);
    }
    assert(client->identity_used == IPC_ID_BYTES && accepted->identity_used == IPC_ID_BYTES);
    int small = 1024;
    assert(setsockopt(client->fd, SOL_SOCKET, SO_SNDBUF, &small, sizeof(small)) == 0);
    unsigned char payload[65536]; memset(payload, 'p', sizeof(payload));
    cog_ipc_message_t frame = {payload, sizeof(payload), sizeof(payload)};
    assert(cog_ipc_endpoint_send(client, &frame) == COG_OK);
    bool blocked = false;
    for (unsigned i = 0U; i < 100U && !blocked; ++i) {
        cog_result_t rc = one(client);
        assert(rc == COG_OK || rc == COG_ERROR_AGAIN);
        blocked = rc == COG_ERROR_AGAIN && (client->direction & 1U) == 0U;
    }
    assert(blocked && client->tx_count == 1U);
    cog_ipc_endpoint_t before; memcpy(&before, client, sizeof(before));
    bool ready = true;
    assert(cog_ipc_internal_ready(client, &ready) == COG_OK && !ready);
    assert(memcmp(&before, client, sizeof(before)) == 0);
    for (unsigned i = 0U; i < 100U && !ready; ++i) {
        cog_result_t rc = one(accepted);
        assert(rc == COG_OK || rc == COG_ERROR_AGAIN);
        assert(cog_ipc_internal_ready(client, &ready) == COG_OK);
    }
    assert(ready && client->tx_count == 1U);
    /* Invalid native descriptor is actionable, not a query-side mutation. */
    assert(close(accepted->fd) == 0);
    memcpy(&before, accepted, sizeof(before)); ready = false;
    assert(cog_ipc_internal_ready(accepted, &ready) == COG_OK && ready);
    assert(memcmp(&before, accepted, sizeof(before)) == 0);
    assert(cog_ipc_endpoint_free(accepted) == COG_OK);
    assert(cog_ipc_endpoint_free(client) == COG_OK);
    assert(cog_ipc_endpoint_free(listener) == COG_OK);
}
int main(void)
{
    blocked_transmit();
    cog_ipc_endpoint_config_t cfg = {0};
    cfg.max_frame_bytes = 64U; cfg.receive_frames = 2U; cfg.send_frames = 2U;
    cfg.wipe_buffers = true;
    cog_ipc_endpoint_t *listener, *client, *accepted;
    assert(cog_ipc_endpoint_new_ex(&listener, &cfg) == COG_OK);
    assert(cog_ipc_endpoint_new_ex(&client, &cfg) == COG_OK);
    bool ready = true;
    assert(cog_ipc_internal_ready(NULL, &ready) == COG_ERROR_NULL_PTR && ready);
    assert(cog_ipc_internal_ready(listener, NULL) == COG_ERROR_NULL_PTR);
    assert(cog_ipc_internal_ready(listener, &ready) == COG_ERROR_INVALID_STATE && ready);
    char path[] = "/tmp/cog-ipc-ready-XXXXXX";
    int temporary = mkstemp(path); assert(temporary >= 0);
    assert(close(temporary) == 0 && unlink(path) == 0);
    assert(cog_ipc_endpoint_listen(listener, path) == COG_OK);
    expect(listener, false, true);
    pending_connect = true;
    assert(cog_ipc_endpoint_connect(client, path, NULL) == COG_ERROR_AGAIN);
    pending_connect = false;
    expect(listener, true, true);
    assert(cog_ipc_endpoint_accept(listener, &accepted, NULL) == COG_OK);
    expect(listener, false, true);
    expect(client, true, true); /* actual connected fd, pending SO_ERROR step */
    assert(one(client) == COG_OK);
    expect(client, true, false); /* native capture local unit */
    assert(one(client) == COG_OK);
    expect(client, false, true); /* no identity bytes yet */
    send_ceiling = 2U;
    for (unsigned i = 0U; i < 4U; ++i) {
        expect(accepted, true, true); /* remaining identity sender bytes */
        assert(one(accepted) == COG_OK);
        expect(client, true, true);
        assert(one(client) == COG_OK);
        if (i != 3U) expect(client, false, true);
    }
    send_ceiling = 0U;
    assert(client->identity_used == IPC_ID_BYTES);
    cog_ipc_peer_info_t proof;
    assert(cog_ipc_endpoint_peer_info(accepted, accepted->peer_id, &proof) == COG_OK);
    expect(client, false, true); expect(accepted, false, true);
    unsigned char data[] = "payload";
    cog_ipc_message_t msg = {data, sizeof(data), sizeof(data)};
    assert(cog_ipc_endpoint_send(accepted, &msg) == COG_OK);
    expect(accepted, true, true); /* writable only because TX is queued */
    assert(one(accepted) == COG_ERROR_AGAIN);
    assert(one(accepted) == COG_OK);
    expect(accepted, false, true);
    expect(client, true, true); /* actual frame prefix */
    until_read_progress(client);
    assert(client->rx_length_known && client->rx_partial == 0U);
    assert(client->prefix_used == 0U); /* bookkeeping retired in prefix unit */
    expect(client, true, true); /* actual payload waiting */
    until_read_progress(client);
    assert(client->rx_partial == sizeof(data) && client->rx_count == 0U);
    expect(client, true, false); /* full payload awaits publication */
    until_read_progress(client);
    assert(client->rx_count == 1U);
    expect(client, true, false);
    unsigned char output[64]; cog_ipc_message_t got = {output,0U,sizeof(output)};
    assert(cog_ipc_endpoint_recv(client, &got) == COG_OK);
    assert(memcmp(output, data, sizeof(data)) == 0);
    expect(client, false, true);

    /* Incomplete prefix/payload alone is not ready without more kernel bytes. */
    const unsigned char prefix[] = {0U,0U,0U,3U};
    assert(write(accepted->fd, prefix, 2U) == 2);
    expect(client, true, true); until_read_progress(client);
    assert(client->prefix_used == 2U); expect(client, false, true);
    assert(write(accepted->fd, prefix + 2U, 2U) == 2);
    until_read_progress(client); expect(client, false, true);
    assert(write(accepted->fd, "abc", 1U) == 1);
    until_read_progress(client); assert(client->rx_partial == 1U);
    expect(client, false, true);
    assert(write(accepted->fd, "bc", 2U) == 2);
    until_read_progress(client); expect(client, true, false);
    until_read_progress(client); expect(client, true, false);

    /* Zero-length publication is still visible while alternating past TX. */
    const unsigned char empty[] = {0U,0U,0U,0U};
    assert(write(accepted->fd, empty, sizeof(empty)) == (ssize_t)sizeof(empty));
    until_read_progress(client);
    /* Current read_unit publishes zero-length frames in its prefix unit. */
    assert(client->rx_count == 2U); expect(client, true, false);
    assert(cog_ipc_endpoint_recv(client, &got) == COG_OK && got.len == 3U);
    expect(client, true, false);
    assert(cog_ipc_endpoint_recv(client, &got) == COG_OK && got.len == 0U);
    expect(client, false, true);

    /* Query errors never overwrite the caller's value or mutate the endpoint. */
    const int errors[] = {EIO, EINTR, EAGAIN};
    for (size_t i = 0U; i < sizeof(errors)/sizeof(errors[0]); ++i) {
        cog_ipc_endpoint_t before; memcpy(&before, client, sizeof(before));
        poll_error = errors[i]; ready = (i & 1U) != 0U;
        bool initial = ready;
        assert(cog_ipc_internal_ready(client, &ready) ==
               (i == 0U ? COG_ERROR_IO : COG_ERROR_AGAIN));
        assert(ready == initial && memcmp(&before, client, sizeof(before)) == 0);
    }
    poll_error = 0;
    assert(shutdown(accepted->fd, SHUT_WR) == 0);
    expect(client, true, true);
    for (unsigned i = 0U; i < 4U && client->failure == COG_OK; ++i) (void)one(client);
    assert(client->failure == COG_ERROR_IO); expect(client, true, false);
    assert(cog_ipc_endpoint_disconnect(client) == COG_OK);
    ready = false;
    assert(cog_ipc_internal_ready(client, &ready) == COG_ERROR_INVALID_STATE && !ready);
    expect(listener, false, true); /* accepted child does not imply new accept */
    assert(cog_ipc_endpoint_free(client) == COG_OK);
    assert(cog_ipc_endpoint_free(accepted) == COG_OK);
    assert(cog_ipc_endpoint_free(listener) == COG_OK);
    puts("IPC readiness truth table and nonmutation PASS");
    return 0;
}

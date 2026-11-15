/** @file test_ipc_nonblocking.c
 * @brief Bounded partial framing, queue ownership, and transactional admission.
 */
#define _POSIX_C_SOURCE 200809L
#include <cog/ipc.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

typedef struct fixture {
    char path[108];
    cog_ipc_endpoint_t *listener, *client, *accepted;
} fixture_t;
static void path_new(char path[108])
{
    int fd;
    memcpy(path, "/tmp/cog-q-ipc-XXXXXX", 22U);
    fd = mkstemp(path);
    assert(fd >= 0);
    assert(close(fd) == 0);
    assert(unlink(path) == 0);
}
static void tick(cog_ipc_endpoint_t *ep)
{
    uint64_t deadline;
    cog_result_t rc = cog_ipc_endpoint_service(ep, 100U, &deadline);
    assert(rc == COG_OK || rc == COG_ERROR_AGAIN);
}
static void pair(fixture_t *f, size_t capacity)
{
    cog_ipc_endpoint_config_t cfg = {0};
    cog_peer_id_t accepted, client = 99U;
    cfg.max_frame_bytes = capacity;
    cfg.receive_frames = 2U;
    cfg.send_frames = 2U;
    cfg.service_quantum = 4U;
    memset(f, 0, sizeof(*f));
    path_new(f->path);
    assert(cog_ipc_endpoint_new_ex(&f->listener, &cfg) == COG_OK);
    assert(cog_ipc_endpoint_new_ex(&f->client, &cfg) == COG_OK);
    assert(cog_ipc_endpoint_listen(f->listener, f->path) == COG_OK);
    assert(cog_ipc_endpoint_connect(f->client, f->path, &client) == COG_ERROR_AGAIN);
    assert(client == 99U);
    assert(cog_ipc_endpoint_accept(f->listener, &f->accepted, &accepted) == COG_OK);
    tick(f->accepted);
    tick(f->client);
    assert(cog_ipc_endpoint_get_peer_id(f->client, &client) == COG_OK);
    assert(client == accepted);
}
static void pair_free(fixture_t *f)
{
    assert(cog_ipc_endpoint_free(f->accepted) == COG_OK);
    assert(cog_ipc_endpoint_free(f->client) == COG_OK);
    assert(cog_ipc_endpoint_free(f->listener) == COG_OK);
}
static int raw_client(const char *path)
{
    struct sockaddr_un address;
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    assert(fd >= 0);
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path, path, strlen(path) + 1U);
    assert(connect(fd, (const struct sockaddr *)&address, sizeof(address)) == 0);
    return fd;
}
static void raw_pair(fixture_t *f, int *fd)
{
    unsigned char identity[8];
    cog_ipc_endpoint_config_t cfg = {0};
    cfg.max_frame_bytes = 16U;
    cfg.service_quantum = 1U;
    cfg.receive_frames = 1U;
    memset(f, 0, sizeof(*f));
    path_new(f->path);
    assert(cog_ipc_endpoint_new_ex(&f->listener, &cfg) == COG_OK);
    assert(cog_ipc_endpoint_listen(f->listener, f->path) == COG_OK);
    *fd = raw_client(f->path);
    assert(cog_ipc_endpoint_accept(f->listener, &f->accepted, NULL) == COG_OK);
    tick(f->accepted);
    assert(read(*fd, identity, sizeof(identity)) == 8);
}
static void fragmented(void)
{
    fixture_t f;
    int fd;
    unsigned char bytes[] = {0U, 0U, 0U, 5U, 'h', 'e', 'l', 'l', 'o'};
    unsigned char output[8] = {0};
    cog_ipc_message_t msg = {output, 77U, 8U};
    size_t i;
    uint64_t deadline;
    raw_pair(&f, &fd);
    for (i = 0U; i < sizeof(bytes); ++i) {
        assert(write(fd, bytes + i, 1U) == 1);
        tick(f.accepted);
        tick(f.accepted);
        assert(cog_ipc_endpoint_recv(f.accepted, &msg) == COG_ERROR_AGAIN);
    }
    tick(f.accepted);
    tick(f.accepted);
    msg.capacity = 2U;
    assert(cog_ipc_endpoint_recv(f.accepted, &msg) == COG_ERROR_OVERFLOW);
    assert(msg.len == 5U);
    assert(output[0] == 0U);
    for (i = 0U; i < 5U; ++i) {
        tick(f.accepted);
    }
    assert(cog_ipc_endpoint_recv(f.accepted, &msg) == COG_ERROR_OVERFLOW);
    msg.capacity = sizeof(output);
    assert(cog_ipc_endpoint_recv(f.accepted, &msg) == COG_OK);
    assert(memcmp(output, "hello", 5U) == 0);
    assert(cog_ipc_endpoint_service(f.accepted, 99U, &deadline) ==
           COG_ERROR_INVALID_ARG);
    assert(close(fd) == 0);
    assert(cog_ipc_endpoint_free(f.accepted) == COG_OK);
    assert(cog_ipc_endpoint_free(f.listener) == COG_OK);
}
static void copied_queues(void)
{
    fixture_t f;
    unsigned char text[] = "first";
    unsigned char output[32];
    cog_ipc_message_t send = {text, 5U, sizeof(text)},
                      recv = {output, 0U, sizeof(output)};
    size_t i;
    pair(&f, 16U);
    assert(cog_ipc_endpoint_send(f.client, &send) == COG_OK);
    text[0] = 'F';
    assert(cog_ipc_endpoint_send(f.client, &send) == COG_OK);
    assert(cog_ipc_endpoint_send(f.client, &send) == COG_ERROR_AGAIN);
    assert(cog_ipc_endpoint_recv(f.accepted, &recv) == COG_ERROR_AGAIN);
    for (i = 0U; i < 20U; ++i) {
        tick(f.client);
        tick(f.accepted);
    }
    assert(cog_ipc_endpoint_recv(f.accepted, &recv) == COG_OK);
    assert(memcmp(output, "first", 5U) == 0);
    assert(cog_ipc_endpoint_recv(f.accepted, &recv) == COG_OK);
    assert(memcmp(output, "First", 5U) == 0);
    assert(cog_ipc_endpoint_recv(f.accepted, &recv) == COG_ERROR_AGAIN);
    send.len = 17U;
    send.capacity = 17U;
    assert(cog_ipc_endpoint_send(f.client, &send) == COG_ERROR_LIMIT);
    pair_free(&f);
}
static void frame_failures(void)
{
    fixture_t f;
    int fd;
    unsigned char prefix[] = {0U, 0U, 0U, 17U};
    uint64_t deadline;
    cog_result_t rc;
    raw_pair(&f, &fd);
    assert(write(fd, prefix, 4U) == 4);
    rc = cog_ipc_endpoint_service(f.accepted, 100U, &deadline);
    assert(rc == COG_ERROR_LIMIT);
    assert(close(fd) == 0);
    assert(cog_ipc_endpoint_free(f.accepted) == COG_OK);
    assert(cog_ipc_endpoint_free(f.listener) == COG_OK);
    raw_pair(&f, &fd);
    assert(write(fd, prefix, 2U) == 2);
    tick(f.accepted);
    assert(close(fd) == 0);
    tick(f.accepted);
    rc = cog_ipc_endpoint_service(f.accepted, 100U, &deadline);
    assert(rc == COG_ERROR_PROTOCOL);
    assert(cog_ipc_endpoint_free(f.accepted) == COG_OK);
    assert(cog_ipc_endpoint_free(f.listener) == COG_OK);
}
static void broadcast_atomic(void)
{
    fixture_t f;
    cog_ipc_endpoint_t *second, *other;
    unsigned char payload[] = "B", output[4];
    cog_ipc_message_t msg = {payload, 1U, sizeof(payload)},
                      recv = {output, 0U, sizeof(output)};
    size_t i;
    pair(&f, 16U);
    assert(cog_ipc_endpoint_new(&second) == COG_OK);
    assert(cog_ipc_endpoint_connect(second, f.path, NULL) == COG_ERROR_AGAIN);
    assert(cog_ipc_endpoint_accept(f.listener, &other, NULL) == COG_OK);
    tick(other);
    tick(second);
    assert(cog_ipc_endpoint_send(f.accepted, &msg) == COG_OK);
    assert(cog_ipc_endpoint_send(f.accepted, &msg) == COG_OK);
    assert(cog_ipc_broadcast(f.listener, &msg) == COG_ERROR_AGAIN);
    for (i = 0U; i < 20U; ++i) {
        tick(other);
        tick(second);
    }
    assert(cog_ipc_endpoint_recv(second, &recv) == COG_ERROR_AGAIN);
    for (i = 0U; i < 20U; ++i) {
        tick(f.accepted);
        tick(f.client);
    }
    assert(cog_ipc_endpoint_recv(f.client, &recv) == COG_OK);
    assert(cog_ipc_endpoint_recv(f.client, &recv) == COG_OK);
    assert(cog_ipc_broadcast(f.listener, &msg) == COG_OK);
    for (i = 0U; i < 20U; ++i) {
        tick(f.listener);
        tick(f.client);
        tick(second);
    }
    assert(cog_ipc_endpoint_recv(second, &recv) == COG_OK);
    assert(cog_ipc_endpoint_recv(f.client, &recv) == COG_OK);
    assert(cog_ipc_endpoint_free(other) == COG_OK);
    assert(cog_ipc_endpoint_free(second) == COG_OK);
    pair_free(&f);
}
static void handshake_timeout(void)
{
    fixture_t f;
    uint64_t deadline;
    cog_ipc_endpoint_config_t cfg = {0};
    cog_peer_id_t id = 55U;
    memset(&f, 0, sizeof(f));
    path_new(f.path);
    cfg.connect_timeout_ms = 3U;
    assert(cog_ipc_endpoint_new(&f.listener) == COG_OK);
    assert(cog_ipc_endpoint_new_ex(&f.client, &cfg) == COG_OK);
    assert(cog_ipc_endpoint_listen(f.listener, f.path) == COG_OK);
    assert(cog_ipc_endpoint_connect(f.client, f.path, &id) == COG_ERROR_AGAIN);
    assert(id == 55U);
    assert(cog_ipc_endpoint_service(f.client, 50U, &deadline) == COG_ERROR_AGAIN);
    assert(deadline == 53U);
    assert(cog_ipc_endpoint_service(f.client, 53U, &deadline) == COG_ERROR_IO);
    assert(cog_ipc_endpoint_free(f.client) == COG_OK);
    assert(cog_ipc_endpoint_free(f.listener) == COG_OK);
}
static void slow_peer(void)
{
    fixture_t f;
    unsigned char *source = malloc(1048576U), *output = malloc(1048576U);
    cog_ipc_message_t send = {source, 1048576U, 1048576U},
                      recv = {output, 0U, 1048576U};
    size_t i;
    cog_result_t rc = COG_ERROR_AGAIN;
    assert(source != NULL && output != NULL);
    for (i = 0U; i < 1048576U; ++i) {
        source[i] = (unsigned char)(i % 251U);
    }
    pair(&f, 1048576U);
    assert(cog_ipc_endpoint_send(f.client, &send) == COG_OK);
    memset(source, 0xCC, 1048576U); /* Accepted output owns the original copy. */
    for (i = 0U; i < 100U; ++i) {
        tick(f.client);
    } /* Receiver deliberately idle. */
    for (i = 0U; i < 2048U; ++i) {
        tick(f.client);
        tick(f.accepted);
        rc = cog_ipc_endpoint_recv(f.accepted, &recv);
        if (rc == COG_OK) {
            break;
        }
        assert(rc == COG_ERROR_AGAIN);
    }
    assert(rc == COG_OK && recv.len == 1048576U);
    for (i = 0U; i < recv.len; ++i) {
        assert(output[i] == (unsigned char)(i % 251U));
    }
    send.len = 1U;
    source[0] = 'Z';
    assert(cog_ipc_endpoint_send(f.client, &send) == COG_OK);
    for (i = 0U; i < 20U; ++i) {
        tick(f.client);
        tick(f.accepted);
    }
    assert(cog_ipc_endpoint_recv(f.accepted, &recv) == COG_OK);
    assert(recv.len == 1U && output[0] == 'Z');
    pair_free(&f);
    free(source);
    free(output);
}
static void constructor_limits(void)
{
    cog_ipc_endpoint_t *ep = (cog_ipc_endpoint_t *)(uintptr_t)1U;
    cog_ipc_endpoint_config_t cfg = {0};
    cfg.receive_frames = SIZE_MAX;
    assert(cog_ipc_endpoint_new_ex(&ep, &cfg) == COG_ERROR_INVALID_ARG);
    assert(ep == NULL);
    cfg.receive_frames = 0U;
    cfg.max_frame_bytes = SIZE_MAX;
    assert(cog_ipc_endpoint_new_ex(&ep, &cfg) == COG_ERROR_INVALID_ARG);
    assert(ep == NULL);
}
static void independent_fabrics(void)
{
    fixture_t a, b;
    cog_peer_id_t a_id, b_id;
    unsigned char source[] = "A", output[4];
    cog_ipc_message_t send = {source, 1U, sizeof(source)},
                      recv = {output, 0U, sizeof(output)};
    size_t i;
    pair(&a, 16U);
    pair(&b, 16U);
    assert(cog_ipc_endpoint_get_peer_id(a.accepted, &a_id) == COG_OK);
    assert(cog_ipc_endpoint_get_peer_id(b.accepted, &b_id) == COG_OK);
    assert(a_id == 1U && b_id == 1U);
    assert(cog_ipc_register_session(a.listener, "same-label", a_id) == COG_OK);
    assert(cog_ipc_register_session(b.listener, "same-label", b_id) == COG_OK);
    assert(cog_ipc_send_to_session(a.listener, "same-label", &send) == COG_OK);
    source[0] = 'B';
    assert(cog_ipc_send_to_session(b.listener, "same-label", &send) == COG_OK);
    for (i = 0U; i < 20U; ++i) {
        tick(a.listener);
        tick(b.listener);
        tick(a.client);
        tick(b.client);
    }
    assert(cog_ipc_endpoint_recv(a.client, &recv) == COG_OK && output[0] == 'A');
    assert(cog_ipc_endpoint_recv(b.client, &recv) == COG_OK && output[0] == 'B');
    assert(cog_ipc_endpoint_recv(a.client, &recv) == COG_ERROR_AGAIN);
    assert(cog_ipc_endpoint_recv(b.client, &recv) == COG_ERROR_AGAIN);
    pair_free(&a);
    pair_free(&b);
}
static void reconnect_resets_handshake_timer(void)
{
    fixture_t f;
    uint64_t deadline;
    cog_result_t rc;
    pair(&f, 16U); /* First clock is 100, first deadline is 1100. */
    assert(cog_ipc_endpoint_disconnect(f.client) == COG_OK);
    assert(cog_ipc_endpoint_free(f.accepted) == COG_OK);
    assert(cog_ipc_endpoint_connect(f.client, f.path, NULL) == COG_ERROR_AGAIN);
    assert(cog_ipc_endpoint_accept(f.listener, &f.accepted, NULL) == COG_OK);
    rc = cog_ipc_endpoint_service(f.accepted, 2000U, &deadline);
    assert(rc == COG_OK || rc == COG_ERROR_AGAIN);
    rc = cog_ipc_endpoint_service(f.client, 2000U, &deadline);
    assert(rc == COG_OK || rc == COG_ERROR_AGAIN);
    pair_free(&f);
}
static void reconnect_after_failed_handshake(void)
{
    fixture_t f = {0};
    cog_ipc_endpoint_config_t cfg = {0};
    uint64_t deadline;
    cog_peer_id_t id;
    cog_result_t rc;
    cfg.connect_timeout_ms = 3U;
    path_new(f.path);
    assert(cog_ipc_endpoint_new_ex(&f.listener, &cfg) == COG_OK);
    assert(cog_ipc_endpoint_listen(f.listener, f.path) == COG_OK);
    assert(cog_ipc_endpoint_new_ex(&f.client, &cfg) == COG_OK);
    assert(cog_ipc_endpoint_connect(f.client, f.path, NULL) == COG_ERROR_AGAIN);
    assert(cog_ipc_endpoint_service(f.client, 5000U, &deadline) == COG_ERROR_AGAIN);
    assert(deadline == 5003U);
    assert(cog_ipc_endpoint_service(f.client, 5003U, &deadline) == COG_ERROR_IO);
    /* Drain the old connection and start a new lifetime with an independent,
     * earlier clock origin. Neither old clock nor expired timer may leak. */
    assert(cog_ipc_endpoint_accept(f.listener, &f.accepted, NULL) == COG_OK);
    assert(cog_ipc_endpoint_free(f.accepted) == COG_OK);
    assert(cog_ipc_endpoint_disconnect(f.client) == COG_OK);
    assert(cog_ipc_endpoint_connect(f.client, f.path, NULL) == COG_ERROR_AGAIN);
    assert(cog_ipc_endpoint_service(f.client, 10U, &deadline) == COG_ERROR_AGAIN);
    assert(deadline == 13U);
    assert(cog_ipc_endpoint_accept(f.listener, &f.accepted, NULL) == COG_OK);
    rc = cog_ipc_endpoint_service(f.accepted, 10U, &deadline);
    assert(rc == COG_OK || rc == COG_ERROR_AGAIN);
    rc = cog_ipc_endpoint_service(f.client, 11U, &deadline);
    assert(rc == COG_OK || rc == COG_ERROR_AGAIN);
    assert(cog_ipc_endpoint_get_peer_id(f.client, &id) == COG_OK && id != 0U);
    assert(deadline == UINT64_MAX);
    pair_free(&f);
}
int main(void)
{
    reconnect_after_failed_handshake();
    reconnect_resets_handshake_timer();
    independent_fabrics();
    fragmented();
    copied_queues();
    frame_failures();
    broadcast_atomic();
    handshake_timeout();
    slow_peer();
    constructor_limits();
    puts("nonblocking IPC tests passed");
    return 0;
}

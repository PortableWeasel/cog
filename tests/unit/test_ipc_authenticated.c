/** @file test_ipc_authenticated.c
 * @brief Actual kernel provenance, directed queues and observable secret retirement.
 */
#define _GNU_SOURCE
#include <cog/ipc.h>
#include "../../libraries/cog-ipc/src/ipc_internal.h"
#include "../../libraries/cog-ipc/private/core_ipc.h"
#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

int __real_getsockopt(int fd, int level, int option, void *value, socklen_t *length);
int __wrap_getsockopt(int fd, int level, int option, void *value, socklen_t *length);
void *__real_malloc(size_t size);
void *__wrap_malloc(size_t size);
void *__real_calloc(size_t count, size_t size);
void *__wrap_calloc(size_t count, size_t size);
void __real_free(void *pointer);
void __wrap_free(void *pointer);
ssize_t __real_send(int fd, const void *data, size_t size, int flags);
ssize_t __wrap_send(int fd, const void *data, size_t size, int flags);

static unsigned credential_mode, credential_calls, allocation_calls, fail_at;
static int track_allocations, verify_frees, send_error;
static size_t short_send;
static struct { void *pointer; size_t size; } payloads[8];
static size_t payload_count, wiped_frees;
static int zero(const void *data, size_t size)
{
    const unsigned char *p = data;
    for (size_t i = 0; i < size; ++i) if (p[i] != 0U) return 0;
    return 1;
}
int __wrap_getsockopt(int fd, int level, int option, void *value, socklen_t *length)
{
    if (level == SOL_SOCKET && option == SO_PEERCRED) {
        ++credential_calls;
        if (credential_mode == 1U) { errno = EIO; return -1; }
        int rc = __real_getsockopt(fd, level, option, value, length);
        if (rc == 0 && credential_mode == 2U) --*length;
        if (rc == 0 && credential_mode == 3U) ((struct ucred *)value)->pid = 0;
        return rc;
    }
    return __real_getsockopt(fd, level, option, value, length);
}
void *__wrap_malloc(size_t size)
{
    if (track_allocations && ++allocation_calls == fail_at) return NULL;
    void *p = __real_malloc(size);
    if (track_allocations && p != NULL) {
        assert(payload_count < 8U);
        payloads[payload_count].pointer = p;
        payloads[payload_count++].size = size;
        memset(p, 0xa5, size); /* Never-used capacity must also be wiped. */
    }
    return p;
}
void *__wrap_calloc(size_t count, size_t size)
{
    if (track_allocations && ++allocation_calls == fail_at) return NULL;
    return __real_calloc(count, size);
}
void __wrap_free(void *pointer)
{
    for (size_t i = 0; i < payload_count; ++i) {
        if (payloads[i].pointer == pointer && pointer != NULL) {
            if (verify_frees) { assert(zero(pointer, payloads[i].size)); ++wiped_frees; }
            payloads[i].pointer = NULL;
        }
    }
    __real_free(pointer);
}
ssize_t __wrap_send(int fd, const void *data, size_t size, int flags)
{
    if (send_error != 0) { errno = send_error; return -1; }
    if (short_send != 0U && size > short_send) size = short_send;
    return __real_send(fd, data, size, flags);
}

typedef struct fixture {
    char path[108];
    cog_ipc_endpoint_t *listener, *client, *accepted;
    cog_peer_id_t id;
} fixture_t;
static cog_ipc_endpoint_config_t config(bool wipe)
{
    cog_ipc_endpoint_config_t c = {0};
    c.max_frame_bytes = 64U; c.receive_frames = 2U; c.send_frames = 2U;
    c.service_quantum = 16U; c.wipe_buffers = wipe;
    return c;
}
static void listen_new(fixture_t *f, bool wipe)
{
    memset(f, 0, sizeof(*f));
    (void)snprintf(f->path, sizeof(f->path), "/tmp/cog-ipc-auth-XXXXXX");
    int fd = mkstemp(f->path); assert(fd >= 0);
    assert(close(fd) == 0); assert(unlink(f->path) == 0);
    cog_ipc_endpoint_config_t c = config(wipe);
    assert(cog_ipc_endpoint_new_ex(&f->listener, &c) == COG_OK);
    assert(cog_ipc_endpoint_listen(f->listener, f->path) == COG_OK);
}
static cog_result_t one(cog_ipc_endpoint_t *ep)
{
    uint64_t deadline;
    return cog_ipc_internal_service_one(ep, 100U, &deadline);
}
static void tick(cog_ipc_endpoint_t *ep)
{
    uint64_t deadline;
    cog_result_t rc = cog_ipc_endpoint_service(ep, 100U, &deadline);
    assert(rc == COG_OK || rc == COG_ERROR_AGAIN);
}
static void connect_new(fixture_t *f, bool wipe)
{
    cog_ipc_endpoint_config_t c = config(wipe);
    assert(cog_ipc_endpoint_new_ex(&f->client, &c) == COG_OK);
    assert(cog_ipc_endpoint_connect(f->client, f->path, NULL) == COG_ERROR_AGAIN);
    assert(cog_ipc_endpoint_accept(f->listener, &f->accepted, &f->id) == COG_OK);
}
static void ready(fixture_t *f)
{
    for (unsigned i = 0; i < 32U; ++i) { tick(f->accepted); tick(f->client); }
    assert(f->accepted->identity_used == IPC_ID_BYTES);
    assert(f->client->identity_used == IPC_ID_BYTES);
}
static void done(fixture_t *f)
{
    assert(cog_ipc_endpoint_free(f->accepted) == COG_OK);
    assert(cog_ipc_endpoint_free(f->client) == COG_OK);
    assert(cog_ipc_endpoint_free(f->listener) == COG_OK);
}
static void unchanged(cog_ipc_endpoint_t *ep, cog_peer_id_t id, cog_result_t expected)
{
    cog_ipc_peer_info_t info, before;
    memset(&info, 0xa5, sizeof(info)); memcpy(&before, &info, sizeof(info));
    assert(cog_ipc_endpoint_peer_info(ep, id, &info) == expected);
    assert(memcmp(&info, &before, sizeof(info)) == 0);
}
static void identity_and_directed(void)
{
    fixture_t f, other;
    listen_new(&f, true); connect_new(&f, true);
    unsigned char secret[] = "secret-wire-PID=999999";
    cog_ipc_message_t msg = {secret, sizeof(secret), sizeof(secret)};
    unchanged(NULL, 1U, COG_ERROR_NULL_PTR);
    unchanged(f.listener, 0U, COG_ERROR_INVALID_ARG);
    unchanged(f.listener, f.id, COG_ERROR_AGAIN);
    unchanged(f.client, f.id, COG_ERROR_AGAIN);
    unchanged(f.listener, f.id + 1U, COG_ERROR_NOT_FOUND);
    assert(cog_ipc_send_to_peer(f.listener, f.id, &msg) == COG_ERROR_INVALID_STATE);
    ready(&f);
    cog_ipc_peer_info_t proof;
    unsigned calls = credential_calls;
    assert(cog_ipc_endpoint_peer_info(f.listener, f.id, &proof) == COG_OK);
    assert(proof.peer_id == f.id && proof.process_id == (uint64_t)getpid());
    assert(proof.user_id == (uint64_t)getuid() && proof.group_id == (uint64_t)getgid());
    assert(cog_ipc_endpoint_peer_info(f.client, f.id, &proof) == COG_OK);
    assert(credential_calls == calls); /* public query performs no native syscall */
    unchanged(f.client, f.id + 1U, COG_ERROR_NOT_FOUND);
    listen_new(&other, true);
    unchanged(other.listener, f.id, COG_ERROR_NOT_FOUND);
    assert(cog_ipc_send_to_peer(other.listener, f.id, &msg) == COG_ERROR_NOT_FOUND);
    assert(cog_ipc_endpoint_free(other.listener) == COG_OK);
    fixture_t sibling; memset(&sibling, 0, sizeof(sibling));
    sibling.listener = f.listener;
    memcpy(sibling.path, f.path, sizeof(sibling.path));
    connect_new(&sibling, true); ready(&sibling);
    assert(sibling.id != f.id);
    assert(cog_ipc_send_to_peer(NULL, f.id, &msg) == COG_ERROR_NULL_PTR);
    assert(cog_ipc_send_to_peer(f.listener, 0U, &msg) == COG_ERROR_INVALID_ARG);
    assert(cog_ipc_send_to_peer(f.client, f.id, &msg) == COG_ERROR_INVALID_STATE);
    cog_ipc_message_t bad = {secret, 65U, 65U};
    assert(cog_ipc_send_to_peer(f.listener, f.id, &bad) == COG_ERROR_LIMIT);
    assert(cog_ipc_send_to_peer(f.listener, f.id, &msg) == COG_OK);
    assert(cog_ipc_send_to_peer(f.listener, f.id, &msg) == COG_OK);
    unsigned char saved[136]; memcpy(saved, f.accepted->tx_data, sizeof(saved));
    assert(cog_ipc_send_to_peer(f.listener, f.id, &msg) == COG_ERROR_AGAIN);
    assert(memcmp(saved, f.accepted->tx_data, sizeof(saved)) == 0);
    memset(secret, 'x', sizeof(secret));
    short_send = 1U;
    for (unsigned i = 0; i < 2U && f.accepted->tx[0].cursor == 0U; ++i) {
        cog_result_t rc = one(f.accepted);
        assert(rc == COG_OK || rc == COG_ERROR_AGAIN);
    }
    assert(f.accepted->tx[0].cursor == 1U && !zero(f.accepted->tx_data, 68U));
    short_send = 0U;
    for (unsigned i = 0; i < 32U; ++i) { tick(f.accepted); tick(f.client); }
    assert(zero(f.accepted->tx_data, 136U));
    tick(sibling.client);
    unsigned char unrelated = 0U;
    cog_ipc_message_t absent = {&unrelated, 0U, 1U};
    assert(cog_ipc_endpoint_recv(sibling.client, &absent) == COG_ERROR_AGAIN);
    assert(cog_ipc_endpoint_free(sibling.accepted) == COG_OK);
    assert(cog_ipc_endpoint_free(sibling.client) == COG_OK);
    unsigned char output[64]; cog_ipc_message_t got = {output, 0U, sizeof(output)};
    for (unsigned i = 0; i < 2U; ++i) {
        assert(cog_ipc_endpoint_recv(f.client, &got) == COG_OK);
        assert(got.len == sizeof(secret));
        assert(memcmp(output, "secret-wire-PID=999999", sizeof(secret)) == 0);
    }
    assert(zero(f.client->rx_data, 136U));
    assert(cog_ipc_endpoint_peer_info(f.accepted, f.id, &proof) == COG_OK);
    assert(proof.process_id == (uint64_t)getpid());
    cog_peer_id_t old = f.id;
    assert(cog_ipc_endpoint_disconnect(f.accepted) == COG_OK);
    unchanged(f.listener, old, COG_ERROR_NOT_FOUND);
    unchanged(f.accepted, old, COG_ERROR_INVALID_STATE);
    assert(cog_ipc_send_to_peer(f.listener, old, &msg) == COG_ERROR_NOT_FOUND);
    assert(cog_ipc_endpoint_free(f.accepted) == COG_OK);
    assert(cog_ipc_endpoint_free(f.client) == COG_OK);
    connect_new(&f, true); assert(f.id > old); ready(&f); done(&f);
}
static void native_failures(void)
{
    for (unsigned mode = 1U; mode <= 3U; ++mode) {
        fixture_t f; listen_new(&f, true); credential_mode = mode;
        connect_new(&f, true); ready(&f); credential_mode = 0U;
        unchanged(f.listener, f.id, COG_ERROR_IO);
        unchanged(f.client, f.id, COG_ERROR_IO);
        unsigned char x = 42U; cog_ipc_message_t msg = {&x, 1U, 1U};
        assert(cog_ipc_send_to_peer(f.listener, f.id, &msg) == COG_OK);
        done(&f);
    }
}
static void retained_and_abandoned(bool tx_failure, bool full)
{
    fixture_t f; listen_new(&f, true); connect_new(&f, true);
    /* Private one-unit handshake is the proof capture barrier. */
    assert(one(f.accepted) == COG_OK);
    assert(f.accepted->identity_used == IPC_ID_BYTES && f.accepted->rx_count == 0U);
    cog_ipc_peer_info_t proof;
    assert(cog_ipc_endpoint_peer_info(f.accepted, f.id, &proof) == COG_OK);
    ready(&f);
    const unsigned char frame[] = {0U,0U,0U,3U,'k','e','y'};
    assert(write(f.client->fd, frame, sizeof(frame)) == (ssize_t)sizeof(frame));
    if (full) assert(write(f.client->fd, frame, sizeof(frame)) == (ssize_t)sizeof(frame));
    else {
        const unsigned char partial[] = {0U,0U,0U,5U,'b','a'};
        assert(write(f.client->fd, partial, sizeof(partial)) == (ssize_t)sizeof(partial));
    }
    for (unsigned i = 0; i < 32U; ++i) tick(f.accepted);
    assert(f.accepted->rx_count == (full ? 2U : 1U));
    if (!full) assert(f.accepted->rx_partial == 2U);
    unsigned char tx[] = "pending secret"; cog_ipc_message_t msg = {tx,sizeof(tx),sizeof(tx)};
    assert(cog_ipc_endpoint_send(f.accepted, &msg) == COG_OK);
    if (tx_failure) send_error = EPIPE;
    else assert(shutdown(f.client->fd, SHUT_WR) == 0);
    /* A full retained RX queue deliberately pauses reads; use TX error there. */
    cog_result_t terminal = COG_OK;
    for (unsigned i = 0; i < 32U && terminal >= COG_OK; ++i) terminal = one(f.accepted);
    if (terminal == COG_ERROR_AGAIN) {
        for (unsigned i = 0; i < 32U && f.accepted->failure == COG_OK; ++i) (void)one(f.accepted);
    }
    send_error = 0;
    assert(f.accepted->failure == (tx_failure ? COG_ERROR_IO : COG_ERROR_PROTOCOL));
    assert(proof.process_id == (uint64_t)getpid());
    unchanged(f.listener, f.id, COG_ERROR_NOT_FOUND);
    assert(cog_ipc_send_to_peer(f.listener, f.id, &msg) == COG_ERROR_NOT_FOUND);
    assert(zero(f.accepted->tx_data, 136U));
    if (!full) assert(zero(f.accepted->rx_data + 68U, 68U));
    unsigned char out[8]; memset(out, 0xa5, sizeof(out));
    cog_ipc_message_t got = {out, 7U, 2U};
    assert(cog_ipc_endpoint_recv(f.accepted, &got) == COG_ERROR_OVERFLOW);
    assert(got.len == 3U && out[0] == 0xa5U);
    assert(one(f.accepted) == f.accepted->failure);
    got.capacity = sizeof(out);
    assert(cog_ipc_endpoint_recv(f.accepted, &got) == COG_OK);
    assert(memcmp(out, "key", 3U) == 0 && zero(f.accepted->rx_data, 68U));
    if (full) {
        assert(cog_ipc_endpoint_recv(f.accepted, &got) == COG_OK);
        assert(memcmp(out, "key", 3U) == 0);
    }
    assert(cog_ipc_endpoint_recv(f.accepted, &got) == f.accepted->failure);
    assert(zero(f.accepted->rx_data, 136U));
    done(&f);
}
static void transient_and_explicit_wipe(void)
{
    for (unsigned mode = 0U; mode < 2U; ++mode) {
        bool wipe = mode != 0U;
        fixture_t f; listen_new(&f, wipe); connect_new(&f, wipe); ready(&f);
        unsigned char bytes[] = "sensitive";
        cog_ipc_message_t message = {bytes, sizeof(bytes), sizeof(bytes)};
        assert(cog_ipc_endpoint_send(f.accepted, &message) == COG_OK);
        unsigned char saved[68]; memcpy(saved, f.accepted->tx_data, sizeof(saved));
        send_error = EINTR;
        for (unsigned i = 0U; i < 4U; ++i) assert(one(f.accepted) == COG_ERROR_AGAIN);
        send_error = 0;
        assert(f.accepted->tx_count == 1U);
        assert(memcmp(saved, f.accepted->tx_data, sizeof(saved)) == 0);
        for (unsigned i = 0U; i < 16U; ++i) { tick(f.accepted); tick(f.client); }
        unsigned char result[64]; cog_ipc_message_t got = {result, 0U, sizeof(result)};
        assert(cog_ipc_endpoint_recv(f.client, &got) == COG_OK);
        assert(memcmp(result, bytes, sizeof(bytes)) == 0);
        assert(zero(f.client->rx_data, 68U) == (wipe ? 1 : 0));
        assert(zero(f.accepted->tx_data, 68U) == (wipe ? 1 : 0));
        assert(cog_ipc_endpoint_send(f.accepted, &message) == COG_OK);
        assert(cog_ipc_endpoint_disconnect(f.accepted) == COG_OK);
        if (wipe) {
            assert(zero(f.accepted->rx_data, 136U));
            assert(zero(f.accepted->tx_data, 136U));
            assert(zero(&f.accepted->native_peer, sizeof(f.accepted->native_peer)));
            assert(f.accepted->native_peer_result == COG_ERROR_AGAIN);
        }
        done(&f);
    }
}
static void allocation_wipes(void)
{
    cog_ipc_endpoint_config_t c = config(true);
    for (unsigned failure = 0U; failure <= 6U; ++failure) {
        allocation_calls = 0U; payload_count = 0U; wiped_frees = 0U;
        fail_at = failure; track_allocations = 1; verify_frees = 1;
        cog_ipc_endpoint_t *ep = NULL;
        cog_result_t rc = cog_ipc_endpoint_new_ex(&ep, &c);
        track_allocations = 0;
        if (failure == 0U) {
            assert(rc == COG_OK && allocation_calls == 6U && payload_count == 2U);
            unchanged(ep, 1U, COG_ERROR_INVALID_STATE);
            assert(cog_ipc_endpoint_free(ep) == COG_OK);
        } else assert(rc == COG_ERROR_NOMEM && ep == NULL);
        assert(wiped_frees == payload_count);
        verify_frees = 0;
    }
    payload_count = 0U;
}
static void fork_identity(void)
{
    fixture_t f; listen_new(&f, true);
    pid_t parent = getpid(), child = fork(); assert(child >= 0);
    if (child == 0) {
        cog_ipc_endpoint_t *ep; cog_ipc_endpoint_config_t c = config(true);
        assert(cog_ipc_endpoint_new_ex(&ep, &c) == COG_OK);
        assert(cog_ipc_endpoint_connect(ep, f.path, NULL) == COG_ERROR_AGAIN);
        cog_peer_id_t id = 0U;
        for (unsigned i = 0U; i < 10000U && id == 0U; ++i) {
            tick(ep); (void)cog_ipc_endpoint_get_peer_id(ep, &id); (void)usleep(100U);
        }
        assert(id != 0U); cog_ipc_peer_info_t info;
        assert(cog_ipc_endpoint_peer_info(ep, id, &info) == COG_OK);
        assert(info.process_id == (uint64_t)parent);
        unsigned char x = 1U; cog_ipc_message_t msg = {&x,1U,1U};
        assert(cog_ipc_endpoint_send(ep, &msg) == COG_OK);
        for (unsigned i = 0U; i < 64U && ep->tx_count != 0U; ++i) tick(ep);
        assert(ep->tx_count == 0U); assert(cog_ipc_endpoint_free(ep) == COG_OK);
        _exit(0);
    }
    cog_result_t rc = COG_ERROR_AGAIN;
    for (unsigned i = 0U; i < 10000U && rc == COG_ERROR_AGAIN; ++i) {
        rc = cog_ipc_endpoint_accept(f.listener, &f.accepted, &f.id); (void)usleep(100U);
    }
    assert(rc == COG_OK); assert(one(f.accepted) == COG_OK);
    cog_ipc_peer_info_t info;
    assert(cog_ipc_endpoint_peer_info(f.listener, f.id, &info) == COG_OK);
    assert(info.process_id == (uint64_t)child && info.user_id == (uint64_t)getuid());
    int status; assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    for (unsigned i = 0; i < 32U && f.accepted->failure == COG_OK; ++i) (void)one(f.accepted);
    assert(f.accepted->failure == COG_ERROR_IO && f.accepted->rx_count == 1U);
    assert(info.process_id == (uint64_t)child);
    assert(cog_ipc_endpoint_free(f.accepted) == COG_OK);
    assert(cog_ipc_endpoint_free(f.listener) == COG_OK);
}
int main(void)
{
    allocation_wipes(); identity_and_directed(); native_failures();
    retained_and_abandoned(false, false);
    retained_and_abandoned(true, false);
    retained_and_abandoned(true, true);
    transient_and_explicit_wipe(); fork_identity();
    puts("authenticated IPC: provenance, copied queues, EOF proof barrier and wipes PASS");
    return 0;
}

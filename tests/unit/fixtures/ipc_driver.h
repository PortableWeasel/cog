/** @file ipc_driver.h
 * @brief Bounded test-only drivers for legacy functional IPC fixtures.
 * Production calls never wait; these test drivers explicitly service sockets.
 * Direct nonblocking semantics are covered separately by test_ipc_nonblocking.
 */
#ifndef COG_TEST_IPC_DRIVER_H
#define COG_TEST_IPC_DRIVER_H
#include <cog/ipc.h>
#include <time.h>
static inline void test_ipc_drive(cog_ipc_endpoint_t *ep)
{
    uint64_t deadline;
    size_t i;
    for (i = 0U; i < 64U; ++i) {
        (void)cog_ipc_endpoint_service(ep, 100U, &deadline);
    }
}
static inline cog_result_t test_ipc_accept(cog_ipc_endpoint_t *listener,
                                           cog_ipc_endpoint_t **client,
                                           cog_peer_id_t *id)
{
    size_t i;
    cog_result_t rc;
    for (i = 0U; i < 1000U; ++i) {
        rc = cog_ipc_endpoint_accept(listener, client, id);
        if (rc != COG_ERROR_AGAIN) {
            if (rc == COG_OK) {
                test_ipc_drive(*client);
            }
            return rc;
        }
        (void)nanosleep(&(struct timespec){0, 1000000L}, NULL);
    }
    return COG_ERROR_AGAIN;
}
static inline cog_result_t test_ipc_send(cog_ipc_endpoint_t *ep,
                                         const cog_ipc_message_t *msg)
{
    cog_result_t rc;
    size_t i;
    for (i = 0U; i < 1000U; ++i) {
        uint64_t deadline;
        cog_result_t progress = cog_ipc_endpoint_service(ep, 100U, &deadline);
        rc = cog_ipc_endpoint_send(ep, msg);
        if (rc != COG_ERROR_INVALID_STATE || progress == COG_ERROR_INVALID_STATE ||
            progress == COG_ERROR_NULL_PTR) {
            if (rc == COG_OK) {
                test_ipc_drive(ep);
            }
            return rc;
        }
        (void)nanosleep(&(struct timespec){0, 1000000L}, NULL);
    }
    return COG_ERROR_AGAIN;
}
static inline cog_result_t test_ipc_recv(cog_ipc_endpoint_t *ep, cog_ipc_message_t *msg)
{
    size_t i;
    for (i = 0U; i < 1000U; ++i) {
        cog_result_t rc;
        test_ipc_drive(ep);
        rc = cog_ipc_endpoint_recv(ep, msg);
        if (rc != COG_ERROR_AGAIN) {
            return rc;
        }
        (void)nanosleep(&(struct timespec){0, 1000000L}, NULL);
    }
    return COG_ERROR_AGAIN;
}
static inline cog_result_t test_ipc_broadcast(cog_ipc_endpoint_t *ep,
                                              const cog_ipc_message_t *msg)
{
    cog_result_t rc = cog_ipc_broadcast(ep, msg);
    if (rc == COG_OK) {
        test_ipc_drive(ep);
    }
    return rc;
}
static inline cog_result_t test_ipc_route(cog_ipc_endpoint_t *ep, const char *id,
                                          const cog_ipc_message_t *msg)
{
    cog_result_t rc = cog_ipc_send_to_session(ep, id, msg);
    if (rc == COG_OK) {
        test_ipc_drive(ep);
    }
    return rc;
}
#define cog_ipc_endpoint_accept test_ipc_accept
#define cog_ipc_endpoint_send test_ipc_send
#define cog_ipc_endpoint_recv test_ipc_recv
#define cog_ipc_broadcast test_ipc_broadcast
#define cog_ipc_send_to_session test_ipc_route
#endif

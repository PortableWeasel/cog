/** @file host_client.h
 * @brief Private installed attach/watch client, no provider/store/model authority.
 */
#ifndef COG_CLI_HOST_CLIENT_H
#define COG_CLI_HOST_CLIENT_H
#include "projection.h"
#include "host_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif
typedef struct cog_cli_host_client cog_cli_host_client_t;
typedef struct cog_cli_host_client_config {
    const char *socket_path; /**< Copied path. */
    bool watch; /**< True read-only; false requests retained controller role. */
    bool terminal; /**< TTY escaped replacement rendering, otherwise NDJSON. */
    FILE *output, *diagnostic; /**< Borrowed checked host-output adapters. */
    int input_fd, signal_fd; /**< Borrowed; -1 absent. No inherited flag changes. */
    size_t projection_bytes; /**< Finite startup bound, max16MiB; server may
       exceed client capacity only by explicit LIMIT, no partial view installed. */
    uint32_t reconnect_ms; /**< Finite default3000, does not extend on retries. */
    /** --job-control opts an attaching controller into HELLO v2. Never
     * accepted for a watcher. No automatic downgrade or command replay
     * after failed capability negotiation. */
    bool job_control;
} cog_cli_host_client_config_t;
/** @brief Reserve client decode/render/pending command arenas, connect no wait.
 * @param[out] out NULL on failure; @param[in] config Copied except borrows.
 * @return OK; NULL_PTR; INVALID_ARG; LIMIT; NOMEM; IO; UNSUPPORTED profile.
 * Fresh checked entropy HELLO nonce; authenticated server assigns logical lease.
 * Endpoint_service drives readiness; retired lease never retargets pending input.
 * No config/credential resolver/net/provider/store constructor or task dispatch. */
cog_result_t cog_cli_host_client_new(cog_cli_host_client_t **out,
    const cog_cli_host_client_config_t *config);
/** @brief Advance bounded socket/input/render/reconnect; never sleep/wait.
 * @param[in,out] client Owner. @param[out] info Finite eligibility/done information.
 * @return OK; AGAIN; NULL_PTR; INVALID_STATE reentry; checked driver/output error.
 * Cursor advances only AFTER full validated replacement renders successfully.
 * GAP replaces actual display+control nonce atomically, then copies after anchor.
 * Retained same-runtime command retransmits SAME bytes/identity; new runtime
 * marks pending result uncertain and never converts it to a fresh command.
 * EOF/signal detaches client; not server shutdown/ALLOW/implicit continuation.
 * Final unterminated explicit command is parsed once before detach, then wait
 * under finite reconnect deadline for final admission result and ACK-of-ACK.
 * Signal/timeout before final result reports INDETERMINATE (outcome unknown),
 * never success from send-OK; after known result missing receipt cleanup is IO.
 * Explicit /shutdown is the only client shutdown command.
 */
cog_result_t cog_cli_host_client_step(cog_cli_host_client_t *client,
    cog_cli_host_runtime_step_t *info);
/** @brief Consume client buffers/endpoint, no effect ownership to drain.
 * @param[in,out] client NULL pointee OK; @param[in] primary First error.
 * @return primary or first cleanup/output error; NULL_PTR. No hidden wait. */
cog_result_t cog_cli_host_client_close(cog_cli_host_client_t **client,
    cog_result_t primary);
#ifdef __cplusplus
}
#endif
#endif

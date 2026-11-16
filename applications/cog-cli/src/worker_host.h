/** @file worker_host.h
 * @brief Private owned-worker process host, one parent fabric reader and drive.
 * No native child reattachment, resolver/env credential inheritance or UI socket.
 */
#ifndef COG_CLI_WORKER_HOST_H
#define COG_CLI_WORKER_HOST_H
#include "host_runtime.h"
#include "control.h"

#ifdef __cplusplus
extern "C" {
#endif
typedef struct cog_cli_worker_host cog_cli_worker_host_t;
typedef struct cog_cli_worker_startup {
    const char *socket_path; /**< Trusted startup control path only. */
    cog_worker_id_t worker;
    uint64_t parent_pid; /**< Trusted argv, verified against native getppid and
       kernel server credentials, never used to signal/kill any process. */
    int signal_fd; /**< Borrowed owner signal source,-1 absent. */
    const cog_net_config_t *net_config; /**< Explicit initialized host template;
       no observer, each executor has its own app-wide bridge/net lifetime. */
    size_t executor_slots; /**< Trusted finite startup reservation≤18. */
    size_t frame_bytes; /**< Trusted finite maximum, same parent bound. */
} cog_cli_worker_startup_t;

/** @brief Reserve bounded worker/socket/config/executor state, connect no wait.
 * @param[out] out NULL on failure; @param[in] startup Copied fixed inputs.
 * @return OK; NULL_PTR; INVALID_ARG; LIMIT; NOMEM; IO; UNSUPPORTED profile.
 * No model/task/effect, fork or borrowed secret until authenticated bootstrap.
 * Parent link readiness progresses via endpoint_service, never synchronous connect
 * assumption. Native exec resets inherited blocked INT/TERM before installing
 * its own explicit host signal lifecycle; no parent's SIGCHLD policy mutation.
 */
cog_result_t cog_cli_worker_host_new(cog_cli_worker_host_t **out,
    const cog_cli_worker_startup_t *startup);
/** @brief One bounded nonblocking parent/heartbeat/executor/drain slice.
 * @param[in,out] worker Published owner. @param[out] info Same finite drive info.
 * @return OK; AGAIN; NULL_PTR; INVALID_STATE reentry; first driver failure.
 * Authentication + fixed bootstrap + exact child/stage admission precede effects.
 * Parent disconnect/signal freezes all ordinary work and cancels/drains local
 * executions; retains factual durable ledgers. No fresh connection/restart of
 * attempt. Copies current CGA item until final receipt, cancellation consumed
 * uses NEW wire item in unaccepted slot, never rewrites local factual result.
 * Native timeout may outlive reachable parent; no success claim from socket EOF.
 */
cog_result_t cog_cli_worker_host_step(cog_cli_worker_host_t *worker,
    cog_cli_host_runtime_step_t *info);
/** @brief Request drain, no wait; idempotent owner-thread API.
 * @param[in,out] worker Owner. @return OK; NULL_PTR. */
cog_result_t cog_cli_worker_host_request_shutdown(cog_cli_worker_host_t *worker);
/** @brief Consume done or never-stepped worker; retain live owner on refusal.
 * @param[in,out] worker NULL pointee OK; @param[in] primary Preserved first error.
 * @return primary/cleanup error; NULL_PTR; INVALID_STATE if work not drained.
 * Provider consumed once before config clear, net before observer free; dedicated
 * bootstrap/codec/IPC buffers wipe on every retirement/failure/free. */
cog_result_t cog_cli_worker_host_close(cog_cli_worker_host_t **worker,
    cog_result_t primary);

#ifdef __cplusplus
}
#endif
#endif

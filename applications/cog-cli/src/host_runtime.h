/**
 * @file host_runtime.h
 * @brief Private installed-host coordinator and bounded executor composition.
 *
 * Design N pass1 subset; no bodies are authorized by this declaration. One
 * owner thread drives one stable coordinator, its UI fabric, the orchestrator's
 * separate worker fabric, and bounded local executors. No nested app_run/run,
 * hidden thread, mutable global, second endpoint reader or dynamic registration.
 * Only the existing core/app stop calls are cross-thread/signal-safe; this host
 * consumes signalfd readiness on its owner thread. See HEADLESS_CONTRACTS.
 */
#ifndef COG_CLI_HOST_RUNTIME_H
#define COG_CLI_HOST_RUNTIME_H
#include <cog_cli/cli.h>
#include <cog/orchestrator.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cog_cli_host_runtime cog_cli_host_runtime_t;

/** Fixed host reservations; zero selects the documented finite default. These
 * are startup/explicit host inputs, never model arguments or live reload keys.
 * No child profile may increase them or acquire resolver/caller callback power.
 */
typedef struct cog_cli_host_runtime_config {
    const cog_cli_options_t *root_options; /**< Borrowed until new returns;
       NEW/RESUME/FORK execution only, durable store required, socket_path=NULL.
       Resolve J precedence/credentials ONCE before constructing the root app.
       net must be NULL: this host constructs a separate transport per executor.
       LIST/RECOVER remain the existing offline single-process host path. */
    const cog_net_config_t *net_config; /**< Copied constructor template, required
       for REAL: host_runtime_initialized=true and explicit dependency memory
       profile. observer/user must be NULL; each executor owns its observation
       bridge beyond provider/net teardown. No curl global init/cleanup here. */
    const char *ui_socket; /**< Required copied local Unix path. One UI endpoint
       belongs to the coordinator core. Never aliases the worker socket. */
    const char *worker_socket; /**< Required iff workers>0; orchestrator-owned. */
    const char *executable; /**< Required iff workers>0; trusted explicit absolute
       installed executable path, copied. No shell, PATH search or model path. */
    size_t workers; /**< 0 disables worker processes; max8, no automatic failover. */
    size_t children; /**< Aggregate admitted child attempts: default4, max16.
       Includes running, draining and retained unacknowledged terminal slots.
       Child delegation depth is ONE; child models cannot delegate recursively. */
    uint64_t child_requests; /**< Finite aggregate request credit, default64.
       Reserve worst-case (rounds+1)*(retries+1) BEFORE child admission, checked
       arithmetic; no refund from unknown usage and no fan-out multiplication.
       Credit exhaustion refuses admission before STARTED/spawn, never truncates. */
    size_t ui_clients; /**< Default4, max16; one controller, others read-only. */
    size_t command_records; /**< Default64, max1024; bounded accepted-command
       receipts. Saturation refuses new commands, not old duplicate lookup. */
    size_t replay_records; /**< Default64, max4096; existing CGR1 bounded ring. */
    size_t replay_bytes; /**< Default4MiB, max64MiB. A full replacement projection
       must fit ONE record and reserved resync/send storage; reject impossible
       combinations at construction, never publish partial view or drop text. */
    uint32_t shutdown_ms; /**< Default3000, finite; does not prove descendants
       stopped after forced worker death. Durable effects remain uncertain. */
    int signal_fd; /**< Borrowed nonblocking signalfd, -1 means none. Never close
       or modify its flags/mask. The host preserves original mask/dispositions. */
    FILE *diagnostic; /**< Borrowed checked host-output stream; NULL=stderr.
       No credentials/bootstrap/opaque/arguments/output in lifecycle diagnostics. */
} cog_cli_host_runtime_config_t;

/** One cooperative host slice, same monotonic clock as core driving. */
typedef struct cog_cli_host_runtime_step {
    bool done; /**< All local apps drained, child borrows returned and native
       owned lifetimes reaped or explicitly lost; NOT proof of success/effect undo. */
    size_t dispatched; /**< Sum of bounded context dispatch, overflow checked. */
    uint64_t next_deadline_ms;
    uint32_t max_wait_ms; /**< Finite minimum across all contexts/readiness;
       zero for retained actionable work. No caller sleep beyond this bound. */
} cog_cli_host_runtime_step_t;

/**
 * @brief Reserve and construct a headless host without launching native children.
 * @param[out] out Owned runtime, NULL on every failure.
 * @param[in] config Inputs copied except documented borrows; one owner thread.
 * @return OK; NULL_PTR; INVALID_ARG conflicting modes/paths/bounds; LIMIT checked
 * reservation arithmetic/identities; NOMEM; UNSUPPORTED profile; J/G/net/app errors.
 * @post Stable coordinator owns its separate control session and every CGA1
 * registry entry. Explicit parent_session_id names the actual executing session;
 * a registry shadow is not remote execution history or a store authority.
 * Root executor retains its actual session/store guard. No fork occurs before
 * successful publication, so failed construction leaves no child to reap.
 * Constructor allocates fixed queues, slots, codecs, projection/resync arenas,
 * immutable command/receipt storage and all scalar identities. Explicit named
 * app/net/provider/IPC constructors remain bounded allocations at admission;
 * service never reallocs a table, grows a registry or duplicates an input string.
 */
cog_result_t cog_cli_host_runtime_new(cog_cli_host_runtime_t **out,
    const cog_cli_host_runtime_config_t *config);

/** @brief Start one drive; launch/service workers only while a published owner
 * exists. No wait/dispatch here. @param[in,out] runtime Constructed inactive host.
 * @return OK; NULL_PTR; INVALID_STATE already started/ended/shutdown/reentrant.
 * A pre-begin shutdown is done without a drive; it never launches workers or
 * admits the stored root prompt merely to make destruction possible. */
cog_result_t cog_cli_host_runtime_begin(cog_cli_host_runtime_t *runtime);

/**
 * @brief Advance each eligible context once in retained round-robin order.
 * @param[in,out] runtime Active owner; no call from its callback/dispatch stack.
 * @param[out] info Slice result, unchanged on invalid call.
 * @return OK; AGAIN if no progress; NULL_PTR; INVALID_STATE; first driver failure.
 * A failure freezes admission and requests cancel/drain, but does NOT release
 * the runtime. Continue step until info.done, preserving the first error through
 * later sink/kill/reap/cleanup failures. No wait, sleep or recursive run; each core
 * uses begin/step/end and the same checked clock. Full ordinary message queues
 * do not consume shutdown/receipt/final-update reservations. No stopped slot is
 * restarted because an execution borrow happened to become available again.
 */
cog_result_t cog_cli_host_runtime_step(cog_cli_host_runtime_t *runtime,
    cog_cli_host_runtime_step_t *info);

/** @brief Freeze admission and request all owned app/worker cancellation/drain.
 * @param[in,out] runtime Owner thread, also valid before begin.
 * @return OK; NULL_PTR. Idempotent, never extends any earlier absolute deadline;
 * pending/retained child results and effect ledgers survive shutdown arbitration.
 * Before begin, records done immediately (no work has been admitted/forked).
 * This is not a signal handler API and never waits for an ACK or process exit. */
cog_result_t cog_cli_host_runtime_request_shutdown(cog_cli_host_runtime_t *runtime);

/**
 * @brief Consume a quiescent runtime after all owned work has drained.
 * @param[in,out] runtime Pointer set NULL on consumption; NULL pointee is OK.
 * @param[in] primary Existing failure, preserved ahead of cleanup errors.
 * @return primary or first cleanup failure; NULL_PTR for NULL pointer argument;
 * INVALID_STATE leaves a begun/not-done runtime untouched for continued stepping.
 * A never-begun quiescent runtime may be consumed directly, with or without a
 * prior request_shutdown; do not begin it just to close it.
 * Close returns execution borrows only after app/core/provider/config/tool/guard
 * borrowers drain; all consumed provider handles are detached and disconnected
 * once on every return, then matching secret storage clears. Net callbacks end
 * before bridge destruction. No blocking waitpid, detached child, implicit task
 * replay, consumed-handle retry or whole-runtime leak is a cleanup strategy.
 */
cog_result_t cog_cli_host_runtime_close(cog_cli_host_runtime_t **runtime,
    cog_result_t primary);

#ifdef __cplusplus
}
#endif
#endif

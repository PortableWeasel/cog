/** @file host_output.h @brief Linux executable-only bounded output adapters. */
#ifndef COG_CLI_HOST_OUTPUT_H
#define COG_CLI_HOST_OUTPUT_H
#include <cog/result.h>
#include <stdbool.h>
#include <stdio.h>

/** Private slot; caller never modifies it after open. No borrowed write buffer. */
typedef struct cog_cli_host_output_slot {
    FILE *original, *stream;
    int descriptor;
    bool socket;
    cog_result_t error;
} cog_cli_host_output_slot_t;
/** Main-owned explicit lifetime; zero initialize. FILE constructors own their
 * matching fclose allocations; stdout uses this fixed buffer, stderr unbuffered.
 * No output queue, output thread, global context or later adapter allocation. */
typedef struct cog_cli_host_output {
    cog_cli_host_output_slot_t slots[2];
    char stdout_buffer[4096];
    bool installed;
} cog_cli_host_output_t;

/** Install executable stdout/stderr adapters before any option/app output.
 * @pre Zero initialized, not already open; owner thread/process only. Main calls
 * before any output: inherited standard FILEs have no pending buffered bytes.
 * SIGPIPE is ignored by the explicit host lifetime through final adapter close.
 * Regular files use a dup of the inherited description, preserving shared offset
 * and append semantics. Sockets use per-call MSG_DONTWAIT; FIFO/character devices
 * reopen /proc/self/fd independently with NONBLOCK/NOCTTY. Inherited file status
 * flags are never changed. Unsupported descriptors or unavailable reopen fail
 * closed rather than falling back to a potentially blocking nonregular write.
 * GNU FILE constructors are explicit here. Fixed stdout buffering and unbuffered
 * stderr preserve checked flush/prompt semantics. Each write performs at most one
 * write/send, never waits/retries/replays. Saturation, EINTR, short write or another
 * output error latches IO; later writes fail without re-sending accepted bytes.
 * No hard real-time guarantee is made for regular-file filesystem I/O.
 * @return OK; NULL_PTR; INVALID_STATE if already installed; NOMEM; IO. Failure
 * consumes only new resources and never changes the process standard FILEs.
 * Neither unwind nor close flushes/closes the inherited original FILEs.
 * The host must not print a fallback diagnostic through inherited streams on
 * construction failure, as they may be the saturated descriptors being fixed.
 */
cog_result_t cog_cli_host_output_open(cog_cli_host_output_t *output);
/** Checked final stdout flush/close before app teardown; restores original stdout
 * first. Idempotent. No subsequent host/app stdout writes are permitted.
 * @return NULL_PTR; otherwise this slot's first IO error or OK. */
cog_result_t cog_cli_host_output_close_stdout(cog_cli_host_output_t *output);
/** After app/net cleanup and final diagnostic, flush/close remaining adapter,
 * restore original standard FILEs, and consume every owned descriptor/FILE even
 * on failure. Preserve a nonOK primary result; otherwise return first output
 * error. Safe after failed open and repeated close. Does not close inherited
 * descriptors/FILEs or change their flags/offset except actual regular writes.
 * @return NULL_PTR for output; otherwise primary first, output error, or OK. */
cog_result_t cog_cli_host_output_close(cog_cli_host_output_t *output, cog_result_t primary);
#endif

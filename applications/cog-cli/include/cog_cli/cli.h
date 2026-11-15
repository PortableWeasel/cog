/**
 * @file cli.h
 * @brief Testable cog CLI application: options, app context, and the
 *        single-process prompt flow.
 *
 * The CLI wires the real cog-core / cog-provider / cog-obs / cog-store
 * libraries into a functional single-process agent: a static const
 * dispatch table drives the (IDLE, AWAITING_LLM) state machine, the mock
 * provider streams scripted chunks, and the session tree records every
 * exchange.  Scenario tests link @c cog::cli and drive this API directly
 * instead of spawning the binary.
 *
 * @section cli_ownership Ownership
 *
 * A @c cog_cli_app_t owns everything it creates: the session, the
 * provider, the obs context, the store, the mock script storage, and the
 * optional IPC listener.  The core borrows the session / provider / obs /
 * store and owns the IPC endpoint once configured, so teardown order is
 * fixed: core first, then provider, obs, store, session, then owned
 * storage.  @c cog_cli_app_free performs exactly that order.
 *
 * @section cli_states States and events
 *
 * The dispatch table is static const and lives in cli.c:
 *
 * | State | Event | Handler |
 * |-------|-------|---------|
 * | IDLE | USER_INPUT | append user message, send to provider, start streaming |
 * | IDLE | CANCEL | no-op (self transition) |
 * | IDLE | SHUTDOWN | request the run loop to stop |
 * | AWAITING_LLM | LLM_CHUNK | receive next chunk, append/print, continue or finish |
 * | AWAITING_LLM | LLM_COMPLETE | close the turn, return to IDLE |
 * | AWAITING_LLM | CANCEL | arm provider cancellation; next receive aborts |
 * | AWAITING_LLM | SHUTDOWN | request the run loop to stop |
 *
 * A retryable provider error (e.g. rate limit) is retried once with a
 * bounded backoff derived from the error's @c retry_after_ms; a
 * non-retryable error ends the turn and is reported on stderr.
 *
 * @section cli_threading Threading
 *
 * The app is single-threaded.  @c cog_cli_app_stop is the only call safe
 * from a signal handler or another thread; it forwards to
 * @c cog_core_stop.
 */
#ifndef COG_CLI_CLI_H
#define COG_CLI_CLI_H

#include <cog/core.h>
#include <cog/obs.h>
#include <cog/provider.h>
#include <cog/result.h>
#include <cog/session.h>
#include <cog/session_store.h>

#include "cog_cli/mock_provider.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Version string reported by --version. */
#define COG_CLI_VERSION "0.1.0"

/**
 * @brief Mock behaviour selected by --mock-mode.
 */
typedef enum cog_cli_mock_mode {
    COG_CLI_MOCK_STREAM = 0,     /**< Stream the configured chunks. */
    COG_CLI_MOCK_ERROR,          /**< One retryable error, then a chunk. */
    COG_CLI_MOCK_CANCEL,         /**< The stream aborts immediately. */
    COG_CLI_MOCK_RATE_LIMIT,     /**< A 429 retryable error, then a chunk. */
} cog_cli_mock_mode_t;

/**
 * @brief Parsed CLI options.
 *
 * Zero-initialise and pass to @c cog_cli_options_parse, which fills the
 * defaults and applies the command line.  String fields point into the
 * caller's @p argv (or static defaults) and are borrowed; the app copies
 * what it needs.
 */
typedef struct cog_cli_options {
    /** Single prompt to run; NULL selects interactive mode. */
    const char *prompt;
    /** Session identifier; NULL generates one. */
    const char *session_id;
    /** Store base directory; NULL disables persistence. */
    const char *store_dir;
    /** Log file path; NULL logs to stderr. */
    const char *log_path;
    /** Unix domain socket path; NULL runs single-process. */
    const char *socket_path;
    /** Model identifier sent in provider requests. */
    const char *model_id;
    /** Endpoint name to connect through (only "mock" exists). */
    const char *endpoint_name;
    /** Comma-separated mock chunks. */
    const char *chunks;
    /** Mock behaviour. */
    cog_cli_mock_mode_t mock_mode;
    /** Custom mock script; overrides @p chunks and @p mock_mode when
     *  non-NULL.  The steps array and its strings are borrowed and must
     *  outlive the app. */
    const cog_mock_script_t *script;
    /** Message queued for the turn after the current one. */
    const char *queue_text;
    /** Read prompts from stdin (default when @p prompt is NULL). */
    bool interactive;
    /** Enable debug logging. */
    bool verbose;
} cog_cli_options_t;

/**
 * @brief Opaque CLI application context.
 */
typedef struct cog_cli_app cog_cli_app_t;

/* ------------------------------------------------------------------ */
/* Options                                                             */
/* ------------------------------------------------------------------ */

/**
 * @brief Parse command-line arguments into @p out.
 *
 * Fills defaults first, then applies @p argv.  On @c --help or
 * @c --version the usage/version text is printed to stdout and
 * @c COG_ERROR_ABORTED is returned with @p *out_exit_code set to 0.  On
 * an unknown option or a missing value, an error is printed to stderr
 * and @c COG_ERROR_ABORTED is returned with @p *out_exit_code set to 2.
 *
 * @param[out] out           Receives the parsed options.
 * @param[in]  argc          Argument count (including argv[0]).
 * @param[in]  argv          Argument vector.
 * @param[out] out_exit_code Receives the process exit code when
 *                           @c COG_ERROR_ABORTED is returned; may be
 *                           NULL.
 * @return COG_OK to continue, COG_ERROR_ABORTED when the process should
 *         exit with @p *out_exit_code, COG_ERROR_NULL_PTR if @p out is
 *         NULL.
 */
cog_result_t cog_cli_options_parse(cog_cli_options_t *out,
                                   int argc,
                                   char **argv,
                                   int *out_exit_code);

/**
 * @brief Print usage text.
 *
 * @param[in] stream Destination (stdout or stderr).
 */
void cog_cli_print_usage(FILE *stream);

/**
 * @brief Return a static human-readable name for a result code.
 *
 * @param[in] rc Result code.
 * @return Static NUL-terminated name (never NULL).
 */
const char *cog_cli_result_name(cog_result_t rc);

/* ------------------------------------------------------------------ */
/* Application lifecycle                                               */
/* ------------------------------------------------------------------ */

/**
 * @brief Create a CLI application from parsed options.
 *
 * Builds the session, mock script, provider, obs context, optional store
 * and optional IPC listener, then the core.  Fails closed: on any error
 * every resource created so far is released and @p *out is NULL.
 *
 * @param[out] out  Uninitialised pointer; set to the new app on success.
 * @param[in]  opts Parsed options; must be non-NULL.
 * @return COG_OK on success.
 * @return COG_ERROR_NULL_PTR if @p out or @p opts is NULL.
 * @return COG_ERROR_INVALID_ARG if @p opts->model_id or
 *         @p opts->endpoint_name is NULL.
 * @return COG_ERROR_NOT_FOUND if @p opts->endpoint_name is not a known
 *         endpoint.
 * @return COG_ERROR_IO if the log file cannot be opened or the IPC
 *         listener cannot bind.
 * @return Another error code from the underlying libraries.
 *
 * @post On success, @p *out is a valid app owned by the caller; release
 *       with @c cog_cli_app_free.
 */
cog_result_t cog_cli_app_new(cog_cli_app_t **out,
                             const cog_cli_options_t *opts);

/**
 * @brief Destroy a CLI application.
 *
 * @param[in,out] app App to destroy; may be NULL (no-op).
 *
 * @post All owned resources are released in the documented order.  The
 *       app must not be used after this call.
 */
void cog_cli_app_free(cog_cli_app_t *app);

/**
 * @brief Run the agent loop until stopped.
 *
 * Enqueues the configured @c --prompt (if any) and @c --queue message
 * (if any) before entering the loop.  In interactive mode the wait hook
 * reads stdin; in single-prompt mode it stops the loop when the turn
 * completes.  May be called again after a stop.
 *
 * @param[in,out] app App to run.
 * @return COG_OK if the loop exited via stop.
 * @return COG_ERROR_NULL_PTR if @p app is NULL.
 * @return Another error code if dispatch or auto-save fails.
 */
cog_result_t cog_cli_app_run(cog_cli_app_t *app);

/**
 * @brief Request the run loop to exit.
 *
 * Signal-safe: forwards to @c cog_core_stop and records the request so
 * the wait hook returns promptly.
 *
 * @param[in,out] app App to stop; may be NULL (no-op).
 */
void cog_cli_app_stop(cog_cli_app_t *app);

/* ------------------------------------------------------------------ */
/* Event injection (scenario harness)                                  */
/* ------------------------------------------------------------------ */

/**
 * @brief Submit a user prompt as a @c COG_EVENT_USER_INPUT.
 *
 * @param[in,out] app  App to submit into.
 * @param[in]     text Prompt text; copied into the event queue.
 * @param[in]     len  Length of @p text in bytes.
 * @return COG_OK on success.
 * @return COG_ERROR_NULL_PTR if @p app or @p text is NULL.
 * @return COG_ERROR_OVERFLOW if the event queue is full.
 */
cog_result_t cog_cli_app_submit(cog_cli_app_t *app,
                                const char *text,
                                size_t len);

/**
 * @brief Queue a message for the turn after the current one.
 *
 * Forwards to @c cog_core_queue_message; the core auto-injects it as a
 * @c COG_EVENT_USER_INPUT when the current turn completes.
 *
 * @param[in,out] app  App to queue into.
 * @param[in]     text Message text; copied.
 * @param[in]     len  Length of @p text in bytes.
 * @return COG_OK on success.
 * @return COG_ERROR_NULL_PTR if @p app or @p text is NULL.
 * @return COG_ERROR_OVERFLOW if the pending queue is full.
 */
cog_result_t cog_cli_app_queue(cog_cli_app_t *app,
                               const char *text,
                               size_t len);

/**
 * @brief Request cancellation of the in-flight turn.
 *
 * Enqueues a @c COG_EVENT_CANCEL; the handler arms provider cancellation
 * and the next receive aborts the stream.
 *
 * @param[in,out] app App to cancel into.
 * @return COG_OK on success.
 * @return COG_ERROR_NULL_PTR if @p app is NULL.
 * @return COG_ERROR_OVERFLOW if the event queue is full.
 */
cog_result_t cog_cli_app_cancel(cog_cli_app_t *app);

/**
 * @brief Request graceful shutdown.
 *
 * Enqueues a @c COG_EVENT_SHUTDOWN; the handler stops the run loop.
 *
 * @param[in,out] app App to shut down.
 * @return COG_OK on success.
 * @return COG_ERROR_NULL_PTR if @p app is NULL.
 * @return COG_ERROR_OVERFLOW if the event queue is full.
 */
cog_result_t cog_cli_app_shutdown(cog_cli_app_t *app);

/* ------------------------------------------------------------------ */
/* Accessors (scenario harness)                                        */
/* ------------------------------------------------------------------ */

/**
 * @brief Return the core instance.
 *
 * @param[in] app App to query; may be NULL.
 * @return The core, or NULL if @p app is NULL.
 */
cog_core_t *cog_cli_app_core(cog_cli_app_t *app);

/**
 * @brief Return the session instance (owned by the app).
 *
 * @param[in] app App to query; may be NULL.
 * @return The session, or NULL if @p app is NULL.
 */
cog_session_t *cog_cli_app_session(cog_cli_app_t *app);

/**
 * @brief Return the provider instance (owned by the app).
 *
 * @param[in] app App to query; may be NULL.
 * @return The provider, or NULL if @p app is NULL.
 */
cog_provider_t *cog_cli_app_provider(cog_cli_app_t *app);

/**
 * @brief Return the obs context (owned by the app).
 *
 * @param[in] app App to query; may be NULL.
 * @return The obs context, or NULL if @p app is NULL.
 */
cog_obs_t *cog_cli_app_obs(cog_cli_app_t *app);

/**
 * @brief Return the session store (owned by the app).
 *
 * @param[in] app App to query; may be NULL.
 * @return The store, or NULL if @p app is NULL or persistence is
 *         disabled.
 */
cog_store_t *cog_cli_app_store(cog_cli_app_t *app);

/**
 * @brief Return the current state-machine state.
 *
 * @param[in] app App to query; may be NULL.
 * @return The current state id, or zero if @p app is NULL.
 */
cog_state_t cog_cli_app_state(cog_cli_app_t *app);

#ifdef __cplusplus
}
#endif

#endif /* COG_CLI_CLI_H */

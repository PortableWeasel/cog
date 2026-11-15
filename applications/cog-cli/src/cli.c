/**
 * @file cli.c
 * @brief Testable cog CLI application (see cli.h).
 *
 * Wires the real cog-core / cog-provider / cog-obs / cog-store libraries
 * into a single-process agent loop:
 *
 *  - a static const dispatch table drives the (IDLE, AWAITING_LLM) state
 *    machine (see @c cli_dispatch_table);
 *  - the mock provider (mock_provider.c) streams scripted chunks with no
 *    network access;
 *  - the session tree records every exchange and the store auto-saves
 *    after each completed turn;
 *  - the wait hook reads stdin in interactive mode and stops the loop
 *    when a single-prompt turn completes;
 *  - retryable provider errors are retried once with a bounded backoff
 *    derived from the error's retry_after_ms; non-retryable errors
 *    propagate out of @c cog_core_run (fail loudly).
 *
 * The handlers need the app context (output stream, options) but the
 * handler signature only receives the core.  The app context is threaded
 * through the core's borrowed @c cog_core_config_t::user pointer and
 * retrieved with @c cog_core_user, so the dispatch table stays static
 * const and no global mutable state exists.
 */
#define _POSIX_C_SOURCE 200809L

#include "cog_cli/cli.h"
#include "cog_cli/mock_provider.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <time.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* Application states                                                  */
/* ------------------------------------------------------------------ */

enum {
    CLI_STATE_IDLE = 0,       /**< Waiting for user input. */
    CLI_STATE_AWAITING_LLM = 1, /**< Streaming from the provider. */
};

/* ------------------------------------------------------------------ */
/* App context bridge                                                  */
/* ------------------------------------------------------------------ */

/**
 * @brief Retrieve the app context threaded through the core's borrowed
 *        user pointer (see @c cog_core_user).
 *
 * The app is supplied as @c cog_core_config_t::user at construction and
 * is never owned or mutated by the core; handlers read it back verbatim.
 *
 * @param[in] core Core to query; may be NULL.
 * @return The app pointer, or NULL if @p core is NULL or no user context
 *         was configured.
 */
static cog_cli_app_t *cli_app_from_core(cog_core_t *core)
{
    return (cog_cli_app_t *)cog_core_user(core);
}

/* ------------------------------------------------------------------ */
/* App layout                                                          */
/* ------------------------------------------------------------------ */

struct cog_cli_app {
    cog_cli_options_t opts;      /**< Parsed options (borrowed strings). */
    cog_session_t *session;      /**< Owned. */
    cog_provider_t *provider;    /**< Owned. */
    cog_obs_t *obs;              /**< Owned. */
    cog_store_t *store;          /**< Owned; NULL when persistence off. */
    cog_core_t *core;            /**< Owned. */
    cog_ipc_endpoint_t *ipc;     /**< Owned until handed to the core. */
    cog_mock_script_t script;    /**< Script handed to the provider. */
    cog_mock_step_t *steps;      /**< Owned step array. */
    size_t step_count;           /**< Live entries in @p steps. */
    char *chunks_copy;           /**< Owned copy of the chunks string. */
    FILE *out;                   /**< Assistant output stream (stdout). */
    FILE *log_file;              /**< Obs log destination; owned when
                                      != stderr. */
    cog_obs_sink_t log_sink;     /**< Mutable sink descriptor. */
    cog_obs_sink_table_t sink_table; /**< Sink table for @p log_sink. */
    bool stop_requested;         /**< Set by cog_cli_app_stop. */
    bool prompt_submitted;       /**< --prompt/--queue submitted once. */
    char line_buf[1024];         /**< Interactive stdin buffer. */
};

/* ------------------------------------------------------------------ */
/* Static endpoint table                                               */
/* ------------------------------------------------------------------ */

static const cog_endpoint_t cli_endpoints[] = {
    {
        .name = "mock",
        .vtable = &cog_mock_provider_vtable,
    },
};

static const cog_endpoint_table_t cli_endpoint_table = {
    cli_endpoints,
    sizeof(cli_endpoints) / sizeof(cli_endpoints[0]),
};

/* ------------------------------------------------------------------ */
/* Handlers                                                            */
/* ------------------------------------------------------------------ */

/**
 * @brief Discard every trailing assistant node (partial streamed turn).
 *
 * The session tree appends one node per chunk, so a cancelled turn leaves
 * a chain of assistant nodes after the last user node.  Discard them from
 * the leaf up until the active node is no longer an assistant node.
 */
static void cli_discard_partial_turn(cog_session_t *session)
{
    cog_session_node_t *active = cog_session_active(session);

    while (active != NULL &&
           cog_node_data(active)->role == COG_NODE_ASSISTANT) {
        cog_session_node_t *parent = cog_node_parent(active);

        if (cog_session_discard_node(session, active) != COG_OK) {
            break;
        }
        active = parent;
    }
}

/**
 * @brief Handle a provider receive error: report, and retry once when
 *        the error is retryable.
 *
 * Reads the retry metadata through the mock provider state API (the mock
 * is the only provider implementation in this phase).  A retryable error
 * sleeps a bounded backoff derived from @c retry_after_ms, re-sends the
 * request rebuilt from session history, and enqueues the next
 * @c COG_EVENT_LLM_CHUNK.  A non-retryable error is reported on stderr
 * and returned to the caller, which propagates it out of the run loop.
 *
 * @return COG_OK when the retry was scheduled, or the original error code
 *         when the error is not retryable (or the retry could not be
 *         scheduled).
 */
static cog_result_t cli_handle_provider_error(cog_core_t *core,
                                              cog_result_t rc)
{
    cog_provider_t *provider = cog_core_provider(core);
    cog_session_t *session = cog_core_session(core);
    cog_cli_app_t *app = cli_app_from_core(core);
    cog_mock_state_t ms;
    cog_provider_request_t req;
    cog_event_t next;
    const cog_message_t *history = NULL;
    size_t history_count = 0U;
    uint32_t backoff;
    struct timespec ts;

    (void)memset(&ms, 0, sizeof(ms));
    (void)cog_mock_provider_state(provider, &ms);

    if (!ms.last_error.retryable) {
        if (app != NULL) {
            (void)fprintf(stderr,
                          "cog-cli: provider error: %s (%s)\n",
                          cog_cli_result_name(rc),
                          ms.last_error.message != NULL
                              ? ms.last_error.message
                              : "no detail");
        }
        return rc;
    }

    backoff = ms.last_error.retry_after_ms;
    if (backoff == 0U) {
        backoff = 100U; /* default backoff */
    }
    if (backoff > 5000U) {
        backoff = 5000U; /* cap */
    }
    if (app != NULL) {
        (void)fprintf(stderr,
                      "cog-cli: provider error: %s (http %d, retryable, "
                      "retrying in %u ms): %s\n",
                      cog_cli_result_name(rc), ms.last_error.http_status,
                      (unsigned)backoff,
                      ms.last_error.message != NULL
                          ? ms.last_error.message
                          : "no detail");
    }

    ts.tv_sec = (time_t)(backoff / 1000U);
    ts.tv_nsec = (long)((backoff % 1000U) * 1000000UL);
    (void)nanosleep(&ts, NULL);

    /* Rebuild the request from session history and re-send. */
    if (cog_session_get_history(session, cog_session_active(session),
                                &history, &history_count) != COG_OK) {
        return COG_ERROR_IO;
    }
    (void)memset(&req, 0, sizeof(req));
    req.model_id = (app != NULL) ? app->opts.model_id : "mock-model";
    req.messages = history;
    req.message_count = history_count;
    req.stream = true;
    if (cog_provider_send(provider, &req) != COG_OK) {
        return COG_ERROR_IO;
    }

    next.id = (cog_event_id_t)COG_EVENT_LLM_CHUNK;
    next.kind = COG_EVENT_LLM_CHUNK;
    next.payload = NULL;
    next.payload_len = 0U;
    next.source_id = 0U;
    if (cog_core_enqueue(core, &next) != COG_OK) {
        return COG_ERROR_IO;
    }
    return COG_OK;
}

/** IDLE + USER_INPUT: append the user message, send to the provider, and
 * start streaming. */
static cog_handler_result_t cli_on_user_input(cog_core_t *core,
                                              const cog_event_t *event)
{
    cog_session_t *session = cog_core_session(core);
    cog_provider_t *provider = cog_core_provider(core);
    cog_cli_app_t *app = cli_app_from_core(core);
    cog_message_t msg;
    cog_provider_request_t req;
    cog_event_t next;
    cog_handler_result_t r = {CLI_STATE_AWAITING_LLM, COG_OK};
    const cog_message_t *history = NULL;
    size_t history_count = 0U;

    if (core == NULL || event == NULL) {
        r.status = COG_ERROR_NULL_PTR;
        return r;
    }

    msg.role = COG_MESSAGE_ROLE_USER;
    msg.content = (const char *)event->payload;
    msg.content_len = event->payload_len;
    msg.name = NULL;
    if (cog_session_append(session, &msg, NULL) != COG_OK) {
        r.status = COG_ERROR_IO;
        return r;
    }

    if (cog_session_get_history(session, cog_session_active(session),
                                &history, &history_count) != COG_OK) {
        r.status = COG_ERROR_IO;
        return r;
    }

    (void)memset(&req, 0, sizeof(req));
    req.model_id = (app != NULL) ? app->opts.model_id : "mock-model";
    req.messages = history;
    req.message_count = history_count;
    req.stream = true;

    if (cog_provider_send(provider, &req) != COG_OK) {
        r.status = COG_ERROR_IO;
        return r;
    }

    next.id = (cog_event_id_t)COG_EVENT_LLM_CHUNK;
    next.kind = COG_EVENT_LLM_CHUNK;
    next.payload = NULL;
    next.payload_len = 0U;
    next.source_id = 0U;
    if (cog_core_enqueue(core, &next) != COG_OK) {
        r.status = COG_ERROR_IO;
        return r;
    }
    return r;
}

/** AWAITING_LLM + LLM_CHUNK: receive the next chunk, append it to the
 * session, print it, and enqueue the next chunk or LLM_COMPLETE.  A
 * cancellation discards the partial turn and returns to IDLE; a provider
 * error is reported and retried once when retryable. */
static cog_handler_result_t cli_on_llm_chunk(cog_core_t *core,
                                             const cog_event_t *event)
{
    cog_provider_t *provider = cog_core_provider(core);
    cog_session_t *session = cog_core_session(core);
    cog_cli_app_t *app = cli_app_from_core(core);
    cog_provider_response_t resp;
    char content[4096];
    cog_message_t msg;
    cog_event_t next;
    cog_handler_result_t r = {COG_TRANSITION_SELF, COG_OK};
    cog_result_t rc;

    if (core == NULL || event == NULL) {
        r.status = COG_ERROR_NULL_PTR;
        return r;
    }

    (void)memset(&resp, 0, sizeof(resp));
    resp.content = content;
    resp.content_capacity = sizeof(content);

    rc = cog_provider_receive(provider, &resp);
    if (rc != COG_OK) {
        if (rc == COG_ERROR_ABORTED) {
            /* Cooperative cancellation: drop the partial turn. */
            cli_discard_partial_turn(session);
            r.transition = CLI_STATE_IDLE;
            return r;
        }
        r.status = cli_handle_provider_error(core, rc);
        if (r.status == COG_OK) {
            /* Retry scheduled; stay in AWAITING_LLM. */
            r.transition = COG_TRANSITION_SELF;
        } else {
            r.transition = CLI_STATE_IDLE;
        }
        return r;
    }

    msg.role = COG_MESSAGE_ROLE_ASSISTANT;
    msg.content = content;
    msg.content_len = resp.content_len;
    msg.name = NULL;
    if (cog_session_append(session, &msg, NULL) != COG_OK) {
        r.status = COG_ERROR_IO;
        return r;
    }

    if (app != NULL && resp.content_len > 0U) {
        (void)fwrite(content, 1U, resp.content_len, app->out);
        (void)fflush(app->out);
    }

    next.id = (cog_event_id_t)(resp.is_complete ? COG_EVENT_LLM_COMPLETE
                                                : COG_EVENT_LLM_CHUNK);
    next.kind = resp.is_complete ? COG_EVENT_LLM_COMPLETE
                                 : COG_EVENT_LLM_CHUNK;
    next.payload = NULL;
    next.payload_len = 0U;
    next.source_id = 0U;
    if (cog_core_enqueue(core, &next) != COG_OK) {
        r.status = COG_ERROR_IO;
        return r;
    }
    return r;
}

/** AWAITING_LLM + LLM_COMPLETE: close the turn and return to IDLE. */
static cog_handler_result_t cli_on_llm_complete(cog_core_t *core,
                                                const cog_event_t *event)
{
    cog_cli_app_t *app = cli_app_from_core(core);
    cog_handler_result_t r = {CLI_STATE_IDLE, COG_OK};

    if (core == NULL || event == NULL) {
        r.status = COG_ERROR_NULL_PTR;
        return r;
    }
    if (app != NULL) {
        (void)fputc('\n', app->out);
        (void)fflush(app->out);
    }
    return r;
}

/** AWAITING_LLM + CANCEL: arm provider cancellation; the next receive
 * aborts and the chunk handler discards the partial turn. */
static cog_handler_result_t cli_on_cancel(cog_core_t *core,
                                          const cog_event_t *event)
{
    cog_handler_result_t r = {COG_TRANSITION_SELF, COG_OK};

    if (core == NULL || event == NULL) {
        r.status = COG_ERROR_NULL_PTR;
        return r;
    }
    (void)cog_mock_provider_request_cancel(cog_core_provider(core));
    return r;
}

/** IDLE + CANCEL: nothing in flight; no-op. */
static cog_handler_result_t cli_on_cancel_idle(cog_core_t *core,
                                               const cog_event_t *event)
{
    cog_handler_result_t r = {COG_TRANSITION_SELF, COG_OK};

    if (core == NULL || event == NULL) {
        r.status = COG_ERROR_NULL_PTR;
        return r;
    }
    return r;
}

/**
 * @brief Cancel the in-flight provider request and discard the partial
 *        assistant turn.
 *
 * Arms a one-shot provider cancellation (mirroring @c cli_on_cancel),
 * drains the resulting abort with a receive so the provider's in-flight
 * state is closed, and discards every trailing assistant node so the
 * session is back to the last user message.  Used by the SHUTDOWN
 * handler so the state machine returns to IDLE before the run loop
 * stops instead of abandoning the request mid-turn.
 *
 * @return COG_OK when the receive aborted as expected; an error code
 *         when the provider did not honour the cancel (the partial turn
 *         is still discarded either way, so shutdown stays bounded).
 */
static cog_result_t cli_cancel_inflight(cog_core_t *core)
{
    cog_provider_t *provider = cog_core_provider(core);
    cog_session_t *session = cog_core_session(core);
    cog_provider_response_t resp;
    char content[4096];
    cog_result_t rc;

    if (provider == NULL || session == NULL) {
        return COG_ERROR_INVALID_STATE;
    }

    /* Arm the one-shot cancel so the next receive aborts. */
    (void)cog_mock_provider_request_cancel(provider);

    /* Drain the abort.  The mock returns COG_ERROR_ABORTED immediately
     * once the cancel is armed; the receive also closes the provider's
     * in-flight state. */
    (void)memset(&resp, 0, sizeof(resp));
    resp.content = content;
    resp.content_capacity = sizeof(content);
    rc = cog_provider_receive(provider, &resp);

    /* Whatever the receive returned, the turn is being shut down: drop
     * the partial assistant nodes and close the turn. */
    cli_discard_partial_turn(session);

    return (rc == COG_ERROR_ABORTED) ? COG_OK : COG_ERROR_INVALID_STATE;
}

/** Any state + SHUTDOWN: cancel in-flight provider work, close the turn
 * back to IDLE, and stop the run loop. */
static cog_handler_result_t cli_on_shutdown(cog_core_t *core,
                                            const cog_event_t *event)
{
    cog_handler_result_t r = {COG_TRANSITION_SELF, COG_OK};

    if (core == NULL || event == NULL) {
        r.status = COG_ERROR_NULL_PTR;
        return r;
    }

    if (cog_core_state(core) == (cog_state_t)CLI_STATE_AWAITING_LLM) {
        /* An assistant turn is in flight: cancel the provider request,
         * drain the abort, and discard the partial turn so the state
         * machine returns to IDLE before the loop stops. */
        (void)cli_cancel_inflight(core);
        r.transition = CLI_STATE_IDLE;
    }

    cog_core_stop(core);
    return r;
}

/* ------------------------------------------------------------------ */
/* Static const dispatch table                                         */
/* ------------------------------------------------------------------ */

static const cog_state_t cli_idle_transitions[] = {
    CLI_STATE_AWAITING_LLM,
    COG_TRANSITION_SELF,
    COG_TRANSITION_END,
};

static const cog_state_t cli_awaiting_transitions[] = {
    CLI_STATE_IDLE,
    COG_TRANSITION_SELF,
    COG_TRANSITION_END,
};

static const cog_dispatch_entry_t cli_dispatch_table[] = {
    COG_DISPATCH_ENTRY((cog_state_t)CLI_STATE_IDLE,
                       (cog_event_id_t)COG_EVENT_USER_INPUT,
                       cli_on_user_input, cli_idle_transitions),
    COG_DISPATCH_ENTRY((cog_state_t)CLI_STATE_IDLE,
                       (cog_event_id_t)COG_EVENT_CANCEL,
                       cli_on_cancel_idle, cli_idle_transitions),
    COG_DISPATCH_ENTRY((cog_state_t)CLI_STATE_IDLE,
                       (cog_event_id_t)COG_EVENT_SHUTDOWN,
                       cli_on_shutdown, cli_idle_transitions),
    COG_DISPATCH_ENTRY((cog_state_t)CLI_STATE_AWAITING_LLM,
                       (cog_event_id_t)COG_EVENT_LLM_CHUNK,
                       cli_on_llm_chunk, cli_awaiting_transitions),
    COG_DISPATCH_ENTRY((cog_state_t)CLI_STATE_AWAITING_LLM,
                       (cog_event_id_t)COG_EVENT_LLM_COMPLETE,
                       cli_on_llm_complete, cli_awaiting_transitions),
    COG_DISPATCH_ENTRY((cog_state_t)CLI_STATE_AWAITING_LLM,
                       (cog_event_id_t)COG_EVENT_CANCEL,
                       cli_on_cancel, cli_awaiting_transitions),
    COG_DISPATCH_ENTRY((cog_state_t)CLI_STATE_AWAITING_LLM,
                       (cog_event_id_t)COG_EVENT_SHUTDOWN,
                       cli_on_shutdown, cli_awaiting_transitions),
};

/* ------------------------------------------------------------------ */
/* Obs sink                                                            */
/* ------------------------------------------------------------------ */

/** Deliver one formatted log record to a FILE stream. */
static cog_result_t cli_obs_deliver(void *user, const char *record,
                                    size_t record_len)
{
    FILE *f = (FILE *)user;

    if (f == NULL) {
        return COG_ERROR_IO;
    }
    if (fwrite(record, 1U, record_len, f) != record_len) {
        return COG_ERROR_IO;
    }
    if (fputc('\n', f) == EOF) {
        return COG_ERROR_IO;
    }
    return COG_OK;
}

/* ------------------------------------------------------------------ */
/* Wait hook                                                           */
/* ------------------------------------------------------------------ */

/** Handle one interactive line: strip the newline, honour exit commands,
 * and submit the rest as a user prompt. */
static void cli_handle_line(cog_cli_app_t *app)
{
    size_t len = strlen(app->line_buf);

    while (len > 0U && (app->line_buf[len - 1U] == '\n' ||
                        app->line_buf[len - 1U] == '\r')) {
        app->line_buf[len - 1U] = '\0';
        --len;
    }
    if (len == 0U) {
        return;
    }
    if (strcmp(app->line_buf, "exit") == 0 ||
        strcmp(app->line_buf, "/exit") == 0 ||
        strcmp(app->line_buf, "quit") == 0) {
        app->stop_requested = true;
        cog_core_stop(app->core);
        return;
    }
    (void)cog_cli_app_submit(app, app->line_buf, len);
}

/** Wait hook: single-prompt mode stops when the turn completes;
 * interactive mode polls stdin with a short timeout so a stop request
 * from a signal handler is honoured promptly. */
static void cli_wait_fn(void *arg)
{
    cog_cli_app_t *app = (cog_cli_app_t *)arg;
    fd_set rfds;
    struct timeval tv;
    int rc;

    if (app == NULL || app->stop_requested) {
        return;
    }

    if (!app->opts.interactive) {
        if (cog_cli_app_state(app) == (cog_state_t)CLI_STATE_IDLE) {
            app->stop_requested = true;
            cog_core_stop(app->core);
        }
        return;
    }

    FD_ZERO(&rfds);
    FD_SET(STDIN_FILENO, &rfds);
    tv.tv_sec = 0;
    tv.tv_usec = 100000; /* 100 ms */
    rc = select(STDIN_FILENO + 1, &rfds, NULL, NULL, &tv);
    if (rc < 0) {
        if (errno == EINTR) {
            return; /* signal; the loop re-checks stop */
        }
        app->stop_requested = true;
        cog_core_stop(app->core);
        return;
    }
    if (rc == 0 || !FD_ISSET(STDIN_FILENO, &rfds)) {
        return; /* timeout; the loop re-checks stop */
    }
    if (fgets(app->line_buf, sizeof(app->line_buf), stdin) == NULL) {
        /* EOF */
        app->stop_requested = true;
        cog_core_stop(app->core);
        return;
    }
    cli_handle_line(app);
}

/* ------------------------------------------------------------------ */
/* Mock script construction                                            */
/* ------------------------------------------------------------------ */

/**
 * @brief Build the owned mock script from the parsed options.
 *
 * STREAM mode turns the comma-separated @p chunks into one CHUNK step
 * each.  ERROR and RATE_LIMIT modes script one retryable error followed
 * by a recovery chunk.  CANCEL mode scripts an immediate abort.  The
 * steps array and the chunks string copy are owned by the app.
 */
static cog_result_t cli_build_script(cog_cli_app_t *app)
{
    const char *chunks = app->opts.chunks;
    size_t chunk_count = 1U;
    size_t step_count;
    size_t i;
    char *copy;
    char *piece;
    const char *p;

    /* A caller-supplied script takes precedence over the chunk/mode
     * options.  Its steps are borrowed, not owned. */
    if (app->opts.script != NULL) {
        app->script = *app->opts.script;
        return COG_OK;
    }

    if (chunks == NULL) {
        chunks = "Hello from cog-cli!";
    }

    for (p = chunks; *p != '\0'; ++p) {
        if (*p == ',') {
            ++chunk_count;
        }
    }

    switch (app->opts.mock_mode) {
    case COG_CLI_MOCK_ERROR:
    case COG_CLI_MOCK_RATE_LIMIT:
        step_count = 2U;
        break;
    case COG_CLI_MOCK_CANCEL:
        step_count = 1U;
        break;
    case COG_CLI_MOCK_STREAM:
    default:
        step_count = chunk_count;
        break;
    }

    app->steps = (cog_mock_step_t *)calloc(step_count,
                                           sizeof(cog_mock_step_t));
    if (app->steps == NULL) {
        return COG_ERROR_NOMEM;
    }
    app->step_count = step_count;

    copy = strdup(chunks);
    if (copy == NULL) {
        free(app->steps);
        app->steps = NULL;
        app->step_count = 0U;
        return COG_ERROR_NOMEM;
    }
    app->chunks_copy = copy;

    switch (app->opts.mock_mode) {
    case COG_CLI_MOCK_STREAM: {
        size_t idx = 0U;

        piece = copy;
        for (i = 0U; i < chunk_count; ++i) {
            char *comma = strchr(piece, ',');

            if (comma != NULL) {
                *comma = '\0';
            }
            app->steps[idx].kind = COG_MOCK_STEP_CHUNK;
            app->steps[idx].chunk = piece;
            ++idx;
            piece = (comma != NULL) ? comma + 1 : piece + strlen(piece);
        }
        break;
    }
    case COG_CLI_MOCK_ERROR:
        app->steps[0].kind = COG_MOCK_STEP_ERROR;
        app->steps[0].error_code = COG_ERROR_UNAVAILABLE;
        app->steps[0].http_status = 500;
        app->steps[0].retryable = true;
        app->steps[0].retry_after_ms = 10U;
        app->steps[0].error_message = "mock provider error";
        app->steps[1].kind = COG_MOCK_STEP_CHUNK;
        app->steps[1].chunk = "retried ok";
        break;
    case COG_CLI_MOCK_RATE_LIMIT:
        app->steps[0].kind = COG_MOCK_STEP_ERROR;
        app->steps[0].error_code = COG_ERROR_UNAVAILABLE;
        app->steps[0].http_status = 429;
        app->steps[0].retryable = true;
        app->steps[0].retry_after_ms = 10U;
        app->steps[0].error_message = "mock rate limited";
        app->steps[1].kind = COG_MOCK_STEP_CHUNK;
        app->steps[1].chunk = "recovered after rate limit";
        break;
    case COG_CLI_MOCK_CANCEL:
        app->steps[0].kind = COG_MOCK_STEP_CANCEL;
        break;
    }

    app->script.steps = app->steps;
    app->script.step_count = app->step_count;
    /* Replay the script for every turn so interactive mode produces the
     * same deterministic response each time. */
    app->script.repeat = SIZE_MAX;
    app->script.send_error = COG_OK;
    app->script.chunk_delay_ms = 0U;
    return COG_OK;
}

/* ------------------------------------------------------------------ */
/* Application lifecycle                                               */
/* ------------------------------------------------------------------ */

cog_result_t cog_cli_app_new(cog_cli_app_t **out,
                             const cog_cli_options_t *opts)
{
    cog_cli_app_t *app = NULL;
    cog_result_t rc;
    const cog_endpoint_t *ep = NULL;
    cog_endpoint_t ep_copy;
    cog_session_config_t sess_cfg;
    cog_obs_config_t obs_cfg;
    cog_core_config_t core_cfg;

    if (out == NULL || opts == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    *out = NULL;
    /* The real application loop is migrated by K; never dispatch the legacy
     * fixture path merely because an endpoint is named mock. */
    if (opts->backend != COG_CLI_BACKEND_MOCK) {
        return COG_ERROR_UNSUPPORTED;
    }
    if (opts->model_id == NULL || opts->endpoint_name == NULL) {
        return COG_ERROR_INVALID_ARG;
    }

    app = (cog_cli_app_t *)calloc(1U, sizeof(*app));
    if (app == NULL) {
        return COG_ERROR_NOMEM;
    }
    app->opts = *opts;
    app->out = stdout;

    /* Session. */
    (void)memset(&sess_cfg, 0, sizeof(sess_cfg));
    sess_cfg.id = app->opts.session_id;
    rc = cog_session_new(&app->session, &sess_cfg);
    if (rc != COG_OK) {
        goto fail;
    }

    /* Mock script. */
    rc = cli_build_script(app);
    if (rc != COG_OK) {
        goto fail;
    }

    /* Endpoint lookup + provider connect.  The script travels through
     * the endpoint's api_key field and is copied by the provider. */
    rc = cog_endpoint_table_lookup(&cli_endpoint_table,
                                   app->opts.endpoint_name, &ep);
    if (rc != COG_OK) {
        (void)fprintf(stderr, "cog-cli: unknown endpoint: %s\n",
                      app->opts.endpoint_name);
        goto fail;
    }
    ep_copy = *ep;
    ep_copy.api_key = (const char *)(const void *)&app->script;
    rc = cog_provider_connect(&ep_copy, &app->provider);
    if (rc != COG_OK) {
        goto fail;
    }

    /* Obs. */
    app->log_file = (app->opts.log_path != NULL)
                        ? fopen(app->opts.log_path, "a")
                        : stderr;
    if (app->log_file == NULL) {
        rc = COG_ERROR_IO;
        goto fail;
    }
    app->log_sink.name = "log";
    app->log_sink.deliver = cli_obs_deliver;
    app->log_sink.user = app->log_file;
    app->log_sink.min_severity = COG_OBS_TRACE;
    app->sink_table.sinks = &app->log_sink;
    app->sink_table.count = 1U;
    (void)memset(&obs_cfg, 0, sizeof(obs_cfg));
    obs_cfg.min_severity = app->opts.verbose ? COG_OBS_DEBUG : COG_OBS_INFO;
    obs_cfg.sinks = &app->sink_table;
    obs_cfg.service = "cog-cli";
    rc = cog_obs_new(&app->obs, &obs_cfg);
    if (rc != COG_OK) {
        goto fail;
    }

    /* Store (optional). */
    if (app->opts.store_dir != NULL) {
        cog_store_config_t store_cfg;

        (void)memset(&store_cfg, 0, sizeof(store_cfg));
        store_cfg.base_dir = app->opts.store_dir;
        store_cfg.auto_save = true;
        rc = cog_store_new(&app->store, &store_cfg);
        if (rc != COG_OK) {
            goto fail;
        }
    }

    /* IPC listener (optional). */
    if (app->opts.socket_path != NULL) {
        rc = cog_ipc_endpoint_new(&app->ipc);
        if (rc != COG_OK) {
            goto fail;
        }
        rc = cog_ipc_endpoint_listen(app->ipc, app->opts.socket_path);
        if (rc != COG_OK) {
            goto fail;
        }
    }

    /* Core.  Ownership of the IPC endpoint transfers to the core. */
    (void)memset(&core_cfg, 0, sizeof(core_cfg));
    core_cfg.session = app->session;
    core_cfg.dispatch_table = cli_dispatch_table;
    core_cfg.dispatch_table_size =
        sizeof(cli_dispatch_table) / sizeof(cli_dispatch_table[0]);
    core_cfg.provider = app->provider;
    core_cfg.obs = app->obs;
    core_cfg.store = app->store;
    core_cfg.ipc_endpoint = app->ipc;
    core_cfg.wait_fn = cli_wait_fn;
    core_cfg.wait_arg = app;
    /* Borrowed caller context: handlers retrieve the app through
     * cog_core_user; the core never owns or mutates it. */
    core_cfg.user = app;
    rc = cog_core_new(&app->core, &core_cfg);
    if (rc != COG_OK) {
        goto fail;
    }
    app->ipc = NULL; /* the core owns the endpoint now */

    *out = app;
    return COG_OK;

fail:
    cog_cli_app_free(app);
    return rc;
}

void cog_cli_app_free(cog_cli_app_t *app)
{
    if (app == NULL) {
        return;
    }
    if (app->core != NULL) {
        cog_core_free(app->core); /* also frees the IPC endpoint */
    }
    if (app->provider != NULL) {
        (void)cog_provider_disconnect(app->provider);
    }
    if (app->obs != NULL) {
        cog_obs_free(app->obs);
    }
    if (app->store != NULL) {
        cog_store_free(app->store);
    }
    if (app->session != NULL) {
        cog_session_free(app->session);
    }
    if (app->ipc != NULL) {
        /* Only reachable when cog_core_new failed before taking
         * ownership. */
        (void)cog_ipc_endpoint_free(app->ipc);
    }
    if (app->log_file != NULL && app->log_file != stderr) {
        (void)fclose(app->log_file);
    }
    free(app->steps);
    free(app->chunks_copy);
    free(app);
}

cog_result_t cog_cli_app_run(cog_cli_app_t *app)
{
    cog_result_t rc;

    if (app == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    app->stop_requested = false;

    if (!app->prompt_submitted) {
        app->prompt_submitted = true;
        if (app->opts.prompt != NULL) {
            rc = cog_cli_app_submit(app, app->opts.prompt,
                                    strlen(app->opts.prompt));
            if (rc != COG_OK) {
                return rc;
            }
        }
        if (app->opts.queue_text != NULL) {
            rc = cog_cli_app_queue(app, app->opts.queue_text,
                                   strlen(app->opts.queue_text));
            if (rc != COG_OK) {
                return rc;
            }
        }
    }

    rc = cog_core_run(app->core);
    return rc;
}

void cog_cli_app_stop(cog_cli_app_t *app)
{
    if (app == NULL) {
        return;
    }
    app->stop_requested = true;
    cog_core_stop(app->core);
}

/* ------------------------------------------------------------------ */
/* Event injection                                                     */
/* ------------------------------------------------------------------ */

cog_result_t cog_cli_app_submit(cog_cli_app_t *app, const char *text,
                                size_t len)
{
    cog_event_t ev;

    if (app == NULL || text == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    (void)memset(&ev, 0, sizeof(ev));
    ev.id = (cog_event_id_t)COG_EVENT_USER_INPUT;
    ev.kind = COG_EVENT_USER_INPUT;
    ev.payload = text;
    ev.payload_len = len;
    return cog_core_enqueue(app->core, &ev);
}

cog_result_t cog_cli_app_queue(cog_cli_app_t *app, const char *text,
                               size_t len)
{
    const char *sid;

    if (app == NULL || text == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    sid = cog_session_id(app->session);
    if (sid == NULL) {
        return COG_ERROR_INVALID_STATE;
    }
    return cog_core_queue_message(app->core, sid, text, len);
}

cog_result_t cog_cli_app_cancel(cog_cli_app_t *app)
{
    cog_event_t ev;

    if (app == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    (void)memset(&ev, 0, sizeof(ev));
    ev.id = (cog_event_id_t)COG_EVENT_CANCEL;
    ev.kind = COG_EVENT_CANCEL;
    return cog_core_enqueue(app->core, &ev);
}

cog_result_t cog_cli_app_shutdown(cog_cli_app_t *app)
{
    cog_event_t ev;

    if (app == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    (void)memset(&ev, 0, sizeof(ev));
    ev.id = (cog_event_id_t)COG_EVENT_SHUTDOWN;
    ev.kind = COG_EVENT_SHUTDOWN;
    return cog_core_enqueue(app->core, &ev);
}

/* ------------------------------------------------------------------ */
/* Accessors                                                           */
/* ------------------------------------------------------------------ */

cog_core_t *cog_cli_app_core(cog_cli_app_t *app)
{
    return (app != NULL) ? app->core : NULL;
}

cog_session_t *cog_cli_app_session(cog_cli_app_t *app)
{
    return (app != NULL) ? app->session : NULL;
}

cog_provider_t *cog_cli_app_provider(cog_cli_app_t *app)
{
    return (app != NULL) ? app->provider : NULL;
}

cog_obs_t *cog_cli_app_obs(cog_cli_app_t *app)
{
    return (app != NULL) ? app->obs : NULL;
}

cog_store_t *cog_cli_app_store(cog_cli_app_t *app)
{
    return (app != NULL) ? app->store : NULL;
}

cog_state_t cog_cli_app_state(cog_cli_app_t *app)
{
    return (app != NULL) ? cog_core_state(app->core) : (cog_state_t)0U;
}

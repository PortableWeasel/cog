/** @file app_turn.c @brief Static application transitions and fair service. */
#define _POSIX_C_SOURCE 200809L
#include "app_internal.h"
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/signalfd.h>
#include <unistd.h>

static void reset_turn(cog_cli_app_t *app)
{
    app->text_len = 0U; app->text[0] = '\0';
    app->opaque_count = 0U; app->opaque_used = 0U;
    app->call_count = 0U; app->tool_index = 0U;
    app->sequence = 0U; app->delta_ready = false;
    memset(&app->usage, 0, sizeof(app->usage));
    for (size_t i = 0U; i < CLI_CALLS; i++) {
        app->calls[i].begun = false; app->calls[i].ended = false;
        app->calls[i].arguments[0] = '\0';
        app->calls[i].view = (cog_message_tool_call_t){0};
    }
}
static bool add_bytes(size_t *total, size_t length, size_t limit)
{
    if (length > limit - *total) return false;
    *total += length; return true;
}
static cog_result_t context_size(cog_cli_app_t *app, size_t count)
{
    size_t size = app->workspace != NULL ? app->tools_json_len : 0U;
    size_t limit = app->opts.max_context_bytes;
    if (size > limit) return COG_ERROR_LIMIT;
    for (size_t i = 0U; i < count; i++) {
        const cog_message_t *m = &app->history[i];
        if (!add_bytes(&size, m->content_len, limit)) return COG_ERROR_LIMIT;
        if (m->name != NULL && !add_bytes(&size, strlen(m->name), limit)) return COG_ERROR_LIMIT;
        if (m->tool_call_id != NULL && !add_bytes(&size, strlen(m->tool_call_id), limit)) return COG_ERROR_LIMIT;
        for (size_t j = 0U; j < m->tool_call_count; j++) {
            const cog_message_tool_call_t *call = &m->tool_calls[j];
            if (!add_bytes(&size, strlen(call->id), limit) || !add_bytes(&size, strlen(call->name), limit) ||
                !add_bytes(&size, call->arguments_len, limit)) return COG_ERROR_LIMIT;
        }
        for (size_t j = 0U; j < m->opaque_count; j++)
            if (!add_bytes(&size, m->opaque[j].size, limit) ||
                !add_bytes(&size, strlen(m->opaque[j].namespace_id), limit) ||
                !add_bytes(&size, strlen(m->opaque[j].format), limit)) return COG_ERROR_LIMIT;
    }
    return COG_OK;
}
cog_result_t cog_cli_begin_request(cog_cli_app_t *app, bool reset)
{
    if (app->recovery || app->shutdown || app->cancelling) return COG_ERROR_INDETERMINATE;
    if (app->now - app->started_at >= app->opts.max_turn_ms) return COG_ERROR_TIMEOUT;
    size_t count = 0U;
    cog_result_t rc = cog_session_copy_history(app->session, NULL, app->history, CLI_NODES, &count);
    if (rc == COG_OK) rc = context_size(app, count);
    if (rc != COG_OK) return rc;
    cog_provider_request_t request = {.model_id = app->opts.model_id,
        .messages = app->history, .message_count = count, .stream = true,
        .max_tokens = app->opts.max_output_tokens,
        .configuration_generation = 1U};
    if (app->workspace != NULL) {
        request.tools_json = app->tools_json; request.tools_json_len = app->tools_json_len;
    }
    rc = cog_provider_send(app->provider, &request);
    if (rc != COG_OK) return rc;
    if (reset) reset_turn(app);
    app->provider_active = true;
    return COG_OK;
}
static cog_result_t append_turn(cog_cli_app_t *app, cog_message_status_t status, bool calls)
{
    cog_message_t message = {.role = COG_MESSAGE_ROLE_ASSISTANT,
        .content = app->text, .content_len = app->text_len, .status = status,
        .opaque = app->opaque_count == 0U ? NULL : app->opaque,
        .opaque_count = app->opaque_count, .usage = app->usage};
    if (calls) {
        for (size_t i = 0U; i < app->call_count; i++) app->call_views[i] = app->calls[i].view;
        message.tool_calls = app->call_views; message.tool_call_count = app->call_count;
    }
    cog_result_t rc = cog_session_append(app->session, &message, &app->assistant);
    if (rc != COG_OK) return rc;
    app->display_sequence++;
    rc = cog_cli_save(app);
    if (rc == COG_ERROR_INDETERMINATE) app->recovery = true;
    return rc;
}
cog_result_t cog_cli_finish_partial(cog_cli_app_t *app, cog_result_t reason)
{
    if (!app->active) return COG_OK;
    cog_result_t rc = append_turn(app, reason == COG_ERROR_ABORTED ? COG_MESSAGE_CANCELLED :
        COG_MESSAGE_INCOMPLETE, false);
    app->active = false; app->provider_active = false;
    if (fputc('\n', app->out) == EOF || fflush(app->out) != 0) return COG_ERROR_IO;
    return rc;
}
static cog_result_t process_finish(cog_cli_app_t *app, cog_state_t *next)
{
    const cog_provider_delta_t *delta = &app->delta;
    app->provider_active = false;
    cog_result_t outcome = delta->outcome;
    if (outcome != COG_OK && outcome != COG_ERROR_ABORTED && !app->cancelling && !app->shutdown) {
        cog_provider_error_t error = {0};
        cog_result_t rc = cog_provider_get_error(app->provider, &error);
        uint32_t delay = 100U;
        if (rc == COG_OK && error.retry_after_ms != 0U) delay = error.retry_after_ms;
        if (delay > 5000U) delay = 5000U;
        uint64_t elapsed = app->now - app->started_at;
        if (rc == COG_OK && error.retryable && error.replay_safe && !error.output_accepted &&
            !app->accepted && !app->effects && app->retries < app->opts.max_retries &&
            elapsed < app->opts.max_retry_elapsed_ms &&
            delay < app->opts.max_retry_elapsed_ms - elapsed &&
            elapsed < app->opts.max_turn_ms && delay < app->opts.max_turn_ms - elapsed) {
            app->retries++; app->retry_at = app->now + delay; *next = CLI_BACKOFF;
            return COG_OK;
        }
    }
    if (outcome == COG_OK && app->deferred_error != COG_OK) outcome = app->deferred_error;
    if (outcome == COG_OK && (delta->finish_reason == COG_FINISH_REASON_LENGTH ||
        delta->finish_reason == COG_FINISH_REASON_CONTENT_FILTER)) outcome = COG_ERROR_LIMIT;
    if (outcome == COG_OK && delta->finish_reason != COG_FINISH_REASON_STOP &&
        delta->finish_reason != COG_FINISH_REASON_TOOL_CALLS) outcome = COG_ERROR_PROTOCOL;
    if (outcome == COG_OK && delta->finish_reason == COG_FINISH_REASON_TOOL_CALLS) {
        if (app->call_count == 0U) outcome = COG_ERROR_PROTOCOL;
        for (size_t i = 0U; i < app->call_count; i++)
            if (!app->calls[i].begun || !app->calls[i].ended) outcome = COG_ERROR_PROTOCOL;
    } else if (outcome == COG_OK && app->call_count != 0U) outcome = COG_ERROR_PROTOCOL;
    if (outcome != COG_OK) {
        cog_result_t saved = cog_cli_finish_partial(app, outcome);
        *next = CLI_IDLE;
        return saved != COG_OK ? saved : outcome == COG_ERROR_ABORTED ? COG_OK : outcome;
    }
    cog_result_t rc = append_turn(app, COG_MESSAGE_COMPLETE, app->call_count != 0U);
    if (rc != COG_OK) return rc;
    if (app->call_count != 0U) {
        app->rounds++;
        if (app->rounds > app->opts.max_tool_rounds) {
            app->deferred_error = COG_ERROR_LIMIT; app->cancelling = true;
        }
        *next = CLI_POLICY;
    } else {
        app->active = false; *next = CLI_IDLE;
        if (fputc('\n', app->out) == EOF || fflush(app->out) != 0) return COG_ERROR_IO;
    }
    return COG_OK;
}
static cog_result_t copy_opaque(cog_cli_app_t *app, const cog_message_opaque_t *opaque)
{
    if (app->opaque_count == CLI_OPAQUE || opaque->namespace_id == NULL || opaque->format == NULL ||
        (opaque->data == NULL && opaque->size != 0U)) return COG_ERROR_LIMIT;
    size_t a = strlen(opaque->namespace_id) + 1U, b = strlen(opaque->format) + 1U;
    size_t remaining = app->opts.max_context_bytes - app->opaque_used;
    if (a > remaining || b > remaining - a || opaque->size > remaining - a - b) return COG_ERROR_LIMIT;
    char *p = app->opaque_storage + app->opaque_used;
    cog_message_opaque_t value = {.namespace_id = p, .format = p + a,
        .data = (unsigned char *)p + a + b, .size = opaque->size};
    memcpy(p, opaque->namespace_id, a); memcpy(p + a, opaque->format, b);
    if (opaque->size != 0U) memcpy(p + a + b, opaque->data, opaque->size);
    app->opaque[app->opaque_count++] = value;
    app->opaque_used += a + b + opaque->size;
    return COG_OK;
}
static cog_result_t accept_delta(cog_cli_app_t *app, cog_state_t *next)
{
    const cog_provider_delta_t *d = &app->delta;
    if (d->sequence != app->sequence + 1U || app->sequence == UINT64_MAX) return COG_ERROR_PROTOCOL;
    app->sequence = d->sequence;
    if (d->kind == COG_PROVIDER_DELTA_FINISH) return process_finish(app, next);
    if (d->kind == COG_PROVIDER_DELTA_USAGE) {
        if (d->usage.currency != NULL) {
            size_t n = strlen(d->usage.currency);
            if (n >= sizeof(app->currency)) return COG_ERROR_LIMIT;
            memcpy(app->currency, d->usage.currency, n + 1U);
        }
        app->usage = d->usage;
        if (d->usage.currency != NULL) app->usage.currency = app->currency;
        return COG_OK;
    }
    app->accepted = true;
    if (d->kind == COG_PROVIDER_DELTA_TEXT) {
        if ((d->data == NULL && d->data_len != 0U) ||
            !cog_cli_text_valid(d->data, d->data_len)) return COG_ERROR_PROTOCOL;
        if (d->data_len > app->opts.max_context_bytes - app->text_len) return COG_ERROR_LIMIT;
        if (d->data_len != 0U) memcpy(app->text + app->text_len, d->data, d->data_len);
        app->text_len += d->data_len; app->text[app->text_len] = '\0';
        app->display_sequence++;
        if ((d->data_len != 0U && fwrite(d->data, 1U, d->data_len, app->out) != d->data_len) || fflush(app->out) != 0)
            return COG_ERROR_IO;
        return COG_OK;
    }
    if (d->kind == COG_PROVIDER_DELTA_OPAQUE) return copy_opaque(app, &d->opaque);
    if (d->tool_index >= CLI_CALLS) return COG_ERROR_LIMIT;
    cli_call_t *call = &app->calls[d->tool_index];
    if (d->kind == COG_PROVIDER_DELTA_TOOL_BEGIN) {
        if (call->begun || d->tool_call_id == NULL || d->tool_name == NULL ||
            *d->tool_call_id == '\0' || *d->tool_name == '\0' ||
            strlen(d->tool_call_id) >= CLI_ID_BYTES || strlen(d->tool_name) >= CLI_ID_BYTES)
            return COG_ERROR_PROTOCOL;
        for (size_t i = 0U; i < CLI_CALLS; i++) if (app->calls[i].begun &&
            strcmp(app->calls[i].id, d->tool_call_id) == 0) return COG_ERROR_PROTOCOL;
        strcpy(call->id, d->tool_call_id); strcpy(call->name, d->tool_name);
        call->begun = true;
        call->view = (cog_message_tool_call_t){call->id, call->name, call->arguments, 0U};
        if (app->call_count <= d->tool_index) app->call_count = d->tool_index + 1U;
        return COG_OK;
    }
    if (!call->begun || call->ended || d->tool_call_id == NULL || strcmp(call->id, d->tool_call_id) != 0)
        return COG_ERROR_PROTOCOL;
    if (d->kind == COG_PROVIDER_DELTA_TOOL_ARGUMENTS) {
        if (d->data == NULL || d->data_len >= CLI_ARGUMENT_BYTES - call->view.arguments_len)
            return COG_ERROR_LIMIT;
        memcpy(call->arguments + call->view.arguments_len, d->data, d->data_len);
        call->view.arguments_len += d->data_len; call->arguments[call->view.arguments_len] = '\0';
        return COG_OK;
    }
    if (d->kind == COG_PROVIDER_DELTA_TOOL_END) { call->ended = true; return COG_OK; }
    return COG_ERROR_PROTOCOL;
}
static cog_handler_result_t input(cog_core_t *core, const cog_event_t *event)
{
    cog_cli_app_t *app = cog_core_user(core);
    cog_handler_result_t r = {COG_TRANSITION_SELF, COG_OK};
    cli_input_t *slot = NULL;
    for (size_t i = 0U; i < CLI_INPUTS; i++)
        if (event->payload == app->inputs[i].text && app->inputs[i].used) slot = &app->inputs[i];
    if (app->active) {
        if (slot != NULL) slot->queued = true;
        else r.status = cog_cli_app_queue(app, event->payload, event->payload_len);
        return r;
    }
    if (app->shutdown || app->recovery) { r.status = COG_ERROR_INDETERMINATE; return r; }
    cog_message_t message = {.role = COG_MESSAGE_ROLE_USER, .content = event->payload,
        .content_len = event->payload_len};
    cog_session_node_t *active = cog_session_active(app->session);
    if (active != NULL && cog_node_data(active)->message->status != COG_MESSAGE_COMPLETE) {
        cog_session_node_t *safe = cog_node_parent(active), *fork = NULL;
        if (safe == NULL) { r.status = COG_ERROR_INVALID_STATE; return r; }
        r.status = cog_session_fork(app->session, safe, &fork);
        if (r.status == COG_OK) r.status = cog_session_add_message(app->session, fork, &message, NULL);
    } else r.status = cog_session_append(app->session, &message, NULL);
    if (slot != NULL) slot->used = false;
    if (r.status != COG_OK) return r;
    app->display_sequence++;
    app->active = true; app->accepted = false; app->effects = false;
    app->cancelling = false; app->retries = 0U; app->rounds = 0U;
    app->deferred_error = COG_OK; app->started_at = app->now;
    app->safe_ancestor = cog_session_active(app->session);
    r.status = cog_cli_save(app);
    if (r.status == COG_OK) r.status = cog_cli_begin_request(app, true);
    if (r.status == COG_OK) r.transition = CLI_MODEL;
    else app->active = false;
    return r;
}
static cog_handler_result_t advance(cog_core_t *core, const cog_event_t *event)
{
    cog_cli_app_t *app = cog_core_user(core);
    cog_handler_result_t r = {cog_core_state(core), COG_OK};
    (void)event;
    app->event_queued = false;
    if (r.transition == CLI_MODEL) {
        if (!app->delta_ready) { r.status = COG_ERROR_INVALID_STATE; return r; }
        r.status = accept_delta(app, &r.transition);
        app->delta_ready = false;
    } else if (r.transition == CLI_BACKOFF) {
        if (app->cancelling) {
            r.status = cog_cli_finish_partial(app, COG_ERROR_ABORTED); r.transition = CLI_IDLE;
            if (r.status == COG_OK) r.status = app->deferred_error;
            return r;
        }
        if (app->now < app->retry_at) return r;
        r.status = cog_cli_begin_request(app, true); r.transition = CLI_MODEL;
    } else {
        r.status = cog_cli_tools_step(app, &r.transition);
        if (r.status == COG_ERROR_AGAIN) r.status = COG_OK;
        if (r.transition == CLI_IDLE && r.status == COG_OK && app->deferred_error != COG_OK)
            r.status = app->deferred_error;
    }
    return r;
}
static cog_handler_result_t cancel(cog_core_t *core, const cog_event_t *event)
{
    cog_cli_app_t *app = cog_core_user(core);
    cog_handler_result_t r = {COG_TRANSITION_SELF, COG_OK};
    if (event->kind == COG_EVENT_SHUTDOWN) app->shutdown = true;
    if (!app->active) {
        if (app->shutdown) cog_core_stop(core);
        return r;
    }
    app->cancelling = true;
    if (app->provider_active) r.status = cog_provider_cancel(app->provider);
    if (app->execution != NULL) r.status = cog_cli_tools_cancel(app);
    if (cog_core_state(core) == CLI_BACKOFF) {
        r.status = cog_cli_finish_partial(app, COG_ERROR_ABORTED); r.transition = CLI_IDLE;
    }
    return r;
}
static cog_handler_result_t stale(cog_core_t *core, const cog_event_t *event)
{
    cog_cli_app_t *app = cog_core_user(core);
    (void)event;
    app->event_queued = false; app->delta_ready = false;
    return (cog_handler_result_t){COG_TRANSITION_SELF, COG_OK};
}
static const cog_state_t transitions[] = {CLI_IDLE, CLI_MODEL, CLI_BACKOFF, CLI_POLICY,
    CLI_TOOL, COG_TRANSITION_SELF, COG_TRANSITION_END};
#define ROW(state, event, handler) COG_DISPATCH_ENTRY(state, event, handler, transitions)
#define COMMON(state) ROW(state, COG_EVENT_USER_INPUT, input), ROW(state, COG_EVENT_CANCEL, cancel), ROW(state, COG_EVENT_SHUTDOWN, cancel)
static const cog_dispatch_entry_t cog_cli_dispatch[] = {
    COMMON(CLI_IDLE), COMMON(CLI_MODEL), COMMON(CLI_BACKOFF), COMMON(CLI_POLICY), COMMON(CLI_TOOL),
    ROW(CLI_MODEL, COG_EVENT_LLM_CHUNK, advance), ROW(CLI_BACKOFF, COG_EVENT_RETRY, advance),
    ROW(CLI_POLICY, COG_EVENT_TOOL_RESULT, advance), ROW(CLI_TOOL, COG_EVENT_TOOL_RESULT, advance),
    ROW(CLI_IDLE, COG_EVENT_LLM_CHUNK, stale), ROW(CLI_IDLE, COG_EVENT_RETRY, stale),
    ROW(CLI_IDLE, COG_EVENT_TOOL_RESULT, stale)};
const cog_dispatch_entry_t *cog_cli_dispatch_table(size_t *count)
{
    *count = sizeof(cog_cli_dispatch) / sizeof(cog_cli_dispatch[0]);
    return cog_cli_dispatch;
}

static cog_result_t schedule(cog_cli_app_t *app, cog_event_kind_t kind)
{
    if (app->event_queued) return COG_OK;
    cog_event_t event = {.id = (cog_event_id_t)kind, .kind = kind};
    cog_result_t rc = cog_core_enqueue(app->core, &event);
    if (rc == COG_OK) app->event_queued = true;
    return rc == COG_ERROR_OVERFLOW ? COG_OK : rc;
}
static cog_result_t read_signal(cog_cli_app_t *app)
{
    if (app->signal_fd < 0) return COG_OK;
    struct signalfd_siginfo info;
    ssize_t n = read(app->signal_fd, &info, sizeof(info));
    if (n == (ssize_t)sizeof(info)) {
        app->shutdown = true;
        cog_cli_app_stop(app);
    } else if (n < 0 && errno != EAGAIN && errno != EINTR) return COG_ERROR_IO;
    return COG_OK;
}
cog_result_t cog_cli_service(void *arg, uint64_t now, uint64_t *deadline)
{
    cog_cli_app_t *app = arg;
    app->now = now; *deadline = UINT64_MAX;
    cog_result_t rc = read_signal(app);
    if (rc != COG_OK) return rc;
    if (app->active && now - app->started_at >= app->opts.max_turn_ms && !app->cancelling) {
        app->deferred_error = COG_ERROR_TIMEOUT;
        app->cancelling = true;
        if (app->provider_active) rc = cog_provider_cancel(app->provider);
        if (rc == COG_OK && app->execution != NULL) rc = cog_cli_tools_cancel(app);
        if (rc != COG_OK) return rc;
    }
    uint64_t next = UINT64_MAX;
    rc = cog_provider_poll(app->provider, now, &next);
    if (rc != COG_OK) return rc;
    if (app->provider_active && next < *deadline) *deadline = next;
    if (app->active && app->started_at + app->opts.max_turn_ms < *deadline)
        *deadline = app->started_at + app->opts.max_turn_ms;
    cog_state_t state = cog_core_state(app->core);
    if (state == CLI_MODEL && app->provider_active) {
        if (!app->delta_ready) {
            size_t required = 0U;
            rc = cog_provider_next(app->provider, &app->delta, app->delta_storage,
                app->delta_capacity, &required);
            if (rc == COG_OK) app->delta_ready = true;
            else if (rc != COG_ERROR_AGAIN) return rc == COG_ERROR_OVERFLOW ? COG_ERROR_LIMIT : rc;
        }
        if (app->delta_ready) return schedule(app, COG_EVENT_LLM_CHUNK);
    } else if (state == CLI_BACKOFF) {
        if (app->cancelling) return schedule(app, COG_EVENT_RETRY);
        if (app->retry_at < *deadline) *deadline = app->retry_at;
        if (now >= app->retry_at) return schedule(app, COG_EVENT_RETRY);
    } else if (state == CLI_POLICY) {
        if (!app->policy_prompt || app->policy_decision != COG_TOOLKIT_POLICY_UNDECIDED || app->cancelling)
            return schedule(app, COG_EVENT_TOOL_RESULT);
    } else if (state == CLI_TOOL) {
        rc = cog_toolkit_execution_poll(app->execution, now, &app->tool_done, &next);
        if (rc != COG_OK) return rc;
        if (next < *deadline) *deadline = next;
        if (app->tool_done) return schedule(app, COG_EVENT_TOOL_RESULT);
    }
    if (!app->active && !app->event_queued && !app->shutdown) {
        cli_input_t *oldest = NULL;
        bool outstanding = false;
        for (size_t i = 0U; i < CLI_INPUTS; i++) if (app->inputs[i].used) {
            outstanding = true;
            if (app->inputs[i].queued && (oldest == NULL || app->inputs[i].order < oldest->order))
                oldest = &app->inputs[i];
        }
        if (oldest != NULL) {
            cog_event_t event = {.id = COG_EVENT_USER_INPUT, .kind = COG_EVENT_USER_INPUT,
                .payload = oldest->text, .payload_len = oldest->length};
            rc = cog_core_enqueue(app->core, &event);
            if (rc == COG_OK) oldest->queued = false;
            return rc == COG_ERROR_OVERFLOW ? COG_OK : rc;
        }
        size_t pending = 0U;
        rc = cog_core_pending_count(app->core, cog_session_id(app->session), &pending);
        if (rc != COG_OK) return rc;
        if (!outstanding && pending == 0U && (!app->opts.interactive || app->stdin_eof))
            cog_core_stop(app->core);
    }
    if (app->shutdown && !app->active) cog_core_stop(app->core);
    return COG_OK;
}
static cog_result_t line(cog_cli_app_t *app)
{
    while (app->line_length != 0U && app->line[app->line_length - 1U] == '\r') app->line_length--;
    app->line[app->line_length] = '\0';
    cog_result_t rc = COG_OK;
    if (app->policy_prompt) {
        if (strcmp(app->line, "allow") == 0) app->policy_decision = COG_TOOLKIT_POLICY_ALLOW;
        else if (strcmp(app->line, "deny") == 0) app->policy_decision = COG_TOOLKIT_POLICY_DENY;
        else (void)fprintf(stderr, "Type allow or deny: ");
    } else if (strcmp(app->line, "/quit") == 0 || strcmp(app->line, "/exit") == 0)
        rc = cog_cli_app_shutdown(app);
    else if (strcmp(app->line, "/cancel") == 0) rc = cog_cli_app_cancel(app);
    else if (app->line_length != 0U) rc = cog_cli_app_submit(app, app->line, app->line_length);
    app->line_length = 0U;
    return rc;
}
cog_result_t cog_cli_idle(void *arg, uint32_t max_wait_ms)
{
    cog_cli_app_t *app = arg;
    struct pollfd fds[2];
    nfds_t count = 0U;
    if (app->opts.interactive && !app->stdin_eof) fds[count++] = (struct pollfd){STDIN_FILENO, POLLIN, 0};
    if (app->signal_fd >= 0) fds[count++] = (struct pollfd){app->signal_fd, POLLIN, 0};
    int ready = poll(fds, count, (int)max_wait_ms);
    if (ready < 0) return errno == EINTR ? COG_OK : COG_ERROR_IO;
    cog_result_t rc = read_signal(app);
    if (rc != COG_OK) return rc;
    if (count != 0U && fds[0].fd == STDIN_FILENO && (fds[0].revents & (POLLIN | POLLHUP)) != 0) {
        char byte;
        ssize_t n = read(STDIN_FILENO, &byte, 1U);
        if (n == 0) {
            app->stdin_eof = true;
            if (app->policy_prompt) app->policy_decision = COG_TOOLKIT_POLICY_DENY;
            else if (app->line_length != 0U) return line(app);
        } else if (n < 0) return errno == EINTR || errno == EAGAIN ? COG_OK : COG_ERROR_IO;
        else if (byte == '\n') return line(app);
        else if (app->line_length + 1U >= sizeof(app->line)) return COG_ERROR_LIMIT;
        else app->line[app->line_length++] = byte;
    }
    return COG_OK;
}
cog_result_t cog_cli_drain(cog_cli_app_t *app)
{
    if (!app->active && app->execution == NULL && !app->provider_active) return COG_OK;
    app->shutdown = true; app->cancelling = true;
    cog_result_t rc = cog_cli_clock(&app->now);
    if (rc != COG_OK) return rc;
    if (app->provider_active) {
        rc = cog_provider_cancel(app->provider);
        if (rc != COG_OK) return rc;
        /* Every retained delta is bounded by provider limits. Preserve visible
           text and opaque snapshots, then store an explicit incomplete turn. */
        for (size_t i = 0U; i < CLI_CALLS * 8U + 64U; i++) {
            size_t required = 0U;
            rc = cog_provider_next(app->provider, &app->delta, app->delta_storage,
                app->delta_capacity, &required);
            if (rc == COG_ERROR_AGAIN) {
                uint64_t deadline;
                rc = cog_provider_poll(app->provider, app->now, &deadline);
                if (rc != COG_OK) return rc;
                continue;
            }
            if (rc != COG_OK) break;
            if (app->delta.kind == COG_PROVIDER_DELTA_FINISH) break;
            cog_state_t next = CLI_MODEL;
            rc = accept_delta(app, &next);
            if (rc != COG_OK) break;
        }
        app->provider_active = false;
        cog_result_t saved = cog_cli_finish_partial(app,
            app->deferred_error != COG_OK ? app->deferred_error : COG_ERROR_ABORTED);
        if (saved != COG_OK) return saved;
        if (rc != COG_OK && rc != COG_ERROR_INVALID_STATE) return rc;
    }
    if (app->execution != NULL) {
        rc = cog_cli_tools_cancel(app);
        if (rc != COG_OK) return rc;
        while (!app->tool_done) {
            uint64_t deadline;
            rc = cog_cli_clock(&app->now);
            if (rc == COG_OK) rc = cog_toolkit_execution_poll(app->execution, app->now,
                &app->tool_done, &deadline);
            if (rc != COG_OK) return rc;
            if (!app->tool_done) (void)poll(NULL, 0U, 1);
        }
        cog_state_t next = CLI_POLICY;
        rc = cog_cli_tools_step(app, &next);
        if (rc != COG_OK) return rc;
    }
    /* A completed assistant's unstarted proposals are known to have no effects.
       Close them explicitly so cancellation never strands PROPOSED history. */
    while (app->active && app->assistant != NULL && app->tool_index < app->call_count && !app->recovery) {
        cog_state_t next = CLI_POLICY;
        rc = cog_cli_tools_step(app, &next);
        if (rc != COG_OK) return rc;
    }
    app->active = false;
    return rc;
}

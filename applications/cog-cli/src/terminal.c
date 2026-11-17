/** @file terminal.c @brief CLI-only terminal compatibility adapter. */
#define _POSIX_C_SOURCE 200809L
#include <cog_cli/cli.h>
#include "app_internal.h"
#include "execution_internal.h"
#include <stdio.h>
#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include <sys/signalfd.h>
#include <unistd.h>
extern cog_result_t cli_read_signal(cog_cli_app_t *app);
static cog_result_t line(cog_cli_app_t *app)
{
    while (app->line_length != 0U && app->line[app->line_length - 1U] == '\r') app->line_length--;
    app->line[app->line_length] = '\0';
    if (!cog_cli_text_valid(app->line, app->line_length)) {
        app->line_length = 0U; return COG_ERROR_INVALID_ARG;
    }
    cog_result_t rc = COG_OK;
    bool reload_handled = false;
    rc = cog_cli_reload_command(app, app->line, &reload_handled);
    if (reload_handled || rc != COG_OK) { app->line_length = 0U; return rc; }
    if (app->policy_prompt) {
        if (strcmp(app->line, "allow") == 0) app->policy_decision = COG_TOOLKIT_POLICY_ALLOW;
        else if (strcmp(app->line, "deny") == 0) app->policy_decision = COG_TOOLKIT_POLICY_DENY;
        else if (fprintf(stderr, "Type allow or deny: ") < 0 || fflush(stderr) != 0)
            rc = COG_ERROR_IO;
    } else if (strcmp(app->line, "/quit") == 0 || strcmp(app->line, "/exit") == 0)
        rc = cog_cli_app_shutdown(app);
    else if (strcmp(app->line, "/cancel") == 0) rc = cog_cli_app_cancel(app);
    else if (app->line_length != 0U) {
        bool handled;
        rc = cog_cli_session_command(app, app->line, &handled);
        if (handled && (rc == COG_ERROR_INVALID_ARG || rc == COG_ERROR_INVALID_STATE ||
            rc == COG_ERROR_NOT_FOUND || rc == COG_ERROR_AGAIN || rc == COG_ERROR_DENIED)) {
            rc = fprintf(stderr, "cog-cli: command refused\n") < 0 ? COG_ERROR_IO : COG_OK;
        }
        if (rc == COG_OK && !handled) rc = cog_cli_app_submit(app, app->line, app->line_length);
    }
    app->line_length = 0U;
    return rc;
}
static cog_result_t terminal_idle(void *arg, uint32_t max_wait_ms)
{
    cog_cli_app_t *app = arg;
    struct pollfd fds[2];
    nfds_t count = 0U;
    if (app->opts.interactive && !app->stdin_eof) fds[count++] = (struct pollfd){STDIN_FILENO, POLLIN, 0};
    if (app->signal_fd >= 0) fds[count++] = (struct pollfd){app->signal_fd, POLLIN, 0};
    int ready = poll(fds, count, (int)max_wait_ms);
    if (ready < 0) return errno == EINTR ? COG_OK : COG_ERROR_IO;
    cog_result_t rc = cli_read_signal(app);
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

static cog_result_t list_sessions(cog_cli_app_t *app)
{
    if (app->store == NULL) return COG_ERROR_INVALID_STATE;
    char **ids = NULL; size_t count = 0U, bytes = 0U;
    cog_result_t rc = cog_store_list(app->store, &ids, &count);
    if (rc != COG_OK) return rc;
    if (count > CLI_LIST_IDS) rc = COG_ERROR_LIMIT;
    for (size_t i = 0U; i < count && rc == COG_OK; i++) {
        size_t n = strlen(ids[i]);
        if (n == 0U || n > 127U || !cog_cli_text_valid(ids[i], n) ||
            strchr(ids[i], '/') != NULL || strchr(ids[i], '\\') != NULL ||
            strcmp(ids[i], ".") == 0 || strcmp(ids[i], "..") == 0) { rc = COG_ERROR_PROTOCOL; break; }
        for (size_t j = 0U; j < n; j++) if ((unsigned char)ids[i][j] < 32U || ids[i][j] == 127) rc = COG_ERROR_PROTOCOL;
        if (n + 1U > app->opts.max_context_bytes - bytes) rc = COG_ERROR_LIMIT;
        else bytes += n + 1U;
    }
    if (rc == COG_OK) for (size_t i = 0U; i < count; i++)
        if (fprintf(app->out, "%s\n", ids[i]) < 0) { rc = COG_ERROR_IO; break; }
    if (rc == COG_OK && fflush(app->out) != 0) rc = COG_ERROR_IO;
    for (size_t i = 0U; i < count; i++) free(ids[i]);
    free(ids);
    return rc;
}
static cog_result_t history(cog_cli_app_t *app)
{
    cog_cli_display_snapshot_t snapshot;
    cog_result_t rc = cog_cli_app_display_snapshot(app, &snapshot);
    if (rc != COG_OK) return rc;
    for (size_t i = 0U; i < snapshot.item_count; i++) {
        const cog_cli_display_item_t *item = &snapshot.items[i];
        static const char *const roles[] = {"system", "user", "assistant", "tool"};
        static const char *const states[] = {"complete", "incomplete", "cancelled", "failed"};
        if (fprintf(app->out, "[node=%" PRIu64 " %s %s] ", item->node_id,
            roles[item->role], states[item->status]) < 0 ||
            (item->text_len != 0U && fwrite(item->text, 1U, item->text_len, app->out) != item->text_len) ||
            fputc('\n', app->out) == EOF) return COG_ERROR_IO;
    }
    return fflush(app->out) == 0 ? COG_OK : COG_ERROR_IO;
}
cog_result_t cog_cli_usage(cog_cli_app_t *app, const cog_session_node_t *node,
    void *stream, bool explicit_request)
{
    if (node == NULL) return COG_ERROR_NOT_FOUND;
    const cog_node_data_t *data = cog_node_data(node);
    const cog_usage_t *usage = &data->message->usage;
    (void)app;
    if (!usage->tokens_known && !usage->reasoning_tokens_known && !usage->cost_known) {
        if (explicit_request && (fprintf(stream, "node=%" PRIu64 " usage unavailable\n", data->node_id) < 0 ||
            fflush(stream) != 0)) return COG_ERROR_IO;
        return COG_OK;
    }
    if (fprintf(stream, "node=%" PRIu64 " reported usage:", data->node_id) < 0) return COG_ERROR_IO;
    if (usage->tokens_known && fprintf(stream, " input=%" PRIu64 " output=%" PRIu64,
        usage->input_tokens, usage->output_tokens) < 0) return COG_ERROR_IO;
    if (usage->reasoning_tokens_known && fprintf(stream, " reasoning=%" PRIu64, usage->reasoning_tokens) < 0)
        return COG_ERROR_IO;
    if (usage->cost_known && fprintf(stream, " cost=%.17g %s", usage->cost, usage->currency) < 0)
        return COG_ERROR_IO;
    return fputc('\n', stream) != EOF && fflush(stream) == 0 ? COG_OK : COG_ERROR_IO;
}
static cog_result_t escaped_id(FILE *stream, const char *text)
{
    if (fputc('"', stream) == EOF) return COG_ERROR_IO;
    for (const unsigned char *p = (const unsigned char *)text; *p != 0U; p++) {
        if (*p == '"' || *p == '\\') {
            if (fputc('\\', stream) == EOF || fputc(*p, stream) == EOF) return COG_ERROR_IO;
        } else if (*p == '\n') {
            if (fputs("\\n", stream) < 0) return COG_ERROR_IO;
        } else if (*p == '\r') {
            if (fputs("\\r", stream) < 0) return COG_ERROR_IO;
        } else if (*p == '\t') {
            if (fputs("\\t", stream) < 0) return COG_ERROR_IO;
        } else if (*p < 32U || *p >= 127U) {
            if (fprintf(stream, "\\x%02x", (unsigned int)*p) < 0) return COG_ERROR_IO;
        } else if (fputc(*p, stream) == EOF) return COG_ERROR_IO;
    }
    return fputc('"', stream) == EOF ? COG_ERROR_IO : COG_OK;
}
static cog_result_t tools(cog_cli_app_t *app)
{
    cog_session_snapshot_t snapshot;
    cog_result_t rc = cog_session_snapshot_view(app->session, &snapshot);
    if (rc != COG_OK) return rc;
    static const char *const states[] = {"proposed", "started", "succeeded", "failed", "denied", "cancelled", "indeterminate", "acknowledged"};
    for (size_t i = 0U; i < snapshot.ledger_count; i++) {
        const cog_tool_ledger_record_t *entry = &snapshot.ledger[i];
        cog_session_node_t *node = cog_cli_session_node(app, entry->assistant_node_id);
        if (node == NULL) return COG_ERROR_PROTOCOL;
        const cog_message_t *message = cog_node_data(node)->message;
        size_t ordinal = 0U;
        for (size_t j = 0U; j < message->tool_call_count; j++)
            if (strcmp(message->tool_calls[j].id, entry->call_id) == 0) { ordinal = j + 1U; break; }
        if (ordinal == 0U) return COG_ERROR_PROTOCOL;
        if (fprintf(app->out, "node=%" PRIu64 " ordinal=%zu call=",
            entry->assistant_node_id, ordinal) < 0) return COG_ERROR_IO;
        rc = escaped_id(app->out, entry->call_id);
        if (rc != COG_OK) return rc;
        if (fprintf(app->out, " state=%s\n", states[entry->state]) < 0) return COG_ERROR_IO;
    }
    return fflush(app->out) == 0 ? COG_OK : COG_ERROR_IO;
}
static bool decimal(const char *text, uint64_t *value)
{
    uint64_t result = 0U;
    if (*text == '\0') return false;
    for (const char *p = text; *p != '\0'; p++) {
        if (*p < '0' || *p > '9' || result > (UINT64_MAX - (uint64_t)(*p - '0')) / 10U) return false;
        result = result * 10U + (uint64_t)(*p - '0');
    }
    *value = result; return true;
}
cog_result_t cog_cli_session_command(cog_cli_app_t *app, const char *text, bool *handled)
{
    *handled = true;
    bool recovery = app->opts.session_action == COG_CLI_SESSION_RECOVER;
    if (strcmp(text, "/history") == 0) return history(app);
    if (strcmp(text, "/tools") == 0) return tools(app);
    if (strcmp(text, "/help") == 0) {
        const char *help = recovery ? "/tools /history /reconcile NODE CALL OUTCOME EVIDENCE /reconcile-index NODE ORDINAL OUTCOME EVIDENCE /quit\n" :
            "/sessions /history /tools /fork NODE /usage /cancel /quit\n";
        return fputs(help, app->out) >= 0 && fflush(app->out) == 0 ? COG_OK : COG_ERROR_IO;
    }
    bool indexed = strncmp(text, "/reconcile-index ", sizeof("/reconcile-index ") - 1U) == 0;
    if (indexed || strncmp(text, "/reconcile ", sizeof("/reconcile ") - 1U) == 0) {
        if (!recovery) return COG_ERROR_INVALID_STATE;
        const char *arguments = text + (indexed ? sizeof("/reconcile-index ") - 1U : sizeof("/reconcile ") - 1U);
        char copy[4096]; size_t n = strlen(arguments);
        if (n >= sizeof(copy)) return COG_ERROR_LIMIT;
        memcpy(copy, arguments, n + 1U);
        char *node = copy, *call = strchr(node, ' ');
        if (call == NULL) return COG_ERROR_INVALID_ARG;
        *call++ = '\0'; char *outcome = strchr(call, ' ');
        if (outcome == NULL) return COG_ERROR_INVALID_ARG;
        *outcome++ = '\0'; char *evidence = strchr(outcome, ' ');
        if (evidence == NULL) return COG_ERROR_INVALID_ARG;
        *evidence++ = '\0'; uint64_t id;
        if (!decimal(node, &id)) return COG_ERROR_INVALID_ARG;
        cog_tool_execution_state_t state;
        if (strcmp(outcome, "succeeded") == 0) state = COG_TOOL_EXECUTION_SUCCEEDED;
        else if (strcmp(outcome, "failed") == 0) state = COG_TOOL_EXECUTION_FAILED;
        else if (strcmp(outcome, "cancelled") == 0) state = COG_TOOL_EXECUTION_CANCELLED;
        else if (strcmp(outcome, "acknowledged") == 0) state = COG_TOOL_EXECUTION_ACKNOWLEDGED;
        else return COG_ERROR_INVALID_ARG;
        const char *call_id = call;
        if (indexed) {
            uint64_t ordinal;
            if (!decimal(call, &ordinal) || ordinal == 0U) return COG_ERROR_INVALID_ARG;
            cog_session_node_t *assistant = cog_cli_session_node(app, id);
            if (assistant == NULL) return COG_ERROR_NOT_FOUND;
            const cog_message_t *message = cog_node_data(assistant)->message;
            if (message->role != COG_MESSAGE_ROLE_ASSISTANT || ordinal > message->tool_call_count)
                return COG_ERROR_NOT_FOUND;
            call_id = message->tool_calls[(size_t)ordinal - 1U].id;
        }
        return cog_cli_app_reconcile(app, id, call_id, state, evidence, strlen(evidence));
    }
    if (recovery) return COG_ERROR_INVALID_STATE;
    if (strcmp(text, "/sessions") == 0) return list_sessions(app);
    if (strncmp(text, "/fork ", 6U) == 0) {
        uint64_t id;
        if (!decimal(text + 6, &id)) return COG_ERROR_INVALID_ARG;
        return cog_cli_app_fork(app, id);
    }
    if (strcmp(text, "/usage") == 0) {
        bool found = false;
        for (cog_session_node_t *node = cog_session_active(app->session); node != NULL; node = cog_node_parent(node))
            if (cog_node_data(node)->message->role == COG_MESSAGE_ROLE_ASSISTANT) {
                found = true; cog_result_t rc = cog_cli_usage(app, node, app->out, true);
                if (rc != COG_OK) return rc;
            }
        return found ? COG_OK : COG_ERROR_NOT_FOUND;
    }
    *handled = text[0] == '/' && strcmp(text, "/quit") != 0 && strcmp(text, "/exit") != 0 && strcmp(text, "/cancel") != 0;
    return *handled ? COG_ERROR_INVALID_ARG : COG_OK;
}
static cog_result_t unresolved(cog_cli_app_t *app)
{
    cog_session_snapshot_t snapshot;
    cog_result_t rc = cog_session_snapshot_view(app->session, &snapshot);
    if (rc != COG_OK) return rc;
    for (size_t i = 0U; i < snapshot.ledger_count; i++)
        if (snapshot.ledger[i].state == COG_TOOL_EXECUTION_INDETERMINATE ||
            snapshot.ledger[i].state == COG_TOOL_EXECUTION_STARTED) return COG_ERROR_INDETERMINATE;
    return COG_OK;
}
cog_result_t cog_cli_metadata_run(cog_cli_app_t *app)
{
    if (app->opts.session_action == COG_CLI_SESSION_LIST) {
        if (atomic_load_explicit(&app->stop_requested, memory_order_acquire)) return COG_OK;
        return list_sessions(app);
    }
    if (!app->opts.interactive) return COG_ERROR_INDETERMINATE;
    if (app->recovery_write_failed) return COG_ERROR_INVALID_STATE;
    if (fputs("Recovery is offline. Your --fenced assertion does not stop old processes.\n"
        "Use /tools, /history, /reconcile NODE CALL OUTCOME EVIDENCE, or /quit.\n"
        "For escaped IDs use /reconcile-index NODE ORDINAL OUTCOME EVIDENCE (1-based original call order).\n", stderr) < 0 ||
        fflush(stderr) != 0) return COG_ERROR_IO;
    cog_result_t rc = COG_OK;
    while (!atomic_load_explicit(&app->stop_requested, memory_order_acquire)) {
        struct pollfd fds[2] = {{STDIN_FILENO, POLLIN, 0}, {app->signal_fd, POLLIN, 0}};
        int ready = poll(fds, app->signal_fd < 0 ? 1U : 2U, 10);
        if (ready < 0) { if (errno == EINTR) continue; rc = COG_ERROR_IO; break; }
        if ((fds[0].revents & (POLLERR | POLLNVAL)) != 0 ||
            (app->signal_fd >= 0 && (fds[1].revents & (POLLERR | POLLNVAL | POLLHUP)) != 0)) {
            rc = COG_ERROR_IO; break;
        }
        if (app->signal_fd >= 0 && (fds[1].revents & POLLIN) != 0) {
            struct signalfd_siginfo info;
            ssize_t got = read(app->signal_fd, &info, sizeof(info));
            if (got == (ssize_t)sizeof(info)) break;
            if (got < 0 && (errno == EAGAIN || errno == EINTR)) continue;
            rc = COG_ERROR_IO; break;
        }
        if ((fds[0].revents & (POLLIN | POLLHUP)) == 0) continue;
        char byte; ssize_t n = read(STDIN_FILENO, &byte, 1U);
        if (n == 0) break;
        if (n < 0) { if (errno == EINTR || errno == EAGAIN) continue; rc = COG_ERROR_IO; break; }
        if (byte != '\n') {
            if (app->line_length + 1U >= sizeof(app->line)) { rc = COG_ERROR_LIMIT; break; }
            app->line[app->line_length++] = byte; continue;
        }
        while (app->line_length != 0U && app->line[app->line_length - 1U] == '\r') app->line_length--;
        app->line[app->line_length] = '\0';
        bool valid = cog_cli_text_valid(app->line, app->line_length);
        app->line_length = 0U;
        if (!valid) { rc = COG_ERROR_INVALID_ARG; break; }
        if (strcmp(app->line, "/quit") == 0 || strcmp(app->line, "/exit") == 0) break;
        bool handled;
        rc = cog_cli_session_command(app, app->line, &handled);
        if (rc == COG_ERROR_IO || rc == COG_ERROR_INDETERMINATE || app->recovery_write_failed) break;
        if (rc != COG_OK && fprintf(stderr, "cog-cli: recovery command refused: %s\n", cog_cli_result_name(rc)) < 0) { rc = COG_ERROR_IO; break; }
        rc = COG_OK;
    }
    atomic_store_explicit(&app->stop_requested, false, memory_order_release);
    return rc == COG_OK ? unresolved(app) : rc;
}

cog_result_t cog_cli_reload_command(cog_cli_app_t *app, const char *text, bool *handled)
{
    if (app == NULL || text == NULL || handled == NULL) return COG_ERROR_NULL_PTR;
    *handled = strncmp(text, "/reload", 7U) == 0;
    if (!*handled) return COG_OK;
    cog_cli_reload_options_t options = {0};
    static const struct { const char *name; size_t length; unsigned field; } commands[] = {
        {"/reload model ", 14U, 0U}, {"/reload base-url ", 17U, 1U}, {"/reload credential-ref ", 23U, 2U}};
    for (size_t i = 0U; i < sizeof(commands) / sizeof(commands[0]); i++)
        if (strncmp(text, commands[i].name, commands[i].length) == 0) {
            const char *value = text + commands[i].length;
            if (commands[i].field == 0U) options.model_id = value;
            else if (commands[i].field == 1U) options.base_url = value;
            else options.credential_ref = value;
            break;
        }
    uint64_t generation = 0U;
    cog_result_t rc = cog_cli_app_reload(app, &options, &generation);
    if (app->reload_cleanup_error != COG_OK) return app->reload_cleanup_error;
    if (app->observation_error != COG_OK) return app->observation_error;
    int written = rc == COG_OK ? fprintf(stderr, "cog-cli: configuration published generation=%" PRIu64 "\n", generation) :
        fprintf(stderr, "cog-cli: reload refused: %s\n", cog_cli_result_name(rc));
    return written < 0 || fflush(stderr) != 0 ? COG_ERROR_IO : COG_OK;
}

static cog_result_t terminal_open(cog_cli_app_t *app)
{
 app->out=stdout;
 if(cog_cli_metadata(app))return COG_OK;
 app->log_file=app->opts.log_path!=NULL ? fopen(app->opts.log_path,"a") : stderr;
 if(app->log_file==NULL) return COG_ERROR_IO;
 if(app->log_file!=stderr && setvbuf(app->log_file,app->log_buffer,_IOFBF,sizeof(app->log_buffer))!=0) return COG_ERROR_IO;
 return COG_OK;
}
static cog_result_t terminal_close(cog_cli_app_t *app)
{
 cog_result_t rc=COG_OK;
 if(app->log_file!=NULL && app->log_file!=stderr && fclose(app->log_file)!=0) rc=COG_ERROR_IO;
 app->log_file=NULL;return rc;
}
static cog_result_t terminal_log(cog_cli_app_t *app,const char *text,size_t bytes)
{
 if(fwrite(text,1U,bytes,app->log_file)==bytes && fputc('\n',app->log_file)!=EOF && fflush(app->log_file)==0) return COG_OK;
 if(app->observation_error==COG_OK) app->observation_error=COG_ERROR_IO;
 return COG_ERROR_IO;
}
static cog_result_t terminal_text(cog_cli_app_t *app,const char *text,size_t bytes)
{
 return (bytes==0U || fwrite(text,1U,bytes,app->out)==bytes) && fflush(app->out)==0 ? COG_OK : COG_ERROR_IO;
}
static cog_result_t terminal_usage(cog_cli_app_t *app,const cog_session_node_t *node)
{ return cog_cli_usage(app,node,stderr,false); }
static cog_result_t terminal_ask(cog_cli_app_t *app,const cog_message_tool_call_t *call)
{
 (void)app;
 return fprintf(stderr,"\nAllow tool %s %.*s? Type allow or deny: ",call->name,(int)call->arguments_len,call->arguments)>=0 && fflush(stderr)==0 ? COG_OK : COG_ERROR_IO;
}
static void terminal_error(cog_cli_app_t *app,const char *label,cog_result_t error)
{ (void)app; (void)fprintf(stderr,"cog-cli: %s: %s\n",label,cog_cli_result_name(error)); }
static const cli_frontend_t terminal = { terminal_open,terminal_close,terminal_log,terminal_text,terminal_usage,terminal_ask,terminal_error,terminal_idle,cog_cli_metadata_run };
cog_result_t cog_cli_app_new(cog_cli_app_t **out,const cog_cli_options_t *options)
{ return cli_app_new_frontend(out,options,&terminal); }

void cog_cli_app_free(cog_cli_app_t *app)
{
 if(app==NULL)return;
 cog_result_t rc=app->headless==NULL ? cli_app_dispose(&app,COG_OK) : cog_cli_execution_close(&app,COG_OK);
 if(rc!=COG_OK)terminal_error(NULL,"application cleanup",rc);
}

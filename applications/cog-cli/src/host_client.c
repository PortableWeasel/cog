/** @file host_client.c @brief Bounded authenticated view/receipt client. */
#define _POSIX_C_SOURCE 200809L
#include "view_internal.h"
#include "codec_internal.h"
#include "core_ipc.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

static cog_result_t client_clock(cog_cli_host_client_t *c)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0 || ts.tv_sec < 0 ||
        ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000L ||
        (uint64_t)ts.tv_sec > UINT64_MAX / 1000U) return COG_ERROR_IO;
    uint64_t seconds_ms = (uint64_t)ts.tv_sec * 1000U;
    uint64_t fraction_ms = (uint64_t)ts.tv_nsec / 1000000U;
    if (fraction_ms > UINT64_MAX - seconds_ms) return COG_ERROR_IO;
    uint64_t now = seconds_ms + fraction_ms;
    if (now < c->now_ms) return COG_ERROR_IO;
    c->now_ms = now;
    return COG_OK;
}
static cog_result_t deadline(cog_cli_host_client_t *c)
{
    if (c->now_ms > UINT64_MAX - c->fixed.reconnect_ms) return COG_ERROR_LIMIT;
    uint64_t next = c->now_ms + c->fixed.reconnect_ms;
    if (c->deadline_ms == 0U || next < c->deadline_ms) c->deadline_ms = next;
    return COG_OK;
}
static bool pending(const cli_client_request_t *r)
{
    return r->phase == CLI_CLIENT_REQUEST_PENDING || r->phase == CLI_CLIENT_REQUEST_FINAL;
}
static bool unknown(const cog_cli_host_client_t *c)
{
    return c->requests[0].phase == CLI_CLIENT_REQUEST_PENDING ||
        c->requests[1].phase == CLI_CLIENT_REQUEST_PENDING;
}
static cog_result_t finish(cog_cli_host_client_t *c, cog_result_t rc)
{
    if (c->primary == COG_OK) c->primary = rc;
    c->phase = CLI_CLIENT_DONE;
    return c->primary;
}
static cog_result_t output(cog_cli_host_client_t *c, const char *bytes, size_t length)
{
    if (fwrite(bytes, 1U, length, c->fixed.output) != length ||
        fflush(c->fixed.output) != 0 || ferror(c->fixed.output)) return COG_ERROR_IO;
    return COG_OK;
}
/* Keep format literals at snprintf call sites so both strict compilers verify
 * every argument without a compiler-specific format attribute. */
static bool formatted(cog_cli_host_client_t *c, int n)
{
    if (n < 0 || (size_t)n >= c->render_capacity - c->render_length) return false;
    c->render_length += (size_t)n;
    return true;
}
#define format(c, ...) ((c)->render_length < (c)->render_capacity && \
    formatted((c), snprintf((c)->render + (c)->render_length, \
        (c)->render_capacity - (c)->render_length, __VA_ARGS__)))
static bool quoted(cog_cli_host_client_t *c, const char *s, size_t length)
{
    if (!format(c, "\"")) return false;
    for (size_t i = 0U; i < length; ++i) {
        unsigned char ch = (unsigned char)s[i];
        if (ch < 32U || ch == 127U) {
            if (!format(c, "\\u%04x", (unsigned int)ch)) return false;
        } else if (ch == '"' || ch == '\\') {
            if (!format(c, "\\%c", (int)ch)) return false;
        } else {
            if (c->render_length + 1U >= c->render_capacity) return false;
            c->render[c->render_length++] = (char)ch;
        }
    }
    return format(c, "\"");
}
static cog_result_t render(cog_cli_host_client_t *c, const cog_cli_projection_view_t *v, bool tools)
{
    c->render_length = 0U;
    if (c->fixed.terminal && !format(c, "\033[2J\033[H")) return COG_ERROR_LIMIT;
    if (!format(c, "{\"type\":\"replacement\",\"session\":") ||
        !quoted(c, v->display.session_id, strlen(v->display.session_id)) ||
        !format(c, ",\"generation\":\"%" PRIu64 "\",\"sequence\":\"%" PRIu64
            "\",\"configuration\":[\"%" PRIu64 "\",\"%" PRIu64 "\"],\"items\":[",
            v->display.generation, v->display.through_sequence,
            v->active_configuration, v->published_configuration)) return COG_ERROR_LIMIT;
    for (size_t i = 0U; !tools && i < v->display.item_count; ++i) {
        const cog_cli_display_item_t *r = &v->display.items[i];
        if (!format(c, "%s[\"%" PRIu64 "\",%d,%d,", i == 0U ? "" : ",",
                r->node_id, (int)r->role, (int)r->status) || !quoted(c, r->text, r->text_len) ||
            !format(c, "]")) return COG_ERROR_LIMIT;
    }
    if (!format(c, "],\"agents\":[")) return COG_ERROR_LIMIT;
    for (size_t i = 0U; !tools && i < v->agent_count; ++i) {
        const cog_cli_agent_view_t *a = &v->agents[i];
        if (!format(c, "%s[\"%" PRIu64 "\",\"%" PRIu64 "\",\"%" PRIu64
                "\",\"%" PRIu64 "\",", i == 0U ? "" : ",", a->agent_id,
                a->execution_epoch, a->group_id, a->configuration_generation) ||
            !quoted(c, a->session_id, strlen(a->session_id)) ||
            !format(c, ",%d,%d,", (int)a->status, (int)a->outcome) ||
            !quoted(c, a->status_text, strlen(a->status_text)) || !format(c, ",") ||
            !quoted(c, a->result_text, strlen(a->result_text)) || !format(c, "]")) return COG_ERROR_LIMIT;
    }
    if (!format(c, "],\"policies\":[")) return COG_ERROR_LIMIT;
    for (size_t i = 0U; i < v->policy_count; ++i) {
        const cog_cli_policy_view_t *p = &v->policies[i];
        if (!format(c, "%s[", i == 0U ? "" : ",") ||
            !quoted(c, p->session_id, strlen(p->session_id)) ||
            !format(c, ",\"%" PRIu64 "\",%u,\"%" PRIu64 "\",\"%" PRIu64
                "\",\"%" PRIu64 "\",", p->assistant_node, (unsigned int)p->call_ordinal,
                p->nonce, p->configuration_generation, p->deadline_ms) ||
            !quoted(c, p->tool_name, strlen(p->tool_name)) || !format(c, "]")) return COG_ERROR_LIMIT;
    }
    if (!format(c, "]}\n")) return COG_ERROR_LIMIT;
    return output(c, c->render, c->render_length);
}
static cog_result_t metadata(cog_cli_host_client_t *c, cog_result_t result, uint64_t serial)
{
    c->render_length = 0U;
    if (!format(c, "{\"type\":\"command-result\",\"input\":\"%" PRIu64 "\",\"result\":%d}\n",
        serial, (int)result)) return COG_ERROR_LIMIT;
    return output(c, c->render, c->render_length);
}
static cog_cli_control_message_t envelope(const cog_cli_host_client_t *c,
    cog_cli_control_kind_t kind, cog_cli_control_lane_t lane, uint64_t wire, uint64_t reply)
{
    cog_cli_control_message_t m = {0};
    m.kind = kind; m.lane = lane; m.runtime_incarnation = c->runtime;
    m.sender_incarnation = c->discovered ? c->lease : c->nonce;
    m.wire_id = wire; m.reply_to = reply; m.session_id = c->session;
    return m;
}
static cog_result_t request(cog_cli_host_client_t *c, cli_client_request_t *r,
    cog_cli_control_kind_t kind, cog_cli_control_lane_t lane, const void *body, size_t length)
{
    size_t index = (size_t)lane - 1U;
    if (pending(r)) return COG_ERROR_AGAIN;
    /* Reserve exact ACK identity before admitting the original frame. */
    if (c->next_wire[index] > UINT64_MAX - 2U) return COG_ERROR_LIMIT;
    uint64_t wire = c->next_wire[index] + 1U;
    cog_cli_control_message_t m = envelope(c, kind, lane, wire, 0U);
    m.payload = body; m.payload_len = length;
    size_t size = 0U;
    cog_result_t rc = cog_cli_control_encode(&m, r->frame, r->frame_capacity, &size);
    if (rc != COG_OK) return rc;
    r->phase = CLI_CLIENT_REQUEST_PENDING; r->lane = lane; r->wire = wire;
    r->ack_wire = wire + 1U; r->result_wire = 0U; r->frame_length = size;
    r->ack_length = 0U; r->sent = false; r->ack_sent = false;
    r->result = COG_OK; r->assigned_lease = 0U; r->related_agent = 0U;
    c->next_wire[index] = r->ack_wire;
    return COG_OK;
}
static cog_result_t hello(cog_cli_host_client_t *c)
{
    if (pending(&c->hello)) { c->hello.sent = false; c->hello.ack_sent = false; return COG_OK; }
    unsigned char body[18];
    cli_wire_writer_t w = {body, 0U, true};
    cli_wire_put(&w, 1U, 1U); cli_wire_put(&w, c->fixed.watch ? 2U : 1U, 1U);
    cli_wire_put(&w, c->replay_generation, 8U); cli_wire_put(&w, c->rendered_sequence, 8U);
    return request(c, &c->hello, COG_CLI_CONTROL_UI_HELLO, COG_CLI_LANE_COMMAND, body, sizeof(body));
}
static cog_result_t send_request(cog_cli_host_client_t *c, cli_client_request_t *r)
{
    if (!pending(r)) return COG_ERROR_AGAIN;
    bool final = r->phase == CLI_CLIENT_REQUEST_FINAL && r->sent;
    bool *sent = final ? &r->ack_sent : &r->sent;
    if (*sent) return COG_ERROR_AGAIN;
    cog_ipc_message_t m = {final ? r->ack : r->frame,
        final ? r->ack_length : r->frame_length, final ? sizeof(r->ack) : r->frame_capacity};
    cog_result_t rc = cog_ipc_endpoint_send(c->endpoint, &m);
    if (rc == COG_OK) *sent = true;
    return rc;
}
static cog_result_t command(cog_cli_host_client_t *c, const cog_cli_command_t *cmd, uint64_t serial)
{
    cli_client_request_t *r = &c->requests[cmd->kind == COG_CLI_COMMAND_CANCEL ? 1U : 0U];
    if (pending(r)) return COG_ERROR_AGAIN;
    size_t bytes = 0U;
    cog_result_t rc = cog_cli_command_encode(cmd, c->command_body, c->command_capacity, &bytes);
    if (rc != COG_OK) return rc;
    rc = request(c, r, COG_CLI_CONTROL_UI_COMMAND,
        cmd->kind == COG_CLI_COMMAND_CANCEL ? COG_CLI_LANE_CANCEL : COG_CLI_LANE_COMMAND,
        c->command_body, bytes);
    if (rc == COG_OK) { r->opcode = cmd->kind; r->input_serial = serial; }
    return rc;
}
static cog_result_t result(cog_cli_host_client_t *c, const cog_cli_control_message_t *m)
{
    if (m->payload_len != 21U) return COG_ERROR_PROTOCOL;
    cli_wire_reader_t reader = {m->payload, m->payload_len, 0U, true};
    cog_result_t code = cli_wire_signed(cli_wire_get(&reader, 4U));
    uint64_t state = cli_wire_get(&reader, 1U);
    uint64_t related = cli_wire_get(&reader, 8U), assigned = cli_wire_get(&reader, 8U);
    if (!cli_wire_result(code) || (state != 1U && state != 2U)) return COG_ERROR_PROTOCOL;
    cli_client_request_t *r = NULL;
    cli_client_request_t *slots[3] = {&c->hello, &c->requests[0], &c->requests[1]};
    for (size_t i = 0U; i < 3U; ++i)
        if (slots[i]->phase != CLI_CLIENT_REQUEST_FREE && slots[i]->lane == m->lane &&
            (m->reply_to == slots[i]->wire || m->reply_to == slots[i]->ack_wire)) r = slots[i];
    if (r == NULL) return COG_OK; /* No stale response grants authority. */
    if (m->reply_to == r->ack_wire) {
        if (r->phase == CLI_CLIENT_REQUEST_ACKED) return COG_OK;
        if (r->phase != CLI_CLIENT_REQUEST_FINAL || state != 2U ||
            code != COG_OK || related != 0U || assigned != 0U) return COG_ERROR_PROTOCOL;
        r->phase = CLI_CLIENT_REQUEST_ACKED;
        return COG_OK;
    }
    if (state == 1U) {
        if (assigned != 0U || (r == &c->hello)) return COG_ERROR_PROTOCOL;
        return COG_OK;
    }
    if (r->phase != CLI_CLIENT_REQUEST_PENDING) {
        if (m->wire_id != r->result_wire || code != r->result ||
            related != r->related_agent || assigned != r->assigned_lease) return COG_ERROR_PROTOCOL;
        if (r->phase == CLI_CLIENT_REQUEST_FINAL) r->ack_sent = false;
        return COG_OK;
    }
    if (r == &c->hello) {
        if (code != COG_OK) return unknown(c) ? COG_ERROR_INDETERMINATE : code;
        if (assigned == 0U || related != 0U || (c->discovered && assigned != c->lease))
            return COG_ERROR_PROTOCOL;
        if (!c->discovered) {
            c->runtime = m->runtime_incarnation; c->lease = assigned;
            memcpy(c->session, m->session_id, strlen(m->session_id) + 1U);
            c->discovered = true;
        }
    } else if (assigned != 0U) return COG_ERROR_PROTOCOL;
    cog_cli_control_message_t ack = envelope(c, COG_CLI_CONTROL_COMMAND_ACK, r->lane, r->ack_wire, m->wire_id);
    size_t size = 0U;
    cog_result_t rc = cog_cli_control_encode(&ack, r->ack, sizeof(r->ack), &size);
    if (rc != COG_OK) return rc;
    r->result = code; r->related_agent = related; r->assigned_lease = assigned;
    r->result_wire = m->wire_id; r->phase = CLI_CLIENT_REQUEST_FINAL;
    r->ack_length = size; r->ack_sent = false;
    if (r != &c->hello && r->input_serial != 0U) {
        if (r->input_serial == c->last_input_serial) c->final_input_result = code;
        return metadata(c, code, r->input_serial);
    }
    return COG_OK;
}
static cog_result_t cursor(cog_cli_host_client_t *c)
{
    size_t index = (size_t)COG_CLI_LANE_REPLAY - 1U;
    if (c->next_wire[index] == UINT64_MAX) return COG_ERROR_LIMIT;
    unsigned char body[CLI_CLIENT_SMALL_FRAME]; size_t bytes = 0U;
    cog_ipc_replay_record_t r = {COG_IPC_REPLAY_CURSOR, c->session, c->replay_generation,
        c->rendered_sequence, 0U, 0U, 0U, NULL, 0U};
    cog_result_t rc = cog_ipc_replay_encode(&r, body, sizeof(body), &bytes);
    if (rc != COG_OK) return rc;
    cog_cli_control_message_t m = envelope(c, COG_CLI_CONTROL_REPLAY, COG_CLI_LANE_REPLAY,
        c->next_wire[index] + 1U, 0U);
    m.payload = body; m.payload_len = bytes;
    rc = cog_cli_control_encode(&m, c->cursor, sizeof(c->cursor), &c->cursor_length);
    if (rc == COG_OK) { ++c->next_wire[index]; c->cursor_pending = true; }
    return rc;
}
static cog_result_t install(cog_cli_host_client_t *c, const void *bytes, size_t length,
    uint64_t generation, uint64_t sequence, bool resync)
{
    if (c->cursor_pending) return COG_ERROR_PROTOCOL; /* receive is gated until copied */
    cli_view_bank_t *b = &c->banks[1U - c->current];
    size_t ni = 0U, na = 0U, np = 0U, nt = 0U;
    cog_result_t rc = cog_cli_projection_decode(bytes, length, &b->view, b->items,
        c->item_capacity, b->agents, CLI_VIEW_AGENTS, b->policies, CLI_VIEW_POLICIES,
        b->text, c->fixed.projection_bytes, &ni, &na, &np, &nt);
    if (rc != COG_OK) return rc == COG_ERROR_OVERFLOW ? COG_ERROR_LIMIT : rc;
    const cog_cli_display_snapshot_t *d = &b->view.display;
    if (strcmp(d->session_id, c->session) != 0 || d->generation != generation ||
        (!resync && d->through_sequence != sequence) ||
        (resync && d->through_sequence < c->gap_last) ||
        (c->have_view && d->generation == c->replay_generation && d->through_sequence < c->rendered_sequence))
        return COG_ERROR_PROTOCOL;
    rc = render(c, &b->view, false);
    if (rc != COG_OK) return rc;
    c->current = 1U - c->current; c->have_view = true;
    c->replay_generation = d->generation; c->rendered_sequence = d->through_sequence;
    c->awaiting_resync = false;
    if (resync) {
        /* The ordinary slot may still hold an effect result during reconnect.
         * Defer RESYNC until that exact receipt is reclaimed. gap_last remains
         * a local marker of this rendered replacement, not a new authority. */
        c->gap_last = d->through_sequence;
    }
    return cursor(c);
}
static cog_result_t receive_frame(cog_cli_host_client_t *c, size_t length)
{
    cog_cli_control_message_t m; size_t required = 0U;
    cog_result_t rc = cog_cli_control_decode(c->receive, length, &m,
        c->envelope_storage, c->envelope_capacity, &required);
    if (rc != COG_OK) return rc;
    if (m.worker.incarnation != 0U || m.worker.number != 0U || m.owner_peer != 0U ||
        m.agent_id != 0U || m.execution_epoch != 0U ||
        m.sender_incarnation != m.runtime_incarnation) return COG_ERROR_PROTOCOL;
    if (c->discovered && (m.runtime_incarnation != c->runtime || strcmp(m.session_id, c->session) != 0))
        return unknown(c) ? COG_ERROR_INDETERMINATE : COG_ERROR_PROTOCOL;
    if (!c->discovered && (m.kind != COG_CLI_CONTROL_COMMAND_RESULT ||
        m.reply_to != c->hello.wire || m.lane != COG_CLI_LANE_COMMAND)) return COG_ERROR_PROTOCOL;
    if (m.kind == COG_CLI_CONTROL_COMMAND_RESULT) return result(c, &m);
    if (m.kind == COG_CLI_CONTROL_RESYNC_VIEW && m.lane == COG_CLI_LANE_REPLAY) {
        if (!c->awaiting_resync) return COG_ERROR_PROTOCOL;
        return install(c, m.payload, m.payload_len, c->replay_generation, 0U, true);
    }
    if (m.kind != COG_CLI_CONTROL_REPLAY || m.lane != COG_CLI_LANE_REPLAY) return COG_ERROR_PROTOCOL;
    cog_ipc_replay_record_t r;
    rc = cog_ipc_replay_decode(m.payload, m.payload_len, &r,
        c->replay_storage, c->replay_capacity, &required);
    if (rc != COG_OK) return rc;
    if (strcmp(r.session_id, c->session) != 0) return COG_ERROR_PROTOCOL;
    if (r.kind == COG_IPC_REPLAY_GAP) {
        if (c->have_view && r.generation != c->replay_generation) return COG_ERROR_PROTOCOL;
        c->awaiting_resync = true; c->replay_generation = r.generation; c->gap_last = r.last_sequence;
        return deadline(c);
    }
    if (r.kind != COG_IPC_REPLAY_DATA || r.type != CLI_VIEW_REPLAY_TYPE || c->awaiting_resync)
        return COG_ERROR_PROTOCOL;
    if (c->have_view && r.generation != c->replay_generation) return COG_ERROR_PROTOCOL;
    if (r.sequence <= c->rendered_sequence) return COG_OK;
    if (c->rendered_sequence == UINT64_MAX || r.sequence != c->rendered_sequence + 1U) {
        c->awaiting_resync = true; c->replay_generation = r.generation; c->gap_last = r.last_sequence;
        return deadline(c);
    }
    return install(c, r.payload, r.payload_len, r.generation, r.sequence, false);
}
static bool number(char **text, uint64_t *value)
{
    char *s = *text;
    uint64_t n = 0U;
    if (*s < '1' || *s > '9') return false;
    do {
        unsigned int digit = (unsigned int)(*s - '0');
        if (n > (UINT64_MAX - digit) / 10U) return false;
        n = n * 10U + digit; ++s;
    } while (*s >= '0' && *s <= '9');
    if (*s != '\0' && *s != ' ') return false;
    *value = n; *text = s;
    return true;
}
static cog_result_t parse(cog_cli_host_client_t *c, cog_cli_command_t *cmd, bool *local)
{
    char *s = c->line;
    *local = false;
    if (s[0] != '/') { cmd->kind = COG_CLI_COMMAND_SUBMIT; cmd->text = s; cmd->text_len = strlen(s); return COG_OK; }
    if (strcmp(s, "/quit") == 0 || strcmp(s, "/exit") == 0) {
        *local = true; c->stopping = true; return deadline(c);
    }
    if (strcmp(s, "/history") == 0 || strcmp(s, "/tools") == 0) {
        *local = true; return render(c, &c->banks[c->current].view, strcmp(s, "/tools") == 0);
    }
    if (strcmp(s, "/resync") == 0) cmd->kind = COG_CLI_COMMAND_RESYNC;
    else if (c->fixed.watch) return COG_ERROR_DENIED;
    else if (strcmp(s, "/shutdown") == 0) cmd->kind = COG_CLI_COMMAND_SHUTDOWN;
    else if (strcmp(s, "/cancel") == 0) cmd->kind = COG_CLI_COMMAND_CANCEL;
    else if (strncmp(s, "/cancel ", 8U) == 0) {
        s += 8U; cmd->kind = COG_CLI_COMMAND_CANCEL;
        if (!number(&s, &cmd->agent_id) || *s != '\0') return COG_ERROR_INVALID_ARG;
    } else if (strncmp(s, "/fork ", 6U) == 0) {
        s += 6U; cmd->kind = COG_CLI_COMMAND_FORK;
        if (!number(&s, &cmd->node_id) || *s != '\0') return COG_ERROR_INVALID_ARG;
    } else if (strncmp(s, "/continue ", 10U) == 0) {
        s += 10U; cmd->kind = COG_CLI_COMMAND_CONTINUE;
        if (!number(&s, &cmd->node_id) || *s++ != ' ' || *s == '\0') return COG_ERROR_INVALID_ARG;
        cmd->text = s; cmd->text_len = strlen(s);
    } else if (strncmp(s, "/allow ", 7U) == 0 || strncmp(s, "/deny ", 6U) == 0) {
        cmd->kind = COG_CLI_COMMAND_POLICY; cmd->decision = s[1] == 'a' ? 2U : 1U;
        s += cmd->decision == 2U ? 7U : 6U;
        if (!number(&s, &cmd->nonce) || *s != '\0') return COG_ERROR_INVALID_ARG;
        const cog_cli_projection_view_t *view = &c->banks[c->current].view;
        if (view->policy_count != 1U || view->policies[0].nonce != cmd->nonce ||
            cmd->observed_generation != c->replay_generation ||
            cmd->observed_sequence != c->rendered_sequence) return COG_ERROR_DENIED;
        cmd->target_session_id = view->policies[0].session_id;
        cmd->node_id = view->policies[0].assistant_node; cmd->call_ordinal = view->policies[0].call_ordinal;
    } else if (strncmp(s, "/reload ", 8U) == 0) {
        s += 8U; cmd->kind = COG_CLI_COMMAND_RELOAD;
        if (strncmp(s, "model ", 6U) == 0) { cmd->reload_field = 1U; s += 6U; }
        else if (strncmp(s, "base-url ", 9U) == 0) { cmd->reload_field = 2U; s += 9U; }
        else if (strncmp(s, "credential-ref ", 15U) == 0) { cmd->reload_field = 3U; s += 15U; }
        else return COG_ERROR_INVALID_ARG;
        if (*s == '\0') return COG_ERROR_INVALID_ARG;
        cmd->text = s; cmd->text_len = strlen(s);
    } else if (strncmp(s, "/handoff ", 9U) == 0) {
        s += 9U; cmd->kind = COG_CLI_COMMAND_HANDOFF;
        if (strcmp(s, "local") != 0 && (!number(&s, &cmd->target.incarnation) ||
            *s++ != ' ' || !number(&s, &cmd->target.number) || *s != '\0')) return COG_ERROR_INVALID_ARG;
    } else return COG_ERROR_INVALID_ARG;
    return COG_OK;
}
static cog_result_t line(cog_cli_host_client_t *c)
{
    c->line[c->line_length] = '\0';
    if (c->line_length == 0U) return COG_OK;
    if (c->next_input_serial == UINT64_MAX) return COG_ERROR_LIMIT;
    c->last_input_serial = ++c->next_input_serial;
    c->final_input_result = COG_OK;
    cog_cli_command_t cmd = {0};
    cmd.observed_generation = c->line_generation; cmd.observed_sequence = c->line_sequence;
    bool local = false;
    cog_result_t rc = parse(c, &cmd, &local);
    if (rc == COG_OK && !local) {
        if (c->fixed.watch && cmd.kind != COG_CLI_COMMAND_RESYNC) rc = COG_ERROR_DENIED;
        else rc = command(c, &cmd, c->last_input_serial);
    }
    c->line_length = 0U;
    if (rc == COG_ERROR_IO || rc == COG_ERROR_LIMIT) return rc;
    if (rc != COG_OK) {
        c->final_input_result = rc;
        return metadata(c, rc, c->last_input_serial);
    }
    return COG_OK;
}
static cog_result_t ready_fd(int fd, bool *ready)
{
    *ready = false;
    if (fd < 0) return COG_OK;
    struct pollfd p = {fd, POLLIN, 0};
    int n = poll(&p, 1U, 0);
    if (n < 0) return errno == EINTR ? COG_ERROR_AGAIN : COG_ERROR_IO;
    if ((p.revents & (POLLERR | POLLNVAL)) != 0) return COG_ERROR_IO;
    *ready = (p.revents & (POLLIN | POLLHUP)) != 0;
    return COG_OK;
}
static cog_result_t input(cog_cli_host_client_t *c, bool *progress)
{
    if (c->input_cursor == c->input_length) {
        c->input_cursor = 0U; c->input_length = 0U;
        if (c->eof || c->stopping) return COG_OK;
        bool available;
        cog_result_t rc = ready_fd(c->fixed.input_fd, &available);
        if (rc != COG_OK || !available) return rc;
        ssize_t n = read(c->fixed.input_fd, c->input, sizeof(c->input));
        if (n < 0) return errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK ? COG_ERROR_AGAIN : COG_ERROR_IO;
        *progress = true;
        c->input_generation = c->replay_generation; c->input_sequence = c->rendered_sequence;
        if (n == 0) {
            c->eof = true;
            rc = c->line_length == 0U ? COG_OK : line(c);
            c->stopping = true;
            if (rc != COG_OK) return rc;
            return deadline(c);
        }
        c->input_length = (size_t)n;
    }
    while (c->input_cursor < c->input_length) {
        unsigned char ch = (unsigned char)c->input[c->input_cursor++];
        *progress = true;
        if (c->line_length == 0U) { c->line_generation = c->input_generation; c->line_sequence = c->input_sequence; }
        if (ch == '\n') return line(c);
        if (ch == 0U) return COG_ERROR_PROTOCOL;
        if (c->line_length == sizeof(c->line) - 1U) return COG_ERROR_LIMIT;
        c->line[c->line_length++] = (char)ch;
    }
    return COG_OK;
}
static cog_result_t reconnect(cog_cli_host_client_t *c)
{
    cog_result_t rc = deadline(c);
    if (rc != COG_OK) return rc;
    rc = cog_ipc_endpoint_disconnect(c->endpoint);
    if (rc != COG_OK) return rc;
    c->authenticated = false; c->phase = CLI_CLIENT_CONNECTING;
    for (size_t i = 0U; i < 2U; ++i) { c->requests[i].sent = false; c->requests[i].ack_sent = false; }
    c->hello.sent = false; c->hello.ack_sent = false;
    c->cursor_pending = false; /* reconnect HELLO conveys the committed cursor */
    rc = cog_ipc_endpoint_connect(c->endpoint, c->socket_path, NULL);
    if (rc == COG_OK || rc == COG_ERROR_AGAIN) return COG_OK;
    return unknown(c) ? COG_ERROR_INDETERMINATE : rc;
}
static cog_result_t drive(cog_cli_host_client_t *c, bool *progress)
{
    uint64_t next = UINT64_MAX;
    cog_result_t service = c->authenticated ? cog_ipc_endpoint_service(c->endpoint, c->now_ms, &next) :
        cog_ipc_internal_service_one(c->endpoint, c->now_ms, &next);
    if (!c->authenticated) {
        cog_peer_id_t peer = 0U;
        cog_result_t rc = cog_ipc_endpoint_get_peer_id(c->endpoint, &peer);
        if (rc == COG_OK) {
            rc = cog_ipc_endpoint_peer_info(c->endpoint, peer, &c->proof);
            if (rc != COG_OK) return rc;
            if (c->proof.user_id != (uint64_t)geteuid()) return COG_ERROR_DENIED;
            c->authenticated = true; c->phase = CLI_CLIENT_DISCOVERING;
            rc = hello(c);
            if (rc != COG_OK) return rc;
            *progress = true;
        } else if (rc != COG_ERROR_AGAIN && rc != COG_ERROR_INVALID_STATE) return rc;
    }
    if (c->authenticated && !c->cursor_pending) {
        cog_ipc_message_t msg = {c->receive, 0U, c->frame_capacity};
        cog_result_t rc = cog_ipc_endpoint_recv(c->endpoint, &msg);
        if (rc == COG_OK) { *progress = true; return receive_frame(c, msg.len); }
        if (rc != COG_ERROR_AGAIN && rc != COG_ERROR_IO) return rc;
    }
    if (service != COG_OK && service != COG_ERROR_AGAIN) {
        if (service != COG_ERROR_IO && service != COG_ERROR_TIMEOUT) return service;
        return reconnect(c);
    }
    if (!c->authenticated) return COG_OK;
    cog_result_t rc = send_request(c, &c->hello);
    if (rc == COG_OK) { *progress = true; return COG_OK; }
    if (rc != COG_ERROR_AGAIN) return rc;
    if (!c->discovered || c->hello.phase != CLI_CLIENT_REQUEST_ACKED) return COG_OK;
    for (size_t i = 0U; i < 2U; ++i) {
        rc = send_request(c, &c->requests[i]);
        if (rc == COG_OK) { *progress = true; return COG_OK; }
        if (rc != COG_ERROR_AGAIN) return rc;
    }
    if (c->cursor_pending) {
        cog_ipc_message_t m = {c->cursor, c->cursor_length, sizeof(c->cursor)};
        rc = cog_ipc_endpoint_send(c->endpoint, &m);
        if (rc == COG_OK) { c->cursor_pending = false; *progress = true; }
        return rc == COG_ERROR_AGAIN ? COG_OK : rc;
    }
    if (!c->have_view || c->awaiting_resync) return COG_OK;
    if (c->gap_last != 0U) {
        if (pending(&c->requests[0])) return COG_OK;
        cog_cli_command_t cmd = {0}; cmd.kind = COG_CLI_COMMAND_RESYNC;
        cmd.observed_generation = c->replay_generation; cmd.observed_sequence = c->rendered_sequence;
        rc = command(c, &cmd, 0U);
        if (rc != COG_OK) return rc;
        c->gap_last = 0U; *progress = true;
        return COG_OK;
    }
    c->phase = c->stopping ? CLI_CLIENT_DETACHING : CLI_CLIENT_ONLINE;
    if (!c->stopping) c->deadline_ms = 0U;
    if (!c->stopping) return input(c, progress);
    return COG_OK;
}

cog_result_t cog_cli_host_client_step(cog_cli_host_client_t *c, cog_cli_host_runtime_step_t *info)
{
    if (c == NULL || info == NULL) return COG_ERROR_NULL_PTR;
    if (c->in_step) return COG_ERROR_INVALID_STATE;
    *info = (cog_cli_host_runtime_step_t){false, 0U, UINT64_MAX, 10U};
    if (c->phase == CLI_CLIENT_DONE) { info->done = true; return c->primary; }
    c->in_step = true;
    bool progress = false, signal = false;
    cog_result_t rc = client_clock(c);
    if (rc == COG_OK) rc = ready_fd(c->fixed.signal_fd, &signal);
    if (rc == COG_OK && signal) {
        c->stopping = true;
        if (unknown(c)) rc = COG_ERROR_INDETERMINATE;
        else rc = deadline(c);
    }
    if (rc == COG_OK) rc = drive(c, &progress);
    if (rc == COG_ERROR_AGAIN) rc = COG_OK;
    if (rc == COG_OK && c->stopping && !pending(&c->requests[0]) &&
        !pending(&c->requests[1]) && !pending(&c->hello)) {
        rc = finish(c, c->final_input_result);
    } else if (rc == COG_OK && c->deadline_ms != 0U && c->now_ms >= c->deadline_ms) {
        rc = unknown(c) ? COG_ERROR_INDETERMINATE : COG_ERROR_IO;
    }
    if (rc != COG_OK) (void)finish(c, rc);
    info->done = c->phase == CLI_CLIENT_DONE;
    info->dispatched = progress ? 1U : 0U;
    info->next_deadline_ms = c->deadline_ms == 0U ? UINT64_MAX : c->deadline_ms;
    bool socket_ready = false;
    if (!info->done) {
        cog_result_t ready = cog_ipc_internal_ready(c->endpoint, &socket_ready);
        if (ready != COG_OK && ready != COG_ERROR_AGAIN) { rc = finish(c, ready); info->done = true; }
    }
    if (progress || socket_ready || (c->phase == CLI_CLIENT_ONLINE &&
        c->input_cursor < c->input_length)) info->max_wait_ms = 0U;
    if (c->deadline_ms > c->now_ms && c->deadline_ms - c->now_ms < info->max_wait_ms)
        info->max_wait_ms = (uint32_t)(c->deadline_ms - c->now_ms);
    c->in_step = false;
    return rc;
}

cog_result_t cog_cli_host_client_close(cog_cli_host_client_t **client, cog_result_t primary)
{
    if (client == NULL) return COG_ERROR_NULL_PTR;
    cog_cli_host_client_t *c = *client;
    if (c == NULL) return primary;
    *client = NULL;
    if (primary == COG_OK) primary = c->primary;
    if (c->endpoint != NULL) {
        cog_result_t rc = cog_ipc_endpoint_free(c->endpoint);
        if (primary == COG_OK) primary = rc;
    }
    for (size_t i = 0U; i < 2U; ++i) {
        free(c->banks[i].items); free(c->banks[i].text); free(c->banks[i].wire);
        free(c->requests[i].frame);
    }
    free(c->receive); free(c->envelope_storage); free(c->replay_storage);
    free(c->command_body); free(c->render); free(c);
    return primary;
}

cog_result_t cog_cli_host_client_new(cog_cli_host_client_t **out,
    const cog_cli_host_client_config_t *config)
{
    if (out == NULL) return COG_ERROR_NULL_PTR;
    *out = NULL;
    if (config == NULL || config->socket_path == NULL) return COG_ERROR_NULL_PTR;
    size_t path = strlen(config->socket_path);
    if (path == 0U || path >= sizeof(((struct sockaddr_un *)0)->sun_path) ||
        config->input_fd < -1 || config->signal_fd < -1 || config->projection_bytes == 0U ||
        config->projection_bytes > 16U * 1024U * 1024U || config->reconnect_ms > 30000U)
        return COG_ERROR_INVALID_ARG;
    cog_cli_host_client_t *c = calloc(1U, sizeof(*c));
    if (c == NULL) return COG_ERROR_NOMEM;
    c->fixed = *config; memcpy(c->socket_path, config->socket_path, path + 1U);
    c->fixed.socket_path = c->socket_path;
    if (c->fixed.output == NULL) c->fixed.output = stdout;
    if (c->fixed.diagnostic == NULL) c->fixed.diagnostic = stderr;
    if (c->fixed.reconnect_ms == 0U) c->fixed.reconnect_ms = 3000U;
    c->frame_capacity = (config->projection_bytes > CLI_CLIENT_LINE_BYTES ? config->projection_bytes : CLI_CLIENT_LINE_BYTES) + 512U;
    c->envelope_capacity = c->frame_capacity; c->replay_capacity = c->frame_capacity;
    c->command_capacity = CLI_CLIENT_LINE_BYTES + 512U;
    c->render_capacity = 6U * config->projection_bytes + 16384U;
    c->item_capacity = config->projection_bytes / 14U;
    c->receive = malloc(c->frame_capacity); c->envelope_storage = malloc(c->envelope_capacity);
    c->replay_storage = malloc(c->replay_capacity); c->command_body = malloc(c->command_capacity);
    c->render = malloc(c->render_capacity);
    c->hello.frame = c->hello_frame; c->hello.frame_capacity = sizeof(c->hello_frame);
    cog_result_t rc = COG_ERROR_NOMEM;
    if (c->receive == NULL || c->envelope_storage == NULL || c->replay_storage == NULL ||
        c->command_body == NULL || c->render == NULL) goto fail;
    for (size_t i = 0U; i < 2U; ++i) {
        c->banks[i].items = calloc(c->item_capacity == 0U ? 1U : c->item_capacity, sizeof(*c->banks[i].items));
        c->banks[i].text = malloc(config->projection_bytes); c->banks[i].wire = malloc(config->projection_bytes);
        c->requests[i].frame = malloc(c->frame_capacity); c->requests[i].frame_capacity = c->frame_capacity;
        if (c->banks[i].items == NULL || c->banks[i].text == NULL || c->banks[i].wire == NULL ||
            c->requests[i].frame == NULL) goto fail;
    }
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) { rc = COG_ERROR_IO; goto fail; }
    ssize_t got = read(fd, &c->nonce, sizeof(c->nonce));
    int closed = close(fd);
    if (got != (ssize_t)sizeof(c->nonce) || closed != 0 || c->nonce == 0U) { rc = COG_ERROR_IO; goto fail; }
    cog_ipc_endpoint_config_t ep = {0};
    ep.max_frame_bytes = c->frame_capacity; ep.receive_frames = 2U; ep.send_frames = 4U;
    ep.service_quantum = 4U; ep.max_routes = 1U; ep.connect_timeout_ms = c->fixed.reconnect_ms;
    rc = cog_ipc_endpoint_new_ex(&c->endpoint, &ep);
    if (rc != COG_OK) goto fail;
    rc = client_clock(c);
    if (rc == COG_OK) rc = deadline(c);
    if (rc != COG_OK) goto fail;
    rc = cog_ipc_endpoint_connect(c->endpoint, c->socket_path, NULL);
    if (rc != COG_OK && rc != COG_ERROR_AGAIN) goto fail;
    c->phase = CLI_CLIENT_CONNECTING;
    *out = c;
    return COG_OK;
fail:
    return cog_cli_host_client_close(&c, rc);
}

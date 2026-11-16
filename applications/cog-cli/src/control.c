/** @file control.c @brief Pure CGL1 envelopes and typed command bodies. */
#include "control.h"
#include "codec_internal.h"

static bool message_valid(const cog_cli_control_message_t *m, size_t sid)
{
    if (m->kind < COG_CLI_CONTROL_UI_HELLO || m->kind > COG_CLI_CONTROL_CHILD_START_ACK ||
        m->lane < COG_CLI_LANE_COMMAND || m->lane > COG_CLI_LANE_CANCEL ||
        m->sender_incarnation == 0U || m->wire_id == 0U ||
        ((m->worker.incarnation == 0U) != (m->worker.number == 0U)) ||
        ((m->agent_id == 0U) != (m->execution_epoch == 0U))) return false;
    if (m->worker.incarnation == 0U) {
        if (m->owner_peer != 0U || m->agent_id != 0U) return false;
    } else if (m->owner_peer == 0U) return false;
    if (m->kind == COG_CLI_CONTROL_UI_HELLO) {
        if (m->worker.incarnation != 0U) return false;
        return sid == 0U || cli_wire_session(m->session_id, sid);
    }
    return m->runtime_incarnation != 0U && cli_wire_session(m->session_id, sid);
}
static bool encode_disjoint(const void *view, size_t view_size, const void *text,
    size_t text_size, const void *payload, size_t payload_size,
    const void *output, size_t capacity, const size_t *required)
{
    return cli_wire_disjoint(output, capacity, required, sizeof(*required)) &&
        cli_wire_disjoint(view, view_size, output, capacity) &&
        cli_wire_disjoint(view, view_size, required, sizeof(*required)) &&
        cli_wire_disjoint(text, text_size, output, capacity) &&
        cli_wire_disjoint(text, text_size, required, sizeof(*required)) &&
        cli_wire_disjoint(payload, payload_size, output, capacity) &&
        cli_wire_disjoint(payload, payload_size, required, sizeof(*required));
}
cog_result_t cog_cli_control_encode(const cog_cli_control_message_t *message,
    void *output, size_t capacity, size_t *required)
{
    if (message == NULL || required == NULL || (output == NULL && capacity != 0U)) return COG_ERROR_NULL_PTR;
    size_t sid;
    if (!cli_wire_cstr(message->session_id, 127U, &sid) ||
        !message_valid(message, sid) || (message->payload == NULL && message->payload_len != 0U) ||
        message->payload_len > UINT32_MAX) return COG_ERROR_INVALID_ARG;
    if (!encode_disjoint(message, sizeof(*message), message->session_id, sid + 1U,
        message->payload, message->payload_len, output, capacity, required)) return COG_ERROR_INVALID_ARG;
    size_t size = 83U + sid;
    if (!cli_wire_add(&size, message->payload_len)) return COG_ERROR_LIMIT;
    *required = size;
    if (capacity < size) return COG_ERROR_OVERFLOW;
    cli_wire_writer_t w = {output, 0U, true};
    cli_wire_bytes(&w, "CGL1", 4U); cli_wire_put(&w, (uint64_t)message->kind, 1U);
    cli_wire_put(&w, (uint64_t)message->lane, 1U);
    cli_wire_put(&w, message->runtime_incarnation, 8U); cli_wire_put(&w, message->sender_incarnation, 8U);
    cli_wire_put(&w, message->wire_id, 8U); cli_wire_put(&w, message->reply_to, 8U);
    cli_wire_put(&w, message->worker.incarnation, 8U); cli_wire_put(&w, message->worker.number, 8U);
    cli_wire_put(&w, message->owner_peer, 8U); cli_wire_put(&w, message->agent_id, 8U);
    cli_wire_put(&w, message->execution_epoch, 8U); cli_wire_put(&w, sid, 1U);
    cli_wire_bytes(&w, message->session_id, sid); cli_wire_put(&w, message->payload_len, 4U);
    cli_wire_bytes(&w, message->payload, message->payload_len);
    return COG_OK;
}
cog_result_t cog_cli_control_decode(const void *input, size_t input_len,
    cog_cli_control_message_t *message, void *storage, size_t capacity, size_t *required)
{
    if (input == NULL || message == NULL || required == NULL ||
        (storage == NULL && capacity != 0U)) return COG_ERROR_NULL_PTR;
    if (input_len < 4U || memcmp(input, "CGL", 3U) != 0) return COG_ERROR_PROTOCOL;
    const unsigned char *p = input;
    if (p[3] != '1') return COG_ERROR_UNSUPPORTED;
    cli_wire_reader_t r = {p, input_len, 4U, true};
    cog_cli_control_message_t m = {0};
    m.kind = (cog_cli_control_kind_t)cli_wire_get(&r, 1U);
    m.lane = (cog_cli_control_lane_t)cli_wire_get(&r, 1U);
    m.runtime_incarnation = cli_wire_get(&r, 8U); m.sender_incarnation = cli_wire_get(&r, 8U);
    m.wire_id = cli_wire_get(&r, 8U); m.reply_to = cli_wire_get(&r, 8U);
    m.worker.incarnation = cli_wire_get(&r, 8U); m.worker.number = cli_wire_get(&r, 8U);
    m.owner_peer = cli_wire_get(&r, 8U); m.agent_id = cli_wire_get(&r, 8U);
    m.execution_epoch = cli_wire_get(&r, 8U);
    size_t sid = (size_t)cli_wire_get(&r, 1U);
    m.session_id = (const char *)cli_wire_take(&r, sid);
    m.payload_len = (size_t)cli_wire_get(&r, 4U);
    m.payload = cli_wire_take(&r, m.payload_len);
    if (!r.valid || r.used != input_len || !message_valid(&m, sid)) return COG_ERROR_PROTOCOL;
    const cli_wire_region_t regions[] = {{input, input_len}, {message, sizeof(*message)},
        {storage, capacity}, {required, sizeof(*required)}};
    if (!cli_wire_regions(regions, 4U)) return COG_ERROR_PROTOCOL;
    size_t need = sid + 1U;
    if (!cli_wire_add(&need, m.payload_len)) return COG_ERROR_LIMIT;
    *required = need;
    if (capacity < need) return COG_ERROR_OVERFLOW;
    char *out = storage;
    m.session_id = cli_wire_copy(&out, m.session_id, sid);
    if (m.payload_len != 0U) memcpy(out, m.payload, m.payload_len);
    m.payload = out; *message = m;
    return COG_OK;
}

static bool command_valid(const cog_cli_command_t *c, size_t *sid)
{
    *sid = 0U;
    if (c->kind < COG_CLI_COMMAND_SUBMIT || c->kind > COG_CLI_COMMAND_FORK ||
        c->observed_generation == 0U || c->observed_sequence == 0U) return false;
    bool text = c->kind == COG_CLI_COMMAND_SUBMIT || c->kind == COG_CLI_COMMAND_CONTINUE || c->kind == COG_CLI_COMMAND_RELOAD;
    bool node = c->kind == COG_CLI_COMMAND_CONTINUE || c->kind == COG_CLI_COMMAND_POLICY || c->kind == COG_CLI_COMMAND_FORK;
    bool policy = c->kind == COG_CLI_COMMAND_POLICY;
    if ((!text && (c->text != NULL || c->text_len != 0U)) ||
        (text && (c->text_len > UINT32_MAX || !cli_wire_utf8(c->text, c->text_len))) ||
        (node ? c->node_id == 0U : c->node_id != 0U) ||
        (policy ? c->nonce == 0U || c->call_ordinal > 15U || c->decision < 1U || c->decision > 2U :
            c->nonce != 0U || c->call_ordinal != 0U || c->decision != 0U) ||
        (c->kind == COG_CLI_COMMAND_RELOAD ? c->reload_field < 1U || c->reload_field > 3U : c->reload_field != 0U) ||
        (c->kind != COG_CLI_COMMAND_CANCEL && c->agent_id != 0U)) return false;
    if (policy) {
        if (!cli_wire_cstr(c->target_session_id, 127U, sid) || !cli_wire_session(c->target_session_id, *sid)) return false;
    } else if (c->target_session_id != NULL) return false;
    if (c->kind == COG_CLI_COMMAND_HANDOFF)
        return (c->target.incarnation == 0U) == (c->target.number == 0U);
    return c->target.incarnation == 0U && c->target.number == 0U;
}
static void command_write(cli_wire_writer_t *w, const cog_cli_command_t *c, size_t sid)
{
    cli_wire_put(w, (uint64_t)c->kind, 1U); cli_wire_put(w, c->observed_generation, 8U);
    cli_wire_put(w, c->observed_sequence, 8U);
    switch (c->kind) {
    case COG_CLI_COMMAND_SUBMIT: cli_wire_string(w, c->text, c->text_len); break;
    case COG_CLI_COMMAND_CONTINUE:
        cli_wire_put(w, c->node_id, 8U); cli_wire_string(w, c->text, c->text_len); break;
    case COG_CLI_COMMAND_POLICY:
        cli_wire_string(w, c->target_session_id, sid); cli_wire_put(w, c->node_id, 8U);
        cli_wire_put(w, c->call_ordinal, 2U); cli_wire_put(w, c->nonce, 8U);
        cli_wire_put(w, c->decision, 1U); break;
    case COG_CLI_COMMAND_CANCEL: cli_wire_put(w, c->agent_id, 8U); break;
    case COG_CLI_COMMAND_RELOAD:
        cli_wire_put(w, c->reload_field, 1U); cli_wire_string(w, c->text, c->text_len); break;
    case COG_CLI_COMMAND_HANDOFF:
        cli_wire_put(w, c->target.incarnation, 8U); cli_wire_put(w, c->target.number, 8U); break;
    case COG_CLI_COMMAND_FORK: cli_wire_put(w, c->node_id, 8U); break;
    case COG_CLI_COMMAND_SHUTDOWN: case COG_CLI_COMMAND_RESYNC: break;
    }
}
cog_result_t cog_cli_command_encode(const cog_cli_command_t *command,
    void *output, size_t capacity, size_t *required)
{
    if (command == NULL || required == NULL || (output == NULL && capacity != 0U)) return COG_ERROR_NULL_PTR;
    size_t sid;
    if (!command_valid(command, &sid) ||
        !encode_disjoint(command, sizeof(*command), command->target_session_id, sid == 0U ? 0U : sid + 1U,
            command->text, command->text_len, output, capacity, required)) return COG_ERROR_INVALID_ARG;
    cli_wire_writer_t w = {NULL, 0U, true}; command_write(&w, command, sid);
    if (!w.valid) return COG_ERROR_LIMIT;
    *required = w.used;
    if (capacity < w.used) return COG_ERROR_OVERFLOW;
    w = (cli_wire_writer_t){output, 0U, true}; command_write(&w, command, sid);
    return COG_OK;
}
cog_result_t cog_cli_command_decode(const void *input, size_t input_len,
    cog_cli_command_t *command, void *storage, size_t capacity, size_t *required)
{
    if (input == NULL || command == NULL || required == NULL || (storage == NULL && capacity != 0U)) return COG_ERROR_NULL_PTR;
    cli_wire_reader_t r = {input, input_len, 0U, true};
    cog_cli_command_t c = {0};
    c.kind = (cog_cli_command_kind_t)cli_wire_get(&r, 1U);
    c.observed_generation = cli_wire_get(&r, 8U); c.observed_sequence = cli_wire_get(&r, 8U);
    size_t sid = 0U, need = 0U;
    switch (c.kind) {
    case COG_CLI_COMMAND_SUBMIT: c.text = cli_wire_read_string(&r, &c.text_len); break;
    case COG_CLI_COMMAND_CONTINUE:
        c.node_id = cli_wire_get(&r, 8U); c.text = cli_wire_read_string(&r, &c.text_len); break;
    case COG_CLI_COMMAND_POLICY:
        c.target_session_id = cli_wire_read_string(&r, &sid); c.node_id = cli_wire_get(&r, 8U);
        c.call_ordinal = (uint16_t)cli_wire_get(&r, 2U); c.nonce = cli_wire_get(&r, 8U);
        c.decision = (uint8_t)cli_wire_get(&r, 1U); break;
    case COG_CLI_COMMAND_CANCEL: c.agent_id = cli_wire_get(&r, 8U); break;
    case COG_CLI_COMMAND_RELOAD:
        c.reload_field = (uint8_t)cli_wire_get(&r, 1U); c.text = cli_wire_read_string(&r, &c.text_len); break;
    case COG_CLI_COMMAND_HANDOFF:
        c.target.incarnation = cli_wire_get(&r, 8U); c.target.number = cli_wire_get(&r, 8U); break;
    case COG_CLI_COMMAND_FORK: c.node_id = cli_wire_get(&r, 8U); break;
    case COG_CLI_COMMAND_SHUTDOWN: case COG_CLI_COMMAND_RESYNC: break;
    default: return COG_ERROR_PROTOCOL;
    }
    if (!r.valid || r.used != input_len) return COG_ERROR_PROTOCOL;
    /* Wire strings have no trailing NUL yet; validate scalars with a bounded ID copy. */
    char id[128];
    if (c.kind == COG_CLI_COMMAND_POLICY) {
        if (!cli_wire_session(c.target_session_id, sid)) return COG_ERROR_PROTOCOL;
        memcpy(id, c.target_session_id, sid); id[sid] = '\0'; c.target_session_id = id;
    }
    size_t checked_sid;
    if (!command_valid(&c, &checked_sid)) return COG_ERROR_PROTOCOL;
    const cli_wire_region_t regions[] = {{input, input_len}, {command, sizeof(*command)},
        {storage, capacity}, {required, sizeof(*required)}};
    if (!cli_wire_regions(regions, 4U)) return COG_ERROR_PROTOCOL;
    if (c.text != NULL && (!cli_wire_add(&need, c.text_len) || !cli_wire_add(&need, 1U))) return COG_ERROR_LIMIT;
    if (c.target_session_id != NULL && !cli_wire_add(&need, sid + 1U)) return COG_ERROR_LIMIT;
    *required = need;
    if (capacity < need) return COG_ERROR_OVERFLOW;
    char *out = storage;
    if (c.text != NULL) c.text = cli_wire_copy(&out, c.text, c.text_len);
    if (c.target_session_id != NULL) c.target_session_id = cli_wire_copy(&out, id, sid);
    *command = c;
    return COG_OK;
}

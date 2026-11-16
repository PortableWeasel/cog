#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
/** @file host_runtime.c @brief Bounded coordinator admission and ownership. */
#include "composition_internal.h"
#include "app_internal.h"
#include "codec_internal.h"
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <time.h>
#include <string.h>

static bool host_product(size_t a, size_t b, size_t *value)
{
    if (a != 0U && b > SIZE_MAX / a) return false;
    *value = a * b;
    return true;
}

static bool host_owner_equal(cog_store_owner_t a, cog_store_owner_t b)
{
    return a.epoch == b.epoch && a.instance_id == b.instance_id;
}

/** Pure resource proof; no allocation or startup resolution. */
cog_result_t cli_host_plan(const cog_cli_host_runtime_config_t *config,
    const cog_cli_options_t *resolved, cli_host_reservations_t *out)
{
    if (config == NULL || resolved == NULL || out == NULL) return COG_ERROR_NULL_PTR;
    cli_host_reservations_t p = {0};
    p.children = config->children == 0U ? 4U : config->children;
    p.clients = config->ui_clients == 0U ? 4U : config->ui_clients;
    p.workers = config->workers;
    p.command_records = config->command_records == 0U ? 64U : config->command_records;
    p.projection_bytes = config->projection_bytes == 0U ? 8U * 1024U * 1024U : config->projection_bytes;
    size_t replay = config->replay_bytes == 0U ? 16U * 1024U * 1024U : config->replay_bytes;
    if (p.children > CLI_HOST_MAX_CHILDREN || p.clients > CLI_HOST_MAX_CLIENTS ||
        p.workers > CLI_HOST_MAX_WORKERS || p.command_records > 1024U ||
        config->replay_records > 4096U || p.projection_bytes > 16U * 1024U * 1024U ||
        replay > 64U * 1024U * 1024U || config->controller_reconnect_ms > 30000U ||
        config->client_receipt_ms > 300000U ||
        (config->child_profile.tools & ~(uint32_t)COG_CLI_CHILD_ALL) != 0U ||
        config->child_profile.policy > COG_CLI_CHILD_POLICY_ASK)
        return COG_ERROR_INVALID_ARG;
    if (resolved->max_context_bytes == 0U || resolved->max_tool_output_bytes == 0U)
        return COG_ERROR_INVALID_ARG;
    p.executors = p.children + 2U;
    p.status_bytes = 1024U;
    p.result_bytes = 4097U;
    p.task_bytes = resolved->max_context_bytes < CLI_ARGUMENT_BYTES ?
        resolved->max_context_bytes : CLI_ARGUMENT_BYTES;
    p.mailbox_entries = 8U;
    p.mailbox_bytes = 65536U;
    /* Match the fixed actual app node reservation plus its active partial row. */
    p.display_items = CLI_NODES + 1U;
    size_t metadata = 0U, rows = 0U;
    if (!host_product(resolved->max_context_bytes, 5U, &p.display_bytes) ||
        !host_product(p.display_items, 14U, &metadata) ||
        !cli_wire_add(&p.display_bytes, metadata) ||
        !cli_wire_add(&p.display_bytes, resolved->max_tool_output_bytes) ||
        !cli_wire_add(&p.display_bytes, 152U) ||
        !host_product(p.children, 174U + p.status_bytes + p.result_bytes, &rows) ||
        !cli_wire_add(&rows, 452U) || !cli_wire_add(&rows, p.display_bytes) ||
        rows > p.projection_bytes || rows > replay) return COG_ERROR_LIMIT;
    /* CGL maximum envelope is 210 bytes. Each lane retains only its schema's
     * largest body; PROFILE credentials never occupy a generic lane cache. */
    p.lane_frame_bytes[COG_CLI_LANE_COMMAND - 1U] = p.task_bytes;
    if (!cli_wire_add(&p.lane_frame_bytes[0], 512U)) return COG_ERROR_LIMIT;
    p.lane_frame_bytes[COG_CLI_LANE_UPDATE - 1U] = p.status_bytes + p.result_bytes + 512U;
    p.lane_frame_bytes[COG_CLI_LANE_VIEW - 1U] = p.projection_bytes + 512U;
    p.lane_frame_bytes[COG_CLI_LANE_POLICY - 1U] = 1024U;
    p.lane_frame_bytes[COG_CLI_LANE_DELEGATE - 1U] = resolved->max_tool_output_bytes;
    size_t tasks;
    if (!host_product(p.task_bytes, p.children, &tasks) ||
        !cli_wire_add(&tasks, 1024U + 4U * p.children)) return COG_ERROR_LIMIT;
    if (tasks > p.lane_frame_bytes[COG_CLI_LANE_DELEGATE - 1U])
        p.lane_frame_bytes[COG_CLI_LANE_DELEGATE - 1U] = tasks;
    p.lane_frame_bytes[COG_CLI_LANE_MAILBOX - 1U] = 65536U + 512U;
    p.lane_frame_bytes[COG_CLI_LANE_PROFILE - 1U] = 256U; /* ACK only */
    p.lane_frame_bytes[COG_CLI_LANE_HANDOFF - 1U] = 512U;
    p.lane_frame_bytes[COG_CLI_LANE_REPLAY - 1U] = p.projection_bytes + 512U;
    p.lane_frame_bytes[COG_CLI_LANE_CANCEL - 1U] = 512U;
    size_t keys = p.executors + p.clients + p.workers;
    if (!host_product(keys, 2U * CLI_HOST_LANES, &p.streams)) return COG_ERROR_LIMIT;
    /* VIEW/POLICY/DELEGATE/HANDOFF exist only for primary/staged executors;
     * replay belongs only to UI clients. Child lanes never reserve full views. */
    for (size_t i = 0U; i < CLI_HOST_LANES; ++i) {
        size_t count = keys, bytes;
        if (i == COG_CLI_LANE_VIEW - 1U || i == COG_CLI_LANE_POLICY - 1U ||
            i == COG_CLI_LANE_DELEGATE - 1U || i == COG_CLI_LANE_HANDOFF - 1U) count = 2U;
        if (i == COG_CLI_LANE_REPLAY - 1U) count = p.clients;
        if (p.lane_frame_bytes[i] > UINT32_MAX ||
            !host_product(p.lane_frame_bytes[i], count * 8U, &bytes) ||
            !cli_wire_add(&p.nonsecret_bytes, bytes)) return COG_ERROR_LIMIT;
    }
    if (!host_product(p.executors + p.workers, 3U * (65536U + 16384U + 8192U),
        &p.secret_bytes)) return COG_ERROR_LIMIT;
    *out = p;
    return COG_OK;
}

static cli_group_record_t *host_group(cog_cli_host_runtime_t *r, uint64_t reservation)
{
    for (size_t i = 0U; i < r->group_capacity; ++i)
        if (r->groups[i].phase != CLI_GROUP_FREE && r->groups[i].reservation == reservation)
            return &r->groups[i];
    return NULL;
}

static bool host_same_request(const cli_group_record_t *g, const cli_group_request_t *q)
{
    if (!host_owner_equal(g->parent_owner, q->owner) ||
        g->pinned_configuration != q->pinned_configuration ||
        g->deadline_ms != q->deadline_ms || g->submitted != q->count) return false;
    for (size_t i = 0U; i < q->count; ++i)
        if (g->task_lengths[i] != q->task_lengths[i] ||
            memcmp(g->tasks.data + g->task_offsets[i], q->tasks[i], q->task_lengths[i]) != 0)
            return false;
    return true;
}

/** Reserve the complete immutable group before any durable STARTED/effect. */
cog_result_t cli_runtime_reserve(cog_cli_host_runtime_t *r,
    const cli_group_request_t *q, uint64_t *reservation)
{
    if (r == NULL || q == NULL || reservation == NULL || q->parent_session == NULL ||
        q->call_id == NULL || q->tasks == NULL || q->task_lengths == NULL) return COG_ERROR_NULL_PTR;
    size_t sid, call;
    if (!cli_wire_cstr(q->parent_session, 127U, &sid) || !cli_wire_session(q->parent_session, sid) ||
        !cli_wire_cstr(q->call_id, 255U, &call) || call == 0U || q->ordinal >= 16U ||
        q->parent_node == 0U || q->count == 0U || q->count > r->bounds.children)
        return COG_ERROR_INVALID_ARG;
    size_t bytes = 0U;
    for (size_t i = 0U; i < q->count; ++i) {
        if (q->tasks[i] == NULL) return COG_ERROR_NULL_PTR;
        if (q->task_lengths[i] == 0U || q->task_lengths[i] >= r->bounds.task_bytes ||
            !cli_wire_utf8(q->tasks[i], q->task_lengths[i]) ||
            !cli_wire_add(&bytes, q->task_lengths[i] + 1U)) return COG_ERROR_LIMIT;
    }
    /* Active exact lookup precedes admission freeze/deadline/identity counters. */
    cli_group_record_t *free_group = NULL;
    for (size_t i = 0U; i < r->group_capacity; ++i) {
        cli_group_record_t *g = &r->groups[i];
        if (g->phase == CLI_GROUP_FREE) { if (free_group == NULL) free_group = g; continue; }
        if (g->parent_node == q->parent_node && g->ordinal == q->ordinal &&
            strcmp(g->parent_session, q->parent_session) == 0 && strcmp(g->call_id, q->call_id) == 0) {
            if (!host_same_request(g, q)) return COG_ERROR_PROTOCOL;
            *reservation = g->reservation;
            return COG_OK;
        }
    }
    if (!r->begun || r->shutdown || r->admission_frozen ||
        !host_owner_equal(r->primary_owner, q->owner) ||
        strcmp(r->primary_session, q->parent_session) != 0 ||
        q->pinned_configuration != r->active_configuration) return COG_ERROR_INVALID_STATE;
    if (q->deadline_ms <= r->now_ms) return COG_ERROR_TIMEOUT;
    const cog_cli_configuration_t *mirror = r->current_mirror;
    const cog_cli_options_t *options = cog_cli_configuration_options(mirror);
    if (options == NULL || options->backend != COG_CLI_BACKEND_REAL || options->workspace_root == NULL)
        return COG_ERROR_UNSUPPORTED;
    uint64_t rounds = options->max_tool_rounds;
    if (r->fixed.child_profile.max_tool_rounds != 0U && r->fixed.child_profile.max_tool_rounds < rounds)
        rounds = r->fixed.child_profile.max_tool_rounds;
    uint64_t retries = options->max_retries;
    if (r->fixed.child_profile.max_retries < retries) retries = r->fixed.child_profile.max_retries;
    ++rounds; ++retries;
    if (rounds > UINT64_MAX / retries || rounds * retries > UINT64_MAX / q->count)
        return COG_ERROR_LIMIT;
    uint64_t credit = rounds * retries * q->count;
    uint64_t ceiling = r->fixed.child_requests == 0U ? 64U : r->fixed.child_requests;
    size_t slots[CLI_HOST_MAX_CHILDREN], available = 0U;
    for (size_t i = 2U; i < r->bounds.executors && available < q->count; ++i)
        if (r->executors[i].phase == CLI_SLOT_FREE) slots[available++] = i;
    size_t overhead = 128U + 1024U * q->count;
    if (free_group == NULL || available < q->count || bytes > free_group->tasks.capacity ||
        r->request_credit_used > ceiling || r->request_credit_held > ceiling - r->request_credit_used ||
        credit > ceiling - r->request_credit_used - r->request_credit_held ||
        options->max_tool_output_bytes <= overhead ||
        options->max_tool_output_bytes > free_group->output.capacity ||
        r->next_group == 0U || r->next_group == UINT64_MAX ||
        r->next_reservation == 0U || r->next_reservation == UINT64_MAX) return COG_ERROR_LIMIT;
    size_t payload = (options->max_tool_output_bytes - overhead) / (6U * q->count);
    if (payload >= r->bounds.result_bytes) payload = r->bounds.result_bytes - 1U;
    if (payload < 256U) return COG_ERROR_LIMIT;
    /* No fallible operation follows the first mutation. */
    cli_group_record_t *g = free_group;
    g->phase = CLI_GROUP_GRANTED;
    g->id = r->next_group++; g->reservation = r->next_reservation++;
    g->parent_owner = q->owner; g->parent_node = q->parent_node; g->ordinal = q->ordinal;
    g->pinned_configuration = q->pinned_configuration; g->deadline_ms = q->deadline_ms;
    g->submitted = q->count; g->credit = credit; g->result_payload_cap = payload;
    memcpy(g->parent_session, q->parent_session, sid + 1U);
    memcpy(g->call_id, q->call_id, call + 1U);
    g->tasks.length = 0U;
    for (size_t i = 0U; i < q->count; ++i) {
        g->task_offsets[i] = g->tasks.length; g->task_lengths[i] = q->task_lengths[i];
        memcpy(g->tasks.data + g->tasks.length, q->tasks[i], q->task_lengths[i]);
        g->tasks.length += q->task_lengths[i]; g->tasks.data[g->tasks.length++] = 0U;
        g->members[i].slot = slots[i];
        r->executors[slots[i]].phase = CLI_SLOT_RESERVED;
        r->executors[slots[i]].group = g->id;
    }
    r->request_credit_held += credit;
    *reservation = g->reservation;
    return COG_OK;
}

/** Release only proven unstarted holds; started work is cancelled in service. */
cog_result_t cli_runtime_release(cog_cli_host_runtime_t *r, uint64_t reservation)
{
    if (r == NULL) return COG_ERROR_NULL_PTR;
    cli_group_record_t *g = host_group(r, reservation);
    if (g == NULL) return COG_ERROR_NOT_FOUND;
    if (!g->started && g->phase == CLI_GROUP_GRANTED) {
        for (size_t i = 0U; i < g->submitted; ++i)
            r->executors[g->members[i].slot].phase = CLI_SLOT_FREE;
        r->request_credit_held -= g->credit;
        /* Keep the exact grant as a cancelled receipt until original call ends;
         * a duplicate reserve can never become a new execution. */
        g->aggregate = COG_ERROR_ABORTED; g->phase = CLI_GROUP_SAVED;
        return COG_OK;
    }
    if (g->phase == CLI_GROUP_SAVED || g->phase == CLI_GROUP_RESULT || g->phase == CLI_GROUP_ROLLBACK)
        return COG_OK;
    g->gate_open = false;
    g->phase = CLI_GROUP_ROLLBACK;
    for (size_t i = 0U; i < g->submitted; ++i)
        r->executors[g->members[i].slot].cancel_latched = true;
    return COG_OK;
}

static cli_executor_slot_t *host_agent(cog_cli_host_runtime_t *r, cog_agent_id_t id)
{
    if (id == 0U) return NULL;
    for (size_t i = 2U; i < r->bounds.executors; ++i)
        if (r->executors[i].phase != CLI_SLOT_FREE && r->executors[i].agent == id)
            return &r->executors[i];
    return NULL;
}

static void host_json_string(cli_wire_writer_t *w, const unsigned char *s, size_t n)
{
    static const char hex[] = "0123456789abcdef";
    cli_wire_bytes(w, "\"", 1U);
    for (size_t i = 0U; i < n; ++i) {
        unsigned char c = s[i];
        if (c == '"' || c == '\\') {
            unsigned char pair[2] = {'\\', c};
            cli_wire_bytes(w, pair, sizeof(pair));
        } else if (c < 32U) {
            char escape[6] = {'\\', 'u', '0', '0', hex[c >> 4U], hex[c & 15U]};
            cli_wire_bytes(w, escape, sizeof(escape));
        } else cli_wire_bytes(w, &c, 1U);
    }
    cli_wire_bytes(w, "\"", 1U);
}

static void host_mailbox_json(cli_wire_writer_t *w, const cli_mailbox_t *m,
    size_t count, bool more)
{
    cli_wire_bytes(w, "{\"messages\":[", 13U);
    for (size_t i = 0U; i < count; ++i) {
        const cli_mailbox_entry_t *e = &m->entries[(m->head + i) % m->entry_capacity];
        char id[21];
        int length = snprintf(id, sizeof(id), "%llu", (unsigned long long)e->source);
        if (length <= 0 || (size_t)length >= sizeof(id)) { w->valid = false; return; }
        if (i != 0U) cli_wire_bytes(w, ",", 1U);
        cli_wire_bytes(w, "{\"agent_id\":", 12U);
        host_json_string(w, (const unsigned char *)id, (size_t)length);
        cli_wire_bytes(w, ",\"message\":", 11U);
        host_json_string(w, m->text + e->offset, e->length);
        cli_wire_bytes(w, "}", 1U);
    }
    const char *tail = more ? "],\"more_pending\":true}" : "],\"more_pending\":false}";
    cli_wire_bytes(w, tail, strlen(tail));
}

static void host_mailbox_result(cli_delegation_handle_t *h, size_t length)
{
    h->output.length = length;
    h->result = (cog_tool_output_t){.status = COG_OK,
        .output = (const char *)h->output.data, .output_len = length};
    h->output_cached = true;
    h->done = true;
}

/** Whole-item mailbox admission and immutable identified receive output. */
cog_result_t cli_runtime_mailbox_step(cog_cli_host_runtime_t *r,
    cli_delegation_handle_t *h)
{
    if (r == NULL || h == NULL || h->context == NULL) return COG_ERROR_NULL_PTR;
    if (h->context->coordinator != r || !h->started) return COG_ERROR_INVALID_STATE;
    if (h->output_cached) return COG_OK;
    cli_executor_slot_t *source = host_agent(r, h->context->agent);
    if (source == NULL) return COG_ERROR_NOT_FOUND;
    if (source->group == 0U || source->group != h->context->group || source->cancel_latched ||
        source->phase != CLI_SLOT_STARTED) return COG_ERROR_INVALID_STATE;
    if (h->operation == CLI_SEND_MESSAGE) {
        cli_executor_slot_t *target = host_agent(r, h->destination);
        if (target == NULL) return COG_ERROR_NOT_FOUND;
        if (target->group != source->group || strcmp(target->parent_session, source->parent_session) != 0 ||
            target->cancel_latched || target->phase != CLI_SLOT_STARTED)
            return COG_ERROR_INVALID_STATE;
        if (h->message == NULL) return COG_ERROR_NULL_PTR;
        if (h->message_length > 4095U || !cli_wire_utf8(h->message, h->message_length))
            return COG_ERROR_LIMIT;
        cli_mailbox_t *m = &target->mailbox;
        /* A single worst-case escaped item must fit the receiver's fixed tool
         * capture. This is checked even when its queue is currently full. */
        size_t capture = 128U;
        size_t escaped;
        if (!host_product(h->message_length, 6U, &escaped) || !cli_wire_add(&capture, escaped) ||
            capture >= m->reply.capacity || h->message_length + 1U > m->text_capacity ||
            h->output.capacity < 14U) return COG_ERROR_LIMIT;
        if (m->count == m->entry_capacity || h->message_length + 1U > m->text_capacity - m->text_used)
            return COG_ERROR_AGAIN;
        size_t index = (m->head + m->count) % m->entry_capacity;
        cli_mailbox_entry_t *e = &m->entries[index];
        e->used = true; e->source = source->agent; e->offset = m->text_used; e->length = h->message_length;
        memcpy(m->text + m->text_used, h->message, h->message_length);
        m->text_used += h->message_length; m->text[m->text_used++] = 0U; ++m->count;
        memcpy(h->output.data, "{\"sent\":true}", 14U);
        host_mailbox_result(h, 13U);
        return COG_OK;
    }
    if (h->operation != CLI_RECEIVE_MESSAGES) return COG_ERROR_INVALID_ARG;
    cli_mailbox_t *m = &source->mailbox;
    size_t count = 0U, length = 0U;
    /* Count first; no output or queue mutation until a complete batch fits. */
    for (size_t n = 0U; n <= m->count; ++n) {
        cli_wire_writer_t measure = {NULL, 0U, true};
        host_mailbox_json(&measure, m, n, n < m->count);
        if (!measure.valid || measure.used >= h->output.capacity) break;
        count = n; length = measure.used;
    }
    if (length == 0U || (m->count != 0U && count == 0U)) return COG_ERROR_LIMIT;
    cli_wire_writer_t writer = {h->output.data, 0U, true};
    host_mailbox_json(&writer, m, count, count < m->count);
    h->output.data[length] = 0U;
    for (size_t i = 0U; i < count; ++i) {
        m->entries[m->head].used = false;
        m->head = (m->head + 1U) % m->entry_capacity;
        --m->count;
    }
    /* Stable FIFO compaction reclaims only the actually copied prefix. */
    size_t offset = 0U;
    for (size_t i = 0U; i < m->count; ++i) {
        cli_mailbox_entry_t *e = &m->entries[(m->head + i) % m->entry_capacity];
        memmove(m->text + offset, m->text + e->offset, e->length + 1U);
        e->offset = offset; offset += e->length + 1U;
    }
    m->text_used = offset;
    host_mailbox_result(h, length);
    return COG_OK;
}

/** Admit one real registry member per invocation, with the group gate closed. */
cog_result_t cli_runtime_start(cog_cli_host_runtime_t *r,
    uint64_t reservation, cog_store_checkpoint_t checkpoint)
{
    if (r == NULL) return COG_ERROR_NULL_PTR;
    cli_group_record_t *g = host_group(r, reservation);
    if (g == NULL || checkpoint.generation == 0U) return COG_ERROR_INVALID_STATE;
    if (g->started) {
        if (g->started_checkpoint.generation != checkpoint.generation) return COG_ERROR_PROTOCOL;
        if (g->phase == CLI_GROUP_RUNNING || g->phase == CLI_GROUP_RESULT) return COG_OK;
        if (g->phase != CLI_GROUP_ADMITTING) return COG_ERROR_ABORTED;
    } else {
        if (g->phase != CLI_GROUP_GRANTED || r->shutdown || r->admission_frozen)
            return COG_ERROR_ABORTED;
        if (r->now_ms >= g->deadline_ms) return COG_ERROR_TIMEOUT;
        g->started = true; g->started_checkpoint = checkpoint;
        g->credit_charged = true;
        r->request_credit_held -= g->credit; r->request_credit_used += g->credit;
        g->phase = CLI_GROUP_ADMITTING;
    }
    if (g->admitted == g->submitted) {
        g->gate_open = true; g->phase = CLI_GROUP_RUNNING; return COG_OK;
    }
    size_t member = g->admitted;
    cli_executor_slot_t *slot = &r->executors[g->members[member].slot];
    cli_worker_record_t *placement = NULL;
    size_t choices = r->bounds.workers + 1U;
    for (size_t n = 0U; n < choices; ++n) {
        size_t candidate = (r->placement_cursor + n) % choices;
        if (candidate == 0U) { r->placement_cursor = 1U % choices; break; }
        cli_worker_record_t *w = &r->workers[candidate - 1U];
        if (w->used && w->profile_ready && w->native.health == COG_HEALTH_HEALTHY &&
            !w->native.reaped && !w->native.ownership_lost && w->native.peer_id != 0U &&
            w->reserved_slots < w->native.capacity) {
            placement = w; r->placement_cursor = (candidate + 1U) % choices; break;
        }
    }
    if (r->next_nonce == 0U || r->next_nonce == UINT64_MAX || r->now_ms >= g->deadline_ms) {
        g->aggregate = r->now_ms >= g->deadline_ms ? COG_ERROR_TIMEOUT : COG_ERROR_LIMIT;
        g->phase = CLI_GROUP_ROLLBACK;
        return g->aggregate;
    }
    uint64_t remaining = g->deadline_ms - r->now_ms;
    cog_agent_config_t config = {
        .task = (const char *)g->tasks.data + g->task_offsets[member],
        .parent_session_id = g->parent_session,
        .timeout_ms = remaining > UINT32_MAX ? UINT32_MAX : (uint32_t)remaining,
        .force_local = placement == NULL, .group_id = g->id,
        .owner_peer = placement == NULL ? 0U : placement->native.peer_id,
        .execution_epoch = r->next_nonce,
        .max_status_bytes = r->bounds.status_bytes,
        .max_result_bytes = g->result_payload_cap + 1U};
    cog_agent_id_t id;
    cog_result_t rc = cog_agent_spawn(r->control_core, &config, &id);
    if (rc == COG_ERROR_OVERFLOW || rc == COG_ERROR_AGAIN) return COG_ERROR_AGAIN;
    if (rc != COG_OK) {
        g->aggregate = rc; g->phase = CLI_GROUP_ROLLBACK;
        for (size_t i = 0U; i < g->submitted; ++i) {
            r->executors[g->members[i].slot].cancel_latched = true;
            if (!g->members[i].present) g->members[i].outcome = rc;
        }
        return rc;
    }
    ++r->next_nonce;
    /* Record real admission before any later fallible query. Rollback therefore
     * always distinguishes an actual identity from an absent member. */
    g->members[member].present = true; g->members[member].agent = id;
    g->members[member].attempt = config.execution_epoch;
    slot->agent = id; slot->attempt = config.execution_epoch;
    slot->owner_peer = config.owner_peer;
    slot->configuration_generation = g->pinned_configuration;
    slot->runtime = r->runtime_incarnation; slot->deadline_ms = g->deadline_ms;
    slot->parent_node = g->parent_node; slot->parent_ordinal = g->ordinal;
    memcpy(slot->parent_session, g->parent_session, sizeof(slot->parent_session));
    memcpy(slot->parent_call, g->call_id, sizeof(slot->parent_call));
    if (placement != NULL) { slot->worker = placement->id; ++placement->reserved_slots; }
    ++g->admitted;
    if (g->admitted != g->submitted) return COG_ERROR_AGAIN;
    /* Full membership is fixed before any executor may obtain its task. */
    for (size_t i = 0U; i < g->submitted; ++i) {
        cli_executor_slot_t *s = &r->executors[g->members[i].slot];
        s->member_count = g->submitted;
        for (size_t j = 0U; j < g->submitted; ++j) s->members[j] = g->members[j].agent;
    }
    g->gate_open = true; g->phase = CLI_GROUP_RUNNING;
    return COG_OK;
}

static cli_control_lane_t *host_client_lane(cog_cli_host_runtime_t *r,
    const cli_client_record_t *client, cog_cli_control_lane_t lane)
{
    size_t index = (size_t)(client - r->clients);
    return &r->lanes[(r->bounds.executors + index) * (2U * CLI_HOST_LANES) +
        CLI_HOST_LANES + (size_t)lane - 1U];
}

static cog_result_t host_ui_result(cog_cli_host_runtime_t *r,
    cog_cli_control_lane_t lane, uint64_t answer, uint64_t lease,
    cog_result_t outcome, cli_host_bytes_t *out, uint64_t *wire)
{
    if (r->next_nonce == 0U || r->next_nonce == UINT64_MAX) return COG_ERROR_LIMIT;
    unsigned char body[21];
    cli_wire_writer_t w = {body, 0U, true};
    cli_wire_put(&w, (uint32_t)outcome, 4U); cli_wire_put(&w, 2U, 1U);
    cli_wire_put(&w, 0U, 8U); cli_wire_put(&w, lease, 8U);
    cog_cli_control_message_t message = {.kind = COG_CLI_CONTROL_COMMAND_RESULT,
        .lane = lane, .runtime_incarnation = r->runtime_incarnation,
        .sender_incarnation = r->runtime_incarnation, .wire_id = r->next_nonce,
        .reply_to = answer, .session_id = r->primary_session,
        .payload = body, .payload_len = sizeof(body)};
    size_t length;
    cog_result_t rc = cog_cli_control_encode(&message, out->data, out->capacity, &length);
    if (rc != COG_OK) return rc;
    out->length = length; *wire = r->next_nonce++;
    return COG_OK;
}

static bool host_bytes_equal(const cli_host_bytes_t *bytes, const void *frame, size_t length)
{ return bytes->length == length && memcmp(bytes->data, frame, length) == 0; }

static cog_result_t host_client_queue(cli_client_record_t *client, const cli_host_bytes_t *frame)
{
    if (client->send.length != 0U) {
        return host_bytes_equal(&client->send, frame->data, frame->length) ? COG_OK : COG_ERROR_AGAIN;
    }
    if (frame->length > client->send.capacity) return COG_ERROR_LIMIT;
    memcpy(client->send.data, frame->data, frame->length);
    client->send.length = frame->length;
    return COG_OK;
}

static cog_result_t host_ui_hello(cog_cli_host_runtime_t *r, const cog_event_ipc_t *event,
    const cog_cli_control_message_t *message)
{
    if (message->lane != COG_CLI_LANE_COMMAND || message->reply_to != 0U ||
        message->payload_len != 18U) return COG_ERROR_PROTOCOL;
    cli_wire_reader_t reader = {message->payload, message->payload_len, 0U, true};
    uint64_t version = cli_wire_get(&reader, 1U), role = cli_wire_get(&reader, 1U);
    uint64_t generation = cli_wire_get(&reader, 8U), sequence = cli_wire_get(&reader, 8U);
    if (version != 1U || (role != 1U && role != 2U)) return COG_ERROR_PROTOCOL;
    cog_cli_projection_view_t view;
    cog_result_t rc = cog_cli_projection_snapshot(r->projection, &view);
    if (rc != COG_OK) return rc;
    bool initial = message->runtime_incarnation == 0U && message->session_id[0] == '\0';
    if (!initial && (message->runtime_incarnation != r->runtime_incarnation ||
        strcmp(message->session_id, r->primary_session) != 0)) return COG_ERROR_INVALID_STATE;
    cli_client_record_t *client = NULL, *vacant = NULL;
    bool controller = false;
    for (size_t i = 0U; i < r->bounds.clients; ++i) {
        cli_client_record_t *c = &r->clients[i];
        if (!c->used) { if (vacant == NULL) vacant = c; continue; }
        if (c->controller && (c->connected || r->now_ms < c->controller_until_ms)) controller = true;
        if ((initial && c->hello.initial_nonce == message->sender_incarnation) ||
            (!initial && c->logical_lease == message->sender_incarnation)) client = c;
    }
    if (client != NULL) {
        if (client->connected && client->peer != event->peer) return COG_ERROR_DENIED;
        cli_client_hello_record_t *h = &client->hello;
        if (h->request_wire == message->wire_id && h->request_sender == message->sender_incarnation) {
            if (!host_bytes_equal(&h->original, event->msg_payload, event->msg_payload_len))
                return COG_ERROR_PROTOCOL;
            client->peer = event->peer; client->proof = event->peer_info; client->connected = true;
            return host_client_queue(client, &h->reply);
        }
        if (initial || !h->acknowledged || message->wire_id <= h->retired_through ||
            message->wire_id <= h->request_wire) return COG_ERROR_INVALID_STATE;
    } else {
        if (!initial) return COG_ERROR_NOT_FOUND;
        if (vacant == NULL || r->next_client_lease == 0U ||
            r->next_client_lease == UINT64_MAX) return COG_ERROR_LIMIT;
        client = vacant;
    }
    cli_client_hello_record_t *h = &client->hello;
    if (event->msg_payload_len > h->original.capacity || r->next_nonce > UINT64_MAX - 16U)
        return COG_ERROR_LIMIT;
    uint64_t lease = client->used ? client->logical_lease : r->next_client_lease;
    uint64_t wire;
    rc = host_ui_result(r, COG_CLI_LANE_COMMAND, message->wire_id, lease, COG_OK, &h->reply, &wire);
    if (rc != COG_OK) return rc;
    if (!client->used) {
        client->used = true; client->logical_lease = r->next_client_lease++;
        client->controller = role == 1U && !controller;
        h->initial_nonce = message->sender_incarnation;
    }
    h->retired_through = h->ack_wire > h->request_wire ? h->ack_wire : h->request_wire;
    memcpy(h->original.data, event->msg_payload, event->msg_payload_len);
    h->original.length = event->msg_payload_len;
    h->request_sender = message->sender_incarnation; h->request_wire = message->wire_id;
    h->result_wire = wire; h->pending = true; h->final_valid = true; h->acknowledged = false;
    h->ack_wire = 0U; h->ack_result_wire = 0U; h->ack_reply.length = 0U;
    client->peer = event->peer; client->proof = event->peer_info; client->connected = true;
    client->rendered_generation = generation; client->rendered_sequence = sequence;
    client->queued_sequence = sequence;
    client->awaiting_resync = initial || generation != view.display.generation ||
        sequence > view.display.through_sequence;
    client->receipts_until_ms = r->now_ms + r->fixed.client_receipt_ms;
    return host_client_queue(client, &h->reply);
}

static cog_result_t host_ui_ack(cog_cli_host_runtime_t *r, cli_client_record_t *client,
    const cog_event_ipc_t *event, const cog_cli_control_message_t *m)
{
    if (m->payload_len != 0U || m->reply_to == 0U ||
        (m->lane != COG_CLI_LANE_COMMAND && m->lane != COG_CLI_LANE_CANCEL)) return COG_ERROR_PROTOCOL;
    cli_client_hello_record_t *h = &client->hello;
    if (m->lane == COG_CLI_LANE_COMMAND && h->final_valid && m->reply_to == h->result_wire) {
        if (h->acknowledged) {
            if (m->wire_id != h->ack_wire) return COG_ERROR_PROTOCOL;
            return host_client_queue(client, &h->ack_reply);
        }
        cog_result_t rc = host_ui_result(r, m->lane, m->wire_id, 0U, COG_OK,
            &h->ack_reply, &h->ack_result_wire);
        if (rc != COG_OK) return rc;
        h->ack_wire = m->wire_id; h->acknowledged = true; h->pending = false;
        return host_client_queue(client, &h->ack_reply);
    }
    cli_control_lane_t *lane = host_client_lane(r, client, m->lane);
    if (lane->final_valid && m->wire_id == lane->final_answered_wire) {
        if (!host_bytes_equal(&lane->final_original, event->msg_payload, event->msg_payload_len))
            return COG_ERROR_PROTOCOL;
        return host_client_queue(client, &lane->final_reply);
    }
    for (size_t i = 0U; i < r->bounds.command_records; ++i) {
        cli_command_record_t *command = &r->commands[i];
        if (command->phase != CLI_COMMAND_FINAL || command->lease != client->logical_lease ||
            command->result_wire != m->reply_to) continue;
        if (command->original.length < 6U || command->original.data[5] != (unsigned char)m->lane)
            return COG_ERROR_PROTOCOL;
        if (event->msg_payload_len > lane->final_original.capacity) return COG_ERROR_LIMIT;
        uint64_t wire;
        cog_result_t rc = host_ui_result(r, m->lane, m->wire_id, 0U, COG_OK, &lane->final_reply, &wire);
        if (rc != COG_OK) return rc;
        memcpy(lane->final_original.data, event->msg_payload, event->msg_payload_len);
        lane->final_original.length = event->msg_payload_len;
        lane->final_valid = true; lane->final_answered_wire = m->wire_id; lane->final_reply_wire = wire;
        if (command->request_wire > lane->retired_through) lane->retired_through = command->request_wire;
        command->phase = CLI_COMMAND_FREE;
        return host_client_queue(client, &lane->final_reply);
    }
    return COG_ERROR_NOT_FOUND;
}

static cog_result_t host_ui_command(cog_cli_host_runtime_t *r, cli_client_record_t *client,
    const cog_event_ipc_t *event, const cog_cli_control_message_t *m)
{
    if (m->reply_to != 0U || (m->lane != COG_CLI_LANE_COMMAND && m->lane != COG_CLI_LANE_CANCEL))
        return COG_ERROR_PROTOCOL;
    cli_command_record_t *vacant = NULL;
    for (size_t i = 0U; i < r->bounds.command_records; ++i) {
        cli_command_record_t *record = &r->commands[i];
        if (record->phase == CLI_COMMAND_FREE) { if (vacant == NULL) vacant = record; continue; }
        if (record->lease == client->logical_lease && record->request_wire == m->wire_id) {
            if (!host_bytes_equal(&record->original, event->msg_payload, event->msg_payload_len))
                return COG_ERROR_PROTOCOL;
            return record->phase == CLI_COMMAND_FINAL ? host_client_queue(client, &record->reply) : COG_ERROR_AGAIN;
        }
    }
    cli_control_lane_t *lane = host_client_lane(r, client, m->lane);
    if (m->wire_id <= lane->retired_through) return COG_ERROR_INVALID_STATE;
    if (vacant == NULL || event->msg_payload_len > vacant->original.capacity ||
        r->next_nonce > UINT64_MAX - 16U) return COG_ERROR_LIMIT;
    cog_cli_command_t command;
    size_t required;
    cog_result_t rc = cog_cli_command_decode(m->payload, m->payload_len, &command,
        vacant->decoded_storage.data, vacant->decoded_storage.capacity, &required);
    if (rc != COG_OK) return rc;
    if ((command.kind == COG_CLI_COMMAND_CANCEL) != (m->lane == COG_CLI_LANE_CANCEL))
        return COG_ERROR_PROTOCOL;
    cog_cli_projection_view_t view;
    rc = cog_cli_projection_snapshot(r->projection, &view);
    if (rc != COG_OK) return rc;
    cog_result_t refusal = COG_OK;
    if (!client->hello.acknowledged) refusal = COG_ERROR_INVALID_STATE;
    else if (command.kind != COG_CLI_COMMAND_RESYNC && !client->controller) refusal = COG_ERROR_DENIED;
    else if (command.kind != COG_CLI_COMMAND_RESYNC &&
        (client->awaiting_resync || client->rendered_generation != command.observed_generation ||
         client->rendered_sequence < command.observed_sequence ||
         command.observed_generation != view.display.generation ||
         command.observed_sequence != view.display.through_sequence)) refusal = COG_ERROR_INVALID_STATE;
    else if (command.kind != COG_CLI_COMMAND_RESYNC && (r->admission_frozen || r->shutdown))
        refusal = COG_ERROR_ABORTED;
    memcpy(vacant->original.data, event->msg_payload, event->msg_payload_len);
    vacant->original.length = event->msg_payload_len;
    vacant->lease = client->logical_lease; vacant->request_wire = m->wire_id;
    vacant->observed_generation = command.observed_generation;
    vacant->observed_sequence = command.observed_sequence; vacant->nonce = command.nonce;
    vacant->owner = r->primary_owner; vacant->result = refusal;
    vacant->phase = CLI_COMMAND_COPIED;
    if (refusal != COG_OK) {
        rc = host_ui_result(r, m->lane, m->wire_id, 0U, refusal, &vacant->reply, &vacant->result_wire);
        if (rc != COG_OK) return rc;
        vacant->phase = CLI_COMMAND_FINAL;
        return host_client_queue(client, &vacant->reply);
    }
    return COG_OK;
}

static cog_result_t host_ui_ingress(cog_cli_host_runtime_t *r, const cog_event_ipc_t *event)
{
    if (!event->peer_info_valid || event->peer_info.peer_id != event->peer ||
        event->peer_info.user_id != (uint64_t)geteuid() || event->peer_info.process_id == 0U)
        return COG_ERROR_DENIED;
    cog_cli_control_message_t m;
    size_t required;
    cog_result_t rc = cog_cli_control_decode(event->msg_payload, event->msg_payload_len,
        &m, r->decode.data, r->decode.capacity, &required);
    if (rc != COG_OK) return rc;
    if (m.worker.incarnation != 0U || m.worker.number != 0U || m.owner_peer != 0U ||
        m.agent_id != 0U || m.execution_epoch != 0U) return COG_ERROR_DENIED;
    if (m.kind == COG_CLI_CONTROL_UI_HELLO) return host_ui_hello(r, event, &m);
    if (m.runtime_incarnation != r->runtime_incarnation || strcmp(m.session_id, r->primary_session) != 0)
        return COG_ERROR_INVALID_STATE;
    cli_client_record_t *client = NULL;
    for (size_t i = 0U; i < r->bounds.clients; ++i)
        if (r->clients[i].used && r->clients[i].logical_lease == m.sender_incarnation &&
            r->clients[i].connected && r->clients[i].peer == event->peer) client = &r->clients[i];
    if (client == NULL) return COG_ERROR_NOT_FOUND;
    switch (m.kind) {
    case COG_CLI_CONTROL_UI_COMMAND: return host_ui_command(r, client, event, &m);
    case COG_CLI_CONTROL_COMMAND_ACK: return host_ui_ack(r, client, event, &m);
    default: return COG_ERROR_DENIED;
    }
}

/** Protected core ingress: retain intent/config bytes before their lease ends. */
cog_handler_result_t cli_host_dispatch(cog_core_t *core, const cog_event_t *event)
{
    cog_handler_result_t result = {COG_TRANSITION_SELF, COG_OK};
    if (core == NULL || event == NULL) { result.status = COG_ERROR_NULL_PTR; return result; }
    cog_cli_host_runtime_t *r = cog_core_user(core);
    if (r == NULL) { result.status = COG_ERROR_INVALID_STATE; return result; }
    if (event->kind == COG_EVENT_SUBAGENT_SPAWN || event->kind == COG_EVENT_SUBAGENT_CANCEL) {
        if (event->payload == NULL || event->payload_len != sizeof(cog_event_agent_intent_t)) {
            result.status = COG_ERROR_PROTOCOL; return result;
        }
        cog_event_agent_intent_t intent;
        memcpy(&intent, event->payload, sizeof(intent));
        cli_executor_slot_t *slot = host_agent(r, intent.agent_id);
        if (slot == NULL || intent.action != event->kind || intent.agent_id != event->source_id ||
            intent.execution_epoch != slot->attempt || intent.owner_peer != slot->owner_peer) {
            result.status = COG_ERROR_PROTOCOL; return result;
        }
        if (event->kind == COG_EVENT_SUBAGENT_CANCEL) { slot->cancel_latched = true; return result; }
        cog_agent_config_t config;
        size_t required;
        result.status = cog_agent_copy_config(core, slot->agent, &config,
            (char *)slot->task.data, slot->task.capacity, &required);
        if (result.status != COG_OK) return result;
        if (config.owner_peer != slot->owner_peer || config.execution_epoch != slot->attempt ||
            config.group_id != slot->group || config.parent_session_id == NULL ||
            strcmp(config.parent_session_id, slot->parent_session) != 0) {
            result.status = COG_ERROR_PROTOCOL; return result;
        }
        slot->task.length = strlen(config.task);
        const char *session = NULL;
        result.status = cog_agent_session_id(core, slot->agent, &session);
        if (result.status != COG_OK) return result;
        size_t n = strlen(session);
        if (n >= sizeof(slot->session)) { result.status = COG_ERROR_LIMIT; return result; }
        memcpy(slot->session, session, n + 1U);
        bool cancelled;
        result.status = cog_agent_is_cancelled(core, slot->agent, &cancelled);
        if (result.status != COG_OK) return result;
        slot->cancel_latched = slot->cancel_latched || cancelled;
        slot->phase = CLI_SLOT_PROFILE; /* no app/task effects in ingress */
        return result;
    }
    if (event->kind == COG_EVENT_IPC_CONNECTED || event->kind == COG_EVENT_IPC_MESSAGE ||
        event->kind == COG_EVENT_IPC_DISCONNECTED) {
        if (event->payload == NULL || event->payload_len != sizeof(cog_event_ipc_t)) {
            result.status = COG_ERROR_PROTOCOL; return result;
        }
        const cog_event_ipc_t *ipc = event->payload;
        if (event->kind == COG_EVENT_IPC_MESSAGE) {
            /* Complete application-frame refusals are isolated. Actual Q driver
             * failures propagate from core_step independently of this return. */
            cog_result_t rc = host_ui_ingress(r, ipc);
            if (rc == COG_ERROR_IO || rc == COG_ERROR_NOMEM || rc == COG_ERROR_INDETERMINATE)
                result.status = rc;
        } else if (event->kind == COG_EVENT_IPC_DISCONNECTED) {
            for (size_t i = 0U; i < r->bounds.clients; ++i) {
                cli_client_record_t *client = &r->clients[i];
                if (!client->used || client->peer != ipc->peer) continue;
                client->connected = false;
                client->controller_until_ms = r->now_ms + r->fixed.controller_reconnect_ms;
                client->receipts_until_ms = r->now_ms + r->fixed.client_receipt_ms;
                client->send.length = 0U;
            }
        }
        return result;
    }
    /* Agent notifications are copied/acknowledged by bounded owner service. */
    if (event->kind != COG_EVENT_SUBAGENT_RESULT && event->kind != COG_EVENT_SUBAGENT_STATUS)
        result.status = COG_ERROR_PROTOCOL;
    return result;
}

static cog_result_t host_bytes_new(cli_host_bytes_t *bytes, size_t capacity)
{
    bytes->data = calloc(capacity == 0U ? 1U : capacity, 1U);
    if (bytes->data == NULL) return COG_ERROR_NOMEM;
    bytes->capacity = capacity;
    return COG_OK;
}
static void host_bytes_free(cli_host_bytes_t *bytes)
{ free(bytes->data); *bytes = (cli_host_bytes_t){0}; }
static void host_secret_clear(cli_secret_exchange_t *secret)
{
    unsigned char *buffers[] = {secret->body, secret->frame, secret->scratch};
    size_t sizes[] = {secret->body_capacity, secret->frame_capacity, secret->scratch_capacity};
    for (size_t i = 0U; i < 3U; ++i) {
        if (buffers[i] == NULL) continue;
        volatile unsigned char *p = buffers[i];
        for (size_t n = 0U; n < sizes[i]; ++n) p[n] = 0U;
        free(buffers[i]);
    }
    *secret = (cli_secret_exchange_t){0};
}
static cog_result_t host_secret_new(cli_secret_exchange_t *secret)
{
    secret->body_capacity = 90112U; secret->frame_capacity = 90368U;
    secret->scratch_capacity = 90112U;
    secret->body = calloc(secret->body_capacity, 1U);
    secret->frame = calloc(secret->frame_capacity, 1U);
    secret->scratch = calloc(secret->scratch_capacity, 1U);
    return secret->body == NULL || secret->frame == NULL || secret->scratch == NULL ? COG_ERROR_NOMEM : COG_OK;
}
static cog_result_t host_forward_new(cli_forwarded_operation_t *op, size_t capacity)
{
    cog_result_t rc = host_bytes_new(&op->original, capacity);
    if (rc == COG_OK) rc = host_bytes_new(&op->decoded_storage, capacity);
    if (rc == COG_OK) rc = host_bytes_new(&op->reply, 512U);
    return rc;
}
static void host_forward_free(cli_forwarded_operation_t *op)
{
    host_bytes_free(&op->original); host_bytes_free(&op->decoded_storage); host_bytes_free(&op->reply);
}
static cog_result_t host_allocations(cog_cli_host_runtime_t *r, const cog_cli_options_t *options)
{
    r->clients = calloc(r->bounds.clients, sizeof(*r->clients));
    r->commands = calloc(r->bounds.command_records, sizeof(*r->commands));
    r->workers = calloc(r->bounds.workers == 0U ? 1U : r->bounds.workers, sizeof(*r->workers));
    r->executors = calloc(r->bounds.executors, sizeof(*r->executors));
    r->groups = calloc(r->bounds.children, sizeof(*r->groups)); r->group_capacity = r->bounds.children;
    r->lanes = calloc(r->bounds.streams, sizeof(*r->lanes));
    if (r->clients == NULL || r->commands == NULL || r->workers == NULL ||
        r->executors == NULL || r->groups == NULL || r->lanes == NULL) return COG_ERROR_NOMEM;
    size_t command = r->bounds.lane_frame_bytes[COG_CLI_LANE_COMMAND - 1U];
    size_t frame = r->bounds.projection_bytes + 512U;
    cog_result_t rc;
#define HOST_BUFFER(field, amount) do { rc = host_bytes_new(&(field), (amount)); if (rc != COG_OK) return rc; } while (0)
    HOST_BUFFER(r->ingress, frame); HOST_BUFFER(r->decode, frame);
    HOST_BUFFER(r->projection_scratch, frame);
    for (size_t i = 0U; i < r->bounds.clients; ++i) {
        cli_client_record_t *c = &r->clients[i];
        HOST_BUFFER(c->hello.original, 512U); HOST_BUFFER(c->hello.reply, 512U);
        HOST_BUFFER(c->hello.ack_reply, 512U); HOST_BUFFER(c->send, frame); HOST_BUFFER(c->resync, frame);
    }
    for (size_t i = 0U; i < r->bounds.command_records; ++i) {
        HOST_BUFFER(r->commands[i].original, command); HOST_BUFFER(r->commands[i].decoded_storage, command);
        HOST_BUFFER(r->commands[i].reply, 512U);
    }
    for (size_t i = 0U; i < r->bounds.streams; ++i) {
        size_t lane = i % CLI_HOST_LANES;
        size_t key = i / (2U * CLI_HOST_LANES);
        bool outbound = i % (2U * CLI_HOST_LANES) >= CLI_HOST_LANES;
        size_t cap = r->bounds.lane_frame_bytes[lane];
        /* Replay bytes have one actual send/resync owner, not eight copies per
         * generic receipt. Worker PROFILE secrets have their separate arenas. */
        if (lane == COG_CLI_LANE_PROFILE - 1U || lane == COG_CLI_LANE_REPLAY - 1U ||
            ((lane == COG_CLI_LANE_VIEW - 1U || lane == COG_CLI_LANE_DELEGATE - 1U) && key >= 2U)) cap = 512U;
        if (lane == COG_CLI_LANE_VIEW - 1U && outbound) cap = 512U;
        HOST_BUFFER(r->lanes[i].pending, cap); HOST_BUFFER(r->lanes[i].final_original, cap);
        HOST_BUFFER(r->lanes[i].provisional_reply, 512U); HOST_BUFFER(r->lanes[i].final_reply, cap);
        r->lanes[i].next_wire = 1U;
    }
    for (size_t i = 0U; i < r->bounds.executors; ++i) {
        cli_executor_slot_t *s = &r->executors[i];
        s->inbound = &r->lanes[i * 2U * CLI_HOST_LANES]; s->outbound = s->inbound + CLI_HOST_LANES;
        HOST_BUFFER(s->task, r->bounds.task_bytes + 128U);
        HOST_BUFFER(s->status, r->bounds.status_bytes); HOST_BUFFER(s->result, r->bounds.result_bytes);
        HOST_BUFFER(s->update, r->bounds.status_bytes + r->bounds.result_bytes + 512U);
        rc = host_forward_new(&s->command, command); if (rc != COG_OK) return rc;
        rc = host_forward_new(&s->cancel, 512U); if (rc != COG_OK) return rc;
        rc = host_secret_new(&s->profile); if (rc != COG_OK) return rc;
        if (i < 2U) continue;
        s->mailbox.entry_capacity = r->bounds.mailbox_entries;
        s->mailbox.text_capacity = r->bounds.mailbox_bytes;
        s->mailbox.entries = calloc(s->mailbox.entry_capacity, sizeof(*s->mailbox.entries));
        s->mailbox.text = calloc(s->mailbox.text_capacity, 1U);
        if (s->mailbox.entries == NULL || s->mailbox.text == NULL) return COG_ERROR_NOMEM;
        HOST_BUFFER(s->mailbox.reply, options->max_tool_output_bytes);
    }
    for (size_t i = 0U; i < r->bounds.workers; ++i) {
        rc = host_secret_new(&r->workers[i].startup_profile); if (rc != COG_OK) return rc;
    }
    size_t task_pool;
    if (!host_product(r->bounds.children, r->bounds.task_bytes, &task_pool)) return COG_ERROR_LIMIT;
    for (size_t i = 0U; i < r->group_capacity; ++i) {
        HOST_BUFFER(r->groups[i].tasks, task_pool);
        HOST_BUFFER(r->groups[i].output, options->max_tool_output_bytes);
    }
#undef HOST_BUFFER
    return COG_OK;
}

static cog_result_t host_first(cog_result_t first, cog_result_t next)
{ return first == COG_OK ? next : first; }

/** Checked consuming host disposal; executor borrows end before registry free. */
cog_result_t cog_cli_host_runtime_close(cog_cli_host_runtime_t **owner, cog_result_t primary)
{
    if (owner == NULL) return COG_ERROR_NULL_PTR;
    cog_cli_host_runtime_t *r = *owner;
    if (r == NULL) return primary;
    if (r->in_step || (r->begun && !r->done)) return COG_ERROR_INVALID_STATE;
    cog_result_t rc = primary;
    if (r->handoff != NULL) {
        cog_result_t end = cog_cli_handoff_close(&r->handoff, COG_OK);
        if (r->handoff != NULL) return end;
        rc = host_first(rc, end);
    }
    if (r->executors != NULL) for (size_t i = 0U; i < r->bounds.executors; ++i) {
        cli_executor_slot_t *s = &r->executors[i];
        cog_result_t end = cog_cli_execution_close(&s->app, COG_OK);
        if (s->app != NULL) return end;
        rc = host_first(rc, end);
        cog_cli_configuration_free(s->pending_profile); s->pending_profile = NULL;
        host_secret_clear(&s->profile);
        if (s->net != NULL) {
            if (s->observer != NULL) rc = host_first(rc, cog_cli_observer_clear(s->observer, s->net));
            cog_net_free(s->net); s->net = NULL;
        }
        cog_cli_observer_free(s->observer); s->observer = NULL;
        if (s->borrow_live) {
            end = cog_agent_session_return(r->control_core, s->agent, s->borrowed_session);
            if (end != COG_OK) return host_first(rc, end);
            s->borrow_live = false; s->borrowed_session = NULL;
        }
        host_bytes_free(&s->task); host_bytes_free(&s->status); host_bytes_free(&s->result);
        host_bytes_free(&s->update); host_forward_free(&s->command); host_forward_free(&s->cancel);
        host_bytes_free(&s->mailbox.reply); free(s->mailbox.entries); free(s->mailbox.text);
    }
    if (r->workers != NULL) for (size_t i = 0U; i < r->bounds.workers; ++i)
        host_secret_clear(&r->workers[i].startup_profile);
    cog_cli_configuration_free(r->future_mirror); cog_cli_configuration_free(r->current_mirror);
    cog_orchestrator_free(r->orchestrator);
    cog_core_free(r->control_core);
    if (!r->ui_transferred && r->ui_alias != NULL) rc = host_first(rc, cog_ipc_endpoint_free(r->ui_alias));
    cog_session_free(r->control_session); cog_store_free(r->catalog);
    cog_cli_projection_free(r->projection);
    if (r->clients != NULL) for (size_t i = 0U; i < r->bounds.clients; ++i) {
        host_bytes_free(&r->clients[i].hello.original); host_bytes_free(&r->clients[i].hello.reply);
        host_bytes_free(&r->clients[i].hello.ack_reply); host_bytes_free(&r->clients[i].send);
        host_bytes_free(&r->clients[i].resync);
    }
    if (r->commands != NULL) for (size_t i = 0U; i < r->bounds.command_records; ++i) {
        host_bytes_free(&r->commands[i].original); host_bytes_free(&r->commands[i].decoded_storage);
        host_bytes_free(&r->commands[i].reply);
    }
    if (r->groups != NULL) for (size_t i = 0U; i < r->group_capacity; ++i) {
        host_bytes_free(&r->groups[i].tasks); host_bytes_free(&r->groups[i].output);
    }
    if (r->lanes != NULL) for (size_t i = 0U; i < r->bounds.streams; ++i) {
        host_bytes_free(&r->lanes[i].pending); host_bytes_free(&r->lanes[i].provisional_reply);
        host_bytes_free(&r->lanes[i].final_original); host_bytes_free(&r->lanes[i].final_reply);
    }
    host_bytes_free(&r->ingress); host_bytes_free(&r->decode); host_bytes_free(&r->projection_scratch);
    free(r->clients); free(r->commands); free(r->workers); free(r->executors); free(r->groups); free(r->lanes);
    if (r->workspace_directory_fd >= 0 && close(r->workspace_directory_fd) != 0) rc = host_first(rc, COG_ERROR_IO);
    if (r->store_directory_fd >= 0 && close(r->store_directory_fd) != 0) rc = host_first(rc, COG_ERROR_IO);
    free(r->canonical_workspace); free(r->canonical_store);
    free(r->ui_path); free(r->worker_path); free(r->executable); free(r->ca_bundle);
    free(r); *owner = NULL;
    return rc;
}

static cog_result_t host_clock(uint64_t *milliseconds)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0 || now.tv_sec < 0 ||
        (uint64_t)now.tv_sec > UINT64_MAX / 1000U) return COG_ERROR_IO;
    *milliseconds = (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
    return COG_OK;
}
static cog_result_t host_incarnation(uint64_t *value)
{
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return COG_ERROR_IO;
    ssize_t n = read(fd, value, sizeof(*value));
    int end = close(fd);
    return n == (ssize_t)sizeof(*value) && *value != 0U && end == 0 ? COG_OK : COG_ERROR_IO;
}
static cog_result_t host_directory(const char *path, bool create, char **canonical, int *pin)
{
    if (path == NULL) return COG_ERROR_NULL_PTR;
    size_t length = strlen(path);
    if (length == 0U || length > 4095U) return COG_ERROR_INVALID_ARG;
    if (create) {
        char copy[4096]; memcpy(copy, path, length + 1U);
        for (size_t i = 1U; i <= length; ++i) {
            if (copy[i] != '/' && copy[i] != '\0') continue;
            char saved = copy[i]; copy[i] = '\0';
            if (mkdir(copy, 0700) != 0 && errno != EEXIST) return COG_ERROR_IO;
            copy[i] = saved;
        }
    }
    char *resolved = realpath(path, NULL);
    if (resolved == NULL) return COG_ERROR_IO;
    if (strlen(resolved) > 4095U) { free(resolved); return COG_ERROR_LIMIT; }
    int fd = open(resolved, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) { free(resolved); return COG_ERROR_IO; }
    struct stat actual, configured;
    if (fstat(fd, &actual) != 0 || stat(path, &configured) != 0 ||
        actual.st_dev != configured.st_dev || actual.st_ino != configured.st_ino) {
        (void)close(fd); free(resolved); return COG_ERROR_IO;
    }
    *canonical = resolved; *pin = fd;
    return COG_OK;
}
static cog_result_t host_transport(cog_cli_host_runtime_t *r, cli_executor_slot_t *slot)
{
    cog_result_t rc = cog_cli_observer_new(&slot->observer, r->net_template.max_connections);
    if (rc != COG_OK) return rc;
    cog_net_config_t config = r->net_template;
    config.observer = cog_cli_observer_capture; config.observer_user = slot->observer;
    return cog_net_new(&slot->net, &config);
}
static const cog_state_t host_transitions[] = {0U, COG_TRANSITION_END};
static const cog_dispatch_entry_t host_dispatch[] = {
    {0U, COG_EVENT_IPC_CONNECTED, cli_host_dispatch, host_transitions},
    {0U, COG_EVENT_IPC_MESSAGE, cli_host_dispatch, host_transitions},
    {0U, COG_EVENT_IPC_DISCONNECTED, cli_host_dispatch, host_transitions},
    {0U, COG_EVENT_SUBAGENT_SPAWN, cli_host_dispatch, host_transitions},
    {0U, COG_EVENT_SUBAGENT_CANCEL, cli_host_dispatch, host_transitions},
    {0U, COG_EVENT_SUBAGENT_STATUS, cli_host_dispatch, host_transitions},
    {0U, COG_EVENT_SUBAGENT_RESULT, cli_host_dispatch, host_transitions}
};

/** Construct all bounded owners before publishing the host; never fork here. */
cog_result_t cog_cli_host_runtime_new(cog_cli_host_runtime_t **out,
    const cog_cli_host_runtime_config_t *config)
{
    if (out == NULL) return COG_ERROR_NULL_PTR;
    *out = NULL;
    if (config == NULL || config->root_options == NULL || config->ui_socket == NULL)
        return COG_ERROR_NULL_PTR;
    if (config->root_options->net != NULL || config->root_options->socket_path != NULL ||
        config->ui_socket[0] == '\0' || config->workers > CLI_HOST_MAX_WORKERS ||
        (config->workers != 0U && (config->worker_socket == NULL || config->executable == NULL ||
         config->executable[0] != '/' || strcmp(config->worker_socket, config->ui_socket) == 0)))
        return COG_ERROR_INVALID_ARG;
    if (config->signal_fd >= 0) {
        int flags = fcntl(config->signal_fd, F_GETFL);
        if (flags < 0 || (flags & O_NONBLOCK) == 0) return COG_ERROR_INVALID_ARG;
    }
    cog_cli_configuration_t *resolved = NULL;
    cog_result_t rc = cog_cli_configuration_new(&resolved, config->root_options, 1U);
    if (rc != COG_OK) return rc;
    const cog_cli_options_t *options = cog_cli_configuration_options(resolved);
    if (options->store_mode != COG_CLI_STORE_DURABLE ||
        (options->session_action != COG_CLI_SESSION_NEW && options->session_action != COG_CLI_SESSION_RESUME &&
         options->session_action != COG_CLI_SESSION_FORK)) {
        cog_cli_configuration_free(resolved); return COG_ERROR_INVALID_ARG;
    }
    if (config->workers != 0U && options->backend != COG_CLI_BACKEND_REAL) {
        cog_cli_configuration_free(resolved); return COG_ERROR_UNSUPPORTED;
    }
    cli_host_reservations_t bounds;
    rc = cli_host_plan(config, options, &bounds);
    if (rc != COG_OK) { cog_cli_configuration_free(resolved); return rc; }
    cog_cli_host_runtime_t *r = calloc(1U, sizeof(*r));
    if (r == NULL) { cog_cli_configuration_free(resolved); return COG_ERROR_NOMEM; }
    r->workspace_directory_fd = -1; r->store_directory_fd = -1;
    r->bounds = bounds; r->fixed = *config; r->fixed.root_options = NULL;
    r->fixed.children = bounds.children; r->fixed.ui_clients = bounds.clients;
    r->fixed.controller_reconnect_ms = config->controller_reconnect_ms == 0U ? 3000U : config->controller_reconnect_ms;
    r->fixed.client_receipt_ms = config->client_receipt_ms == 0U ? 30000U : config->client_receipt_ms;
    r->fixed.shutdown_ms = config->shutdown_ms == 0U ? 3000U : config->shutdown_ms;
    r->next_client_lease = 1U; r->next_group = 1U; r->next_reservation = 1U; r->next_nonce = 1U;
    r->active_configuration = 1U; r->published_configuration = 1U;
    r->ui_path = strdup(config->ui_socket);
    if (config->workers != 0U) {
        r->worker_path = strdup(config->worker_socket); r->executable = strdup(config->executable);
    }
    if (r->ui_path == NULL || (config->workers != 0U && (r->worker_path == NULL || r->executable == NULL))) {
        rc = COG_ERROR_NOMEM; goto fail;
    }
    r->fixed.ui_socket = r->ui_path; r->fixed.worker_socket = r->worker_path; r->fixed.executable = r->executable;
    rc = host_allocations(r, options); if (rc != COG_OK) goto fail;
    rc = host_incarnation(&r->runtime_incarnation); if (rc != COG_OK) goto fail;
    rc = host_clock(&r->now_ms); if (rc != COG_OK) goto fail;
    rc = host_directory(options->store_dir, true, &r->canonical_store, &r->store_directory_fd);
    if (rc != COG_OK) goto fail;
    if (options->workspace_root != NULL) {
        rc = host_directory(options->workspace_root, false, &r->canonical_workspace, &r->workspace_directory_fd);
        if (rc != COG_OK) goto fail;
    }
    cli_executor_slot_t *root = &r->executors[0];
    if (options->backend == COG_CLI_BACKEND_REAL) {
        if (config->net_config == NULL || config->net_config->observer != NULL ||
            config->net_config->observer_user != NULL || !config->net_config->host_runtime_initialized) {
            rc = COG_ERROR_INVALID_ARG; goto fail;
        }
        r->net_template = *config->net_config;
#define HOST_DEFAULT(member, value) if (r->net_template.member == 0U) r->net_template.member = (value)
        HOST_DEFAULT(max_connections, 4U); HOST_DEFAULT(connect_timeout_ms, 10000U);
        HOST_DEFAULT(stall_timeout_ms, 30000U); HOST_DEFAULT(max_request_bytes, 1048576U);
        HOST_DEFAULT(max_header_bytes, 65536U); HOST_DEFAULT(chunk_bytes, 16384U);
        HOST_DEFAULT(chunks_per_request, 8U); HOST_DEFAULT(service_quantum, 16U);
#undef HOST_DEFAULT
        if (config->net_config->ca_bundle != NULL) {
            r->ca_bundle = strdup(config->net_config->ca_bundle);
            if (r->ca_bundle == NULL) { rc = COG_ERROR_NOMEM; goto fail; }
        }
        /* Template is an external ceiling, not a request to reserve that ceiling
         * for every small executor. Derive its actual full request bound. */
        size_t request_bytes;
        if (!host_product(options->max_context_bytes, 4U, &request_bytes) ||
            !cli_wire_add(&request_bytes, 65536U)) { rc = COG_ERROR_LIMIT; goto fail; }
        if (request_bytes < r->net_template.max_request_bytes)
            r->net_template.max_request_bytes = request_bytes;
        r->net_template.ca_bundle = r->ca_bundle; r->fixed.net_config = &r->net_template;
        rc = host_transport(r, root); if (rc != COG_OK) goto fail;
    } else r->fixed.net_config = NULL;
    rc = cog_cli_configuration_finish_host(resolved, root->net, r->canonical_workspace, r->canonical_store);
    if (rc != COG_OK) goto fail;
    rc = cog_cli_configuration_clone_new(&r->current_mirror, resolved, NULL);
    if (rc != COG_OK) goto fail;
    options = cog_cli_configuration_options(r->current_mirror);
    cog_cli_execution_admission_t admission = {.kind = COG_CLI_EXECUTION_ROOT,
        .configuration = resolved, .configuration_generation = 1U, .observer = root->observer};
    resolved = NULL;
    rc = cog_cli_execution_new(&root->app, &admission); if (rc != COG_OK) goto fail;
    root->phase = CLI_SLOT_PREPARED; root->kind = COG_CLI_EXECUTION_ROOT;
    root->runtime = r->runtime_incarnation; root->configuration_generation = 1U;
    const char *sid = cog_session_id(root->app->session);
    if (sid == NULL || strlen(sid) >= sizeof(r->primary_session)) { rc = COG_ERROR_LIMIT; goto fail; }
    strcpy(r->primary_session, sid); strcpy(root->session, sid);
    cog_store_config_t store = {.base_dir = r->canonical_store, .require_owner = true,
        .max_record_bytes = options->max_context_bytes * 32U + 65536U};
    rc = cog_store_new(&r->catalog, &store); if (rc != COG_OK) goto fail;
    cog_store_checkpoint_t checkpoint;
    rc = cog_store_owner_read(r->catalog, sid, &r->primary_owner, &checkpoint);
    if (rc != COG_OK) goto fail;
    root->owner = r->primary_owner;
    cog_cli_delegation_context_t *context;
    rc = cog_cli_delegation_context_new(&context, root->app, r, NULL, 0U, 0U,
        bounds.mailbox_entries, bounds.mailbox_bytes); if (rc != COG_OK) goto fail;
    cog_session_config_t session = {.max_nodes = 1U, .max_payload_bytes = 1024U,
        .max_tool_calls_per_message = 1U};
    rc = cog_session_new(&r->control_session, &session); if (rc != COG_OK) goto fail;
    size_t frame = bounds.projection_bytes + 512U;
    size_t agent_bytes = bounds.task_bytes + 128U;
    if (agent_bytes < bounds.result_bytes) agent_bytes = bounds.result_bytes;
    if (agent_bytes < bounds.status_bytes) agent_bytes = bounds.status_bytes;
    cog_ipc_endpoint_config_t ipc = {.max_frame_bytes = frame, .receive_frames = 2U,
        .send_frames = 2U, .service_quantum = 4U, .max_routes = bounds.executors, .wipe_buffers = true};
    rc = cog_ipc_endpoint_new_ex(&r->ui_alias, &ipc); if (rc != COG_OK) goto fail;
    rc = cog_ipc_endpoint_listen(r->ui_alias, r->ui_path); if (rc != COG_OK) goto fail;
    cog_core_config_t core = {.session = r->control_session, .dispatch_table = host_dispatch,
        .dispatch_table_size = sizeof(host_dispatch)/sizeof(host_dispatch[0]), .user = r,
        .ipc_endpoint = r->ui_alias, .require_peer_credentials = true,
        .ipc_peer_capacity = bounds.clients, .ipc_max_frame_bytes = frame,
        .ipc_payload_bytes = frame * 2U, .ipc_service_quantum = 4U, .step_event_limit = 1U,
        .event_queue_capacity = 2U * bounds.children + bounds.clients + 8U,
        .max_agents = bounds.children, .max_agent_message_bytes = agent_bytes,
        .pending_session_capacity = bounds.children + 1U, .pending_messages_per_session = 1U,
        .pending_message_bytes = 16U};
    rc = cog_core_new(&r->control_core, &core); if (rc != COG_OK) goto fail;
    r->ui_transferred = true;
    cog_cli_projection_config_t projection = {.session_id = sid, .generation = r->runtime_incarnation,
        .records = config->replay_records == 0U ? 64U : config->replay_records,
        .replay_bytes = config->replay_bytes == 0U ? 16U*1024U*1024U : config->replay_bytes,
        .projection_bytes = bounds.projection_bytes, .max_display_bytes = bounds.display_bytes,
        .max_agents = bounds.children, .max_status_bytes = bounds.status_bytes, .max_result_bytes = bounds.result_bytes};
    rc = cog_cli_projection_new(&r->projection, &projection); if (rc != COG_OK) goto fail;
    if (bounds.workers != 0U) {
        cog_orchestrator_config_t orch = {.socket_path = r->worker_path, .max_aux_cores = bounds.workers,
            .default_core_capacity = bounds.children, .service_quantum = 4U, .max_sessions = bounds.executors,
            .store = r->catalog, .message_events = bounds.executors, .message_bytes = frame * 2U,
            .max_frame_bytes = frame, .shutdown_timeout_ms = r->fixed.shutdown_ms};
        rc = cog_orchestrator_new(&r->orchestrator, &orch); if (rc != COG_OK) goto fail;
        rc = cog_cli_handoff_new(&r->handoff, r, r->orchestrator, r->catalog, r->fixed.shutdown_ms);
        if (rc != COG_OK) goto fail;
    }
    *out = r;
    return COG_OK;
fail:
    cog_cli_configuration_free(resolved);
    return cog_cli_host_runtime_close(&r, rc);
}

/** Publish drive ownership without dispatch or waiting for any worker. */
cog_result_t cog_cli_host_runtime_begin(cog_cli_host_runtime_t *r)
{
    if (r == NULL) return COG_ERROR_NULL_PTR;
    if (r->begun || r->in_step) return COG_ERROR_INVALID_STATE;
    if (r->shutdown) { r->done = true; return r->primary; }
    r->begun = true;
    cog_result_t rc = cog_core_drive_begin(r->control_core);
    if (rc != COG_OK) {
        r->control_drive_ended = true; r->primary = rc;
        r->shutdown = true; r->admission_frozen = true;
        return rc;
    }
    /* Every published native reservation remains owned on prefix failure. The
     * later step/drain path services it; no synchronous rollback or waitpid. */
    for (size_t i = 0U; i < r->bounds.workers; ++i) {
        cli_worker_record_t *worker = &r->workers[i];
        rc = cog_orchestrator_spawn(r->orchestrator, r->executable, &worker->id);
        if (rc != COG_OK) {
            r->primary = rc;
            (void)cog_cli_host_runtime_request_shutdown(r);
            return rc;
        }
        worker->used = true;
    }
    return COG_OK;
}

/** Freeze new admission and request bounded actual cleanup, never wait here. */
cog_result_t cog_cli_host_runtime_request_shutdown(cog_cli_host_runtime_t *r)
{
    if (r == NULL) return COG_ERROR_NULL_PTR;
    if (r->shutdown) return COG_OK;
    r->shutdown = true; r->admission_frozen = true;
    if (!r->begun) { r->done = true; return COG_OK; }
    uint64_t now;
    cog_result_t rc = host_clock(&now);
    if (rc == COG_OK) r->now_ms = now;
    else r->primary = host_first(r->primary, rc);
    r->shutdown_deadline_ms = r->now_ms > UINT64_MAX - r->fixed.shutdown_ms ?
        UINT64_MAX : r->now_ms + r->fixed.shutdown_ms;
    for (size_t i = 0U; i < r->group_capacity; ++i) {
        cli_group_record_t *group = &r->groups[i];
        if (group->phase != CLI_GROUP_FREE) (void)cli_runtime_release(r, group->reservation);
    }
    for (size_t i = 0U; i < r->bounds.executors; ++i) {
        cli_executor_slot_t *slot = &r->executors[i];
        if (slot->phase == CLI_SLOT_FREE) continue;
        slot->cancel_latched = true;
        if (slot->drain_deadline_ms == 0U || slot->drain_deadline_ms > r->shutdown_deadline_ms)
            slot->drain_deadline_ms = r->shutdown_deadline_ms;
        if (slot->app != NULL) cog_cli_app_stop(slot->app);
    }
    if (r->handoff != NULL) rc = host_first(rc, cog_cli_handoff_abort(r->handoff));
    if (r->orchestrator != NULL)
        rc = host_first(rc, cog_orchestrator_request_shutdown(r->orchestrator, r->now_ms, r->fixed.shutdown_ms));
    if (!r->control_drive_ended) cog_core_stop(r->control_core);
    r->primary = host_first(r->primary, rc);
    return rc;
}

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
    const cog_cli_configuration_t *mirror = r->future_mirror != NULL &&
        q->pinned_configuration == r->published_configuration ? r->future_mirror : r->current_mirror;
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
    bool new_lease = !client->used;
    uint64_t lease = client->used ? client->logical_lease : r->next_client_lease;
    uint64_t wire;
    rc = host_ui_result(r, COG_CLI_LANE_COMMAND, message->wire_id, lease, COG_OK, &h->reply, &wire);
    if (rc != COG_OK) return rc;
    if (!client->used) {
        /* A physical cell is reusable only after its old receipt lease expired.
         * Its constructor arenas survive; stream identity and receipt state do
         * not cross into the newly assigned logical lease. Reconnects bypass
         * this branch and retain every original command/HELLO receipt. */
        size_t base = (r->bounds.executors + (size_t)(client - r->clients)) * (2U * CLI_HOST_LANES);
        for (size_t i = 0U; i < 2U * CLI_HOST_LANES; ++i) {
            cli_control_lane_t previous = r->lanes[base + i];
            r->lanes[base + i] = (cli_control_lane_t){.pending = previous.pending,
                .provisional_reply = previous.provisional_reply, .final_original = previous.final_original,
                .final_reply = previous.final_reply, .next_wire = 1U};
            r->lanes[base + i].pending.length = 0U;
            r->lanes[base + i].provisional_reply.length = 0U;
            r->lanes[base + i].final_original.length = 0U;
            r->lanes[base + i].final_reply.length = 0U;
        }
        client->send.length = 0U; client->resync.length = 0U; client->gap_last = 0U;
        client->controller_until_ms = 0U;
        client->used = true; client->logical_lease = r->next_client_lease++;
        client->controller = role == 1U && !controller;
        h->initial_nonce = message->sender_incarnation;
    }
    h->retired_through = new_lease ? 0U :
        (h->ack_wire > h->request_wire ? h->ack_wire : h->request_wire);
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
            cog_result_t replay = host_client_queue(client, &h->ack_reply);
            h->pending = replay == COG_ERROR_AGAIN;
            return replay;
        }
        cog_result_t rc = host_ui_result(r, m->lane, m->wire_id, 0U, COG_OK,
            &h->ack_reply, &h->ack_result_wire);
        if (rc != COG_OK) return rc;
        h->ack_wire = m->wire_id; h->acknowledged = true;
        rc = host_client_queue(client, &h->ack_reply);
        h->pending = rc == COG_ERROR_AGAIN;
        return rc;
    }
    cli_control_lane_t *lane = host_client_lane(r, client, m->lane);
    if (lane->final_valid && m->wire_id == lane->final_answered_wire) {
        if (!host_bytes_equal(&lane->final_original, event->msg_payload, event->msg_payload_len))
            return COG_ERROR_PROTOCOL;
        cog_result_t replay = host_client_queue(client, &lane->final_reply);
        lane->pending_valid = replay == COG_ERROR_AGAIN;
        return replay;
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
        rc = host_client_queue(client, &lane->final_reply);
        lane->pending_valid = rc == COG_ERROR_AGAIN;
        return rc;
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
            if (record->original.length < 6U) return COG_ERROR_PROTOCOL;
            if (record->original.data[5] != (unsigned char)m->lane) continue;
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
    case COG_CLI_CONTROL_REPLAY: {
        if (m.lane != COG_CLI_LANE_REPLAY || m.reply_to != 0U || client->awaiting_resync)
            return COG_ERROR_INVALID_STATE;
        cog_ipc_replay_record_t cursor;
        rc = cog_ipc_replay_decode(m.payload, m.payload_len, &cursor,
            r->ingress.data, r->ingress.capacity, &required);
        if (rc != COG_OK) return rc;
        if (cursor.kind != COG_IPC_REPLAY_CURSOR || cursor.generation != r->runtime_incarnation ||
            strcmp(cursor.session_id, r->primary_session) != 0 || cursor.sequence > client->queued_sequence)
            return COG_ERROR_PROTOCOL;
        if (cursor.sequence >= client->rendered_sequence) {
            client->rendered_generation = cursor.generation; client->rendered_sequence = cursor.sequence;
        }
        return COG_OK;
    }
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
static size_t host_frame_capacity(const cli_host_reservations_t *bounds)
{
    size_t capacity = bounds->projection_bytes + 512U;
    if (capacity < 90368U) capacity = 90368U;
    for (size_t i = 0U; i < CLI_HOST_LANES; ++i)
        if (capacity < bounds->lane_frame_bytes[i]) capacity = bounds->lane_frame_bytes[i];
    return capacity;
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
    size_t frame = host_frame_capacity(&r->bounds);
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
        if (lane == COG_CLI_LANE_DELEGATE - 1U && key >= 2U && key < r->bounds.executors)
            cap = r->bounds.task_bytes + 1024U;
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
static const cog_state_t host_transitions[] = {0U, COG_TRANSITION_SELF, COG_TRANSITION_END};
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
    size_t frame = host_frame_capacity(&bounds);
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
    /* Keep protected control ingress alive during bounded shutdown receipt drain. */
    r->primary = host_first(r->primary, rc);
    return rc;
}

static bool host_policy_equal(const cog_cli_policy_view_t *a, const cog_cli_policy_view_t *b)
{
    return a->assistant_node == b->assistant_node && a->call_ordinal == b->call_ordinal &&
        a->configuration_generation == b->configuration_generation && a->deadline_ms == b->deadline_ms &&
        strcmp(a->session_id, b->session_id) == 0 && strcmp(a->tool_name, b->tool_name) == 0;
}
static bool host_agent_view_equal(const cog_cli_agent_view_t *a, const cog_cli_agent_view_t *b)
{
    return a->agent_id == b->agent_id && a->execution_epoch == b->execution_epoch &&
        a->group_id == b->group_id && a->configuration_generation == b->configuration_generation &&
        a->status == b->status && a->outcome == b->outcome &&
        strcmp(a->session_id, b->session_id) == 0 && strcmp(a->status_text, b->status_text) == 0 &&
        strcmp(a->result_text, b->result_text) == 0;
}
static size_t host_agent_rows(cog_cli_host_runtime_t *r, cog_cli_agent_view_t *agents)
{
    size_t count = 0U;
    for (size_t i = 2U; i < r->bounds.executors; ++i) {
        cli_executor_slot_t *slot = &r->executors[i];
        if (slot->phase == CLI_SLOT_FREE || slot->agent == 0U || slot->session[0] == '\0') continue;
        cog_cli_agent_view_t row = {.agent_id = slot->agent, .execution_epoch = slot->attempt,
            .group_id = slot->group, .configuration_generation = slot->configuration_generation,
            .session_id = slot->session, .status = slot->accepted_status, .outcome = slot->accepted_outcome,
            .status_text = (const char *)slot->status.data, .result_text = (const char *)slot->result.data};
        size_t at = count;
        while (at != 0U && agents[at - 1U].agent_id > row.agent_id) { agents[at] = agents[at - 1U]; --at; }
        agents[at] = row; ++count;
    }
    return count;
}
static cog_result_t host_project_local(cog_cli_host_runtime_t *r, cli_executor_slot_t *root)
{
    uint64_t serial = 0U;
    cog_cli_display_snapshot_t display;
    cog_result_t rc = cog_cli_execution_copy_view(root->app, &serial, &display);
    bool pending = rc == COG_OK;
    if (!pending && rc != COG_ERROR_AGAIN) return rc;
    cog_cli_projection_view_t previous;
    cog_result_t old = cog_cli_projection_snapshot(r->projection, &previous);
    if (old != COG_OK && old != COG_ERROR_AGAIN) return old;
    if (!pending) {
        if (old != COG_OK) return COG_ERROR_AGAIN;
        display = previous.display;
    }
    uint64_t active, published;
    uint32_t attempts;
    rc = cog_cli_execution_copy_configuration(root->app, &active, &published, &attempts);
    if (rc != COG_OK) return rc;
    if (attempts > CLI_RELOAD_ATTEMPTS) return COG_ERROR_PROTOCOL;
    r->active_configuration = active; r->published_configuration = published;
    if (attempts > r->reload_attempts_used) r->reload_attempts_used = attempts;
    cog_cli_agent_view_t agents[CLI_HOST_MAX_CHILDREN];
    size_t count = host_agent_rows(r, agents);
    cog_cli_policy_view_t policy;
    uint64_t prompt_serial;
    rc = cog_cli_execution_copy_policy(root->app, &prompt_serial, &policy);
    size_t policies = rc == COG_OK ? 1U : 0U;
    if (rc != COG_OK && rc != COG_ERROR_AGAIN) return rc;
    if (policies != 0U) {
        if (old == COG_OK && previous.policy_count == 1U && host_policy_equal(&policy, previous.policies))
            policy.nonce = previous.policies[0].nonce;
        else {
            if (r->next_nonce == 0U || r->next_nonce > UINT64_MAX - 16U) return COG_ERROR_LIMIT;
            policy.nonce = r->next_nonce++;
        }
    }
    bool changed = pending || old != COG_OK;
    if (!changed) {
        changed = count != previous.agent_count || policies != previous.policy_count ||
            active != previous.active_configuration || published != previous.published_configuration;
        for (size_t i = 0U; !changed && i < count; ++i)
            changed = !host_agent_view_equal(&agents[i], &previous.agents[i]);
        if (!changed && policies != 0U) changed = !host_policy_equal(&policy, previous.policies);
    }
    if (!changed) return COG_ERROR_AGAIN;
    cog_cli_projection_view_t next = {.display = display, .active_configuration = active,
        .published_configuration = published, .agents = agents, .agent_count = count,
        .policies = policies == 0U ? NULL : &policy, .policy_count = policies};
    uint64_t sequence;
    rc = cog_cli_projection_commit(r->projection, &next, &sequence);
    if (rc != COG_OK) return rc;
    if (pending) return cog_cli_execution_commit_view(root->app, serial, r->runtime_incarnation, sequence);
    return COG_OK;
}

static cog_result_t host_project_remote_rows(cog_cli_host_runtime_t *r)
{
    cog_cli_projection_view_t view;
    cog_result_t rc = cog_cli_projection_snapshot(r->projection, &view);
    if (rc != COG_OK) return rc;
    cog_cli_agent_view_t agents[CLI_HOST_MAX_CHILDREN];
    size_t count = host_agent_rows(r, agents);
    bool changed = count != view.agent_count;
    for (size_t i = 0U; !changed && i < count; ++i)
        changed = !host_agent_view_equal(&agents[i], &view.agents[i]);
    if (!changed) return COG_ERROR_AGAIN;
    view.agents = agents; view.agent_count = count;
    uint64_t sequence;
    return cog_cli_projection_commit(r->projection, &view, &sequence);
}

static cli_group_record_t *host_slot_group(cog_cli_host_runtime_t *r, const cli_executor_slot_t *slot)
{
    for (size_t i = 0U; i < r->group_capacity; ++i)
        if (r->groups[i].phase != CLI_GROUP_FREE && r->groups[i].id == slot->group) return &r->groups[i];
    return NULL;
}
static cog_result_t host_local_admit(cog_cli_host_runtime_t *r, cli_executor_slot_t *slot)
{
    cli_group_record_t *group = host_slot_group(r, slot);
    if (group == NULL) return COG_ERROR_INVALID_STATE;
    if (!group->gate_open || slot->cancel_latched || r->admission_frozen) return COG_ERROR_AGAIN;
    if (slot->app != NULL || slot->started || slot->owner_peer != 0U) return COG_ERROR_INVALID_STATE;
    cog_result_t rc = host_transport(r, slot);
    if (rc != COG_OK) return rc;
    const cog_cli_configuration_t *mirror = r->future_mirror != NULL &&
        slot->configuration_generation == r->published_configuration ? r->future_mirror : r->current_mirror;
    rc = cog_cli_configuration_child_new(&slot->pending_profile, mirror,
        &r->fixed.child_profile, slot->configuration_generation, r->now_ms,
        slot->deadline_ms, slot->net, &slot->deadline_ms);
    if (rc != COG_OK) return rc;
    rc = cog_agent_session_borrow(r->control_core, slot->agent, &slot->borrowed_session);
    if (rc != COG_OK) return rc;
    slot->borrow_live = true;
    slot->mailbox.reply.capacity = cog_cli_configuration_options(slot->pending_profile)->max_tool_output_bytes;
    char provenance[4096];
    size_t needed;
    cog_cli_child_context_t child = {.parent_session_id = slot->parent_session,
        .parent_call_id = slot->parent_call, .parent_node = slot->parent_node,
        .parent_call_ordinal = slot->parent_ordinal, .runtime_incarnation = r->runtime_incarnation,
        .group_id = slot->group, .agent_id = slot->agent, .execution_epoch = slot->attempt,
        .configuration_generation = slot->configuration_generation, .members = slot->members,
        .member_count = slot->member_count};
    rc = cog_cli_child_context_encode(&child, provenance, sizeof(provenance), &needed);
    if (rc != COG_OK) return rc;
    cog_cli_execution_admission_t admission = {.kind = COG_CLI_EXECUTION_LOCAL_CHILD,
        .configuration = slot->pending_profile, .local_session = slot->borrowed_session,
        .parent_session_id = slot->parent_session, .task = (const char *)slot->task.data,
        .provenance = provenance, .provenance_len = needed - 1U,
        .configuration_generation = slot->configuration_generation,
        .absolute_deadline_ms = slot->deadline_ms, .observer = slot->observer};
    slot->pending_profile = NULL;
    rc = cog_cli_execution_new(&slot->app, &admission); if (rc != COG_OK) return rc;
    cog_cli_delegation_context_t *context;
    rc = cog_cli_delegation_context_new(&context, slot->app, r, NULL, slot->agent, slot->group,
        r->bounds.mailbox_entries, r->bounds.mailbox_bytes); if (rc != COG_OK) return rc;
    rc = cog_cli_execution_begin(slot->app); if (rc != COG_OK) return rc;
    slot->started = true; slot->kind = COG_CLI_EXECUTION_LOCAL_CHILD; slot->phase = CLI_SLOT_STARTED;
    slot->factual_status = COG_AGENT_STATUS_RUNNING; slot->factual_outcome = COG_OK;
    return COG_OK;
}
static cog_result_t host_local_update(cog_cli_host_runtime_t *r, cli_executor_slot_t *slot,
    cog_agent_status_t status, cog_result_t outcome, const char *text, size_t length)
{
    cog_agent_update_t update = {.owner_peer = slot->owner_peer, .execution_epoch = slot->attempt,
        .sequence = slot->accepted_sequence + 1U, .status = status, .outcome = outcome,
        .data = text, .data_len = length};
    cog_result_t rc = cog_agent_apply_update(r->control_core, slot->agent, &update);
    if (rc != COG_OK) return rc;
    cli_host_bytes_t *destination = status == COG_AGENT_STATUS_RUNNING ? &slot->status : &slot->result;
    size_t required;
    rc = cog_agent_copy_update(r->control_core, slot->agent, &update,
        (char *)destination->data, destination->capacity, &required);
    if (rc != COG_OK) return rc;
    destination->length = update.data_len;
    slot->accepted_status = update.status; slot->accepted_outcome = update.outcome;
    slot->accepted_sequence = update.sequence;
    rc = cog_agent_ack_update(r->control_core, slot->agent, slot->attempt, update.sequence);
    if (rc == COG_OK && status != COG_AGENT_STATUS_RUNNING) slot->terminal_acked = true;
    return rc;
}
static cog_result_t host_local_terminal(cog_cli_host_runtime_t *r, cli_executor_slot_t *slot)
{
    const char *text = "child did not execute";
    size_t length = strlen(text);
    cog_result_t outcome = slot->primary;
    cog_agent_status_t status = outcome == COG_ERROR_TIMEOUT ? COG_AGENT_STATUS_TIMEOUT : COG_AGENT_STATUS_FAILED;
    if (slot->app != NULL) {
        cog_cli_display_snapshot_t view;
        cog_result_t rc = cog_cli_app_display_snapshot(slot->app, &view);
        if (rc != COG_OK) return rc;
        const cog_cli_display_item_t *last = NULL;
        for (size_t i = 0U; i < view.item_count; ++i)
            if (view.items[i].role == COG_MESSAGE_ROLE_ASSISTANT) last = &view.items[i];
        if (last != NULL) {
            text = last->text; length = last->text_len;
            if (last->status == COG_MESSAGE_COMPLETE && outcome == COG_OK) status = COG_AGENT_STATUS_COMPLETED;
            else if (last->status == COG_MESSAGE_CANCELLED) {
                status = COG_AGENT_STATUS_CANCELLED; outcome = COG_ERROR_ABORTED;
            } else if (outcome == COG_OK) outcome = COG_ERROR_INDETERMINATE;
        } else if (outcome == COG_OK) outcome = COG_ERROR_INDETERMINATE;
    } else if (outcome == COG_OK) outcome = COG_ERROR_ABORTED;
    slot->factual_status = status; slot->factual_outcome = outcome;
    if (slot->cancel_latched && status == COG_AGENT_STATUS_COMPLETED) {
        status = COG_AGENT_STATUS_CANCELLED; outcome = COG_ERROR_ABORTED;
    }
    if (slot->cancel_latched && !slot->started) { status = COG_AGENT_STATUS_CANCELLED; outcome = COG_ERROR_ABORTED; }
    cli_group_record_t *group = host_slot_group(r, slot);
    if (group == NULL) return COG_ERROR_INVALID_STATE;
    if (length > group->result_payload_cap) {
        status = COG_AGENT_STATUS_FAILED; outcome = COG_ERROR_LIMIT;
        text = "result exceeds negotiated capture; inspect durable child session"; length = strlen(text);
    }
    if (outcome == COG_ERROR_INDETERMINATE) slot->effects_uncertain = true;
    cog_result_t rc = host_local_update(r, slot, status, outcome, text, length);
    if (rc != COG_OK) return rc;
    slot->terminal_copied = true;
    for (size_t i = 0U; i < group->submitted; ++i) if (group->members[i].agent == slot->agent) {
        group->members[i].terminal = true; group->members[i].status = status;
        group->members[i].outcome = outcome; group->members[i].effects_uncertain = slot->effects_uncertain;
    }
    return COG_OK;
}
static cog_result_t host_local_dispose(cog_cli_host_runtime_t *r, cli_executor_slot_t *slot)
{
    cog_result_t rc = cog_cli_execution_close(&slot->app, COG_OK);
    if (slot->app != NULL) return rc;
    cog_cli_configuration_free(slot->pending_profile); slot->pending_profile = NULL;
    slot->profile.source = NULL;
    if (slot->net != NULL) {
        if (slot->observer != NULL) rc = host_first(rc, cog_cli_observer_clear(slot->observer, slot->net));
        cog_net_free(slot->net); slot->net = NULL;
    }
    cog_cli_observer_free(slot->observer); slot->observer = NULL;
    if (slot->borrow_live) {
        cog_result_t returned = cog_agent_session_return(r->control_core, slot->agent, slot->borrowed_session);
        if (returned != COG_OK) return host_first(rc, returned);
        slot->borrow_live = false; slot->borrowed_session = NULL;
    }
    slot->phase = CLI_SLOT_RETAINED;
    return rc;
}

static bool host_worker_equal(cog_worker_id_t a, cog_worker_id_t b)
{ return a.incarnation == b.incarnation && a.number == b.number; }
static void host_secret_wipe(cli_secret_exchange_t *s)
{
    volatile unsigned char *buffers[] = {s->body, s->frame, s->scratch};
    size_t capacities[] = {s->body_capacity, s->frame_capacity, s->scratch_capacity};
    for (size_t i = 0U; i < 3U; ++i)
        for (size_t j = 0U; j < capacities[i]; ++j) buffers[i][j] = 0U;
    s->body_length = 0U; s->frame_length = 0U;
}
/* Only accepted copies start a retry clock; transport pressure retains eligibility. */
static cog_result_t host_copied_send(cog_cli_host_runtime_t *r, cog_worker_id_t worker,
    const cli_host_bytes_t *frame, uint64_t wire, uint64_t *copied, uint64_t *next,
    bool one_way)
{
    if (wire == 0U || frame->length == 0U) return COG_ERROR_INVALID_STATE;
    if (*copied == wire && (one_way || r->now_ms < *next)) return COG_ERROR_AGAIN;
    if (!one_way && r->now_ms > UINT64_MAX - CLI_CONTROL_RETRY_MS) return COG_ERROR_LIMIT;
    /* Missed heartbeat is not a lost native reservation. Service owns its
     * escalation; retain the exact exchange and its original deadline. */
    if (cog_orchestrator_health(r->orchestrator, worker) == COG_HEALTH_DEGRADED)
        return COG_ERROR_AGAIN;
    cog_ipc_message_t bytes = {.data = frame->data, .len = frame->length, .capacity = frame->capacity};
    cog_result_t rc = cog_orchestrator_send(r->orchestrator, worker, &bytes);
    if (rc == COG_OK) {
        *copied = wire;
        *next = one_way ? 0U : r->now_ms + CLI_CONTROL_RETRY_MS;
    }
    return rc;
}
static cog_result_t host_lane_send(cog_cli_host_runtime_t *r, cog_worker_id_t worker,
    cli_control_lane_t *lane, const cli_host_bytes_t *frame, bool one_way)
{
    return host_copied_send(r, worker, frame, lane->pending_wire,
        &lane->copied_wire, &lane->next_send_ms, one_way);
}
static cog_result_t host_reply_send(cog_cli_host_runtime_t *r, cog_worker_id_t worker,
    cli_control_lane_t *lane, const cli_host_bytes_t *frame)
{
    uint64_t wire = frame == &lane->final_reply ? lane->final_reply_wire : lane->provisional_wire;
    /* An already copied valid successor retires older response delivery. */
    if (wire < lane->copied_wire || wire < lane->pending_reply_wire) return COG_ERROR_AGAIN;
    lane->pending_reply_wire = wire;
    cog_result_t rc = host_copied_send(r, worker, frame, wire,
        &lane->copied_wire, &lane->next_send_ms, false);
    if (rc == COG_OK) lane->pending_reply_wire = 0U;
    return rc;
}
static cog_result_t host_profile_send(cog_cli_host_runtime_t *r, cli_worker_record_t *worker,
    cli_executor_slot_t *slot, cli_secret_exchange_t *secret, uint8_t purpose, bool *accepted)
{
    if (accepted != NULL) *accepted = false;
    if (secret->acknowledged) return COG_OK;
    if (!secret->pending) {
        if (r->next_nonce == 0U || r->next_nonce > UINT64_MAX - 16U) return COG_ERROR_LIMIT;
        if (secret->source == NULL) secret->source = r->current_mirror;
        secret->purpose = purpose;
        if (purpose != 4U)
            secret->generation = slot == NULL ? r->active_configuration : slot->configuration_generation;
        secret->reload_attempts_used = purpose == 1U || purpose == 2U ? 0U : r->reload_attempts_used;
        if (secret->probe.session_id[0] == '\0') {
            memcpy(secret->probe.session_id, r->primary_session, sizeof(secret->probe.session_id));
            cog_result_t rc = cog_store_owner_read(r->catalog, r->primary_session,
                &secret->probe.owner, &secret->probe.checkpoint);
            if (rc != COG_OK) return rc;
        }
        cog_result_t rc = cog_cli_configuration_bootstrap_encode(secret->source, &secret->probe,
            &r->net_template, secret->body, secret->body_capacity, &secret->body_length);
        if (rc != COG_OK) return rc;
        if (secret->body_length > secret->scratch_capacity - 5U) return COG_ERROR_LIMIT;
        cli_wire_writer_t payload = {secret->scratch, 0U, true};
        cli_wire_put(&payload, purpose, 1U); cli_wire_put(&payload, secret->reload_attempts_used, 4U);
        cli_wire_bytes(&payload, secret->body, secret->body_length);
        cog_cli_control_message_t m = {.kind = COG_CLI_CONTROL_BOOTSTRAP, .lane = COG_CLI_LANE_PROFILE,
            .runtime_incarnation = r->runtime_incarnation, .sender_incarnation = r->runtime_incarnation,
            .wire_id = r->next_nonce, .worker = worker->id, .owner_peer = worker->native.peer_id,
            .agent_id = slot == NULL ? 0U : slot->agent, .execution_epoch = slot == NULL ? 0U : slot->attempt,
            .session_id = slot == NULL ? r->primary_session : slot->session,
            .payload = secret->scratch, .payload_len = payload.used};
        rc = cog_cli_control_encode(&m, secret->frame, secret->frame_capacity, &secret->frame_length);
        if (rc != COG_OK) return rc;
        secret->wire = r->next_nonce++; secret->pending = true;
    }
    cli_host_bytes_t bytes = {.data = secret->frame, .length = secret->frame_length, .capacity = secret->frame_capacity};
    cog_result_t rc = host_copied_send(r, worker->id, &bytes, secret->wire,
        &secret->copied_wire, &secret->next_send_ms, false);
    if (rc == COG_OK && accepted != NULL) *accepted = true;
    if (rc == COG_OK && slot != NULL) {
        slot->first_admission_sent = true;
        if (slot->agent > worker->first_admission_high_watermark) worker->first_admission_high_watermark = slot->agent;
    }
    return rc == COG_OK ? COG_ERROR_AGAIN : rc;
}
static cog_result_t host_bootstrap_ack(cog_cli_host_runtime_t *r, cli_worker_record_t *worker,
    const cog_cli_control_message_t *m)
{
    if (m->lane != COG_CLI_LANE_PROFILE || m->payload_len != 13U) return COG_ERROR_PROTOCOL;
    cli_secret_exchange_t *secret = &worker->startup_profile;
    cli_executor_slot_t *slot = NULL;
    if (m->agent_id != 0U) {
        slot = host_agent(r, m->agent_id);
        if (slot == NULL || slot->attempt != m->execution_epoch || slot->owner_peer != m->owner_peer ||
            !host_worker_equal(slot->worker, worker->id) || strcmp(slot->session, m->session_id) != 0)
            return COG_ERROR_INVALID_STATE;
        secret = &slot->profile;
    } else if (m->reply_to == r->executors[r->primary_slot].profile.wire &&
        r->executors[r->primary_slot].profile.purpose == 4U) {
        slot = &r->executors[r->primary_slot]; secret = &slot->profile;
        if (!host_worker_equal(slot->worker, worker->id) || slot->owner_peer != m->owner_peer ||
            strcmp(slot->session, m->session_id) != 0) return COG_ERROR_INVALID_STATE;
    } else if (r->handoff != NULL && r->handoff->phase != CLI_HANDOFF_IDLE &&
        m->reply_to == r->executors[r->handoff->target_slot].profile.wire) {
        slot = &r->executors[r->handoff->target_slot]; secret = &slot->profile;
    }
    if (m->reply_to != secret->wire || (!secret->pending && !secret->acknowledged))
        return COG_ERROR_INVALID_STATE;
    cli_wire_reader_t reader = {m->payload, m->payload_len, 0U, true};
    cog_result_t result = cli_wire_signed(cli_wire_get(&reader, 4U));
    uint64_t generation = cli_wire_get(&reader, 8U), state = cli_wire_get(&reader, 1U);
    if (!cli_wire_result(result) || generation != secret->generation ||
        state != (secret->purpose == 4U && result == COG_OK ? 2U : 1U)) return COG_ERROR_PROTOCOL;
    if (secret->purpose == 4U) {
        cli_forwarded_operation_t *op = &slot->command;
        if (!op->used || !op->forward_committed || op->executor_wire != secret->wire)
            return COG_ERROR_INVALID_STATE;
        if (secret->acknowledged) return op->result == result && secret->replied_wire == m->wire_id ?
            COG_OK : COG_ERROR_PROTOCOL;
        op->result = result; op->final = true;
        secret->acknowledged = true; secret->pending = false; secret->replied_wire = m->wire_id;
        secret->source = NULL; host_secret_wipe(secret);
        if (result == COG_OK) r->published_configuration = generation;
        else { cog_cli_configuration_free(r->future_mirror); r->future_mirror = NULL; }
        return COG_OK;
    }
    if (secret->acknowledged && (result != COG_OK || m->wire_id != secret->replied_wire)) return COG_ERROR_PROTOCOL;
    if (result != COG_OK) return result;
    secret->acknowledged = true; secret->pending = false; secret->replied_wire = m->wire_id;
    host_secret_wipe(secret);
    if (slot == NULL) worker->profile_ready = true;
    return COG_OK;
}

static cog_result_t host_remote_frame(cog_cli_host_runtime_t *r, cli_executor_slot_t *slot,
    cog_cli_control_kind_t kind, cog_cli_control_lane_t lane_id, uint64_t reply_to,
    const void *body, size_t length, cli_host_bytes_t *out, uint64_t *wire)
{
    if (r->next_nonce == 0U || r->next_nonce == UINT64_MAX) return COG_ERROR_LIMIT;
    cog_cli_control_message_t m = {.kind = kind, .lane = lane_id,
        .runtime_incarnation = r->runtime_incarnation, .sender_incarnation = r->runtime_incarnation,
        .wire_id = r->next_nonce, .reply_to = reply_to, .worker = slot->worker,
        .owner_peer = slot->owner_peer, .agent_id = slot->agent, .execution_epoch = slot->attempt,
        .session_id = slot->session, .payload = body, .payload_len = length};
    size_t required;
    cog_result_t rc = cog_cli_control_encode(&m, out->data, out->capacity, &required);
    if (rc != COG_OK) return rc;
    out->length = required; *wire = r->next_nonce++;
    return COG_OK;
}
static cog_result_t host_remote_update(cog_cli_host_runtime_t *r, cli_executor_slot_t *slot,
    const cog_cli_control_message_t *m, const cog_orchestrator_event_t *event)
{
    if (m->lane != COG_CLI_LANE_UPDATE || m->reply_to != 0U) return COG_ERROR_PROTOCOL;
    cli_control_lane_t *lane = &slot->inbound[COG_CLI_LANE_UPDATE - 1U];
    if (lane->final_valid && lane->final_answered_wire == m->wire_id) {
        if (!host_bytes_equal(&lane->final_original, event->data, event->data_len)) return COG_ERROR_PROTOCOL;
        return host_reply_send(r, slot->worker, lane, &lane->final_reply);
    }
    if (m->wire_id <= lane->retired_through ||
        (lane->final_valid && m->wire_id <= lane->final_answered_wire)) return COG_ERROR_INVALID_STATE;
    if (lane->pending_valid && (lane->pending_wire != m->wire_id ||
        !host_bytes_equal(&lane->pending, event->data, event->data_len))) return COG_ERROR_PROTOCOL;
    if (event->data_len > lane->pending.capacity || event->data_len > lane->final_original.capacity ||
        r->next_nonce > UINT64_MAX - 4U) return COG_ERROR_LIMIT;
    cog_agent_id_t id;
    cog_agent_update_t update;
    size_t required;
    cog_result_t rc = cog_agent_update_decode(m->payload, m->payload_len, &id, &update,
        (char *)r->projection_scratch.data, r->projection_scratch.capacity, &required);
    if (rc != COG_OK) return rc;
    if (id != slot->agent || update.owner_peer != slot->owner_peer || update.execution_epoch != slot->attempt)
        return COG_ERROR_PROTOCOL;
    memcpy(lane->pending.data, event->data, event->data_len); lane->pending.length = event->data_len;
    lane->pending_valid = true; lane->pending_wire = m->wire_id;
    cog_cli_control_receipt_disposition_t disposition = COG_CLI_CONTROL_RECEIPT_ACCEPTED;
    bool fixed = slot->accepted_status >= COG_AGENT_STATUS_COMPLETED;
    if (fixed) disposition = COG_CLI_CONTROL_RECEIPT_TERMINAL_FIXED;
    else {
        rc = cog_agent_apply_update(r->control_core, slot->agent, &update);
        if (rc == COG_ERROR_INVALID_STATE && slot->cancel_latched &&
            (update.status == COG_AGENT_STATUS_RUNNING || update.status == COG_AGENT_STATUS_COMPLETED) &&
            update.sequence == slot->accepted_sequence + 1U)
            disposition = COG_CLI_CONTROL_RECEIPT_CANCEL_CONSUMED;
        else if (rc == COG_ERROR_AGAIN) disposition = COG_CLI_CONTROL_RECEIPT_RETRY;
        else if (rc != COG_OK) return rc;
        else {
            cli_host_bytes_t *storage = update.status == COG_AGENT_STATUS_RUNNING ? &slot->status : &slot->result;
            rc = cog_agent_copy_update(r->control_core, slot->agent, &update,
                (char *)storage->data, storage->capacity, &required);
            if (rc != COG_OK) return rc;
            storage->length = update.data_len;
            slot->accepted_sequence = update.sequence; slot->accepted_status = update.status;
            slot->accepted_outcome = update.outcome;
            rc = cog_agent_ack_update(r->control_core, slot->agent, slot->attempt, update.sequence);
            if (rc != COG_OK) return rc;
            if (update.status >= COG_AGENT_STATUS_COMPLETED) {
                slot->terminal_copied = true; slot->terminal_acked = true;
                slot->local_done = true; slot->phase = CLI_SLOT_RETAINED;
                slot->effects_uncertain = update.outcome == COG_ERROR_INDETERMINATE;
                cli_group_record_t *group = host_slot_group(r, slot);
                if (group == NULL) return COG_ERROR_INVALID_STATE;
                for (size_t i = 0U; i < group->submitted; ++i) if (group->members[i].agent == slot->agent) {
                    group->members[i].terminal = true; group->members[i].status = update.status;
                    group->members[i].outcome = update.outcome;
                    group->members[i].effects_uncertain = slot->effects_uncertain;
                }
            }
        }
    }
    cog_cli_control_receipt_t receipt = {.parent_runtime_incarnation = r->runtime_incarnation,
        .agent_id = slot->agent, .execution_epoch = slot->attempt, .owner_peer = slot->owner_peer,
        .wire_id = m->wire_id, .parent_accepted_sequence = slot->accepted_sequence, .disposition = disposition,
        .accepted_terminal_status = slot->accepted_status >= COG_AGENT_STATUS_COMPLETED ? slot->accepted_status : COG_AGENT_STATUS_PENDING,
        .accepted_terminal_outcome = slot->accepted_status >= COG_AGENT_STATUS_COMPLETED ? slot->accepted_outcome : COG_OK};
    unsigned char body[COG_CLI_CONTROL_RECEIPT_PAYLOAD_BYTES];
    rc = cog_cli_control_receipt_encode(&receipt, body, sizeof(body), &required);
    if (rc != COG_OK) return rc;
    if (disposition == COG_CLI_CONTROL_RECEIPT_RETRY) {
        if (lane->provisional_wire == 0U || lane->provisional_prefix != slot->accepted_sequence) {
            rc = host_remote_frame(r, slot, COG_CLI_CONTROL_UPDATE_RECEIPT, COG_CLI_LANE_UPDATE,
                m->wire_id, body, sizeof(body), &lane->provisional_reply, &lane->provisional_wire);
            if (rc != COG_OK) return rc;
            lane->provisional_prefix = slot->accepted_sequence;
        }
        return host_reply_send(r, slot->worker, lane, &lane->provisional_reply);
    }
    rc = host_remote_frame(r, slot, COG_CLI_CONTROL_UPDATE_RECEIPT, COG_CLI_LANE_UPDATE,
        m->wire_id, body, sizeof(body), &lane->final_reply, &lane->final_reply_wire);
    if (rc != COG_OK) return rc;
    if (lane->final_valid) lane->retired_through = lane->final_answered_wire;
    memcpy(lane->final_original.data, event->data, event->data_len); lane->final_original.length = event->data_len;
    lane->final_answered_wire = m->wire_id; lane->final_valid = true;
    lane->pending_valid = false; lane->pending.length = 0U; lane->provisional_wire = 0U;
    slot->receipt = receipt; slot->pending_update_wire = m->wire_id;
    return host_reply_send(r, slot->worker, lane, &lane->final_reply);
}

static void host_json_number(cli_wire_writer_t *writer, uint64_t value, bool quoted)
{
    char text[21]; int n = snprintf(text, sizeof(text), "%llu", (unsigned long long)value);
    if (n <= 0 || (size_t)n >= sizeof(text)) { writer->valid = false; return; }
    if (quoted) host_json_string(writer, (const unsigned char *)text, (size_t)n);
    else cli_wire_bytes(writer, text, (size_t)n);
}
static void host_json_result(cli_wire_writer_t *writer, cog_result_t result)
{
    char text[16]; int n = snprintf(text, sizeof(text), "%d", (int)result);
    if (n <= 0 || (size_t)n >= sizeof(text)) { writer->valid = false; return; }
    cli_wire_bytes(writer, text, (size_t)n);
}
static void host_group_json(cog_cli_host_runtime_t *r, cli_group_record_t *group, cli_wire_writer_t *w)
{
    cli_wire_bytes(w, "{\"group\":", 9U); host_json_number(w, group->id, true);
    cli_wire_bytes(w, ",\"submitted\":", 13U); host_json_number(w, group->submitted, false);
    cli_wire_bytes(w, ",\"members\":[", 12U);
    for (size_t i = 0U; i < group->submitted; ++i) {
        cli_group_member_t *member = &group->members[i];
        if (i != 0U) cli_wire_bytes(w, ",", 1U);
        if (!member->present) {
            cli_wire_bytes(w, "{\"outcome\":", 11U); host_json_result(w, member->outcome);
            cli_wire_bytes(w, "}", 1U); continue;
        }
        cli_executor_slot_t *slot = &r->executors[member->slot];
        cli_wire_bytes(w, "{\"agent_id\":", 12U); host_json_number(w, member->agent, true);
        cli_wire_bytes(w, ",\"attempt\":", 11U); host_json_number(w, member->attempt, true);
        cli_wire_bytes(w, ",\"status\":", 10U); host_json_number(w, member->status, false);
        cli_wire_bytes(w, ",\"outcome\":", 11U); host_json_result(w, member->outcome);
        cli_wire_bytes(w, ",\"session\":", 11U);
        host_json_string(w, (const unsigned char *)slot->session, strlen(slot->session));
        cli_wire_bytes(w, ",\"result\":", 10U); host_json_string(w, slot->result.data, slot->result.length);
        const char *tail = member->effects_uncertain ? ",\"side_effects_possible\":true}" : ",\"side_effects_possible\":false}";
        cli_wire_bytes(w, tail, strlen(tail));
    }
    cli_wire_bytes(w, "]}", 2U);
}
static void host_recycle_child(cli_executor_slot_t *slot)
{
    cli_executor_slot_t old = *slot;
    host_secret_wipe(&old.profile);
    cli_secret_exchange_t profile = {.body = old.profile.body, .frame = old.profile.frame,
        .scratch = old.profile.scratch, .body_capacity = old.profile.body_capacity,
        .frame_capacity = old.profile.frame_capacity, .scratch_capacity = old.profile.scratch_capacity};
    old.command.used = false; old.command.final = false; old.command.forward_committed = false;
    old.cancel.used = false; old.cancel.final = false; old.cancel.forward_committed = false;
    old.mailbox.head = 0U; old.mailbox.count = 0U; old.mailbox.text_used = 0U;
    old.mailbox.reply_pending = false; old.mailbox.reply.length = 0U;
    *slot = (cli_executor_slot_t){.task = old.task, .status = old.status, .result = old.result,
        .update = old.update, .command = old.command, .cancel = old.cancel,
        .inbound = old.inbound, .outbound = old.outbound, .profile = profile, .mailbox = old.mailbox};
    slot->task.length = 0U; slot->status.length = 0U; slot->result.length = 0U; slot->update.length = 0U;
    slot->status.data[0] = 0U; slot->result.data[0] = 0U;
    for (size_t direction = 0U; direction < 2U; ++direction) {
        cli_control_lane_t *lanes = direction == 0U ? slot->inbound : slot->outbound;
        for (size_t i = 0U; i < CLI_HOST_LANES; ++i) {
            cli_control_lane_t prior = lanes[i];
            lanes[i] = (cli_control_lane_t){.pending = prior.pending, .provisional_reply = prior.provisional_reply,
                .final_original = prior.final_original, .final_reply = prior.final_reply, .next_wire = prior.next_wire};
            lanes[i].pending.length = 0U; lanes[i].provisional_reply.length = 0U;
            lanes[i].final_original.length = 0U; lanes[i].final_reply.length = 0U;
        }
    }
}
static cog_result_t host_group_slice(cog_cli_host_runtime_t *r, cli_group_record_t *group)
{
    if (group->phase == CLI_GROUP_FREE) return COG_ERROR_AGAIN;
    if (group->phase == CLI_GROUP_GRANTED) {
        if (r->now_ms >= group->deadline_ms || r->shutdown) return cli_runtime_release(r, group->reservation);
        return COG_ERROR_AGAIN;
    }
    if (group->phase == CLI_GROUP_ADMITTING)
        return cli_runtime_start(r, group->reservation, group->started_checkpoint);
    if (group->phase == CLI_GROUP_SAVED) {
        for (size_t i = 0U; i < group->submitted; ++i) {
            cli_group_member_t *member = &group->members[i];
            cli_executor_slot_t *slot = &r->executors[member->slot];
            if (!member->present || slot->phase == CLI_SLOT_FREE) continue;
            if (slot->app != NULL || slot->borrow_live || !slot->terminal_acked) return COG_ERROR_AGAIN;
            if (slot->owner_peer != 0U) {
                cli_control_lane_t *receipt = &slot->inbound[COG_CLI_LANE_UPDATE - 1U];
                if (slot->first_admission_sent && (receipt->final_valid ||
                    receipt->retired_through <= slot->pending_update_wire)) return COG_ERROR_AGAIN;
                if (slot->net != NULL || slot->pending_profile != NULL) return host_local_dispose(r, slot);
            }
            cog_result_t rc = cog_agent_release(r->control_core, slot->agent);
            if (rc == COG_ERROR_INVALID_STATE) return COG_ERROR_AGAIN;
            if (rc != COG_OK) return rc;
            for (size_t w = 0U; w < r->bounds.workers; ++w)
                if (host_worker_equal(r->workers[w].id, slot->worker) && r->workers[w].reserved_slots != 0U)
                    --r->workers[w].reserved_slots;
            host_recycle_child(slot);
            return COG_OK;
        }
        cli_host_bytes_t tasks = group->tasks, output = group->output;
        *group = (cli_group_record_t){.tasks = tasks, .output = output};
        return COG_OK;
    }
    if (group->phase == CLI_GROUP_RESULT) return COG_ERROR_AGAIN;
    bool failure = group->phase == CLI_GROUP_ROLLBACK || r->now_ms >= group->deadline_ms;
    for (size_t i = 0U; i < group->submitted; ++i)
        if (group->members[i].terminal && group->members[i].outcome != COG_OK) failure = true;
    if (failure) {
        group->gate_open = false; group->phase = CLI_GROUP_ROLLBACK;
        for (size_t i = 0U; i < group->submitted; ++i) {
            cli_group_member_t *member = &group->members[i];
            if (!member->present) {
                if (member->outcome == COG_OK) member->outcome = r->now_ms >= group->deadline_ms ? COG_ERROR_TIMEOUT : COG_ERROR_ABORTED;
                r->executors[member->slot].phase = CLI_SLOT_FREE;
                continue;
            }
            if (member->terminal) continue;
            cli_executor_slot_t *slot = &r->executors[member->slot];
            slot->cancel_latched = true;
            cog_result_t rc = cog_agent_cancel(r->control_core, member->agent);
            if (rc != COG_OK && rc != COG_ERROR_INVALID_STATE && rc != COG_ERROR_OVERFLOW) return rc;
        }
    }
    cog_result_t aggregate = group->aggregate;
    bool uncertain = false;
    for (size_t i = 0U; i < group->submitted; ++i) {
        cli_group_member_t *member = &group->members[i];
        if (member->present) {
            cli_executor_slot_t *slot = &r->executors[member->slot];
            if (!member->terminal || !slot->terminal_acked || slot->borrow_live || slot->app != NULL)
                return COG_ERROR_AGAIN;
        }
        aggregate = host_first(aggregate, member->outcome);
        uncertain = uncertain || member->effects_uncertain;
    }
    if (uncertain) aggregate = COG_ERROR_INDETERMINATE;
    cli_wire_writer_t count = {NULL, 0U, true}; host_group_json(r, group, &count);
    if (!count.valid || count.used >= group->output.capacity) return COG_ERROR_LIMIT;
    cli_wire_writer_t writer = {group->output.data, 0U, true}; host_group_json(r, group, &writer);
    group->output.data[writer.used] = 0U; group->output.length = writer.used;
    group->aggregate = aggregate; group->phase = CLI_GROUP_RESULT;
    return COG_OK;
}

static cog_result_t host_ui_frame(cog_cli_host_runtime_t *r, cli_client_record_t *client,
    cog_cli_control_kind_t kind, const void *body, size_t length)
{
    if (r->next_nonce == 0U || r->next_nonce == UINT64_MAX) return COG_ERROR_LIMIT;
    cog_cli_control_message_t m = {.kind = kind, .lane = COG_CLI_LANE_REPLAY,
        .runtime_incarnation = r->runtime_incarnation, .sender_incarnation = r->runtime_incarnation,
        .wire_id = r->next_nonce, .session_id = r->primary_session, .payload = body, .payload_len = length};
    size_t required;
    cog_result_t rc = cog_cli_control_encode(&m, client->send.data, client->send.capacity, &required);
    if (rc == COG_OK) { client->send.length = required; ++r->next_nonce; }
    return rc;
}
static cog_result_t host_client_slice(cog_cli_host_runtime_t *r, cli_client_record_t *client)
{
    if (!client->used) return COG_ERROR_AGAIN;
    if (!client->connected) {
        if (r->now_ms >= client->controller_until_ms) client->controller = false;
        if (r->now_ms >= client->receipts_until_ms) {
            for (size_t i = 0U; i < r->bounds.command_records; ++i)
                if (r->commands[i].lease == client->logical_lease) r->commands[i].phase = CLI_COMMAND_FREE;
            client->used = false; client->controller = false;
        }
        return COG_ERROR_AGAIN;
    }
    if (client->send.length != 0U) {
        cog_ipc_message_t message = {.data = client->send.data, .len = client->send.length,
            .capacity = client->send.capacity};
        cog_result_t rc = cog_ipc_send_to_peer(cog_core_ipc(r->control_core), client->peer, &message);
        if (rc == COG_OK) client->send.length = 0U;
        if (rc == COG_ERROR_NOT_FOUND || rc == COG_ERROR_IO) {
            client->connected = false; client->controller_until_ms = r->now_ms + r->fixed.controller_reconnect_ms;
            client->receipts_until_ms = r->now_ms + r->fixed.client_receipt_ms;
            client->send.length = 0U; return COG_ERROR_AGAIN;
        }
        return rc;
    }
    /* Consuming an ACK retires the command record, so its retained response
     * must survive a full client send slot independently of that record. */
    if (client->hello.acknowledged && client->hello.pending) {
        cog_result_t rc = host_client_queue(client, &client->hello.ack_reply);
        if (rc == COG_OK) client->hello.pending = false;
        return rc;
    }
    const cog_cli_control_lane_t reply_lanes[] = {COG_CLI_LANE_COMMAND, COG_CLI_LANE_CANCEL};
    for (size_t i = 0U; i < sizeof(reply_lanes) / sizeof(reply_lanes[0]); ++i) {
        cli_control_lane_t *lane = host_client_lane(r, client, reply_lanes[i]);
        if (!lane->pending_valid) continue;
        cog_result_t rc = host_client_queue(client, &lane->final_reply);
        if (rc == COG_OK) lane->pending_valid = false;
        return rc;
    }
    if (!client->hello.acknowledged) return host_client_queue(client, &client->hello.reply);
    if (client->resync.length != 0U) {
        cog_result_t rc = host_ui_frame(r, client, COG_CLI_CONTROL_RESYNC_VIEW,
            client->resync.data, client->resync.length);
        if (rc == COG_OK) client->resync.length = 0U;
        return rc;
    }
    if (client->awaiting_resync && client->gap_last != 0U) return COG_ERROR_AGAIN;
    cog_ipc_replay_record_t record;
    size_t required;
    cog_result_t rc = cog_cli_projection_copy_after(r->projection,
        client->awaiting_resync ? 0U : client->rendered_generation,
        client->queued_sequence, &record, r->projection_scratch.data,
        r->projection_scratch.capacity, &required);
    if (rc != COG_OK) return rc;
    if (record.kind == COG_IPC_REPLAY_CURSOR) return COG_ERROR_AGAIN;
    rc = cog_ipc_replay_encode(&record, r->ingress.data, r->ingress.capacity, &required);
    if (rc != COG_OK) return rc;
    rc = host_ui_frame(r, client, COG_CLI_CONTROL_REPLAY, r->ingress.data, required);
    if (rc != COG_OK) return rc;
    if (record.kind == COG_IPC_REPLAY_GAP) {
        cog_cli_projection_view_t view;
        rc = cog_cli_projection_snapshot(r->projection, &view); if (rc != COG_OK) return rc;
        rc = cog_cli_projection_encode(&view, client->resync.data, client->resync.capacity, &required);
        if (rc != COG_OK) return rc;
        client->resync.length = required; client->awaiting_resync = true;
        client->gap_last = record.last_sequence; client->queued_sequence = view.display.through_sequence;
    } else client->queued_sequence = record.sequence;
    return COG_OK;
}
static void host_retire_profile(cli_secret_exchange_t *secret)
{
    if (secret->wire > secret->retired_through) secret->retired_through = secret->wire;
    secret->source = NULL; secret->pending = false; secret->acknowledged = false;
    secret->wire = 0U; secret->replied_wire = 0U; secret->generation = 0U; secret->purpose = 0U;
    memset(&secret->probe, 0, sizeof(secret->probe)); host_secret_wipe(secret);
}
static void host_mirror_retire(cog_cli_host_runtime_t *r)
{
    if (r->future_mirror == NULL || r->active_configuration != r->published_configuration ||
        (r->executors[r->primary_slot].command.used && !r->executors[r->primary_slot].command.final) ||
        (r->handoff != NULL && r->handoff->phase != CLI_HANDOFF_IDLE)) return;
    for (size_t i = 0U; i < r->group_capacity; ++i)
        if (r->groups[i].phase != CLI_GROUP_FREE && r->groups[i].pinned_configuration != r->published_configuration)
            return;
    for (size_t i = 2U; i < r->bounds.executors; ++i)
        if (r->executors[i].phase != CLI_SLOT_FREE &&
            r->executors[i].configuration_generation != r->published_configuration) return;
    for (size_t i = 0U; i < r->bounds.workers; ++i) {
        cli_secret_exchange_t *profile = &r->workers[i].startup_profile;
        if (profile->source == r->current_mirror && !profile->acknowledged) return;
    }
    for (size_t i = 0U; i < r->bounds.executors; ++i) {
        cli_secret_exchange_t *profile = &r->executors[i].profile;
        if (profile->source == r->current_mirror && !profile->acknowledged) return;
    }
    for (size_t i = 0U; i < r->bounds.workers; ++i)
        if (r->workers[i].startup_profile.source == r->current_mirror)
            host_retire_profile(&r->workers[i].startup_profile);
    for (size_t i = 0U; i < r->bounds.executors; ++i)
        if (r->executors[i].profile.source == r->current_mirror)
            host_retire_profile(&r->executors[i].profile);
    cog_cli_configuration_free(r->current_mirror);
    r->current_mirror = r->future_mirror; r->future_mirror = NULL;
}
static cog_result_t host_reload(cog_cli_host_runtime_t *r, cli_executor_slot_t *slot,
    cli_command_record_t *record, const cog_cli_command_t *command)
{
    cli_forwarded_operation_t *op = &slot->command;
    if (op->used && (op->lease != record->lease || op->request_wire != record->request_wire)) return COG_ERROR_AGAIN;
    if (!op->used) {
        if (r->future_mirror != NULL || slot->profile.pending ||
            (r->handoff != NULL && r->handoff->phase != CLI_HANDOFF_IDLE)) return COG_ERROR_AGAIN;
        if (r->reload_attempts_used == CLI_RELOAD_ATTEMPTS) return COG_ERROR_LIMIT;
        if (record->original.length > op->original.capacity) return COG_ERROR_LIMIT;
        cog_cli_reload_options_t replacement = {0};
        if (command->reload_field == 1U) replacement.model_id = command->text;
        else if (command->reload_field == 2U) replacement.base_url = command->text;
        else if (command->reload_field == 3U) replacement.credential_ref = command->text;
        else return COG_ERROR_PROTOCOL;
        ++r->reload_attempts_used;
        uint64_t generation = (uint64_t)r->reload_attempts_used + 1U;
        cog_result_t rc = cog_cli_configuration_reload_new(&r->future_mirror, r->current_mirror,
            &replacement, generation);
        memcpy(op->original.data, record->original.data, record->original.length);
        op->original.length = record->original.length; op->used = true; op->lease = record->lease;
        op->request_wire = record->request_wire; op->owner = record->owner; op->executor_wire = 0U;
        op->reply.length = 0U; op->forward_committed = false; op->final = false;
        if (rc == COG_OK && slot->app != NULL) {
            cog_cli_configuration_t *candidate = NULL;
            rc = cog_cli_configuration_clone_new(&candidate, r->future_mirror, slot->net);
            uint64_t published;
            if (rc == COG_OK) rc = cog_cli_execution_reload_resolved(slot->app, &candidate,
                r->reload_attempts_used, &published);
            if (rc == COG_OK) r->published_configuration = published;
        }
        if (rc != COG_OK || slot->app != NULL) {
            if (rc != COG_OK) { cog_cli_configuration_free(r->future_mirror); r->future_mirror = NULL; }
            op->final = true; op->forward_committed = true; op->result = rc;
            record->phase = CLI_COMMAND_FORWARDED; return COG_ERROR_AGAIN;
        }
        host_retire_profile(&slot->profile);
        slot->profile.source = r->future_mirror; slot->profile.purpose = 4U;
        slot->profile.generation = generation;
    }
    cli_worker_record_t *worker = NULL;
    for (size_t i = 0U; i < r->bounds.workers; ++i)
        if (host_worker_equal(r->workers[i].id, slot->worker)) worker = &r->workers[i];
    if (worker == NULL) return COG_ERROR_INVALID_STATE;
    bool accepted = false;
    cog_result_t rc = host_profile_send(r, worker, slot, &slot->profile, 4U, &accepted);
    op->executor_wire = slot->profile.wire;
    if (accepted) { op->forward_committed = true; record->phase = CLI_COMMAND_FORWARDED; }
    return rc;
}

static cog_result_t host_forward_command(cog_cli_host_runtime_t *r, cli_executor_slot_t *slot,
    cli_command_record_t *record, const cog_cli_command_t *command)
{
    cog_cli_control_lane_t id = command->kind == COG_CLI_COMMAND_CANCEL ? COG_CLI_LANE_CANCEL : COG_CLI_LANE_COMMAND;
    cli_forwarded_operation_t *op = id == COG_CLI_LANE_CANCEL ? &slot->cancel : &slot->command;
    cli_control_lane_t *lane = &slot->outbound[(size_t)id - 1U];
    if (op->used && (op->lease != record->lease || op->request_wire != record->request_wire)) return COG_ERROR_AGAIN;
    if (!op->used) {
        if (lane->pending_valid || record->original.length > op->original.capacity ||
            r->projection_scratch.capacity < 40U) return COG_ERROR_AGAIN;
        uint64_t serial = 0U;
        cog_cli_projection_view_t view;
        cog_result_t rc = cog_cli_projection_snapshot(r->projection, &view); if (rc != COG_OK) return rc;
        if (command->kind == COG_CLI_COMMAND_POLICY) {
            if (view.policy_count != 1U || view.policies[0].nonce != command->nonce ||
                view.policies[0].assistant_node != command->node_id ||
                view.policies[0].call_ordinal != command->call_ordinal ||
                strcmp(view.policies[0].session_id, command->target_session_id) != 0)
                return COG_ERROR_INVALID_STATE;
            serial = slot->inbound[COG_CLI_LANE_POLICY - 1U].provisional_prefix;
        } else if (command->kind == COG_CLI_COMMAND_CONTINUE || command->kind == COG_CLI_COMMAND_FORK)
            serial = slot->inbound[COG_CLI_LANE_VIEW - 1U].provisional_prefix;
        size_t length;
        rc = cog_cli_command_encode(command, r->projection_scratch.data + 40U,
            r->projection_scratch.capacity - 40U, &length); if (rc != COG_OK) return rc;
        cli_wire_writer_t writer = {r->projection_scratch.data, 0U, true};
        cli_wire_put(&writer, record->lease, 8U); cli_wire_put(&writer, record->request_wire, 8U);
        cli_wire_put(&writer, record->owner.epoch, 8U); cli_wire_put(&writer, record->owner.instance_id, 8U);
        cli_wire_put(&writer, serial, 8U);
        rc = host_remote_frame(r, slot, COG_CLI_CONTROL_EXECUTOR_COMMAND, id, 0U,
            writer.bytes, length + 40U, &lane->pending, &lane->pending_wire); if (rc != COG_OK) return rc;
        if (command->kind == COG_CLI_COMMAND_POLICY) {
            view.policies = NULL; view.policy_count = 0U;
            uint64_t sequence;
            rc = cog_cli_projection_commit(r->projection, &view, &sequence); if (rc != COG_OK) return rc;
        }
        memcpy(op->original.data, record->original.data, record->original.length);
        op->original.length = record->original.length; op->used = true; op->forward_committed = false;
        op->final = false; op->lease = record->lease; op->request_wire = record->request_wire;
        op->owner = record->owner; op->executor_serial = serial; op->executor_wire = lane->pending_wire;
        op->reply.length = 0U; lane->pending_valid = true;
    }
    cog_result_t rc = host_lane_send(r, slot->worker, lane, &lane->pending, false);
    if (rc == COG_OK) { op->forward_committed = true; record->phase = CLI_COMMAND_FORWARDED; }
    return rc == COG_OK ? COG_ERROR_AGAIN : rc;
}
static cog_result_t host_forwarded_slice(cog_cli_host_runtime_t *r, cli_executor_slot_t *slot)
{
    cli_forwarded_operation_t *operations[] = {&slot->command, &slot->cancel};
    const cog_cli_control_lane_t ids[] = {COG_CLI_LANE_COMMAND, COG_CLI_LANE_CANCEL};
    for (size_t i = 0U; i < 2U; ++i) {
        cli_forwarded_operation_t *op = operations[i];
        cli_control_lane_t *lane = &slot->outbound[(size_t)ids[i] - 1U];
        if (!op->used) continue;
        if (i == 0U && op->forward_committed && slot->profile.pending && slot->profile.purpose == 4U) {
            for (size_t w = 0U; w < r->bounds.workers; ++w)
                if (host_worker_equal(r->workers[w].id, slot->worker)) {
                    cog_result_t rc = host_profile_send(r, &r->workers[w], slot, &slot->profile, 4U, NULL);
                    if (rc != COG_OK && rc != COG_ERROR_AGAIN) return rc;
                }
        }
        if (op->forward_committed && lane->pending_valid) {
            cog_result_t rc = host_lane_send(r, slot->worker, lane, &lane->pending, false);
            if (rc != COG_OK && rc != COG_ERROR_AGAIN) return rc;
        }
        if (!op->final || lane->pending_valid) continue;
        bool referenced = false;
        for (size_t n = 0U; n < r->bounds.command_records; ++n)
            if (r->commands[n].phase == CLI_COMMAND_FORWARDED && r->commands[n].lease == op->lease &&
                r->commands[n].request_wire == op->request_wire) referenced = true;
        if (!referenced) { op->used = false; op->original.length = 0U; op->reply.length = 0U; }
    }
    return COG_OK;
}

static cog_result_t host_command_slice(cog_cli_host_runtime_t *r, cli_command_record_t *record)
{
    if (record->phase == CLI_COMMAND_FREE) return COG_ERROR_AGAIN;
    cli_client_record_t *client = NULL;
    for (size_t i = 0U; i < r->bounds.clients; ++i)
        if (r->clients[i].used && r->clients[i].logical_lease == record->lease) client = &r->clients[i];
    if (record->phase == CLI_COMMAND_FINAL)
        return client != NULL && client->connected ? host_client_queue(client, &record->reply) : COG_ERROR_AGAIN;
    cog_cli_control_message_t envelope;
    size_t required;
    cog_result_t rc = cog_cli_control_decode(record->original.data, record->original.length,
        &envelope, r->decode.data, r->decode.capacity, &required);
    if (rc != COG_OK) return rc;
    cog_cli_command_t command;
    rc = cog_cli_command_decode(envelope.payload, envelope.payload_len, &command,
        record->decoded_storage.data, record->decoded_storage.capacity, &required);
    if (rc != COG_OK) return rc;
    cli_executor_slot_t *root = &r->executors[r->primary_slot];
    if (record->phase == CLI_COMMAND_FORWARDED) {
        cli_forwarded_operation_t *op = command.kind == COG_CLI_COMMAND_CANCEL ? &root->cancel : &root->command;
        if (!op->used || op->lease != record->lease || op->request_wire != record->request_wire)
            return COG_ERROR_INVALID_STATE;
        if (!op->final) return COG_ERROR_AGAIN;
        record->result = op->result;
    } else {
        cog_result_t outcome = COG_OK;
        cog_cli_projection_view_t view;
        rc = cog_cli_projection_snapshot(r->projection, &view); if (rc != COG_OK) return rc;
        bool authorized = client != NULL && client->connected && client->controller;
        cli_forwarded_operation_t *held = command.kind == COG_CLI_COMMAND_CANCEL ? &root->cancel : &root->command;
        bool own_policy_retirement = command.kind == COG_CLI_COMMAND_POLICY && held->used &&
            held->lease == record->lease && held->request_wire == record->request_wire &&
            held->executor_serial == root->inbound[COG_CLI_LANE_POLICY - 1U].provisional_prefix;
        if (command.kind != COG_CLI_COMMAND_RESYNC && !authorized) outcome = COG_ERROR_DENIED;
        else if (command.kind != COG_CLI_COMMAND_RESYNC &&
            (r->admission_frozen || r->shutdown || !host_owner_equal(record->owner, r->primary_owner) ||
             command.observed_generation != view.display.generation ||
             (!own_policy_retirement && command.observed_sequence != view.display.through_sequence))) outcome = COG_ERROR_INVALID_STATE;
        else if (command.kind == COG_CLI_COMMAND_RESYNC) {
            if (client == NULL || !client->awaiting_resync || command.observed_generation != view.display.generation ||
                command.observed_sequence != client->queued_sequence || command.observed_sequence < client->gap_last)
                outcome = COG_ERROR_INVALID_STATE;
            else {
                client->awaiting_resync = false; client->gap_last = 0U;
                client->rendered_generation = command.observed_generation;
                client->rendered_sequence = command.observed_sequence;
            }
        } else if (command.kind == COG_CLI_COMMAND_SHUTDOWN) outcome = cog_cli_host_runtime_request_shutdown(r);
        else if (command.kind == COG_CLI_COMMAND_HANDOFF)
            outcome = r->handoff == NULL ? COG_ERROR_UNSUPPORTED : cog_cli_handoff_request(r->handoff, command.target);
        else if (command.kind == COG_CLI_COMMAND_CANCEL && command.agent_id != 0U) {
            cli_executor_slot_t *child = host_agent(r, command.agent_id);
            outcome = child == NULL ? COG_ERROR_NOT_FOUND : cog_agent_cancel(r->control_core, command.agent_id);
            if (outcome == COG_OK) child->cancel_latched = true;
        } else if (command.kind == COG_CLI_COMMAND_RELOAD) outcome = host_reload(r, root, record, &command);
        else if (root->app != NULL) {
            uint64_t executor_serial = 0U;
            if (command.kind == COG_CLI_COMMAND_POLICY) {
                cog_cli_policy_view_t policy;
                outcome = cog_cli_execution_copy_policy(root->app, &executor_serial, &policy);
                if (outcome == COG_OK && (view.policy_count != 1U || view.policies[0].nonce != command.nonce ||
                    !host_policy_equal(&policy, view.policies))) outcome = COG_ERROR_INVALID_STATE;
                if (outcome == COG_OK) {
                    /* Consume the visible decision before owner admission. */
                    cog_cli_projection_view_t consumed = view; consumed.policy_count = 0U; consumed.policies = NULL;
                    uint64_t sequence;
                    outcome = cog_cli_projection_commit(r->projection, &consumed, &sequence);
                }
            } else if (command.kind == COG_CLI_COMMAND_CONTINUE || command.kind == COG_CLI_COMMAND_FORK)
                executor_serial = root->app->headless->committed_serial;
            if (outcome == COG_OK) outcome = cog_cli_execution_apply_command(root->app, &command, record->owner, executor_serial);
        } else outcome = host_forward_command(r, root, record, &command);
        if (outcome == COG_ERROR_AGAIN || outcome == COG_ERROR_OVERFLOW) return COG_ERROR_AGAIN;
        if (held->used && !held->forward_committed && held->lease == record->lease &&
            held->request_wire == record->request_wire) {
            held->used = false;
            if (command.kind == COG_CLI_COMMAND_RELOAD) {
                host_retire_profile(&root->profile);
                cog_cli_configuration_free(r->future_mirror); r->future_mirror = NULL;
            }
            root->outbound[(size_t)envelope.lane - 1U].pending_valid = false;
        }
        record->result = outcome;
    }
    rc = host_ui_result(r, envelope.lane, record->request_wire, 0U, record->result,
        &record->reply, &record->result_wire);
    if (rc != COG_OK) return rc;
    record->phase = CLI_COMMAND_FINAL;
    return client != NULL && client->connected ? host_client_queue(client, &record->reply) : COG_OK;
}

static cog_result_t host_remote_child_slice(cog_cli_host_runtime_t *r, cli_executor_slot_t *slot)
{
    cli_worker_record_t *worker = NULL;
    for (size_t i = 0U; i < r->bounds.workers; ++i)
        if (r->workers[i].used && host_worker_equal(r->workers[i].id, slot->worker)) worker = &r->workers[i];
    if (worker == NULL || !worker->profile_ready || worker->native.health != COG_HEALTH_HEALTHY)
        return COG_ERROR_AGAIN;
    if (!slot->first_admission_sent) {
        for (size_t i = 2U; i < r->bounds.executors; ++i) {
            cli_executor_slot_t *earlier = &r->executors[i];
            if (earlier->phase != CLI_SLOT_FREE && earlier->agent != 0U && earlier->agent < slot->agent &&
                host_worker_equal(earlier->worker, slot->worker) && !earlier->first_admission_sent &&
                !earlier->terminal_copied) return COG_ERROR_AGAIN;
        }
    }
    if (slot->cancel_latched) {
        if (!slot->first_admission_sent) {
            slot->local_done = true;
            cog_result_t rc = host_local_terminal(r, slot);
            if (rc == COG_OK) slot->phase = CLI_SLOT_RETAINED;
            return rc;
        }
        cli_control_lane_t *lane = &slot->outbound[COG_CLI_LANE_CANCEL - 1U];
        if (!lane->pending_valid) {
            if (slot->drain_deadline_ms == 0U) slot->drain_deadline_ms = r->now_ms + r->fixed.shutdown_ms;
            unsigned char body[8]; cli_wire_writer_t w = {body, 0U, true};
            cli_wire_put(&w, slot->drain_deadline_ms, 8U);
            cog_result_t rc = host_remote_frame(r, slot, COG_CLI_CONTROL_CHILD_CANCEL,
                COG_CLI_LANE_CANCEL, 0U, body, sizeof(body), &lane->pending, &lane->pending_wire);
            if (rc != COG_OK) return rc;
            lane->pending_valid = true;
        }
        return host_lane_send(r, slot->worker, lane, &lane->pending, true);
    }
    if (slot->phase != CLI_SLOT_PROFILE) return COG_ERROR_AGAIN;
    cli_group_record_t *group = host_slot_group(r, slot);
    if (group == NULL || !group->gate_open) return COG_ERROR_AGAIN;
    if (slot->pending_profile == NULL) {
        cog_result_t rc = host_transport(r, slot); if (rc != COG_OK) return rc;
        const cog_cli_configuration_t *mirror = r->future_mirror != NULL &&
            slot->configuration_generation == r->published_configuration ? r->future_mirror : r->current_mirror;
        rc = cog_cli_configuration_child_new(&slot->pending_profile, mirror, &r->fixed.child_profile,
            slot->configuration_generation, r->now_ms, slot->deadline_ms, slot->net, &slot->deadline_ms);
        if (rc != COG_OK) return rc;
        slot->profile.source = slot->pending_profile;
        slot->mailbox.reply.capacity = cog_cli_configuration_options(slot->pending_profile)->max_tool_output_bytes;
    }
    cog_result_t rc = host_profile_send(r, worker, slot, &slot->profile, 2U, NULL);
    if (rc != COG_OK) return rc;
    cli_control_lane_t *lane = &slot->outbound[COG_CLI_LANE_DELEGATE - 1U];
    if (!lane->pending_valid) {
        size_t need = strlen(slot->parent_session) + strlen(slot->parent_call) + slot->task.length + 512U;
        if (need > r->ingress.capacity) return COG_ERROR_LIMIT;
        cli_wire_writer_t w = {r->ingress.data, 0U, true};
        cli_wire_string(&w, slot->parent_session, strlen(slot->parent_session));
        cli_wire_put(&w, slot->parent_node, 8U); cli_wire_put(&w, slot->parent_ordinal, 2U);
        cli_wire_string(&w, slot->parent_call, strlen(slot->parent_call));
        cli_wire_string(&w, (const char *)slot->task.data, slot->task.length);
        cli_wire_put(&w, slot->configuration_generation, 8U); cli_wire_put(&w, slot->deadline_ms, 8U);
        cli_wire_put(&w, slot->group, 8U); cli_wire_put(&w, r->bounds.status_bytes, 4U);
        cli_wire_put(&w, group->result_payload_cap + 1U, 4U); cli_wire_put(&w, slot->member_count, 2U);
        for (size_t i = 0U; i < slot->member_count; ++i) cli_wire_put(&w, slot->members[i], 8U);
        rc = host_remote_frame(r, slot, COG_CLI_CONTROL_CHILD_START, COG_CLI_LANE_DELEGATE,
            0U, w.bytes, w.used, &lane->pending, &lane->pending_wire);
        if (rc != COG_OK) return rc;
        lane->pending_valid = true;
    }
    return host_lane_send(r, slot->worker, lane, &lane->pending, false);
}


static cog_result_t host_request_cache(cli_executor_slot_t *slot,
    const cog_cli_control_message_t *m, const cog_orchestrator_event_t *event, bool *duplicate)
{
    cli_control_lane_t *lane = &slot->inbound[(size_t)m->lane - 1U];
    *duplicate = false;
    if (lane->final_valid && m->wire_id == lane->final_answered_wire) {
        if (!host_bytes_equal(&lane->final_original, event->data, event->data_len)) return COG_ERROR_PROTOCOL;
        *duplicate = true; return COG_OK;
    }
    if (m->wire_id <= lane->retired_through || (lane->final_valid && m->wire_id < lane->final_answered_wire))
        return COG_ERROR_INVALID_STATE;
    if (lane->pending_valid && (lane->pending_wire != m->wire_id ||
        !host_bytes_equal(&lane->pending, event->data, event->data_len))) return COG_ERROR_AGAIN;
    if (event->data_len > lane->pending.capacity || event->data_len > lane->final_original.capacity)
        return COG_ERROR_LIMIT;
    if (m->wire_id == UINT64_MAX) return COG_ERROR_LIMIT;
    memcpy(lane->pending.data, event->data, event->data_len); lane->pending.length = event->data_len;
    lane->pending_wire = m->wire_id; lane->pending_valid = true;
    return COG_OK;
}
static cog_result_t host_request_finish(cog_cli_host_runtime_t *r, cli_executor_slot_t *slot,
    cog_cli_control_lane_t id, cog_cli_control_kind_t kind, const void *body, size_t length)
{
    cli_control_lane_t *lane = &slot->inbound[(size_t)id - 1U];
    if (!lane->pending_valid) return COG_ERROR_INVALID_STATE;
    cog_result_t rc = host_remote_frame(r, slot, kind, id, lane->pending_wire,
        body, length, &lane->final_reply, &lane->final_reply_wire);
    if (rc != COG_OK) return rc;
    if (lane->final_valid) lane->retired_through = lane->final_answered_wire;
    memcpy(lane->final_original.data, lane->pending.data, lane->pending.length);
    lane->final_original.length = lane->pending.length;
    lane->final_answered_wire = lane->pending_wire; lane->final_valid = true; lane->pending_valid = false;
    lane->provisional_wire = 0U;
    return host_reply_send(r, slot->worker, lane, &lane->final_reply);
}
static cog_result_t host_remote_mailbox(cog_cli_host_runtime_t *r, cli_executor_slot_t *slot,
    const cog_cli_control_message_t *m, const cog_orchestrator_event_t *event)
{
    if (m->lane != COG_CLI_LANE_MAILBOX || m->reply_to != 0U || slot->agent == 0U ||
        slot->phase != CLI_SLOT_STARTED) return COG_ERROR_INVALID_STATE;
    bool duplicate;
    cog_result_t rc = host_request_cache(slot, m, event, &duplicate);
    if (rc != COG_OK) return rc;
    cli_control_lane_t *lane = &slot->inbound[COG_CLI_LANE_MAILBOX - 1U];
    if (duplicate) return host_reply_send(r, slot->worker, lane, &lane->final_reply);
    if (r->next_nonce > UINT64_MAX - 4U) return COG_ERROR_LIMIT;
    if (m->kind == COG_CLI_CONTROL_CHILD_MESSAGE) {
        cli_wire_reader_t reader = {m->payload, m->payload_len, 0U, true};
        cog_agent_id_t target = cli_wire_get(&reader, 8U);
        size_t n; const char *text = cli_wire_read_string(&reader, &n);
        if (!reader.valid || reader.used != reader.length || target == 0U) return COG_ERROR_PROTOCOL;
        cog_cli_delegation_context_t context = {.coordinator = r, .agent = slot->agent, .group = slot->group};
        cli_delegation_handle_t request = {.context = &context, .operation = CLI_SEND_MESSAGE,
            .started = true, .destination = target, .message = text, .message_length = n,
            .output = {.data = r->projection_scratch.data, .capacity = r->projection_scratch.capacity}};
        rc = cli_runtime_mailbox_step(r, &request);
        unsigned char body[5]; cli_wire_writer_t writer = {body, 0U, true};
        cli_wire_put(&writer, (uint32_t)rc, 4U); cli_wire_put(&writer, rc == COG_ERROR_AGAIN ? 1U : 2U, 1U);
        if (rc == COG_ERROR_AGAIN) {
            if (lane->provisional_wire == 0U) {
                rc = host_remote_frame(r, slot, COG_CLI_CONTROL_MESSAGE_RESULT, m->lane, m->wire_id,
                    body, sizeof(body), &lane->provisional_reply, &lane->provisional_wire);
                if (rc != COG_OK) return rc;
            }
            return host_reply_send(r, slot->worker, lane, &lane->provisional_reply);
        }
        return host_request_finish(r, slot, m->lane, COG_CLI_CONTROL_MESSAGE_RESULT, body, sizeof(body));
    }
    if (m->kind != COG_CLI_CONTROL_MAILBOX_POLL || m->payload_len != 0U) return COG_ERROR_PROTOCOL;
    cli_mailbox_t *mailbox = &slot->mailbox;
    size_t count = 0U, measured = 0U;
    for (size_t n = 0U; n <= mailbox->count; ++n) {
        cli_wire_writer_t json = {NULL, 0U, true}; host_mailbox_json(&json, mailbox, n, n < mailbox->count);
        if (!json.valid || json.used >= mailbox->reply.capacity) break;
        count = n; measured = json.used;
    }
    cog_result_t outcome = measured == 0U || (mailbox->count != 0U && count == 0U) ? COG_ERROR_LIMIT : COG_OK;
    if (outcome != COG_OK) count = 0U;
    cli_wire_writer_t writer = {r->projection_scratch.data, 0U, true};
    cli_wire_put(&writer, (uint32_t)outcome, 4U);
    cli_wire_put(&writer, outcome == COG_OK && count < mailbox->count ? 1U : 0U, 1U);
    cli_wire_put(&writer, count, 2U);
    for (size_t i = 0U; i < count; ++i) {
        cli_mailbox_entry_t *item = &mailbox->entries[(mailbox->head + i) % mailbox->entry_capacity];
        if (item->length + 12U > r->projection_scratch.capacity - writer.used) return COG_ERROR_LIMIT;
        cli_wire_put(&writer, item->source, 8U);
        cli_wire_string(&writer, (const char *)mailbox->text + item->offset, item->length);
    }
    rc = host_request_finish(r, slot, m->lane, COG_CLI_CONTROL_MAILBOX_RESULT, writer.bytes, writer.used);
    /* The immutable reply owns the selected prefix even if the transport
     * fails after caching. A retry must never expose that prefix twice. */
    if (!lane->final_valid || lane->final_answered_wire != m->wire_id) return rc;
    if (outcome == COG_OK) {
        for (size_t i = 0U; i < count; ++i) {
            mailbox->entries[mailbox->head].used = false;
            mailbox->head = (mailbox->head + 1U) % mailbox->entry_capacity; --mailbox->count;
        }
        size_t offset = 0U;
        for (size_t i = 0U; i < mailbox->count; ++i) {
            cli_mailbox_entry_t *item = &mailbox->entries[(mailbox->head + i) % mailbox->entry_capacity];
            memmove(mailbox->text + offset, mailbox->text + item->offset, item->length + 1U);
            item->offset = offset; offset += item->length + 1U;
        }
        mailbox->text_used = offset;
    }
    return rc;
}

static cog_result_t host_remote_command_result(cog_cli_host_runtime_t *r, cli_executor_slot_t *slot,
    const cog_cli_control_message_t *m, const cog_orchestrator_event_t *event)
{
    if (m->lane != COG_CLI_LANE_COMMAND && m->lane != COG_CLI_LANE_CANCEL) return COG_ERROR_PROTOCOL;
    cli_forwarded_operation_t *op = m->lane == COG_CLI_LANE_CANCEL ? &slot->cancel : &slot->command;
    cli_control_lane_t *lane = &slot->outbound[(size_t)m->lane - 1U];
    if (!op->used || !op->forward_committed) return COG_ERROR_INVALID_STATE;
    cli_wire_reader_t reader = {m->payload, m->payload_len, 0U, true};
    cog_result_t outcome = cli_wire_signed(cli_wire_get(&reader, 4U));
    uint64_t state = cli_wire_get(&reader, 1U);
    if (!reader.valid || !cli_wire_result(outcome) || (state != 1U && state != 2U)) return COG_ERROR_PROTOCOL;
    if (m->kind == COG_CLI_CONTROL_COMMAND_RESULT) {
        if (!op->final || m->payload_len != 21U || m->reply_to != lane->pending_wire ||
            outcome != COG_OK || state != 2U || cli_wire_get(&reader, 8U) != 0U ||
            cli_wire_get(&reader, 8U) != 0U) return COG_ERROR_INVALID_STATE;
        lane->retired_through = lane->pending_wire; lane->pending_valid = false;
        return COG_OK;
    }
    if (m->payload_len != 5U || m->reply_to != op->executor_wire) return COG_ERROR_PROTOCOL;
    if (state == 1U) return COG_ERROR_AGAIN;
    if (op->final) {
        if (!host_bytes_equal(&op->reply, event->data, event->data_len)) return COG_ERROR_PROTOCOL;
        return lane->pending_valid ? host_lane_send(r, slot->worker, lane, &lane->pending, false) : COG_OK;
    }
    if (event->data_len > op->reply.capacity) return COG_ERROR_LIMIT;
    cog_result_t rc = host_remote_frame(r, slot, COG_CLI_CONTROL_COMMAND_ACK, m->lane, m->wire_id,
        NULL, 0U, &lane->pending, &lane->pending_wire);
    if (rc != COG_OK) return rc;
    memcpy(op->reply.data, event->data, event->data_len); op->reply.length = event->data_len;
    op->result = outcome; op->final = true; lane->pending_valid = true;
    return host_lane_send(r, slot->worker, lane, &lane->pending, false);
}

static cog_result_t host_remote_publication(cog_cli_host_runtime_t *r, cli_executor_slot_t *slot,
    const cog_cli_control_message_t *m, const cog_orchestrator_event_t *event)
{
    if (slot != &r->executors[r->primary_slot] || slot->agent != 0U || m->reply_to != 0U ||
        (m->kind == COG_CLI_CONTROL_EXECUTOR_VIEW ? m->lane != COG_CLI_LANE_VIEW :
            m->lane != COG_CLI_LANE_POLICY)) return COG_ERROR_INVALID_STATE;
    bool duplicate;
    cog_result_t rc = host_request_cache(slot, m, event, &duplicate);
    if (rc != COG_OK) return rc;
    cli_control_lane_t *lane = &slot->inbound[(size_t)m->lane - 1U];
    if (duplicate) return host_reply_send(r, slot->worker, lane, &lane->final_reply);
    cog_cli_projection_view_t view;
    rc = cog_cli_projection_snapshot(r->projection, &view); if (rc != COG_OK) return rc;
    cli_wire_reader_t reader = {m->payload, m->payload_len, 0U, true};
    if (m->kind == COG_CLI_CONTROL_EXECUTOR_VIEW) {
        uint64_t serial = cli_wire_get(&reader, 8U);
        cog_store_owner_t owner = {cli_wire_get(&reader, 8U), cli_wire_get(&reader, 8U)};
        uint64_t active = cli_wire_get(&reader, 8U), published = cli_wire_get(&reader, 8U);
        uint32_t attempts = (uint32_t)cli_wire_get(&reader, 4U);
        if (!reader.valid || serial == 0U || serial <= lane->provisional_prefix ||
            !host_owner_equal(owner, r->primary_owner) || active == 0U || published < active ||
            published != r->published_configuration || attempts > r->reload_attempts_used)
            return COG_ERROR_PROTOCOL;
        cog_cli_display_item_t items[CLI_NODES + 1U]; size_t required_items, required_text;
        rc = cog_cli_display_snapshot_decode(reader.bytes + reader.used, reader.length - reader.used,
            &view.display, items, CLI_NODES + 1U, (char *)r->projection_scratch.data,
            r->projection_scratch.capacity, &required_items, &required_text);
        if (rc != COG_OK) return rc;
        if (strcmp(view.display.session_id, slot->session) != 0) return COG_ERROR_PROTOCOL;
        view.active_configuration = active; view.published_configuration = published;
        uint64_t sequence;
        rc = cog_cli_projection_commit(r->projection, &view, &sequence); if (rc != COG_OK) return rc;
        r->active_configuration = active; lane->provisional_prefix = serial;
        unsigned char body[40]; cli_wire_writer_t writer = {body, 0U, true};
        cli_wire_put(&writer, serial, 8U); cli_wire_put(&writer, owner.epoch, 8U);
        cli_wire_put(&writer, owner.instance_id, 8U); cli_wire_put(&writer, r->runtime_incarnation, 8U);
        cli_wire_put(&writer, sequence, 8U);
        return host_request_finish(r, slot, m->lane, COG_CLI_CONTROL_VIEW_COMMIT, body, sizeof(body));
    }
    uint64_t present = cli_wire_get(&reader, 1U), serial = cli_wire_get(&reader, 8U);
    cog_cli_policy_view_t policy = {.session_id = slot->session};
    policy.assistant_node = cli_wire_get(&reader, 8U); policy.call_ordinal = (uint16_t)cli_wire_get(&reader, 2U);
    policy.configuration_generation = cli_wire_get(&reader, 8U); policy.deadline_ms = cli_wire_get(&reader, 8U);
    size_t tool_length; const char *tool = cli_wire_read_string(&reader, &tool_length);
    char name[256];
    if (!reader.valid || reader.used != reader.length || (present != 1U && present != 2U) ||
        serial == 0U || tool_length == 0U || tool_length >= sizeof(name) ||
        (present == 1U ? serial <= lane->provisional_prefix : serial != lane->provisional_prefix))
        return COG_ERROR_PROTOCOL;
    memcpy(name, tool, tool_length); name[tool_length] = '\0'; policy.tool_name = name;
    if (present == 1U) {
        if (policy.assistant_node == 0U || policy.call_ordinal >= 16U ||
            policy.configuration_generation != r->active_configuration || r->next_nonce > UINT64_MAX - 16U)
            return COG_ERROR_PROTOCOL;
        policy.nonce = r->next_nonce++; view.policies = &policy; view.policy_count = 1U;
    } else { view.policies = NULL; view.policy_count = 0U; }
    uint64_t sequence;
    rc = cog_cli_projection_commit(r->projection, &view, &sequence); if (rc != COG_OK) return rc;
    lane->provisional_prefix = serial;
    unsigned char body[21]; cli_wire_writer_t writer = {body, 0U, true};
    cli_wire_put(&writer, 0U, 4U); cli_wire_put(&writer, 2U, 1U);
    cli_wire_put(&writer, 0U, 8U); cli_wire_put(&writer, 0U, 8U);
    return host_request_finish(r, slot, m->lane, COG_CLI_CONTROL_COMMAND_RESULT, body, sizeof(body));
}

static void host_group_wire(cog_cli_host_runtime_t *r, const cli_group_record_t *group,
    cli_wire_writer_t *writer)
{
    cli_wire_put(writer, group->id, 8U); cli_wire_put(writer, (uint32_t)group->aggregate, 4U);
    cli_wire_put(writer, group->submitted, 2U);
    for (size_t i = 0U; i < group->submitted; ++i) {
        const cli_group_member_t *member = &group->members[i];
        cli_wire_put(writer, member->present ? 1U : 0U, 1U);
        if (!member->present) { cli_wire_put(writer, (uint32_t)member->outcome, 4U); continue; }
        const cli_executor_slot_t *child = &r->executors[member->slot];
        cli_wire_put(writer, member->agent, 8U); cli_wire_put(writer, member->attempt, 8U);
        cli_wire_put(writer, (uint64_t)member->status, 1U);
        cli_wire_put(writer, (uint32_t)member->outcome, 4U);
        cli_wire_string(writer, child->session, strlen(child->session));
        cli_wire_string(writer, (const char *)child->result.data, child->result.length);
        cli_wire_put(writer, member->effects_uncertain ? 1U : 0U, 1U);
    }
}
static cog_result_t host_remote_delegate(cog_cli_host_runtime_t *r, cli_executor_slot_t *slot,
    const cog_cli_control_message_t *m, const cog_orchestrator_event_t *event)
{
    if (slot != &r->executors[r->primary_slot] || slot->agent != 0U || m->reply_to != 0U ||
        (m->lane != COG_CLI_LANE_DELEGATE && m->lane != COG_CLI_LANE_CANCEL)) return COG_ERROR_INVALID_STATE;
    cli_control_lane_t *lane = &slot->inbound[(size_t)m->lane - 1U];
    cli_wire_reader_t reader = {m->payload, m->payload_len, 0U, true};
    if (m->kind == COG_CLI_CONTROL_DELEGATE_RESULT_ACK) {
        if (m->lane != COG_CLI_LANE_DELEGATE || m->payload_len != 16U) return COG_ERROR_PROTOCOL;
        uint64_t id = cli_wire_get(&reader, 8U), checkpoint = cli_wire_get(&reader, 8U);
        cli_group_record_t *group = NULL;
        for (size_t i = 0U; i < r->group_capacity; ++i)
            if (r->groups[i].phase != CLI_GROUP_FREE && r->groups[i].id == id) group = &r->groups[i];
        if (group == NULL || !host_owner_equal(group->parent_owner, slot->owner) ||
            strcmp(group->parent_session, m->session_id) != 0 || checkpoint <= group->started_checkpoint.generation)
            return COG_ERROR_INVALID_STATE;
        if (group->phase == CLI_GROUP_SAVED)
            return group->result_checkpoint.generation == checkpoint ? COG_OK : COG_ERROR_PROTOCOL;
        if (group->phase != CLI_GROUP_RESULT || !lane->final_valid ||
            lane->final_answered_wire != group->request_wire || m->wire_id <= lane->final_answered_wire)
            return COG_ERROR_INVALID_STATE;
        group->result_checkpoint.generation = checkpoint; group->result_acknowledged = true;
        group->phase = CLI_GROUP_SAVED; lane->retired_through = m->wire_id;
        lane->final_valid = false; lane->pending_reply_wire = 0U;
        lane->final_original.length = 0U; lane->final_reply.length = 0U;
        return COG_OK;
    }
    bool duplicate;
    cog_result_t rc = host_request_cache(slot, m, event, &duplicate);
    if (rc != COG_OK) return rc;
    if (duplicate) return host_reply_send(r, slot->worker, lane, &lane->final_reply);
    if (m->kind == COG_CLI_CONTROL_DELEGATE_RESERVATION_RELEASE) {
        if (m->lane != COG_CLI_LANE_CANCEL || m->payload_len != 8U) return COG_ERROR_PROTOCOL;
        uint64_t reservation = cli_wire_get(&reader, 8U);
        cli_group_record_t *group = host_group(r, reservation);
        if (group == NULL || !host_owner_equal(group->parent_owner, slot->owner) ||
            strcmp(group->parent_session, m->session_id) != 0) return COG_ERROR_INVALID_STATE;
        rc = cli_runtime_release(r, reservation);
        unsigned char body[21]; cli_wire_writer_t writer = {body, 0U, true};
        cli_wire_put(&writer, (uint32_t)rc, 4U); cli_wire_put(&writer, 2U, 1U);
        cli_wire_put(&writer, 0U, 8U); cli_wire_put(&writer, 0U, 8U);
        return host_request_finish(r, slot, m->lane, COG_CLI_CONTROL_COMMAND_RESULT, body, sizeof(body));
    }
    if (m->lane != COG_CLI_LANE_DELEGATE ||
        (m->kind != COG_CLI_CONTROL_DELEGATE_RESERVE && m->kind != COG_CLI_CONTROL_DELEGATE_START))
        return COG_ERROR_PROTOCOL;
    bool start = m->kind == COG_CLI_CONTROL_DELEGATE_START;
    cli_group_request_t request = {0};
    request.owner.epoch = cli_wire_get(&reader, 8U); request.owner.instance_id = cli_wire_get(&reader, 8U);
    uint64_t reservation = start ? cli_wire_get(&reader, 8U) : 0U;
    request.pinned_configuration = cli_wire_get(&reader, 8U);
    cog_store_checkpoint_t checkpoint = {start ? cli_wire_get(&reader, 8U) : 0U};
    request.parent_node = cli_wire_get(&reader, 8U); request.ordinal = (uint16_t)cli_wire_get(&reader, 2U);
    size_t call_length; const char *call = cli_wire_read_string(&reader, &call_length);
    char call_id[256];
    if (!reader.valid || call_length == 0U || call_length >= sizeof(call_id)) return COG_ERROR_PROTOCOL;
    memcpy(call_id, call, call_length); call_id[call_length] = '\0';
    request.call_id = call_id; request.parent_session = m->session_id;
    request.deadline_ms = cli_wire_get(&reader, 8U); request.count = (size_t)cli_wire_get(&reader, 2U);
    const char *tasks[CLI_HOST_MAX_CHILDREN]; size_t lengths[CLI_HOST_MAX_CHILDREN];
    if (!reader.valid || request.count == 0U || request.count > CLI_HOST_MAX_CHILDREN) return COG_ERROR_PROTOCOL;
    for (size_t i = 0U; i < request.count; ++i) tasks[i] = cli_wire_read_string(&reader, &lengths[i]);
    request.tasks = tasks; request.task_lengths = lengths;
    if (!reader.valid || reader.used != reader.length || !host_owner_equal(request.owner, slot->owner))
        return COG_ERROR_PROTOCOL;
    if (!start) {
        rc = cli_runtime_reserve(r, &request, &reservation);
        unsigned char body[20]; cli_wire_writer_t writer = {body, 0U, true};
        cli_wire_put(&writer, (uint32_t)rc, 4U); cli_wire_put(&writer, rc == COG_OK ? reservation : 0U, 8U);
        cli_wire_put(&writer, request.deadline_ms, 8U);
        return host_request_finish(r, slot, m->lane, COG_CLI_CONTROL_DELEGATE_RESERVATION, body, sizeof(body));
    }
    cli_group_record_t *group = host_group(r, reservation);
    if (group == NULL || !host_same_request(group, &request) || group->parent_node != request.parent_node ||
        group->ordinal != request.ordinal || strcmp(group->call_id, call_id) != 0 ||
        strcmp(group->parent_session, m->session_id) != 0) return COG_ERROR_PROTOCOL;
    group->request_wire = m->wire_id;
    rc = cli_runtime_start(r, reservation, checkpoint);
    if (rc != COG_OK && rc != COG_ERROR_AGAIN && group->phase != CLI_GROUP_RESULT) return rc;
    if (group->phase != CLI_GROUP_RESULT) return COG_ERROR_AGAIN;
    cli_wire_writer_t count = {NULL, 0U, true}; host_group_wire(r, group, &count);
    if (!count.valid || count.used > r->projection_scratch.capacity) return COG_ERROR_LIMIT;
    cli_wire_writer_t writer = {r->projection_scratch.data, 0U, true}; host_group_wire(r, group, &writer);
    return host_request_finish(r, slot, m->lane, COG_CLI_CONTROL_DELEGATE_RESULT, writer.bytes, writer.used);
}

static cog_result_t host_worker_message(cog_cli_host_runtime_t *r, const cog_orchestrator_event_t *event)
{
    cli_worker_record_t *worker = NULL;
    for (size_t i = 0U; i < r->bounds.workers; ++i)
        if (r->workers[i].used && host_worker_equal(r->workers[i].id, event->worker_id)) worker = &r->workers[i];
    if (worker == NULL || event->peer_id == 0U || event->peer_id != worker->native.peer_id ||
        event->peer_info.peer_id != event->peer_id || event->peer_info.user_id != (uint64_t)geteuid() ||
        worker->native.pid <= 0 || event->peer_info.process_id != (uint64_t)worker->native.pid)
        return COG_ERROR_DENIED;
    cog_cli_control_message_t m;
    size_t required;
    cog_result_t rc = cog_cli_control_decode(event->data, event->data_len, &m,
        r->decode.data, r->decode.capacity, &required);
    if (rc != COG_OK) return rc;
    if (!host_worker_equal(m.worker, worker->id) || m.runtime_incarnation != r->runtime_incarnation ||
        m.sender_incarnation != worker->id.incarnation || m.owner_peer != event->peer_id)
        return COG_ERROR_DENIED;
    if (m.kind == COG_CLI_CONTROL_BOOTSTRAP_ACK) return host_bootstrap_ack(r, worker, &m);
    if (m.kind == COG_CLI_CONTROL_HANDOFF) {
        cog_cli_handoff_t *h = r->handoff;
        if (h == NULL || h->pending.length == 0U || m.lane != COG_CLI_LANE_HANDOFF ||
            m.agent_id != 0U || strcmp(m.session_id, r->primary_session) != 0) return COG_ERROR_INVALID_STATE;
        cog_cli_control_message_t request;
        rc = cog_cli_control_decode(h->pending.data, h->pending.length, &request,
            r->projection_scratch.data, r->projection_scratch.capacity, &required);
        if (rc != COG_OK) return rc;
        if (m.reply_to != request.wire_id || !host_worker_equal(m.worker, request.worker)) return COG_ERROR_INVALID_STATE;
        if (event->data_len > h->reply.capacity) return COG_ERROR_LIMIT;
        if (h->reply.length != 0U && !host_bytes_equal(&h->reply, event->data, event->data_len)) return COG_ERROR_PROTOCOL;
        memcpy(h->reply.data, event->data, event->data_len); h->reply.length = event->data_len;
        return COG_OK;
    }
    cli_executor_slot_t *slot = m.agent_id == 0U ? &r->executors[r->primary_slot] : host_agent(r, m.agent_id);
    if (slot == NULL || slot->attempt != m.execution_epoch || slot->owner_peer != event->peer_id ||
        !host_worker_equal(slot->worker, worker->id) || strcmp(slot->session, m.session_id) != 0)
        return COG_ERROR_INVALID_STATE;
    if (m.kind == COG_CLI_CONTROL_EXECUTOR_COMMAND_RESULT ||
        (m.kind == COG_CLI_CONTROL_COMMAND_RESULT && (m.lane == COG_CLI_LANE_COMMAND || m.lane == COG_CLI_LANE_CANCEL)))
        return host_remote_command_result(r, slot, &m, event);
    if (m.kind == COG_CLI_CONTROL_EXECUTOR_VIEW || m.kind == COG_CLI_CONTROL_POLICY_STATE)
        return host_remote_publication(r, slot, &m, event);
    if (m.kind == COG_CLI_CONTROL_DELEGATE_RESERVE || m.kind == COG_CLI_CONTROL_DELEGATE_START ||
        m.kind == COG_CLI_CONTROL_DELEGATE_RESULT_ACK || m.kind == COG_CLI_CONTROL_DELEGATE_RESERVATION_RELEASE)
        return host_remote_delegate(r, slot, &m, event);
    if (m.kind == COG_CLI_CONTROL_CHILD_UPDATE) return host_remote_update(r, slot, &m, event);
    if (m.kind == COG_CLI_CONTROL_CHILD_MESSAGE || m.kind == COG_CLI_CONTROL_MAILBOX_POLL)
        return host_remote_mailbox(r, slot, &m, event);
    if (m.kind == COG_CLI_CONTROL_COMMAND_ACK && m.lane == COG_CLI_LANE_UPDATE) {
        cli_control_lane_t *lane = &slot->inbound[COG_CLI_LANE_UPDATE - 1U];
        if (m.payload_len != 0U || !lane->final_valid || m.reply_to != lane->final_reply_wire ||
            m.wire_id <= lane->final_answered_wire || !slot->terminal_copied ||
            slot->receipt.accepted_terminal_status < COG_AGENT_STATUS_COMPLETED ||
            (slot->receipt.disposition != COG_CLI_CONTROL_RECEIPT_ACCEPTED &&
             slot->receipt.disposition != COG_CLI_CONTROL_RECEIPT_TERMINAL_FIXED))
            return COG_ERROR_INVALID_STATE;
        /* This one-way proof is distinct from local core ACK and parent save.
         * It certifies actual worker cleanup under the trusted current binding. */
        lane->retired_through = m.wire_id; lane->final_valid = false; lane->pending_reply_wire = 0U;
        lane->final_original.length = 0U; lane->final_reply.length = 0U;
        return COG_OK;
    }
    if (m.kind == COG_CLI_CONTROL_CHILD_START_ACK) {
        cli_control_lane_t *lane = &slot->outbound[COG_CLI_LANE_DELEGATE - 1U];
        if (m.lane != COG_CLI_LANE_DELEGATE || m.payload_len != 4U ||
            m.reply_to != lane->pending_wire) return COG_ERROR_PROTOCOL;
        cli_wire_reader_t reader = {m.payload, m.payload_len, 0U, true};
        rc = cli_wire_signed(cli_wire_get(&reader, 4U));
        if (!cli_wire_result(rc)) return COG_ERROR_PROTOCOL;
        if (rc != COG_OK) { slot->primary = rc; slot->cancel_latched = true; }
        else { slot->started = true; slot->phase = CLI_SLOT_STARTED; }
        lane->pending_valid = false;
        return COG_OK;
    }
    return COG_ERROR_PROTOCOL;
}

static cog_result_t host_worker_slice(cog_cli_host_runtime_t *r, bool *done, uint64_t *deadline)
{
    if (r->orchestrator == NULL) { *done = true; *deadline = UINT64_MAX; return COG_OK; }
    cog_result_t primary = cog_orchestrator_service(r->orchestrator, r->now_ms, done, deadline);
    cog_core_info_t native[CLI_HOST_MAX_WORKERS];
    size_t count;
    cog_result_t rc = cog_orchestrator_list(r->orchestrator, native, CLI_HOST_MAX_WORKERS, &count);
    if (rc != COG_OK) return host_first(primary, rc);
    for (size_t i = 0U; i < count; ++i)
        for (size_t j = 0U; j < r->bounds.workers; ++j)
            if (host_worker_equal(native[i].worker_id, r->workers[j].id)) r->workers[j].native = native[i];
    for (unsigned int which = 0U; which < 2U; ++which) {
        cog_orchestrator_event_t event;
        size_t required;
        rc = cog_orchestrator_copy_event(r->orchestrator,
            which == 0U ? COG_ORCHESTRATOR_CONTROL_EVENTS : COG_ORCHESTRATOR_MESSAGE_EVENTS,
            &event, r->ingress.data, r->ingress.capacity, &required);
        if (rc == COG_ERROR_AGAIN || rc == COG_ERROR_NOT_FOUND) continue;
        if (rc != COG_OK) return host_first(primary, rc);
        if (event.kind == COG_ORCHESTRATOR_MESSAGE) rc = host_worker_message(r, &event);
        else if ((event.kind == COG_ORCHESTRATOR_DISCONNECTED &&
            (!r->shutdown || (event.outcome != COG_OK && event.outcome != COG_ERROR_IO))) ||
            event.kind == COG_ORCHESTRATOR_OWNERSHIP_LOST ||
            (event.kind == COG_ORCHESTRATOR_REAPED && !r->shutdown)) {
            rc = event.outcome == COG_OK ? COG_ERROR_IO : event.outcome;
            for (size_t i = 2U; i < r->bounds.executors; ++i) {
                cli_executor_slot_t *slot = &r->executors[i];
                if (slot->phase != CLI_SLOT_FREE && host_worker_equal(slot->worker, event.worker_id)) {
                    slot->cancel_latched = true; slot->effects_uncertain = slot->first_admission_sent;
                    slot->primary = slot->first_admission_sent ? COG_ERROR_INDETERMINATE : COG_ERROR_ABORTED;
                }
            }
        } else rc = COG_OK;
        if (rc != COG_OK && rc != COG_ERROR_AGAIN && rc != COG_ERROR_INVALID_STATE)
            primary = host_first(primary, rc);
        cog_result_t ack = cog_orchestrator_ack_event(r->orchestrator, event.event_id);
        primary = host_first(primary, ack);
    }
    return primary;
}

static void host_fail(cog_cli_host_runtime_t *r, cog_result_t result)
{
    if (result == COG_OK || result == COG_ERROR_AGAIN) return;
    r->primary = host_first(r->primary, result);
    (void)cog_cli_host_runtime_request_shutdown(r);
}
static bool host_handoff_owns(const cog_cli_host_runtime_t *r, size_t index)
{
    const cog_cli_handoff_t *h = r->handoff;
    if (h == NULL || h->phase == CLI_HANDOFF_IDLE) return false;
    if (index == h->target_slot && h->activation_acked && !h->abort_requested) return false;
    return index == h->source_slot || index == h->target_slot;
}
static void host_send_due(cog_cli_host_runtime_t *r, cog_cli_host_runtime_step_t *out,
    uint64_t wire, uint64_t copied, uint64_t next, bool one_way)
{
    if (wire == 0U || (one_way && wire == copied)) return;
    uint64_t due = wire == copied ? next : r->now_ms;
    if (due < out->next_deadline_ms) out->next_deadline_ms = due;
}
static cog_result_t host_retained_sends(cog_cli_host_runtime_t *r, cog_cli_host_runtime_step_t *out)
{
    for (size_t i = 0U; i < r->bounds.workers; ++i) {
        cli_secret_exchange_t *profile = &r->workers[i].startup_profile;
        if (profile->pending && !profile->acknowledged)
            host_send_due(r, out, profile->wire, profile->copied_wire, profile->next_send_ms, false);
    }
    for (size_t i = 0U; i < r->bounds.executors; ++i) {
        cli_executor_slot_t *slot = &r->executors[i];
        if (slot->phase == CLI_SLOT_FREE || slot->owner_peer == 0U) continue;
        cli_secret_exchange_t *profile = &slot->profile;
        if (profile->pending && !profile->acknowledged)
            host_send_due(r, out, profile->wire, profile->copied_wire, profile->next_send_ms, false);
        for (size_t j = 0U; j < CLI_HOST_LANES; ++j) {
            cli_control_lane_t *lane = &slot->inbound[j];
            uint64_t wire = lane->pending_reply_wire;
            if (wire != 0U) {
                const cli_host_bytes_t *frame = NULL;
                if (lane->final_valid && wire == lane->final_reply_wire) frame = &lane->final_reply;
                else if (wire == lane->provisional_wire) frame = &lane->provisional_reply;
                if (frame == NULL) return COG_ERROR_INVALID_STATE;
                cog_result_t rc = host_reply_send(r, slot->worker, lane, frame);
                if (rc != COG_OK && rc != COG_ERROR_AGAIN) return rc;
                if (lane->pending_reply_wire != 0U)
                    host_send_due(r, out, wire, lane->copied_wire, lane->next_send_ms, false);
            }
            lane = &slot->outbound[j];
            if (lane->pending_valid) {
                bool one_way = j == COG_CLI_LANE_CANCEL - 1U && slot->agent != 0U;
                host_send_due(r, out, lane->pending_wire, lane->copied_wire, lane->next_send_ms, one_way);
            }
        }
    }
    return COG_OK;
}
static void host_step_merge(cog_cli_host_runtime_t *r, cog_cli_host_runtime_step_t *out,
    const cog_core_step_info_t *info)
{
    if (info->dispatched > SIZE_MAX - out->dispatched) host_fail(r, COG_ERROR_LIMIT);
    else out->dispatched += info->dispatched;
    if (info->next_deadline_ms < out->next_deadline_ms) out->next_deadline_ms = info->next_deadline_ms;
    if (info->max_wait_ms < out->max_wait_ms) out->max_wait_ms = info->max_wait_ms;
}

/** One fair bounded offer to each eligible owner; no nested run or wait. */
cog_result_t cog_cli_host_runtime_step(cog_cli_host_runtime_t *r,
    cog_cli_host_runtime_step_t *info)
{
    if (r == NULL || info == NULL) return COG_ERROR_NULL_PTR;
    if (r->in_step || (!r->begun && !(r->shutdown && r->done))) return COG_ERROR_INVALID_STATE;
    cog_cli_host_runtime_step_t out = {.done = r->done, .next_deadline_ms = UINT64_MAX, .max_wait_ms = 10U};
    if (r->done) { *info = out; return r->primary; }
    r->in_step = true;
    uint64_t now;
    cog_result_t rc = host_clock(&now);
    if (rc == COG_OK) r->now_ms = now; else host_fail(r, rc);
    if (r->fixed.signal_fd >= 0) {
        unsigned char signal_record[128];
        ssize_t amount = read(r->fixed.signal_fd, signal_record, sizeof(signal_record));
        if (amount == (ssize_t)sizeof(signal_record)) (void)cog_cli_host_runtime_request_shutdown(r);
        else if (amount > 0 || (amount < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR))
            host_fail(r, COG_ERROR_IO);
    }
    bool native_done;
    uint64_t native_deadline;
    rc = host_worker_slice(r, &native_done, &native_deadline);
    host_fail(r, rc);
    if (native_deadline < out.next_deadline_ms) out.next_deadline_ms = native_deadline;
    if (!r->control_drive_ended) {
        if (r->shutdown && r->now_ms >= r->shutdown_deadline_ms) cog_core_stop(r->control_core);
        cog_core_step_info_t core_info = {0};
        rc = cog_core_step(r->control_core, &core_info);
        host_step_merge(r, &out, &core_info);
        if (core_info.done) {
            cog_result_t ended = cog_core_drive_end(r->control_core);
            r->control_drive_ended = true;
            if (rc != COG_OK) r->coordinator_failed = true;
            host_fail(r, host_first(rc, ended));
        } else host_fail(r, rc);
    }
    bool ready = true;
    for (size_t i = 0U; i < r->bounds.workers; ++i) {
        cli_worker_record_t *worker = &r->workers[i];
        if (!worker->profile_ready) {
            ready = false;
            if (!r->shutdown && worker->native.health == COG_HEALTH_HEALTHY) {
                rc = host_profile_send(r, worker, NULL, &worker->startup_profile, 1U, NULL);
                host_fail(r, rc); out.max_wait_ms = 0U;
            }
        }
    }
    if (r->handoff != NULL) {
        bool handoff_done;
        uint64_t deadline;
        rc = cog_cli_handoff_step(r->handoff, &handoff_done, &deadline);
        host_fail(r, rc);
        if (deadline < out.next_deadline_ms) out.next_deadline_ms = deadline;
        cog_cli_handoff_t *h = r->handoff;
        if (!handoff_done) out.max_wait_ms = 0U;
        if (h->pending.length != 0U) {
            cog_cli_control_message_t request;
            size_t required;
            rc = cog_cli_control_decode(h->pending.data, h->pending.length, &request,
                r->decode.data, r->decode.capacity, &required);
            if (rc == COG_OK) {
                cli_executor_slot_t *target = &r->executors[h->target_slot];
                cli_executor_slot_t *source = &r->executors[h->source_slot];
                cli_executor_slot_t *addressed = host_worker_equal(request.worker, target->worker) &&
                    request.owner_peer == target->owner_peer ? target : source;
                cli_control_lane_t *lane = &addressed->outbound[COG_CLI_LANE_HANDOFF - 1U];
                if (!host_worker_equal(request.worker, addressed->worker) ||
                    request.owner_peer != addressed->owner_peer || request.wire_id != lane->pending_wire)
                    rc = COG_ERROR_INVALID_STATE;
                else rc = host_lane_send(r, request.worker, lane, &h->pending, false);
            }
            host_fail(r, rc);
        }
        if (h->phase == CLI_HANDOFF_PREPARE && h->target_peer != 0U) {
            cli_executor_slot_t *target = &r->executors[h->target_slot];
            for (size_t i = 0U; i < r->bounds.workers; ++i)
                if (host_worker_equal(target->worker, r->workers[i].id) && target->profile.source != NULL) {
                    rc = host_profile_send(r, &r->workers[i], target, &target->profile, 3U, NULL);
                    host_fail(r, rc);
                }
        }
    }
    for (size_t n = 0U; n < r->bounds.executors; ++n) {
        size_t index = (r->round_robin + n) % r->bounds.executors;
        cli_executor_slot_t *slot = &r->executors[index];
        if (slot->phase == CLI_SLOT_FREE || host_handoff_owns(r, index)) continue;
        bool primary = index == r->primary_slot;
        if (primary && slot->app != NULL && !slot->started && !r->shutdown && ready) {
            rc = cog_cli_execution_begin(slot->app);
            if (rc == COG_OK) { slot->started = true; slot->phase = CLI_SLOT_STARTED; }
            host_fail(r, rc); out.max_wait_ms = 0U;
        }
        if (!primary && slot->owner_peer != 0U) {
            bool ended = false;
            for (size_t i = 0U; i < r->bounds.workers; ++i)
                if (host_worker_equal(slot->worker, r->workers[i].id))
                    ended = r->workers[i].native.reaped || r->workers[i].native.ownership_lost;
            if (ended && !slot->terminal_copied) {
                slot->local_done = true;
                slot->primary = slot->first_admission_sent ? COG_ERROR_INDETERMINATE : COG_ERROR_ABORTED;
                rc = host_local_terminal(r, slot);
                if (rc == COG_OK) slot->phase = CLI_SLOT_RETAINED;
                host_fail(r, rc);
            } else if (!slot->terminal_copied) host_fail(r, host_remote_child_slice(r, slot));
            continue;
        }
        if (!primary && slot->phase == CLI_SLOT_PROFILE && !slot->cancel_latched && !r->shutdown) {
            rc = host_local_admit(r, slot);
            if (rc != COG_OK && rc != COG_ERROR_AGAIN) {
                slot->primary = rc; slot->cancel_latched = true; slot->local_done = true;
                cli_group_record_t *group = host_slot_group(r, slot);
                if (group != NULL) { group->aggregate = rc; group->phase = CLI_GROUP_ROLLBACK; }
            }
            out.max_wait_ms = 0U;
            continue;
        }
        if (slot->app != NULL && slot->started && !slot->local_done) {
            if (slot->cancel_latched || r->shutdown) cog_cli_app_stop(slot->app);
            if (primary) host_fail(r, host_project_local(r, slot));
            else if (slot->accepted_sequence == 0U && !slot->cancel_latched)
                host_fail(r, host_local_update(r, slot, COG_AGENT_STATUS_RUNNING, COG_OK, "running", 7U));
            cog_core_step_info_t execution_info = {0};
            rc = cog_cli_execution_step(slot->app, &execution_info);
            host_step_merge(r, &out, &execution_info);
            if (rc != COG_OK && rc != COG_ERROR_AGAIN) {
                slot->primary = host_first(slot->primary, rc); slot->cancel_latched = true;
                if (primary) host_fail(r, rc);
            }
            if (execution_info.done) {
                slot->local_done = true;
                slot->primary = host_first(slot->primary, cog_cli_execution_end(slot->app));
            }
            if (primary) host_fail(r, host_project_local(r, slot));
        }
        if (!primary && !slot->started && slot->cancel_latched) slot->local_done = true;
        if (!primary && slot->local_done && !slot->terminal_copied) {
            rc = host_local_terminal(r, slot); host_fail(r, rc); out.max_wait_ms = 0U;
        } else if (!primary && slot->terminal_copied && slot->phase != CLI_SLOT_RETAINED) {
            host_fail(r, host_local_dispose(r, slot)); out.max_wait_ms = 0U;
        }
    }
    r->round_robin = (r->round_robin + 1U) % r->bounds.executors;
    for (size_t i = 0U; i < r->group_capacity; ++i) {
        rc = host_group_slice(r, &r->groups[i]);
        if (rc != COG_ERROR_ABORTED && rc != COG_ERROR_TIMEOUT) host_fail(r, rc);
    }
    if (r->executors[r->primary_slot].app == NULL) {
        host_fail(r, host_project_remote_rows(r));
    }
    host_fail(r, host_forwarded_slice(r, &r->executors[r->primary_slot]));
    host_mirror_retire(r);
    if (!r->control_drive_ended) {
        for (size_t i = 0U; i < r->bounds.command_records; ++i) host_fail(r, host_command_slice(r, &r->commands[i]));
        for (size_t i = 0U; i < r->bounds.clients; ++i) host_fail(r, host_client_slice(r, &r->clients[i]));
    }
    if (r->shutdown) {
        bool local_done = true;
        for (size_t i = 0U; i < r->bounds.executors; ++i) {
            cli_executor_slot_t *slot = &r->executors[i];
            if (slot->app != NULL && slot->started && !slot->local_done) local_done = false;
            if (slot->borrow_live) local_done = false;
        }
        bool handoff_done = r->handoff == NULL || r->handoff->phase == CLI_HANDOFF_IDLE ||
            (r->handoff->phase == CLI_HANDOFF_FROZEN && r->handoff->pending.length == 0U);
        /* With live UI leases, keep the established deadline drain so queued
         * transport bytes and protected final/ACK receipts remain serviceable. */
        bool ui_drained = true;
        for (size_t i = 0U; i < r->bounds.clients; ++i)
            if (r->clients[i].used) ui_drained = false;
        for (size_t i = 0U; i < r->bounds.command_records; ++i)
            if (r->commands[i].phase != CLI_COMMAND_FREE) ui_drained = false;
        /* The deadline is a fallback, not a minimum lifetime without UI owners. */
        if (local_done && native_done && handoff_done && ui_drained && !r->control_drive_ended)
            cog_core_stop(r->control_core);
        r->done = local_done && native_done && r->control_drive_ended && handoff_done;
        if (r->shutdown_deadline_ms < out.next_deadline_ms) out.next_deadline_ms = r->shutdown_deadline_ms;
    }
    host_fail(r, host_retained_sends(r, &out));
    if (out.next_deadline_ms <= r->now_ms) out.max_wait_ms = 0U;
    out.done = r->done; r->in_step = false; *info = out;
    return r->primary;
}

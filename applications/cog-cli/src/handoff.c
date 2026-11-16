/** @file handoff.c @brief One finite, correlated, same-host ownership move. */
#define _POSIX_C_SOURCE 200809L
#include "composition_internal.h"
#include "app_internal.h"
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define HANDOFF_BYTES 512U
#define HANDOFF_IDS 16U

static bool handoff_worker_equal(cog_worker_id_t a, cog_worker_id_t b)
{ return a.incarnation == b.incarnation && a.number == b.number; }
static bool handoff_owner_equal(cog_store_owner_t a, cog_store_owner_t b)
{ return a.epoch == b.epoch && a.instance_id == b.instance_id; }
static cog_result_t handoff_first(cog_result_t a, cog_result_t b)
{ return a == COG_OK ? b : a; }
static cli_worker_record_t *handoff_worker(cog_cli_handoff_t *h, cog_worker_id_t id)
{
    for (size_t i = 0U; i < h->runtime->bounds.workers; ++i)
        if (h->runtime->workers[i].used && handoff_worker_equal(h->runtime->workers[i].id, id))
            return &h->runtime->workers[i];
    return NULL;
}
static bool handoff_lane_key(const cli_control_lane_t *lane,
    const cog_cli_host_runtime_t *r, cog_worker_id_t worker, cog_peer_id_t peer)
{
    return !lane->bound || (lane->key.fabric == CLI_FABRIC_WORKER &&
        lane->key.runtime == r->runtime_incarnation && lane->key.sender == r->runtime_incarnation &&
        handoff_worker_equal(lane->key.worker, worker) && lane->key.owner_peer == peer &&
        lane->key.agent == 0U && lane->key.attempt == 0U && lane->key.lane == COG_CLI_LANE_HANDOFF &&
        strcmp(lane->key.session, r->primary_session) == 0);
}
static cog_result_t handoff_lane_ready(const cli_executor_slot_t *s,
    const cog_cli_host_runtime_t *r, cog_worker_id_t worker, cog_peer_id_t peer)
{
    if (peer == 0U) return COG_OK;
    if (s->outbound == NULL) return COG_ERROR_INVALID_STATE;
    const cli_control_lane_t *lane = &s->outbound[COG_CLI_LANE_HANDOFF - 1U];
    if (!handoff_lane_key(lane, r, worker, peer) || lane->pending_valid) return COG_ERROR_AGAIN;
    if (lane->pending.capacity < HANDOFF_BYTES || lane->final_original.capacity < HANDOFF_BYTES ||
        lane->final_reply.capacity < HANDOFF_BYTES) return COG_ERROR_LIMIT;
    return COG_OK;
}
static cog_result_t handoff_random(uint64_t *out)
{
    uint64_t value = 0U;
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return COG_ERROR_IO;
    ssize_t bytes;
    do { bytes = read(fd, &value, sizeof(value)); } while (bytes < 0 && errno == EINTR);
    int ended = close(fd);
    if (bytes != (ssize_t)sizeof(value) || ended != 0 || value == 0U) return COG_ERROR_IO;
    *out = value;
    return COG_OK;
}
static void handoff_secret_clear(cli_secret_exchange_t *p)
{
    volatile unsigned char *areas[] = {p->body, p->frame, p->scratch};
    const size_t capacities[] = {p->body_capacity, p->frame_capacity, p->scratch_capacity};
    for (size_t i = 0U; i < 3U; ++i)
        for (size_t j = 0U; j < capacities[i]; ++j) areas[i][j] = 0U;
    p->body_length = 0U; p->frame_length = 0U;
    p->pending = false; p->acknowledged = false; p->source = NULL;
}
static cog_result_t handoff_transport_close(cli_executor_slot_t *s, cog_result_t rc)
{
    cog_cli_configuration_free(s->pending_profile); s->pending_profile = NULL;
    if (s->net != NULL) {
        if (s->observer != NULL) rc = handoff_first(rc, cog_cli_observer_clear(s->observer, s->net));
        cog_net_free(s->net); s->net = NULL;
    }
    cog_cli_observer_free(s->observer); s->observer = NULL;
    return rc;
}
static cog_result_t handoff_local_reserve(cog_cli_handoff_t *h, cli_executor_slot_t *s)
{
    const cog_cli_options_t *options = cog_cli_configuration_options(h->runtime->current_mirror);
    if (options == NULL) return COG_ERROR_INVALID_STATE;
    cog_result_t rc = COG_OK;
    if (options->backend == COG_CLI_BACKEND_REAL) {
        rc = cog_cli_observer_new(&s->observer, h->runtime->net_template.max_connections);
        if (rc != COG_OK) return rc;
        cog_net_config_t config = h->runtime->net_template;
        config.observer = cog_cli_observer_capture; config.observer_user = s->observer;
        rc = cog_net_new(&s->net, &config);
        if (rc != COG_OK) return handoff_transport_close(s, rc);
    }
    rc = cog_cli_configuration_clone_new(&s->pending_profile, h->runtime->current_mirror, s->net);
    if (rc != COG_OK) return handoff_transport_close(s, rc);
    return COG_OK;
}

cog_result_t cog_cli_handoff_new(cog_cli_handoff_t **out,
    cog_cli_host_runtime_t *runtime, cog_orchestrator_t *orchestrator,
    cog_store_t *catalog, uint32_t timeout_ms)
{
    if (out == NULL) return COG_ERROR_NULL_PTR;
    *out = NULL;
    if (runtime == NULL || orchestrator == NULL || catalog == NULL) return COG_ERROR_NULL_PTR;
    if (timeout_ms == 0U) return COG_ERROR_INVALID_ARG;
    cog_cli_handoff_t *h = calloc(1U, sizeof(*h));
    if (h == NULL) return COG_ERROR_NOMEM;
    h->pending.data = malloc(HANDOFF_BYTES); h->reply.data = malloc(HANDOFF_BYTES);
    if (h->pending.data == NULL || h->reply.data == NULL) {
        free(h->pending.data); free(h->reply.data); free(h); return COG_ERROR_NOMEM;
    }
    h->pending.capacity = HANDOFF_BYTES; h->reply.capacity = HANDOFF_BYTES;
    h->runtime = runtime; h->orchestrator = orchestrator; h->catalog = catalog;
    h->timeout_ms = timeout_ms; *out = h;
    return COG_OK;
}

/* Mirror the executor's no-effect quiescence preflight before reserving a
 * target. The execution operation remains the final authority and may refuse. */
static cog_result_t handoff_local_quiet(const cog_cli_app_t *app)
{
    if (app == NULL || app->headless == NULL || app->headless->in_step ||
        app->headless->authority != CLI_AUTH_NORMAL || app->headless->drive != CLI_DRIVE_BEGUN ||
        app->shutdown || app->guard == NULL)
        return COG_ERROR_INVALID_STATE;
    if (app->active || app->provider_active || app->execution != NULL || app->delta_ready ||
        app->event_queued || app->assistant_pending || app->terminal_result_pending ||
        app->resuming_policy || app->recovery || app->future_configuration != NULL ||
        app->retiring_configuration != NULL || app->reload_notice_pending || app->reload_notice_queued ||
        app->reload_in_progress || app->headless->publication_pending || app->headless->policy_pending ||
        app->continuation_required) return COG_ERROR_AGAIN;
    for (size_t i = 0U; i < CLI_INPUTS; ++i)
        if (app->inputs[i].used) return COG_ERROR_AGAIN;
    size_t count = 0U;
    cog_result_t rc = cog_core_pending_count(app->core, cog_session_id(app->session), &count);
    return rc != COG_OK ? rc : count == 0U ? COG_OK : COG_ERROR_AGAIN;
}

cog_result_t cog_cli_handoff_request(cog_cli_handoff_t *h, cog_worker_id_t target)
{
    if (h == NULL) return COG_ERROR_NULL_PTR;
    cog_cli_host_runtime_t *r = h->runtime;
    if (h->phase != CLI_HANDOFF_IDLE || r->shutdown || r->done || r->admission_frozen ||
        r->coordinator_failed || !r->begun || r->primary_slot > 1U ||
        handoff_worker_equal(target, r->primary_worker)) return COG_ERROR_INVALID_STATE;
    if ((target.incarnation == 0U) != (target.number == 0U)) return COG_ERROR_NOT_FOUND;
    if (r->current_mirror == NULL || r->future_mirror != NULL ||
        r->active_configuration != r->published_configuration) return COG_ERROR_AGAIN;
    for (size_t i = 0U; i < r->group_capacity; ++i)
        if (r->groups[i].phase != CLI_GROUP_FREE) return COG_ERROR_AGAIN;
    for (size_t i = 2U; i < r->bounds.executors; ++i)
        if (r->executors[i].phase != CLI_SLOT_FREE) return COG_ERROR_AGAIN;
    cli_executor_slot_t *source = &r->executors[r->primary_slot];
    size_t target_index = 1U - r->primary_slot;
    cli_executor_slot_t *dest = &r->executors[target_index];
    if (source->command.used || source->cancel.used || source->update.length != 0U ||
        dest->phase != CLI_SLOT_FREE || dest->app != NULL || dest->net != NULL ||
        dest->observer != NULL || dest->pending_profile != NULL) return COG_ERROR_AGAIN;
    if (!handoff_worker_equal(source->worker, r->primary_worker) || source->owner_peer != r->primary_peer ||
        !handoff_owner_equal(source->owner, r->primary_owner)) return COG_ERROR_INVALID_STATE;
    if (r->primary_owner.epoch == 0U || r->primary_owner.instance_id == 0U ||
        r->primary_owner.epoch == UINT64_MAX || r->next_nonce == 0U ||
        r->next_nonce > UINT64_MAX - HANDOFF_IDS || r->now_ms > UINT64_MAX - h->timeout_ms)
        return COG_ERROR_LIMIT;
    cog_peer_id_t peer = 0U;
    cli_worker_record_t *worker = NULL;
    if (target.number != 0U) {
        worker = handoff_worker(h, target);
        if (worker == NULL) return COG_ERROR_NOT_FOUND;
        if (!worker->profile_ready || worker->native.health != COG_HEALTH_HEALTHY ||
            worker->native.peer_id == 0U) return COG_ERROR_AGAIN;
        peer = worker->native.peer_id;
    }
    if (peer == source->owner_peer) return COG_ERROR_INVALID_STATE;
    if (source->owner_peer != 0U) {
        cli_worker_record_t *old = handoff_worker(h, source->worker);
        if (old == NULL || old->native.peer_id != source->owner_peer || old->native.reaped ||
            old->native.ownership_lost || old->native.health != COG_HEALTH_HEALTHY) return COG_ERROR_NOT_FOUND;
    }
    cog_result_t rc = handoff_lane_ready(source, r, source->worker, source->owner_peer);
    if (rc != COG_OK) return rc;
    rc = handoff_lane_ready(dest, r, target, peer); if (rc != COG_OK) return rc;
    if (source->owner_peer == 0U) {
        rc = handoff_local_quiet(source->app); if (rc != COG_OK) return rc;
    }
    uint64_t instance;
    rc = handoff_random(&instance); if (rc != COG_OK) return rc;
    if (instance == r->primary_owner.instance_id) return COG_ERROR_IO;
    /* Reserve real local resources before freezing the source. No slot metadata
       or authoritative counter changes until every preflight has succeeded. */
    cli_executor_slot_t reservation = {0};
    if (peer == 0U) {
        rc = handoff_local_reserve(h, &reservation); if (rc != COG_OK) return rc;
    }
    h->source_slot = r->primary_slot; h->target_slot = target_index;
    h->source_worker = r->primary_worker; h->target_worker = target;
    h->source_peer = r->primary_peer; h->target_peer = peer;
    h->expected = r->primary_owner;
    h->next = (cog_store_owner_t){r->primary_owner.epoch + 1U, instance};
    h->checkpoint.generation = 0U; h->identity = r->next_nonce; r->next_nonce += HANDOFF_IDS;
    h->deadline_ms = r->now_ms + h->timeout_ms; h->reload_attempts_used = r->reload_attempts_used;
    h->primary = COG_OK; h->abort_requested = false;
    h->checkpoint_saved = false; h->prepared = false; h->source_released = false;
    h->cas_committed = false; h->guard_acquired = false; h->route_published = false; h->activation_acked = false;
    h->pending.length = 0U; h->reply.length = 0U;
    dest->app = NULL; dest->net = reservation.net; dest->observer = reservation.observer;
    dest->pending_profile = reservation.pending_profile;
    dest->phase = CLI_SLOT_RESERVED; dest->kind = COG_CLI_EXECUTION_STAGED;
    dest->worker = target; dest->owner_peer = peer; dest->owner = h->next;
    dest->runtime = r->runtime_incarnation; dest->configuration_generation = r->active_configuration;
    memcpy(dest->session, r->primary_session, sizeof(dest->session));
    dest->agent = 0U; dest->attempt = 0U; dest->started = false; dest->local_done = false;
    dest->cancel_latched = false; dest->first_admission_sent = false; dest->primary = COG_OK;
    dest->deadline_ms = h->deadline_ms;
    handoff_secret_clear(&dest->profile);
    if (worker != NULL) ++worker->reserved_slots;
    h->phase = CLI_HANDOFF_QUIESCE;
    return COG_OK;
}

static cog_result_t handoff_decode(const cli_host_bytes_t *bytes, cog_cli_control_message_t *m,
    unsigned char *storage, size_t capacity)
{
    size_t needed;
    return cog_cli_control_decode(bytes->data, bytes->length, m, storage, capacity, &needed);
}
static cog_result_t handoff_queue(cog_cli_handoff_t *h, size_t index, cog_handoff_kind_t kind, uint64_t offset)
{
    if (h->pending.length != 0U) return COG_OK;
    cli_executor_slot_t *s = &h->runtime->executors[index];
    cog_worker_id_t worker = index == h->source_slot ? h->source_worker : h->target_worker;
    cog_peer_id_t peer = index == h->source_slot ? h->source_peer : h->target_peer;
    if (!handoff_worker_equal(s->worker, worker) || s->owner_peer != peer || peer == 0U || s->outbound == NULL)
        return COG_ERROR_INVALID_STATE;
    cli_control_lane_t *lane = &s->outbound[COG_CLI_LANE_HANDOFF - 1U];
    if (!handoff_lane_key(lane, h->runtime, s->worker, s->owner_peer) || lane->pending_valid)
        return COG_ERROR_INVALID_STATE;
    unsigned char body[189]; size_t bytes;
    cog_handoff_message_t message = {.kind = kind, .session_id = h->runtime->primary_session,
        .source_peer = h->source_peer, .target_peer = h->target_peer, .expected = h->expected,
        .next = h->next, .checkpoint = kind == COG_HANDOFF_QUIESCE ? (cog_store_checkpoint_t){0U} : h->checkpoint};
    cog_result_t rc = cog_orchestrator_handoff_encode(&message, body, sizeof(body), &bytes);
    if (rc != COG_OK) return rc;
    cog_cli_control_message_t envelope = {.kind = COG_CLI_CONTROL_HANDOFF, .lane = COG_CLI_LANE_HANDOFF,
        .runtime_incarnation = h->runtime->runtime_incarnation, .sender_incarnation = h->runtime->runtime_incarnation,
        .wire_id = h->identity + offset, .worker = s->worker, .owner_peer = s->owner_peer,
        .session_id = h->runtime->primary_session, .payload = body, .payload_len = bytes};
    rc = cog_cli_control_encode(&envelope, h->pending.data, h->pending.capacity, &h->pending.length);
    if (rc != COG_OK) return rc;
    /* A higher request on this same stream follows actual consumption of the
       preceding final. PROFILE never enters this cache or retires its proof. */
    if (lane->final_valid) {
        lane->retired_through = lane->final_answered_wire;
        lane->final_valid = false; lane->final_original.length = 0U; lane->final_reply.length = 0U;
    }
    lane->key = (cli_stream_key_t){.fabric = CLI_FABRIC_WORKER,
        .runtime = h->runtime->runtime_incarnation, .sender = h->runtime->runtime_incarnation,
        .worker = s->worker, .owner_peer = s->owner_peer, .lane = COG_CLI_LANE_HANDOFF};
    memcpy(lane->key.session, h->runtime->primary_session, sizeof(lane->key.session));
    lane->bound = true; lane->pending_valid = true; lane->pending_wire = envelope.wire_id;
    lane->next_wire = envelope.wire_id + 1U; lane->deadline_ms = h->deadline_ms;
    lane->reserved_cleanup_first = h->identity + 12U; lane->reserved_cleanup_count = 2U;
    memcpy(lane->pending.data, h->pending.data, h->pending.length); lane->pending.length = h->pending.length;
    return COG_OK;
}
static cog_result_t handoff_receive(cog_cli_handoff_t *h, size_t index, cog_handoff_kind_t expected)
{
    if (h->reply.length == 0U) return COG_ERROR_AGAIN;
    unsigned char request_storage[HANDOFF_BYTES], reply_storage[HANDOFF_BYTES];
    cog_cli_control_message_t request, reply;
    cog_result_t rc = handoff_decode(&h->pending, &request, request_storage, sizeof(request_storage));
    if (rc != COG_OK) return rc;
    rc = handoff_decode(&h->reply, &reply, reply_storage, sizeof(reply_storage)); if (rc != COG_OK) return rc;
    cli_executor_slot_t *s = &h->runtime->executors[index];
    cog_worker_id_t worker = index == h->source_slot ? h->source_worker : h->target_worker;
    cog_peer_id_t peer = index == h->source_slot ? h->source_peer : h->target_peer;
    if (!handoff_worker_equal(s->worker, worker) || s->owner_peer != peer) return COG_ERROR_INVALID_STATE;
    if (reply.kind != COG_CLI_CONTROL_HANDOFF || reply.lane != COG_CLI_LANE_HANDOFF ||
        reply.runtime_incarnation != h->runtime->runtime_incarnation ||
        reply.sender_incarnation != s->worker.incarnation || !handoff_worker_equal(reply.worker, s->worker) ||
        reply.owner_peer != s->owner_peer || reply.agent_id != 0U || reply.execution_epoch != 0U ||
        strcmp(reply.session_id, h->runtime->primary_session) != 0 || reply.reply_to != request.wire_id)
        return COG_ERROR_PROTOCOL;
    cog_handoff_message_t body; char session[128]; size_t needed;
    rc = cog_orchestrator_handoff_decode(reply.payload, reply.payload_len, &body, session, sizeof(session), &needed);
    if (rc != COG_OK) return rc;
    if ((body.kind != expected && body.kind != COG_HANDOFF_FAILED) ||
        strcmp(body.session_id, h->runtime->primary_session) != 0 || body.source_peer != h->source_peer ||
        body.target_peer != h->target_peer || !handoff_owner_equal(body.expected, h->expected) ||
        !handoff_owner_equal(body.next, h->next) ||
        (expected != COG_HANDOFF_CHECKPOINTED && body.checkpoint.generation != h->checkpoint.generation) ||
        (expected == COG_HANDOFF_CHECKPOINTED && body.kind != COG_HANDOFF_FAILED && body.checkpoint.generation == 0U) ||
        (body.kind == COG_HANDOFF_FAILED && body.checkpoint.generation != h->checkpoint.generation))
        return COG_ERROR_PROTOCOL;
    cli_control_lane_t *lane = &s->outbound[COG_CLI_LANE_HANDOFF - 1U];
    if (!lane->pending_valid || lane->pending_wire != request.wire_id ||
        lane->pending.length != h->pending.length || memcmp(lane->pending.data, h->pending.data, h->pending.length) != 0)
        return COG_ERROR_INVALID_STATE;
    memcpy(lane->final_original.data, h->pending.data, h->pending.length);
    lane->final_original.length = h->pending.length;
    memcpy(lane->final_reply.data, h->reply.data, h->reply.length); lane->final_reply.length = h->reply.length;
    lane->final_answered_wire = request.wire_id; lane->final_reply_wire = reply.wire_id;
    lane->final_valid = true; lane->pending_valid = false; lane->pending.length = 0U;
    h->pending.length = 0U; h->reply.length = 0U;
    if (body.kind == COG_HANDOFF_FAILED) return COG_ERROR_ABORTED;
    if (expected == COG_HANDOFF_CHECKPOINTED) h->checkpoint = body.checkpoint;
    return COG_OK;
}
static cog_result_t handoff_exchange(cog_cli_handoff_t *h, size_t index,
    cog_handoff_kind_t request, cog_handoff_kind_t response, uint64_t offset)
{
    cog_result_t rc = handoff_queue(h, index, request, offset);
    return rc == COG_OK ? handoff_receive(h, index, response) : rc;
}

static cog_result_t handoff_prepare(cog_cli_handoff_t *h)
{
    cli_executor_slot_t *s = &h->runtime->executors[h->target_slot];
    if (h->target_peer != 0U) {
        cli_secret_exchange_t *p = &s->profile;
        if (p->source == NULL) {
            p->source = h->runtime->current_mirror; p->purpose = 3U;
            p->generation = s->configuration_generation; p->reload_attempts_used = h->reload_attempts_used;
            memcpy(p->probe.session_id, h->runtime->primary_session, sizeof(p->probe.session_id));
            p->probe.owner = h->expected; p->probe.checkpoint = h->checkpoint;
            s->phase = CLI_SLOT_PROFILE;
        }
        if (!p->acknowledged) return COG_ERROR_AGAIN;
        return handoff_exchange(h, h->target_slot, COG_HANDOFF_CHECKPOINTED, COG_HANDOFF_PREPARED, 1U);
    }
    cog_cli_execution_admission_t admission = {.kind = COG_CLI_EXECUTION_STAGED,
        .configuration = s->pending_profile, .session_id = h->runtime->primary_session,
        .configuration_generation = s->configuration_generation, .reload_attempts_used = h->reload_attempts_used,
        .expected = h->expected, .next = h->next, .checkpoint = h->checkpoint, .observer = s->observer};
    s->pending_profile = NULL;
    cog_result_t rc = cog_cli_execution_new(&s->app, &admission);
    if (rc != COG_OK) return rc;
    cog_cli_delegation_context_t *context;
    rc = cog_cli_delegation_context_new(&context, s->app, h->runtime, NULL, 0U, 0U,
        h->runtime->bounds.mailbox_entries, h->runtime->bounds.mailbox_bytes);
    return rc;
}

static cog_result_t handoff_local_dispose(cli_executor_slot_t *s)
{
    if (s->app != NULL) {
        cli_execution_state_t *e = s->app->headless;
        if (e == NULL) return COG_ERROR_INVALID_STATE;
        if (e->drive == CLI_DRIVE_BEGUN) {
            if (!s->cancel_latched) {
                cog_result_t rc = cog_cli_app_shutdown(s->app);
                if (rc == COG_ERROR_OVERFLOW) return COG_ERROR_AGAIN;
                if (rc != COG_OK) return rc;
                s->cancel_latched = true;
            }
            cog_core_step_info_t info = {0};
            cog_result_t rc = cog_cli_execution_step(s->app, &info);
            if (rc != COG_OK && rc != COG_ERROR_AGAIN) s->primary = handoff_first(s->primary, rc);
            if (info.done) {
                rc = cog_cli_execution_end(s->app);
                if (rc != COG_OK) s->primary = handoff_first(s->primary, rc);
            }
            return COG_ERROR_AGAIN;
        }
        if (e->drive == CLI_DRIVE_DRAINING) {
            cog_core_step_info_t info = {0};
            cog_result_t rc = cog_cli_execution_drain_step(s->app, &info);
            if (rc != COG_OK && rc != COG_ERROR_AGAIN) s->primary = handoff_first(s->primary, rc);
            if (!info.done) return COG_ERROR_AGAIN;
            s->primary = handoff_first(s->primary, cog_cli_execution_end(s->app));
        }
        cog_result_t rc = cog_cli_execution_close(&s->app, COG_OK);
        if (s->app != NULL) return rc;
        s->primary = handoff_first(s->primary, rc);
    }
    s->primary = handoff_transport_close(s, s->primary);
    handoff_secret_clear(&s->profile);
    s->local_done = true;
    s->phase = s->primary == COG_OK ? CLI_SLOT_FREE : CLI_SLOT_RETAINED;
    return s->primary;
}

static cog_result_t handoff_dispose(cog_cli_handoff_t *h, size_t index, uint64_t offset)
{
    cli_executor_slot_t *s = &h->runtime->executors[index];
    if (s->local_done) return s->primary;
    if (s->owner_peer == 0U) return handoff_local_dispose(s);
    /* Before any PROFILE was accepted there is no remote target resource; the
       locally reserved profile still must be wiped and its borrow detached. */
    if (index == h->target_slot && !s->first_admission_sent && !h->prepared && h->pending.length == 0U) {
        handoff_secret_clear(&s->profile); s->local_done = true; s->phase = CLI_SLOT_FREE;
    } else {
        cog_result_t rc = handoff_exchange(h, index, COG_HANDOFF_ABORT, COG_HANDOFF_ABORT, offset);
        if (rc != COG_OK) return rc;
        handoff_secret_clear(&s->profile); s->local_done = true; s->phase = CLI_SLOT_FREE;
    }
    cli_worker_record_t *worker = handoff_worker(h, s->worker);
    if (worker != NULL && worker->reserved_slots != 0U) --worker->reserved_slots;
    return COG_OK;
}

static cog_result_t handoff_phase(cog_cli_handoff_t *h)
{
    cog_cli_host_runtime_t *r = h->runtime;
    cli_executor_slot_t *source = &r->executors[h->source_slot];
    cli_executor_slot_t *target = &r->executors[h->target_slot];
    cog_result_t rc;
    switch (h->phase) {
    case CLI_HANDOFF_QUIESCE:
        rc = h->source_peer == 0U ? cog_cli_execution_quiesce(source->app, h->expected, &h->checkpoint) :
            handoff_exchange(h, h->source_slot, COG_HANDOFF_QUIESCE, COG_HANDOFF_CHECKPOINTED, 0U);
        if (rc == COG_OK) { h->checkpoint_saved = true; h->phase = CLI_HANDOFF_PREPARE; }
        return rc;
    case CLI_HANDOFF_PREPARE:
        rc = handoff_prepare(h);
        if (rc == COG_OK) { h->prepared = true; target->phase = CLI_SLOT_PREPARED; h->phase = CLI_HANDOFF_RELEASE; }
        return rc;
    case CLI_HANDOFF_RELEASE:
        rc = h->source_peer == 0U ? cog_cli_execution_release_source(source->app, h->checkpoint) :
            handoff_exchange(h, h->source_slot, COG_HANDOFF_PREPARED, COG_HANDOFF_PREPARED, 2U);
        if (rc == COG_OK) { h->source_released = true; h->phase = CLI_HANDOFF_CAS; }
        return rc;
    case CLI_HANDOFF_CAS:
        rc = cog_store_owner_compare_exchange(h->catalog, r->primary_session, h->expected, h->next, h->checkpoint);
        if (rc == COG_OK) { h->cas_committed = true; h->phase = CLI_HANDOFF_GUARD; }
        return rc;
    case CLI_HANDOFF_GUARD:
        rc = h->target_peer == 0U ? cog_cli_execution_stage_guard(target->app, h->next, h->checkpoint) :
            handoff_exchange(h, h->target_slot, COG_HANDOFF_COMMIT, COG_HANDOFF_READY, 3U);
        if (rc == COG_OK) { h->guard_acquired = true; h->phase = CLI_HANDOFF_PUBLISH; }
        return rc;
    case CLI_HANDOFF_PUBLISH:
        rc = cog_orchestrator_publish_owner(h->orchestrator, r->primary_session, h->target_peer, h->next, h->checkpoint);
        if (rc == COG_OK) { h->route_published = true; h->phase = CLI_HANDOFF_ACTIVATE; }
        return rc;
    case CLI_HANDOFF_ACTIVATE:
        rc = h->target_peer == 0U ? cog_cli_execution_stage_activate(target->app, h->next, h->checkpoint) :
            handoff_exchange(h, h->target_slot, COG_HANDOFF_ACTIVATE, COG_HANDOFF_ACTIVATE, 4U);
        if (rc != COG_OK) return rc;
        h->activation_acked = true;
        r->primary_slot = h->target_slot; r->primary_owner = h->next;
        r->primary_worker = h->target_worker; r->primary_peer = h->target_peer;
        target->kind = COG_CLI_EXECUTION_ROOT; target->phase = CLI_SLOT_STARTED;
        if (h->target_peer == 0U && !h->abort_requested) {
            rc = cog_cli_execution_begin(target->app);
            if (rc != COG_OK) return rc;
        }
        target->started = true; h->phase = CLI_HANDOFF_CLEANUP;
        return COG_OK;
    case CLI_HANDOFF_CLEANUP:
        rc = handoff_dispose(h, h->source_slot, 12U);
        if (rc == COG_OK) h->phase = CLI_HANDOFF_IDLE;
        return rc;
    default: return COG_ERROR_INVALID_STATE;
    }
}

/* Native terminal facts end transport ownership, never prove disposal or
 * permit physical reuse. Keep the exact unresolved lane original for diagnosis. */
static bool handoff_native_terminal(cog_cli_handoff_t *h, size_t index)
{
    cli_executor_slot_t *s = &h->runtime->executors[index];
    if (s->owner_peer == 0U || s->local_done) return false;
    cli_worker_record_t *w = handoff_worker(h, s->worker);
    if (w == NULL || (!w->native.reaped && !w->native.ownership_lost)) return false;
    s->primary = handoff_first(s->primary, COG_ERROR_IO);
    s->effects_uncertain = true; s->local_done = true; s->phase = CLI_SLOT_RETAINED;
    handoff_secret_clear(&s->profile);
    if (h->pending.length != 0U) {
        unsigned char storage[HANDOFF_BYTES]; cog_cli_control_message_t request;
        if (handoff_decode(&h->pending, &request, storage, sizeof(storage)) == COG_OK &&
            handoff_worker_equal(request.worker, s->worker) && request.owner_peer == s->owner_peer) {
            h->pending.length = 0U; h->reply.length = 0U;
        }
    }
    h->primary = handoff_first(h->primary, COG_ERROR_IO);
    h->abort_requested = true; h->runtime->admission_frozen = true;
    return true;
}

cog_result_t cog_cli_handoff_abort(cog_cli_handoff_t *h)
{
    if (h == NULL) return COG_ERROR_NULL_PTR;
    if (h->phase == CLI_HANDOFF_IDLE) return COG_OK;
    h->abort_requested = true;
    h->runtime->admission_frozen = true;
    return COG_OK;
}

cog_result_t cog_cli_handoff_step(cog_cli_handoff_t *h, bool *done, uint64_t *deadline_ms)
{
    if (h == NULL || done == NULL || deadline_ms == NULL) return COG_ERROR_NULL_PTR;
    *done = h->phase == CLI_HANDOFF_IDLE; *deadline_ms = *done ? 0U : h->deadline_ms;
    if (*done) return h->primary;
    (void)handoff_native_terminal(h, h->source_slot);
    (void)handoff_native_terminal(h, h->target_slot);
    if (h->runtime->now_ms >= h->deadline_ms) {
        h->primary = handoff_first(h->primary, COG_ERROR_TIMEOUT);
        h->abort_requested = true; h->runtime->admission_frozen = true;
    }
    cog_result_t rc;
    if (!h->abort_requested) {
        rc = handoff_phase(h);
        if (rc != COG_OK && rc != COG_ERROR_AGAIN) {
            h->primary = handoff_first(h->primary, rc);
            h->abort_requested = true; h->runtime->admission_frozen = true;
        }
    } else {
        /* Never replace an unresolved immutable exchange with a new operation.
           Its possible effect must first reach a correlated disposition. */
        if (h->pending.length != 0U && h->phase != CLI_HANDOFF_FROZEN) {
            rc = handoff_phase(h);
            if (rc != COG_OK && rc != COG_ERROR_AGAIN) h->primary = handoff_first(h->primary, rc);
            if (h->pending.length != 0U) return h->primary == COG_OK ? COG_ERROR_AGAIN : h->primary;
        }
        h->phase = CLI_HANDOFF_FROZEN;
        rc = handoff_dispose(h, h->source_slot, 12U);
        if (rc != COG_OK && rc != COG_ERROR_AGAIN) h->primary = handoff_first(h->primary, rc);
        if (h->runtime->executors[h->source_slot].local_done) {
            rc = handoff_dispose(h, h->target_slot, 13U);
            if (rc != COG_OK && rc != COG_ERROR_AGAIN) h->primary = handoff_first(h->primary, rc);
        }
        *done = h->runtime->executors[h->source_slot].local_done &&
            h->runtime->executors[h->target_slot].local_done && h->pending.length == 0U;
    }
    if (h->phase == CLI_HANDOFF_IDLE) { *done = true; *deadline_ms = 0U; }
    if (h->primary != COG_OK) return h->primary;
    return rc;
}

cog_result_t cog_cli_handoff_close(cog_cli_handoff_t **owner, cog_result_t primary)
{
    if (owner == NULL) return COG_ERROR_NULL_PTR;
    cog_cli_handoff_t *h = *owner;
    if (h == NULL) return primary;
    if (h->phase != CLI_HANDOFF_IDLE && (h->phase != CLI_HANDOFF_FROZEN ||
        !h->runtime->executors[h->source_slot].local_done || !h->runtime->executors[h->target_slot].local_done ||
        h->pending.length != 0U)) return COG_ERROR_INVALID_STATE;
    cog_result_t rc = handoff_first(primary, h->primary);
    free(h->pending.data); free(h->reply.data); free(h); *owner = NULL;
    return rc;
}

/** @file core.c
 * @brief Fair, constructor-bounded single-threaded event scheduler.
 */
#define _POSIX_C_SOURCE 200809L
#include "cog/core.h"
#include "agent_internal.h"
#include "session_internal.h"
#include "../../cog-ipc/private/core_ipc.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "core_internal.h"

static size_t core_default(size_t value, size_t fallback)
{
    return value != 0U ? value : fallback;
}
static cog_result_t core_now(cog_core_t *core, uint64_t *now)
{
    struct timespec time;
    if (clock_gettime(CLOCK_MONOTONIC, &time) != 0 || time.tv_sec < 0 ||
        time.tv_nsec < 0 || time.tv_nsec >= 1000000000L ||
        (uint64_t)time.tv_sec > (UINT64_MAX - (uint64_t)time.tv_nsec / 1000000U) / 1000U) {
        return COG_ERROR_IO;
    }
    *now = (uint64_t)time.tv_sec * 1000U + (uint64_t)time.tv_nsec / 1000000U;
    if (core->clock_seen && *now < core->last_now) return COG_ERROR_IO;
    core->clock_seen = true;
    core->last_now = *now;
    return COG_OK;
}
static cog_result_t core_dispatch(cog_core_t *core, const cog_event_t *event)
{
    const cog_dispatch_entry_t *entry = NULL;
    cog_handler_result_t hr;
    cog_result_t rc;

    rc = cog_state_lookup(core->dispatch_table, core->dispatch_table_size, core->state,
                          event->id, &entry);
    if (rc != COG_OK) {
        return rc;
    }
    if (entry->handler == NULL) {
        return COG_ERROR_UNHANDLED;
    }

    hr = entry->handler(core, event);
    if (hr.status != COG_OK) {
        return hr.status;
    }

    rc = cog_state_transition_allowed(entry->allowed_transitions, hr.transition);
    if (rc != COG_OK) {
        return rc;
    }

    if (hr.transition != COG_TRANSITION_SELF) {
        core->state = hr.transition;
    }

    /* Auto-save the session once after a successfully handled
     * COG_EVENT_LLM_COMPLETE exchange, immediately after the valid state
     * transition is applied.  The store is borrowed (caller-owned); its
     * auto_save flag gates the save.  Save errors propagate to the caller
     * (cog_core_run); the state transition has already been applied.  No
     * save occurs on handler failure, denied transition, cancellation,
     * shutdown, or streaming chunks — only LLM_COMPLETE reaches here. */
    if (event->kind == COG_EVENT_LLM_COMPLETE && core->store != NULL &&
        cog_store_auto_save(core->store)) {
        return cog_store_save(core->store, core->session);
    }

    return COG_OK;
}

static size_t core_pending_owner(const cog_core_t *core, const char *id)
{
    size_t i;
    for (i = 0U; i < core->pending_owner_capacity; ++i) {
        if ((core->pending_owners[i].count != 0U || core->pending_owners[i].leased) &&
            strcmp(core->pending_owners[i].id, id) == 0) {
            return i;
        }
    }
    return SIZE_MAX;
}
static char *core_pending_text(cog_core_t *core, size_t owner, size_t index)
{
    return core->pending_text +
           (owner * core->pending_per_owner + index) * core->pending_bytes;
}
static void core_pending_remove(cog_core_t *core, size_t owner)
{
    core_pending_owner_t *entry = &core->pending_owners[owner];
    size_t base = owner * core->pending_per_owner;
    assert(entry->count != 0U);
    --entry->count;
    if (entry->count != 0U) {
        memmove(core->pending_items + base, core->pending_items + base + 1U,
                entry->count * sizeof(*core->pending_items));
        memmove(core_pending_text(core, owner, 0U), core_pending_text(core, owner, 1U),
                entry->count * core->pending_bytes);
    }
}
static cog_result_t core_inject_pending(cog_core_t *core)
{
    size_t owner = core_pending_owner(core, cog_session_id(core->session));
    cog_event_t event = {0};
    cog_result_t rc;
    if (owner == SIZE_MAX || core->pending_owners[owner].count == 0U ||
        core->injected) {
        return COG_ERROR_NOT_FOUND;
    }
    memcpy(core->injected_payload, core_pending_text(core, owner, 0U),
           core->pending_items[owner * core->pending_per_owner].length + 1U);
    event.id = (cog_event_id_t)COG_EVENT_USER_INPUT;
    event.kind = COG_EVENT_USER_INPUT;
    event.payload = core->injected_payload;
    event.payload_len = core->pending_items[owner * core->pending_per_owner].length;
    rc = cog_event_queue_push(core->event_queue, &event);
    if (rc == COG_OK) {
        core->injected = true;
        core->injected_owner = owner;
        core->injected_length = event.payload_len;
        core->pending_owners[owner].leased = true;
        core_pending_remove(core, owner);
    }
    /* Successful enqueue logically removes the item. Its independent copy
     * owns the event bytes; the immutable owner lease reserves restoration
     * capacity without leaving the injected item publicly dequeuable. */
    return rc;
}
static cog_result_t core_ipc_enqueue(cog_core_t *core, cog_event_kind_t kind,
                                     const core_peer_t *peer, const void *data,
                                     size_t length)
{
    cog_event_ipc_t ipc = {0};
    cog_event_t event = {0};
    ipc.peer = peer->id;
    ipc.peer_info_valid = peer->proof_valid;
    ipc.peer_info = peer->proof;
    ipc.msg_payload = data;
    ipc.msg_payload_len = length;
    event.id = (cog_event_id_t)kind;
    event.kind = kind;
    event.payload = &ipc;
    event.payload_len = sizeof(ipc);
    return cog_event_queue_push(core->ipc_events, &event);
}
static void core_ipc_release(cog_core_t *core, const void *payload)
{
    core_slice_t *slice;
    if (core->ipc_slice_count == 0U) {
        return;
    }
    slice = &core->ipc_slices[core->ipc_slice_head];
    if (payload != core->ipc_arena + slice->offset) {
        return;
    }
    core->ipc_arena_used -= slice->charge;
    core->ipc_arena_head = (slice->offset + slice->length) % core->ipc_arena_capacity;
    core->ipc_slice_head = (core->ipc_slice_head + 1U) % core->ipc_slice_capacity;
    --core->ipc_slice_count;
    if (core->ipc_slice_count == 0U) {
        core->ipc_arena_head = 0U;
        core->ipc_arena_tail = 0U;
        core->ipc_arena_used = 0U;
    }
}
static void core_ipc_remove(cog_core_t *core, size_t index)
{
    (void)cog_ipc_endpoint_free(core->ipc_clients[index].endpoint);
    --core->ipc_client_count;
    memmove(core->ipc_clients + index, core->ipc_clients + index + 1U,
            (core->ipc_client_count - index) * sizeof(*core->ipc_clients));
    memset(core->ipc_clients + core->ipc_client_count, 0, sizeof(*core->ipc_clients));
    if (core->ipc_cursor > index) {
        --core->ipc_cursor;
    }
}
static cog_result_t core_ipc_receive(cog_core_t *core, core_peer_t *peer)
{
    unsigned char dummy;
    cog_ipc_message_t probe = {&dummy, 0U, 0U};
    cog_ipc_endpoint_t *ep = peer->endpoint;
    cog_result_t rc;
    size_t offset, charge, padding = 0U, slot;
    core_slice_t *slice;
    if (cog_event_queue_is_full(core->ipc_events)) {
        return COG_ERROR_AGAIN;
    }
    (void)cog_ipc_endpoint_get_peer_id(ep, &peer->id);
    rc = cog_ipc_endpoint_recv(ep, &probe);
    if (rc == COG_OK) {
        return core_ipc_enqueue(core, COG_EVENT_IPC_MESSAGE, peer, NULL, 0U);
    }
    if (rc != COG_ERROR_OVERFLOW) {
        return rc;
    }
    if (probe.len > core->ipc_max_frame) {
        return COG_ERROR_LIMIT;
    }
    if (core->ipc_slice_count == core->ipc_slice_capacity) {
        return COG_ERROR_AGAIN;
    }
    offset = core->ipc_arena_tail;
    if (probe.len > core->ipc_arena_capacity - offset) {
        padding = core->ipc_arena_capacity - offset;
        offset = 0U;
    }
    if (padding > core->ipc_arena_capacity - core->ipc_arena_used ||
        probe.len > core->ipc_arena_capacity - core->ipc_arena_used - padding) {
        return COG_ERROR_AGAIN;
    }
    charge = padding + probe.len;
    probe.data = core->ipc_arena + offset;
    probe.capacity = probe.len;
    /* Event and arena reservations are established before consuming the frame.
     * No callback can mutate either reservation between receive and enqueue. */
    rc = cog_ipc_endpoint_recv(ep, &probe);
    if (rc != COG_OK) {
        return rc;
    }
    rc = core_ipc_enqueue(core, COG_EVENT_IPC_MESSAGE, peer, probe.data, probe.len);
    assert(rc == COG_OK);
    if (rc != COG_OK) {
        return rc;
    }
    slot = (core->ipc_slice_head + core->ipc_slice_count) % core->ipc_slice_capacity;
    slice = &core->ipc_slices[slot];
    slice->offset = offset;
    slice->length = probe.len;
    slice->charge = charge;
    ++core->ipc_slice_count;
    core->ipc_arena_used += charge;
    core->ipc_arena_tail = (offset + probe.len) % core->ipc_arena_capacity;
    return COG_OK;
}
static cog_result_t core_peer_capture(core_peer_t *peer)
{
    if (peer->proof_valid) return COG_OK;
    if (cog_ipc_endpoint_get_peer_id(peer->endpoint, &peer->id) != COG_OK)
        return COG_ERROR_AGAIN;
    cog_ipc_peer_info_t info;
    cog_result_t rc = cog_ipc_endpoint_peer_info(peer->endpoint, peer->id, &info);
    if (rc == COG_OK) { peer->proof = info; peer->proof_valid = true; }
    return rc;
}
static void core_peer_remove(cog_core_t *core, size_t index)
{
    if (core->ipc_is_listener == 1) core_ipc_remove(core, index);
    else {
        (void)cog_ipc_endpoint_disconnect(core->ipc_endpoint);
        memset(&core->ipc_attached, 0, sizeof(core->ipc_attached));
        core->ipc_is_listener = 0;
    }
}
static cog_result_t core_ipc_step(cog_core_t *core, uint64_t *deadline)
{
    if (core->ipc_endpoint == NULL || core->ipc_is_listener == 0) return COG_OK;
    for (size_t unit = 0U; unit < core->ipc_quantum; ++unit) {
        if (core->ipc_is_listener == 0) break;
        core_peer_t *peer;
        cog_result_t rc, service_rc;
        uint64_t next = UINT64_MAX, now;
        size_t index = 0U;
        if (atomic_load_explicit(&core->stop_requested, memory_order_acquire)) break;
        rc = core_now(core, &now);
        if (rc != COG_OK) return rc;
        if (core->ipc_is_listener == 1 && core->ipc_cursor >= core->ipc_client_count) {
            cog_ipc_endpoint_t *client = NULL;
            cog_peer_id_t id = 0U;
            core->ipc_cursor = 0U;
            if ((core->require_peer_credentials || !cog_event_queue_is_full(core->ipc_events)) &&
                core->ipc_client_count < core->ipc_client_capacity) {
                rc = cog_ipc_endpoint_accept(core->ipc_endpoint, &client, &id);
                if (rc == COG_OK) {
                    peer = &core->ipc_clients[core->ipc_client_count++];
                    peer->endpoint = client; peer->id = id;
                    if (!core->require_peer_credentials) {
                        rc = core_ipc_enqueue(core, COG_EVENT_IPC_CONNECTED, peer, NULL, 0U);
                        assert(rc == COG_OK); peer->connected_queued = true;
                    }
                }
                if (rc != COG_OK && rc != COG_ERROR_AGAIN) return rc;
            }
            if (core->ipc_client_count == core->ipc_client_capacity) {
                rc = cog_ipc_internal_refuse(core->ipc_endpoint);
                if (rc != COG_OK && rc != COG_ERROR_AGAIN) return rc;
            }
            if (core->ipc_client_count == 0U) break;
            continue;
        }
        if (core->ipc_is_listener == 1) {
            index = core->ipc_cursor++; peer = &core->ipc_clients[index];
        } else peer = &core->ipc_attached;
        /* Already-ready transferred endpoints need proof BEFORE first I/O. */
        if (core->require_peer_credentials && !peer->proof_valid) {
            rc = core_peer_capture(peer);
            if (rc == COG_ERROR_IO || rc == COG_ERROR_UNSUPPORTED) {
                core_peer_remove(core, index); continue;
            }
        }
        service_rc = cog_ipc_internal_service_one(peer->endpoint, now, &next);
        if (next < *deadline) *deadline = next;
        if (core->require_peer_credentials && !peer->proof_valid) {
            /* Mandatory handshake barrier: never gate this capture on queues. */
            rc = core_peer_capture(peer);
            if (rc != COG_OK) {
                if (rc == COG_ERROR_AGAIN &&
                    (service_rc == COG_OK || service_rc == COG_ERROR_AGAIN)) continue;
                core_peer_remove(core, index);
                if (service_rc != COG_OK && service_rc != COG_ERROR_AGAIN &&
                    service_rc != COG_ERROR_IO) return service_rc;
                continue;
            }
        }
        /* Fatal framing/service outcomes freeze dispatch even when descriptor
         * or payload reservations are full. Only ordinary IO/EOF may drain
         * retained complete frames before peer retirement. Event proof copies
         * already queued remain valid after this exact endpoint is removed. */
        if (service_rc != COG_OK && service_rc != COG_ERROR_AGAIN &&
            service_rc != COG_ERROR_IO) {
            core_peer_remove(core, index);
            return service_rc;
        }
        if (!peer->connected_queued) {
            if (cog_event_queue_is_full(core->ipc_events)) continue;
            rc = core_ipc_enqueue(core, COG_EVENT_IPC_CONNECTED, peer, NULL, 0U);
            if (rc != COG_OK) return rc;
            peer->connected_queued = true;
        }
        /* Complete retained frames precede ordinary disconnect, using cached proof. */
        rc = core_ipc_receive(core, peer);
        /* Ordinary IO/EOF waits for retained RX/event reservations to drain.
         * Receive-side/core-ceiling errors retire the endpoint before returning
         * so explicit rebegin cannot redispatch the same failure. */
        if (rc == COG_OK || rc == COG_ERROR_AGAIN || rc == COG_ERROR_INVALID_STATE) continue;
        if (cog_event_queue_is_full(core->ipc_events)) continue;
        (void)core_ipc_enqueue(core, COG_EVENT_IPC_DISCONNECTED, peer, NULL, 0U);
        core_peer_remove(core, index);
        if (rc != COG_ERROR_IO) return rc;
    }
    return COG_OK;
}
static void core_storage_free(cog_core_t *core)
{
    free(core->pending_owners);
    free(core->pending_items);
    free(core->pending_text);
    free(core->injected_payload);
    free(core->ipc_clients);
    free(core->ipc_arena);
    free(core->ipc_slices);
    cog_event_queue_free(core->event_queue);
    cog_event_queue_free(core->ipc_events);
    cog_agent_registry_reset(&core->agents);
    free(core);
}
cog_result_t cog_core_new(cog_core_t **out, const cog_core_config_t *config)
{
    cog_core_t *core;
    size_t queue, entries;
    cog_result_t rc;
    if (out == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    *out = NULL;
    if (config == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    if (config->session == NULL || config->dispatch_table == NULL ||
        config->dispatch_table_size == 0U ||
        (config->wait_fn != NULL &&
         (config->service_fn != NULL || config->idle_fn != NULL)) ||
        config->step_event_limit > 1024U) {
        return COG_ERROR_INVALID_ARG;
    }
    core = calloc(1U, sizeof(*core));
    if (core == NULL) {
        return COG_ERROR_NOMEM;
    }
    queue = core_default(config->event_queue_capacity, 64U);
    core->pending_owner_capacity = core_default(config->pending_session_capacity, 17U);
    core->pending_per_owner = core_default(config->pending_messages_per_session, 8U);
    core->pending_bytes = core_default(config->pending_message_bytes, 65536U);
    core->ipc_client_capacity = core_default(config->ipc_peer_capacity, 16U);
    core->ipc_max_frame = core_default(config->ipc_max_frame_bytes, 1048576U);
    core->ipc_arena_capacity = core_default(config->ipc_payload_bytes, 8388608U);
    core->ipc_quantum = core_default(config->ipc_service_quantum, 16U);
    core->ipc_slice_capacity = core->ipc_quantum;
    if (core->pending_owner_capacity > SIZE_MAX / core->pending_per_owner ||
        core->pending_owner_capacity > SIZE_MAX / sizeof(*core->pending_owners) ||
        core->ipc_client_capacity > SIZE_MAX / sizeof(*core->ipc_clients) ||
        core->ipc_slice_capacity > SIZE_MAX / sizeof(*core->ipc_slices) ||
        core->ipc_max_frame > UINT32_MAX ||
        core->ipc_arena_capacity < core->ipc_max_frame) {
        free(core);
        return COG_ERROR_INVALID_ARG;
    }
    entries = core->pending_owner_capacity * core->pending_per_owner;
    if (entries > SIZE_MAX / sizeof(*core->pending_items) ||
        entries > SIZE_MAX / core->pending_bytes) {
        free(core);
        return COG_ERROR_INVALID_ARG;
    }
    rc = cog_event_queue_new(&core->event_queue, queue);
    if (rc != COG_OK) {
        free(core);
        return rc;
    }
    rc = cog_event_queue_new(&core->ipc_events, core->ipc_slice_capacity);
    if (rc != COG_OK) {
        core_storage_free(core);
        return rc;
    }
    core->pending_owners =
        calloc(core->pending_owner_capacity, sizeof(*core->pending_owners));
    core->pending_items = calloc(entries, sizeof(*core->pending_items));
    core->pending_text = malloc(entries * core->pending_bytes);
    core->injected_payload = malloc(core->pending_bytes);
    core->ipc_clients = calloc(core->ipc_client_capacity, sizeof(*core->ipc_clients));
    core->ipc_slices = calloc(core->ipc_slice_capacity, sizeof(*core->ipc_slices));
    core->ipc_arena = malloc(core->ipc_arena_capacity);
    if (core->pending_owners == NULL || core->pending_items == NULL ||
        core->pending_text == NULL || core->injected_payload == NULL ||
        core->ipc_clients == NULL || core->ipc_slices == NULL ||
        core->ipc_arena == NULL) {
        core_storage_free(core);
        return COG_ERROR_NOMEM;
    }
    rc = cog_agent_registry_init(&core->agents, core_default(config->max_agents, 16U),
                                 core_default(config->max_agent_message_bytes, 65536U));
    if (rc != COG_OK) {
        core_storage_free(core);
        return rc;
    }
    core->session = config->session;
    core->dispatch_table = config->dispatch_table;
    core->dispatch_table_size = config->dispatch_table_size;
    core->tool_table = config->tool_table;
    core->wait_fn = config->wait_fn;
    core->wait_arg = config->wait_arg;
    core->user = config->user;
    core->service_fn = config->service_fn;
    core->service_arg = config->service_arg;
    core->idle_fn = config->idle_fn;
    core->idle_arg = config->idle_arg;
    core->service_interval = core_default(config->service_interval, 16U);
    core->step_event_limit = core_default(config->step_event_limit, 16U);
    core->require_peer_credentials = config->require_peer_credentials;
    core->max_idle_wait_ms =
        config->max_idle_wait_ms != 0U ? config->max_idle_wait_ms : 10U;
    core->ipc_endpoint = config->ipc_endpoint;
    core->ipc_is_listener = cog_ipc_internal_role(config->ipc_endpoint);
    core->ipc_attached.endpoint = config->ipc_endpoint;
    core->ipc_attached.connected_queued = true;
    core->local_peer_id = config->local_peer_id;
    core->provider = config->provider;
    core->obs = config->obs;
    core->store = config->store;
    atomic_init(&core->running, false);
    atomic_init(&core->stop_requested, false);
    ((struct cog_session *)core->session)->attached = true;
    if (core->obs != NULL) {
        (void)cog_obs_log(core->obs, COG_OBS_INFO, "core.created", NULL, 0U);
    }
    *out = core;
    return COG_OK;
}
void cog_core_free(cog_core_t *core)
{
    size_t i;
    if (core == NULL) {
        return;
    }
    assert(core->drive.kind == CORE_DRIVER_NONE && !core->drive.inside_call);
    core->drive.inside_call = true;
    cog_event_t discarded;
    while (cog_event_queue_pop(core->event_queue, &discarded) == COG_OK)
        cog_agent_intent_dispatched(core, &discarded);
    ((struct cog_session *)core->session)->attached = false;
    for (i = 0U; i < core->ipc_client_count; ++i) {
        (void)cog_ipc_endpoint_free(core->ipc_clients[i].endpoint);
    }
    if (core->ipc_endpoint != NULL) {
        (void)cog_ipc_endpoint_free(core->ipc_endpoint);
    }
    if (core->obs != NULL) {
        (void)cog_obs_log(core->obs, COG_OBS_INFO, "core.destroyed", NULL, 0U);
    }
    core_storage_free(core);
}
static cog_result_t core_begin(cog_core_t *core, core_driver_kind_t kind)
{
    if (core == NULL) return COG_ERROR_NULL_PTR;
    if (core->drive.kind != CORE_DRIVER_NONE || core->drive.inside_call)
        return COG_ERROR_INVALID_STATE;
    core->drive = (core_drive_state_t){0};
    core->drive.kind = kind; core->drive.inside_call = true;
    core->drive.service_due = true;
    atomic_store_explicit(&core->stop_requested, false, memory_order_relaxed);
    atomic_store_explicit(&core->running, true, memory_order_relaxed);
    if (core->obs != NULL) (void)cog_obs_log(core->obs, COG_OBS_INFO, "core.run_start", NULL, 0U);
    core->drive.inside_call = false;
    return COG_OK;
}
static void core_finish(cog_core_t *core, cog_result_t result)
{
    if (!core->drive.done) {
        core->drive.done = true;
        core->drive.result = result;
    }
    atomic_store_explicit(&core->running, false, memory_order_relaxed);
}
static cog_result_t core_end(cog_core_t *core)
{
    cog_result_t rc = core->drive.result;
    core->drive.inside_call = true;
    /* The queued injected copy has not reached its handler. Its owner and
     * one FIFO slot stayed leased despite public drain/queue operations, so
     * restoring it at the head cannot overwrite a replacement or overflow. */
    if (core->injected) {
        cog_event_t event;
        size_t owner = core->injected_owner;
        core_pending_owner_t *entry = &core->pending_owners[owner];
        size_t base = owner * core->pending_per_owner;
        cog_result_t pop = cog_event_queue_pop(core->event_queue, &event);
        assert(pop == COG_OK && event.payload == core->injected_payload);
        assert(entry->leased && entry->count < core->pending_per_owner);
        (void)pop;
        if (entry->count != 0U) {
            memmove(core->pending_items + base + 1U, core->pending_items + base,
                    entry->count * sizeof(*core->pending_items));
            memmove(core_pending_text(core, owner, 1U),
                    core_pending_text(core, owner, 0U),
                    entry->count * core->pending_bytes);
        }
        memcpy(core_pending_text(core, owner, 0U), core->injected_payload,
               core->injected_length + 1U);
        core->pending_items[base].length = core->injected_length;
        ++entry->count;
        entry->leased = false;
        core->injected = false;
    }
    if (core->obs != NULL)
        (void)cog_obs_log(core->obs, rc == COG_OK ? COG_OBS_INFO : COG_OBS_ERROR,
                          rc == COG_OK ? "core.run_stop" : "core.run_error", NULL, 0U);
    core->drive.kind = CORE_DRIVER_NONE;
    core->drive.inside_call = false;
    return rc;
}
static cog_result_t core_wait_info(cog_core_t *core, cog_core_step_info_t *info)
{
    uint64_t now;
    cog_result_t rc = core_now(core, &now);
    bool ready = core->drive.service_due ||
        core->drive.dispatched_since_service >= core->service_interval ||
        !cog_event_queue_is_empty(core->event_queue) ||
        !cog_event_queue_is_empty(core->ipc_events);
    if (rc != COG_OK) return rc;
    size_t owner = core_pending_owner(core, cog_session_id(core->session));
    if (owner != SIZE_MAX && core->pending_owners[owner].count != 0U) ready = true;
    if (!ready && core->ipc_is_listener != 0) {
        size_t count = core->ipc_is_listener == 1 ? core->ipc_client_count + 1U : 1U;
        for (size_t i = 0U; i < count; ++i) {
            if (atomic_load_explicit(&core->stop_requested, memory_order_acquire)) break;
            cog_ipc_endpoint_t *ep = i == 0U ? core->ipc_endpoint : core->ipc_clients[i-1U].endpoint;
            bool endpoint_ready = false;
            rc = cog_ipc_internal_ready(ep, &endpoint_ready);
            if (rc == COG_ERROR_AGAIN) ready = true;
            else if (rc != COG_OK) return rc;
            if (endpoint_ready) ready = true;
            rc = core_now(core, &now);
            if (rc != COG_OK) return rc;
            if (ready) break;
        }
    }
    info->next_deadline_ms = core->drive.deadline;
    info->max_wait_ms = core->max_idle_wait_ms;
    if (ready || info->next_deadline_ms <= now) {
        info->max_wait_ms = 0U;
        if (now < info->next_deadline_ms) info->next_deadline_ms = now;
    } else if (info->next_deadline_ms - now < info->max_wait_ms)
        info->max_wait_ms = (uint32_t)(info->next_deadline_ms - now);
    return COG_OK;
}
static cog_result_t core_slice(cog_core_t *core, cog_core_step_info_t *output)
{
    cog_core_step_info_t info = {0};
    cog_result_t rc = COG_OK;
    info.next_deadline_ms = UINT64_MAX;
    core->drive.inside_call = true;
    while (info.dispatched < core->step_event_limit) {
        uint64_t now;
        cog_event_t event;
        bool from_ipc;
        if (atomic_load_explicit(&core->stop_requested, memory_order_acquire)) break;
        rc = core_now(core, &now);
        if (rc != COG_OK) break;
        if (core->drive.service_due || core->drive.dispatched_since_service >= core->service_interval ||
            now >= core->drive.deadline || cog_event_queue_is_empty(core->event_queue)) {
            uint64_t next = UINT64_MAX;
            core->drive.deadline = UINT64_MAX;
            if (core->service_fn != NULL) {
                rc = core->service_fn(core->service_arg, now, &next);
                if (rc != COG_OK && rc != COG_ERROR_AGAIN) break;
                core->drive.deadline = next;
            }
            rc = COG_OK;
            if (atomic_load_explicit(&core->stop_requested, memory_order_acquire)) break;
            rc = core_ipc_step(core, &core->drive.deadline);
            if (rc != COG_OK) break;
            core->drive.dispatched_since_service = 0U;
            core->drive.service_due = false;
            if (atomic_load_explicit(&core->stop_requested, memory_order_acquire)) break;
        }
        /* Clock failures never dequeue an event whose lease cannot be handled. */
        rc = core_now(core, &now);
        if (rc != COG_OK) break;
        if (atomic_load_explicit(&core->stop_requested, memory_order_acquire)) break;
        from_ipc = core->prefer_ipc;
        rc = cog_event_queue_pop(from_ipc ? core->ipc_events : core->event_queue, &event);
        if (rc == COG_ERROR_AGAIN) {
            from_ipc = !from_ipc;
            rc = cog_event_queue_pop(from_ipc ? core->ipc_events : core->event_queue, &event);
        }
        core->prefer_ipc = !core->prefer_ipc;
        if (rc == COG_ERROR_AGAIN) {
            rc = core_inject_pending(core);
            if (rc == COG_OK) continue;
            if (rc == COG_ERROR_NOT_FOUND) rc = COG_OK;
            break;
        }
        if (rc != COG_OK) break;
        if (core->injected && event.payload == core->injected_payload) {
            core->pending_owners[core->injected_owner].leased = false;
            core->injected = false;
        }
        rc = core_dispatch(core, &event);
        cog_agent_intent_dispatched(core, &event);
        if (from_ipc && event.kind == COG_EVENT_IPC_MESSAGE && event.payload != NULL) {
            const cog_event_ipc_t *ipc = event.payload;
            core_ipc_release(core, ipc->msg_payload);
        }
        ++info.dispatched;
        ++core->drive.dispatched_since_service;
        if (rc != COG_OK) break;
    }
    if (rc == COG_OK && !atomic_load_explicit(&core->stop_requested, memory_order_acquire))
        rc = core_wait_info(core, &info);
    if (rc != COG_OK || atomic_load_explicit(&core->stop_requested, memory_order_acquire))
        core_finish(core, rc);
    info.done = core->drive.done;
    if (info.done) { info.max_wait_ms = 0U; info.next_deadline_ms = UINT64_MAX; }
    *output = info;
    core->drive.inside_call = false;
    return core->drive.done ? core->drive.result : COG_OK;
}
cog_result_t cog_core_drive_begin(cog_core_t *core)
{
    return core_begin(core, CORE_DRIVER_MANUAL);
}
cog_result_t cog_core_step(cog_core_t *core, cog_core_step_info_t *info)
{
    if (core == NULL || info == NULL) return COG_ERROR_NULL_PTR;
    if (core->drive.kind != CORE_DRIVER_MANUAL || core->drive.inside_call || core->drive.done)
        return COG_ERROR_INVALID_STATE;
    return core_slice(core, info);
}
cog_result_t cog_core_drive_end(cog_core_t *core)
{
    if (core == NULL) return COG_ERROR_NULL_PTR;
    if (core->drive.kind != CORE_DRIVER_MANUAL || core->drive.inside_call || !core->drive.done)
        return COG_ERROR_INVALID_STATE;
    return core_end(core);
}
cog_result_t cog_core_run(cog_core_t *core)
{
    cog_result_t rc = core_begin(core, CORE_DRIVER_RUN);
    if (rc != COG_OK) return rc;
    while (!core->drive.done) {
        cog_core_step_info_t info;
        rc = core_slice(core, &info);
        if (core->drive.done) break;
        if (info.dispatched != 0U) continue;
        if (atomic_load_explicit(&core->stop_requested, memory_order_acquire)) continue;
        core->drive.inside_call = true;
        if (core->wait_fn != NULL) { core->wait_fn(core->wait_arg); rc = COG_OK; }
        else if (core->idle_fn != NULL) rc = core->idle_fn(core->idle_arg, info.max_wait_ms);
        else {
            struct timespec pause = {(time_t)(info.max_wait_ms / 1000U),
                                     (long)(info.max_wait_ms % 1000U) * 1000000L};
            if (info.max_wait_ms != 0U) (void)nanosleep(&pause, NULL);
            rc = COG_OK;
        }
        if (rc != COG_OK && rc != COG_ERROR_AGAIN) core_finish(core, rc);
        core->drive.service_due = true;
        core->drive.inside_call = false;
    }
    return core_end(core);
}
void cog_core_stop(cog_core_t *core)
{
    if (core == NULL) {
        return;
    }
    /* Release so the run loop's acquire loads observe the stop request and
     * exit before dequeuing/injecting any pending message. */
    atomic_store_explicit(&core->stop_requested, true, memory_order_release);
    atomic_store_explicit(&core->running, false, memory_order_relaxed);
}

cog_result_t cog_core_enqueue(cog_core_t *core, const cog_event_t *event)
{
    if (core == NULL || event == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    if (event->kind == COG_EVENT_SUBAGENT_SPAWN || event->kind == COG_EVENT_SUBAGENT_CANCEL) {
        return COG_ERROR_INVALID_ARG;
    }
    if ((event->kind == COG_EVENT_IPC_MESSAGE || event->kind == COG_EVENT_IPC_CONNECTED ||
         event->kind == COG_EVENT_IPC_DISCONNECTED) && event->payload != NULL) {
        if (event->payload_len < sizeof(cog_event_ipc_t)) return COG_ERROR_INVALID_ARG;
        const cog_event_ipc_t *ipc = event->payload;
        if (ipc->peer_info_valid) return COG_ERROR_INVALID_ARG;
    }
    return cog_event_queue_push(core->event_queue, event);
}

cog_result_t cog_core_enqueue_agent_intent(cog_core_t *core, const cog_event_t *event)
{
    return cog_event_queue_push(core->event_queue, event);
}

/* ------------------------------------------------------------------ */
/* Inspectors and accessors                                            */
/* ------------------------------------------------------------------ */

cog_session_t *cog_core_session(cog_core_t *core)
{
    return (core != NULL) ? core->session : NULL;
}

cog_state_t cog_core_state(const cog_core_t *core)
{
    return (core != NULL) ? core->state : 0U;
}

bool cog_core_is_running(const cog_core_t *core)
{
    return (core != NULL) && atomic_load_explicit(&core->running, memory_order_relaxed);
}

void *cog_core_user(const cog_core_t *core)
{
    return (core != NULL) ? core->user : NULL;
}

cog_ipc_endpoint_t *cog_core_ipc(cog_core_t *core)
{
    return (core != NULL) ? core->ipc_endpoint : NULL;
}

cog_provider_t *cog_core_provider(cog_core_t *core)
{
    return (core != NULL) ? core->provider : NULL;
}

cog_result_t cog_core_set_provider(cog_core_t *core, cog_provider_t *provider)
{
    if (core == NULL) { return COG_ERROR_NULL_PTR; }
    core->provider = provider;
    return COG_OK;
}

cog_obs_t *cog_core_obs(cog_core_t *core)
{
    return (core != NULL) ? core->obs : NULL;
}

cog_store_t *cog_core_store(cog_core_t *core)
{
    return (core != NULL) ? core->store : NULL;
}

/* ------------------------------------------------------------------ */
/* Subagent registry host (internal; see agent_internal.h)             */
/* ------------------------------------------------------------------ */

cog_agent_registry_t *cog_core_agent_registry(cog_core_t *core)
{
    return (core != NULL) ? &core->agents : NULL;
}

cog_result_t cog_core_queue_message(cog_core_t *core, const char *session_id,
                                    const char *text, size_t text_len)
{
    size_t owner, i, n, slot;
    core_pending_owner_t *entry;
    if (core == NULL || session_id == NULL || text == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    n = strnlen(session_id, sizeof(core->pending_owners[0].id));
    if (n == sizeof(core->pending_owners[0].id) || text_len >= core->pending_bytes) {
        return COG_ERROR_LIMIT;
    }
    owner = core_pending_owner(core, session_id);
    if (owner == SIZE_MAX) {
        for (i = 0U; i < core->pending_owner_capacity; ++i) {
            if (core->pending_owners[i].count == 0U &&
                !core->pending_owners[i].leased) {
                break;
            }
        }
        if (i == core->pending_owner_capacity) {
            return COG_ERROR_LIMIT;
        }
        owner = i;
    }
    entry = &core->pending_owners[owner];
    if (entry->count + (entry->leased ? 1U : 0U) >= core->pending_per_owner) {
        return COG_ERROR_OVERFLOW;
    }
    memcpy(entry->id, session_id, n + 1U);
    slot = owner * core->pending_per_owner + entry->count;
    memmove(core_pending_text(core, owner, entry->count), text, text_len);
    core_pending_text(core, owner, entry->count)[text_len] = '\0';
    core->pending_items[slot].length = text_len;
    ++entry->count;
    return COG_OK;
}
cog_result_t cog_core_dequeue_message(cog_core_t *core, const char *session_id,
                                      char *out_buf, size_t capacity, size_t *out_len)
{
    size_t owner, length;
    if (core == NULL || session_id == NULL || out_buf == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    owner = core_pending_owner(core, session_id);
    if (owner == SIZE_MAX || core->pending_owners[owner].count == 0U) {
        return COG_ERROR_NOT_FOUND;
    }
    length = core->pending_items[owner * core->pending_per_owner].length;
    if (out_len != NULL) {
        *out_len = length;
    }
    if (length >= capacity) {
        return COG_ERROR_OVERFLOW;
    }
    memmove(out_buf, core_pending_text(core, owner, 0U), length + 1U);
    core_pending_remove(core, owner);
    return COG_OK;
}
cog_result_t cog_core_pending_count(const cog_core_t *core, const char *session_id,
                                    size_t *out_count)
{
    size_t owner;
    if (core == NULL || session_id == NULL || out_count == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    owner = core_pending_owner(core, session_id);
    *out_count = owner == SIZE_MAX ? 0U : core->pending_owners[owner].count;
    return COG_OK;
}

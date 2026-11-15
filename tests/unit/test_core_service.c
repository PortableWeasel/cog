/** @file test_core_service.c
 * @brief Sustained queue fairness, bounded waits, IPC arena and FIFO admission.
 */
#define _POSIX_C_SOURCE 200809L
#include <cog/core.h>
#include <cog/agent.h>
#include "../../libraries/cog-core/src/agent_internal.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct fixture {
    cog_core_t *core;
    cog_session_t *session;
    cog_ipc_endpoint_t *peer, *accepted;
    size_t handled, services, last_dispatch, maximum_gap, messages, connected;
    size_t stop_at;
    uint32_t idle_budget;
    int mode;
    cog_result_t failure;
    char drained[16], delivered[16];
    char deliveries[4][16];
    size_t delivery_count;
    int pending_action;
    cog_result_t pending_error;
} fixture_t;
static cog_handler_result_t handle(cog_core_t *core, const cog_event_t *event)
{
    fixture_t *f = cog_core_user(core);
    cog_handler_result_t result = {COG_TRANSITION_SELF, COG_OK};
    if (event->kind == COG_EVENT_IPC_CONNECTED) {
        ++f->connected;
    } else if (event->kind == COG_EVENT_IPC_MESSAGE) {
        const cog_event_ipc_t *ipc = event->payload;
        assert(ipc->msg_payload_len == 5U);
        assert(memcmp(ipc->msg_payload, "hello", 5U) == 0);
        ++f->messages;
    } else {
        cog_event_t next = *event;
        if (f->mode == 4) {
            assert(event->payload_len < sizeof(f->delivered));
            memcpy(f->delivered, event->payload, event->payload_len);
            f->delivered[event->payload_len] = '\0';
            assert(f->delivery_count < 4U);
            memcpy(f->deliveries[f->delivery_count++], f->delivered,
                   event->payload_len + 1U);
        }
        ++f->handled;
        if (f->mode == 1 || f->mode == 3) {
            assert(cog_core_enqueue(core, &next) == COG_OK);
        }
    }
    if (f->handled >= f->stop_at && (f->peer == NULL || f->messages >= 2U)) {
        cog_core_stop(core);
    }
    return result;
}
static const cog_state_t transitions[] = {COG_TRANSITION_SELF, COG_TRANSITION_END};
static const cog_dispatch_entry_t table[] = {
    COG_DISPATCH_ENTRY(0U, COG_EVENT_USER_INPUT, handle, transitions),
    COG_DISPATCH_ENTRY(0U, COG_EVENT_IPC_MESSAGE, handle, transitions),
    COG_DISPATCH_ENTRY(0U, COG_EVENT_IPC_CONNECTED, handle, transitions),
    COG_DISPATCH_ENTRY(0U, COG_EVENT_IPC_DISCONNECTED, handle, transitions),
};
static cog_result_t service(void *arg, uint64_t now, uint64_t *deadline)
{
    fixture_t *f = arg;
    size_t gap = f->handled - f->last_dispatch;
    if (gap > f->maximum_gap) {
        f->maximum_gap = gap;
    }
    f->last_dispatch = f->handled;
    ++f->services;
    *deadline = UINT64_MAX;
    if (f->mode == 4) {
        *deadline = now;
        if (f->services == 2U) {
            const char *id = cog_session_id(f->session);
            size_t count = SIZE_MAX;
            assert(cog_core_pending_count(f->core, id, &count) == COG_OK);
            assert(count == 1U); /* Injected first is already logically removed. */
            assert(cog_core_dequeue_message(f->core, id, f->drained, sizeof(f->drained),
                                            NULL) == COG_OK);
            if (f->pending_action != 0) {
                char untouched[16] = "untouched";
                assert(cog_core_dequeue_message(f->core, id, untouched,
                                                sizeof(untouched),
                                                NULL) == COG_ERROR_NOT_FOUND);
                assert(strcmp(untouched, "untouched") == 0);
                assert(cog_core_pending_count(f->core, id, &count) == COG_OK &&
                       count == 0U);
                assert(cog_core_queue_message(f->core, "other-owner", "other", 5U) ==
                       COG_OK);
                /* An empty logical FIFO still has a stable owner lease. */
                assert(cog_core_queue_message(f->core, "third-owner", "third", 5U) ==
                       COG_ERROR_LIMIT);
                if (f->pending_action >= 2) {
                    assert(cog_core_queue_message(f->core, id, "replacement", 11U) ==
                           COG_OK);
                    /* One physical slot stays reserved for restoration. */
                    assert(cog_core_queue_message(f->core, id, "extra", 5U) ==
                           COG_ERROR_OVERFLOW);
                }
                if (f->pending_action == 2) {
                    cog_core_stop(f->core);
                }
                if (f->pending_action == 3) {
                    return f->pending_error;
                }
            }
        }
    }
    if (f->peer != NULL) {
        uint64_t next;
        cog_result_t rc = cog_ipc_endpoint_service(f->peer, now, &next);
        assert(rc == COG_OK || rc == COG_ERROR_AGAIN);
        if (f->accepted != NULL) {
            rc = cog_ipc_endpoint_service(f->accepted, now, &next);
            assert(rc == COG_OK || rc == COG_ERROR_AGAIN);
        }
    }
    if (f->mode == 2) {
        *deadline = now;
    }
    if (f->mode == 3 && f->services == 4U) {
        cog_core_stop(f->core);
    }
    return f->failure;
}
static cog_result_t idle(void *arg, uint32_t budget)
{
    fixture_t *f = arg;
    f->idle_budget = budget;
    cog_core_stop(f->core);
    return f->failure;
}
static void create(fixture_t *f, cog_core_config_t *cfg)
{
    cog_session_config_t scfg = {0};
    scfg.id = "core-service";
    assert(cog_session_new(&f->session, &scfg) == COG_OK);
    cfg->session = f->session;
    cfg->dispatch_table = table;
    cfg->dispatch_table_size = sizeof(table) / sizeof(table[0]);
    cfg->user = f;
    cfg->service_fn = service;
    cfg->service_arg = f;
    cfg->service_interval = 3U;
    cfg->event_queue_capacity = 2U;
    cfg->pending_session_capacity = 2U;
    cfg->pending_messages_per_session = 2U;
    cfg->pending_message_bytes = 16U;
    cfg->max_agents = 2U;
    cfg->max_agent_message_bytes = 64U;
    assert(cog_core_new(&f->core, cfg) == COG_OK);
}
static void destroy(fixture_t *f)
{
    cog_core_free(f->core);
    cog_session_free(f->session);
}
static void queued_fairness(void)
{
    fixture_t f = {0};
    cog_core_config_t cfg = {0};
    cog_event_t event = {COG_EVENT_USER_INPUT, COG_EVENT_USER_INPUT, NULL, 0U, 0U};
    f.mode = 1;
    f.stop_at = 101U;
    create(&f, &cfg);
    assert(cog_core_enqueue(f.core, &event) == COG_OK);
    assert(cog_core_enqueue(f.core, &event) == COG_OK);
    assert(cog_core_run(f.core) == COG_OK);
    assert(f.handled == 101U);
    assert(f.services >= 34U);
    assert(f.maximum_gap <= 3U);
    destroy(&f);
    memset(&f, 0, sizeof(f));
    memset(&cfg, 0, sizeof(cfg));
    f.mode = 3;
    f.stop_at = 1000U;
    create(&f, &cfg);
    assert(cog_core_enqueue(f.core, &event) == COG_OK);
    assert(cog_core_run(f.core) == COG_OK);
    assert(f.services == 4U);
    assert(f.handled <= 9U);
    destroy(&f);
}
static void hook_errors(void)
{
    fixture_t f = {0};
    cog_core_config_t cfg = {0};
    f.failure = COG_ERROR_PROTOCOL;
    create(&f, &cfg);
    assert(cog_core_run(f.core) == COG_ERROR_PROTOCOL);
    destroy(&f);
    memset(&f, 0, sizeof(f));
    memset(&cfg, 0, sizeof(cfg));
    f.mode = 2;
    cfg.idle_fn = idle;
    cfg.idle_arg = &f;
    cfg.max_idle_wait_ms = 7U;
    create(&f, &cfg);
    assert(cog_core_run(f.core) == COG_OK);
    assert(f.idle_budget == 0U);
    destroy(&f);
}
static void fifo_limits(void)
{
    fixture_t f = {0};
    cog_core_config_t cfg = {0};
    char out[16];
    size_t length;
    create(&f, &cfg);
    assert(cog_core_queue_message(f.core, "one", "first", 5U) == COG_OK);
    assert(cog_core_queue_message(f.core, "one", "second", 6U) == COG_OK);
    assert(cog_core_queue_message(f.core, "one", "third", 5U) == COG_ERROR_OVERFLOW);
    assert(cog_core_queue_message(f.core, "two", "ok", 2U) == COG_OK);
    assert(cog_core_queue_message(f.core, "three", "ok", 2U) == COG_ERROR_LIMIT);
    assert(cog_core_dequeue_message(f.core, "one", out, 2U, &length) ==
           COG_ERROR_OVERFLOW);
    assert(length == 5U);
    assert(cog_core_dequeue_message(f.core, "one", out, sizeof(out), &length) ==
           COG_OK);
    assert(strcmp(out, "first") == 0);
    assert(cog_core_dequeue_message(f.core, "one", out, sizeof(out), &length) ==
           COG_OK);
    assert(strcmp(out, "second") == 0);
    assert(cog_core_queue_message(f.core, "three", "ok", 2U) == COG_OK);
    assert(cog_core_queue_message(f.core, "three", "too long", SIZE_MAX) ==
           COG_ERROR_LIMIT);
    destroy(&f);
}
static void ipc_fairness(void)
{
    fixture_t f = {0};
    cog_core_config_t cfg = {0};
    cog_ipc_endpoint_config_t ipc_cfg = {0};
    cog_ipc_endpoint_t *listener;
    char path[] = "/tmp/cog-q-core-XXXXXX";
    unsigned char text[] = "hello";
    cog_ipc_message_t message = {text, 5U, sizeof(text)};
    cog_event_t event = {COG_EVENT_USER_INPUT, COG_EVENT_USER_INPUT, NULL, 0U, 0U};
    int fd = mkstemp(path);
    assert(fd >= 0);
    assert(close(fd) == 0);
    assert(unlink(path) == 0);
    ipc_cfg.max_frame_bytes = 16U;
    assert(cog_ipc_endpoint_new_ex(&listener, &ipc_cfg) == COG_OK);
    assert(cog_ipc_endpoint_new_ex(&f.peer, &ipc_cfg) == COG_OK);
    assert(cog_ipc_endpoint_listen(listener, path) == COG_OK);
    assert(cog_ipc_endpoint_connect(f.peer, path, NULL) == COG_ERROR_AGAIN);
    assert(cog_ipc_endpoint_accept(listener, &f.accepted, NULL) == COG_OK);
    {
        uint64_t deadline;
        assert(cog_ipc_endpoint_service(f.accepted, 0U, &deadline) == COG_OK);
        assert(cog_ipc_endpoint_service(f.peer, 0U, &deadline) == COG_OK);
    }
    assert(cog_ipc_endpoint_send(f.peer, &message) == COG_OK);
    assert(cog_ipc_endpoint_send(f.peer, &message) == COG_OK);
    cfg.ipc_endpoint = f.accepted;
    cfg.ipc_max_frame_bytes = 16U;
    cfg.ipc_payload_bytes = 16U;
    cfg.ipc_service_quantum = 1U;
    f.mode = 1;
    f.stop_at = 60U;
    create(&f, &cfg);
    assert(cog_core_enqueue(f.core, &event) == COG_OK);
    assert(cog_core_enqueue(f.core, &event) == COG_OK);
    assert(cog_core_run(f.core) == COG_OK);
    assert(f.messages == 2U);
    assert(f.maximum_gap <= 3U);
    destroy(&f);
    assert(cog_ipc_endpoint_free(f.peer) == COG_OK);
    assert(cog_ipc_endpoint_free(listener) == COG_OK);
}
static void agent_reservations(void)
{
    fixture_t f = {0};
    cog_core_config_t cfg = {0};
    cog_agent_config_t agent = {0};
    cog_agent_id_t first = 0U, second = 0U, unchanged = 777U;
    const cog_agent_entry_t *entry;
    char task[] = "task", status[] = "ok";
    char result[8];
    size_t length;
    create(&f, &cfg);
    agent.task = task;
    agent.force_local = true;
    agent.max_status_bytes = 4U;
    agent.max_result_bytes = 4U;
    assert(cog_agent_spawn(f.core, &agent, &first) == COG_OK);
    task[0] = 'T';
    entry = cog_agent_internal_get(f.core, first);
    assert(entry != NULL && strcmp(entry->task, "task") == 0);
    assert(cog_agent_notify_parent(f.core, first, status) == COG_OK);
    status[0] = 'X';
    assert(strcmp(entry->status_msg, "ok") == 0);
    assert(cog_agent_notify_parent(f.core, first, "long") == COG_ERROR_LIMIT);
    assert(strcmp(entry->status_msg, "ok") == 0);
    assert(cog_agent_internal_start(f.core, first) == COG_OK);
    assert(cog_agent_internal_complete(f.core, first, "long", 4U) == COG_ERROR_LIMIT);
    assert(cog_agent_internal_complete(f.core, first, "yes", 3U) == COG_OK);
    assert(cog_agent_result(f.core, first, result, sizeof(result), &length) == COG_OK);
    assert(length == 3U && strcmp(result, "yes") == 0);
    assert(cog_agent_spawn(f.core, &agent, &second) == COG_OK);
    assert(cog_agent_spawn(f.core, &agent, &unchanged) == COG_ERROR_LIMIT);
    assert(unchanged == 777U && second != first);
    destroy(&f);
}
static void pending_drain_during_injection(void)
{
    fixture_t f = {0};
    cog_core_config_t cfg = {0};
    size_t remaining;
    f.mode = 4;
    f.stop_at = 1U;
    create(&f, &cfg);
    assert(cog_core_queue_message(f.core, cog_session_id(f.session), "first", 5U) ==
           COG_OK);
    assert(cog_core_queue_message(f.core, cog_session_id(f.session), "second", 6U) ==
           COG_OK);
    assert(cog_core_run(f.core) == COG_OK);
    /* Public draining at the service boundary must neither deliver the
     * already injected prompt twice nor discard a different FIFO entry. */
    assert(strcmp(f.drained, "second") == 0 && strcmp(f.delivered, "first") == 0);
    assert(cog_core_pending_count(f.core, cog_session_id(f.session), &remaining) ==
           COG_OK);
    assert(remaining == 0U);
    destroy(&f);
}
static void pending_lease_survives_public_mutation(void)
{
    int action;
    for (action = 1; action <= 3; ++action) {
        fixture_t f = {0};
        cog_core_config_t cfg = {0};
        const char *id;
        size_t count;
        f.mode = 4;
        f.stop_at = 1U;
        f.pending_action = action;
        f.pending_error = COG_ERROR_PROTOCOL;
        create(&f, &cfg);
        id = cog_session_id(f.session);
        assert(cog_core_queue_message(f.core, id, "first", 5U) == COG_OK);
        assert(cog_core_queue_message(f.core, id, "second", 6U) == COG_OK);
        assert(cog_core_run(f.core) == (action == 3 ? COG_ERROR_PROTOCOL : COG_OK));
        assert(strcmp(f.drained, "second") == 0);
        if (action == 1) {
            assert(f.delivery_count == 1U && strcmp(f.deliveries[0], "first") == 0);
            assert(cog_core_pending_count(f.core, id, &count) == COG_OK && count == 0U);
            /* Dispatch releases the lease, permitting an empty owner to be reused. */
            assert(cog_core_queue_message(f.core, "third-owner", "third", 5U) ==
                   COG_OK);
        } else {
            assert(f.delivery_count == 0U && f.handled == 0U);
            assert(cog_core_pending_count(f.core, id, &count) == COG_OK && count == 2U);
            f.stop_at = 2U;
            assert(cog_core_run(f.core) == COG_OK);
            assert(f.delivery_count == 2U && f.handled == 2U);
            assert(strcmp(f.deliveries[0], "first") == 0);
            assert(strcmp(f.deliveries[1], "replacement") == 0);
            assert(cog_core_pending_count(f.core, id, &count) == COG_OK && count == 0U);
        }
        assert(cog_core_pending_count(f.core, "other-owner", &count) == COG_OK &&
               count == 1U);
        destroy(&f);
    }
}
int main(void)
{
    pending_lease_survives_public_mutation();
    pending_drain_during_injection();
    agent_reservations();
    queued_fairness();
    hook_errors();
    fifo_limits();
    ipc_fairness();
    puts("core service tests passed");
    return 0;
}

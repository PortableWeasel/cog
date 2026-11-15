/**
 * @file core.c
 * @brief Agent core implementation for cog-core.
 *
 * Implements the public contract declared in @c cog/core.h: construction
 * and destruction, the agent event loop (@c cog_core_run), the stop
 * request, event enqueue, the state/running inspectors, the provider /
 * obs / store / ipc accessors, and the session-scoped pending-message
 * FIFO (queue / dequeue / pending_count).
 *
 * Design notes (the full executable contract is recorded in
 * libraries/cog-core/contracts/core/CONTRACT.md):
 *
 * - The core owns its event queue, its pending-message FIFO, and (when
 *   configured) its IPC endpoint.  The session, provider, obs, and store
 *   are borrowed (the caller retains ownership).  A non-NULL
 *   config->ipc_endpoint is accepted and owned by the core: it is drained
 *   during the event loop (incoming connections and messages are enqueued
 *   as COG_EVENT_IPC_* events) and destroyed by cog_core_free.
 * - The run loop pops one event, looks up the (state, event) row in the
 *   static dispatch table, invokes the handler, validates the requested
 *   transition against the row's allowed list, and applies it.  A
 *   non-COG_OK handler status or a denied transition aborts the loop and
 *   is returned to the caller.  When the queue is empty the loop first
 *   tries to inject a pending message for the core's session (delivering
 *   it as a COG_EVENT_USER_INPUT), then falls back to the caller's wait
 *   hook (or a short yield when none is supplied).
 * - The pending FIFO is a single bounded array (default capacity 8)
 *   whose entries are tagged with a session id.  queue_message appends;
 *   dequeue_message / pending_count filter by session id, giving
 *   per-session isolation while keeping the whole FIFO bounded.  The
 *   core auto-injects the oldest pending message for its own session
 *   when the event queue drains (i.e. after the current turn's events
 *   have been processed).
 * - Session attachment: the core marks the session's private `attached`
 *   flag so cog_session_reset refuses while the core is alive, and
 *   clears it on cog_core_free.  The flag lives in the private
 *   session_internal.h layout shared with session.c (there is no public
 *   attach API); see session_internal.h.
 * - Subagent registry host: the core embeds the subagent registry
 *   (agent_internal.h) and exposes it through cog_core_agent_registry().
 *   agent.c owns every operation on it; cog_core_free releases it via
 *   cog_agent_registry_reset().
 */
#define _POSIX_C_SOURCE 200809L

#include "cog/core.h"

#include "agent_internal.h"
#include "session_internal.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/** Default event-queue capacity when config->event_queue_capacity is zero. */
#define COG_CORE_DEFAULT_EVENT_QUEUE_CAPACITY ((size_t)64)

/** Default pending-message FIFO capacity (per the header: 8 messages). */
#define COG_CORE_DEFAULT_PENDING_CAPACITY ((size_t)8)

/**
 * @brief Capacity of the core-owned IPC receive buffer.
 *
 * Matches the IPC layer's maximum frame payload (UINT16_MAX), so the
 * core accepts every valid frame.  A frame larger than this is rejected
 * by cog_ipc_endpoint_recv with COG_ERROR_OVERFLOW and the connection is
 * closed (reported as a disconnect).
 */
#define COG_CORE_IPC_RECV_CAPACITY ((size_t)UINT16_MAX)

/* ------------------------------------------------------------------ */
/* Internal structures                                                 */
/* ------------------------------------------------------------------ */

/** One entry in the session-scoped pending-message FIFO. */
typedef struct cog_pending_msg {
    char *session_id;  /**< Owned copy of the target session id. */
    char *text;        /**< Owned copy of the message text, NUL-terminated. */
    size_t text_len;   /**< Byte length of @p text, excluding the NUL. */
} cog_pending_msg_t;

/**
 * @brief Opaque core layout (definition of @c cog_core_t).
 *
 * The session, provider, obs, and store are borrowed.  The event queue,
 * the pending FIFO, and the subagent registry are owned.  The IPC
 * endpoint is owned per the header but its release is deferred to the
 * IPC phase.
 */
struct cog_core {
    cog_session_t *session;
    const cog_dispatch_entry_t *dispatch_table;
    size_t dispatch_table_size;
    const cog_tool_table_t *tool_table;
    cog_event_queue_t *event_queue;
    cog_wait_fn wait_fn;
    void *wait_arg;
    cog_ipc_endpoint_t *ipc_endpoint;
    cog_peer_id_t local_peer_id;
    int ipc_is_listener;          /**< 1=listener, 2=connected, 0=unbound. */
    cog_ipc_endpoint_t **ipc_clients; /**< Owned accepted clients (listener). */
    size_t ipc_client_count;      /**< Live entries in @p ipc_clients. */
    size_t ipc_client_capacity;   /**< Allocated slots in @p ipc_clients. */
    cog_ipc_message_t ipc_recv;   /**< Owned receive buffer. */
    void **ipc_inflight;          /**< Owned in-flight IPC payload copies. */
    size_t ipc_inflight_count;    /**< Live entries in @p ipc_inflight. */
    size_t ipc_inflight_capacity; /**< Allocated slots in @p ipc_inflight. */
    cog_provider_t *provider;
    cog_obs_t *obs;
    cog_store_t *store;
    cog_state_t state;
    bool running;
    bool stop_requested;
    cog_pending_msg_t *pending;   /**< Owned FIFO array. */
    size_t pending_capacity;      /**< Allocated entries in @p pending. */
    size_t pending_count;         /**< Live entries in @p pending. */
    void *injected_payload;       /**< Text of the pending message currently
                                       injected into the event queue; freed
                                       after dispatch. */
    cog_agent_registry_t agents;  /**< Subagent registry; see agent_internal.h. */
};

/* ------------------------------------------------------------------ */
/* Internal helpers                                                    */
/* ------------------------------------------------------------------ */

/** Yield the CPU briefly (used when no wait hook is configured). */
static void core_yield(void)
{
    struct timespec ts;

    ts.tv_sec = 0;
    ts.tv_nsec = 1000000L; /* 1 ms */
    (void)nanosleep(&ts, NULL);
}

/**
 * @brief Dispatch one event through the static table.
 *
 * Looks up the (state, event->id) row, invokes the handler, validates the
 * requested transition against the row's allowed list, and applies it.
 * Returns the handler status or the transition-validation result on
 * failure; the state is left unchanged on any failure.
 */
static cog_result_t core_dispatch(cog_core_t *core, const cog_event_t *event)
{
    const cog_dispatch_entry_t *entry = NULL;
    cog_handler_result_t hr;
    cog_result_t rc;

    rc = cog_state_lookup(core->dispatch_table, core->dispatch_table_size,
                          core->state, event->id, &entry);
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
    if (event->kind == COG_EVENT_LLM_COMPLETE &&
        core->store != NULL &&
        cog_store_auto_save(core->store)) {
        return cog_store_save(core->store, core->session);
    }

    return COG_OK;
}

/**
 * @brief Inject the oldest pending message for the core's session.
 *
 * Called when the event queue is empty.  If the core's session has a
 * pending message, the oldest one is dequeued and enqueued as a
 * COG_EVENT_USER_INPUT whose payload borrows the message text.  The text
 * ownership transfers to @c core->injected_payload and is freed after the
 * event is dispatched.
 *
 * @return COG_OK if a message was injected.
 * @return COG_ERROR_NOT_FOUND if the session has no pending message.
 * @return Another error code if the enqueue fails (the message is left in
 *         the FIFO).
 */
static cog_result_t core_inject_pending(cog_core_t *core)
{
    const char *sid = cog_session_id(core->session);
    size_t i;
    cog_pending_msg_t *msg;
    cog_event_t event;
    cog_result_t rc;

    if (sid == NULL) {
        return COG_ERROR_NOT_FOUND;
    }

    for (i = 0U; i < core->pending_count; ++i) {
        if (strcmp(core->pending[i].session_id, sid) == 0) {
            break;
        }
    }
    if (i == core->pending_count) {
        return COG_ERROR_NOT_FOUND;
    }

    msg = &core->pending[i];

    event.id = (cog_event_id_t)COG_EVENT_USER_INPUT;
    event.kind = COG_EVENT_USER_INPUT;
    event.payload = msg->text;
    event.payload_len = msg->text_len;
    event.source_id = 0U;

    rc = cog_event_queue_push(core->event_queue, &event);
    if (rc != COG_OK) {
        return rc; /* queue full; the message stays in the FIFO */
    }

    /* Transfer text ownership to the core and remove the entry. */
    core->injected_payload = msg->text;
    free(msg->session_id);
    for (; i + 1U < core->pending_count; ++i) {
        core->pending[i] = core->pending[i + 1U];
    }
    --core->pending_count;
    return COG_OK;
}

/* ------------------------------------------------------------------ */
/* IPC integration                                                     */
/* ------------------------------------------------------------------ */

/**
 * @brief Probe the role of an IPC endpoint.
 *
 * Returns 1 for a listener, 2 for a connected endpoint, and 0 for an
 * unbound endpoint.  A connected endpoint reports a peer id; a listener
 * reports none but accepts poll on its bound socket; an unbound endpoint
 * has no file descriptor and fails poll with COG_ERROR_INVALID_STATE.
 */
static int core_ipc_probe_role(cog_ipc_endpoint_t *ep)
{
    cog_result_t rc;
    cog_peer_id_t peer_id = COG_PEER_ID_INVALID;

    rc = cog_ipc_endpoint_get_peer_id(ep, &peer_id);
    if (rc == COG_OK) {
        return 2; /* connected */
    }
    rc = cog_ipc_endpoint_poll(ep, COG_IPC_POLL_IN, 0, NULL);
    if (rc == COG_OK || rc == COG_ERROR_AGAIN) {
        return 1; /* listener */
    }
    return 0; /* unbound */
}

/**
 * @brief Track an in-flight IPC payload copy.
 *
 * @return COG_OK on success, COG_ERROR_NOMEM if the list cannot grow.
 */
static cog_result_t core_ipc_inflight_add(cog_core_t *core, void *ptr)
{
    if (core->ipc_inflight_count >= core->ipc_inflight_capacity) {
        size_t new_capacity = core->ipc_inflight_capacity == 0U
                                  ? 8U
                                  : core->ipc_inflight_capacity * 2U;
        void **new_list = (void **)realloc(
            core->ipc_inflight, new_capacity * sizeof(*new_list));

        if (new_list == NULL) {
            return COG_ERROR_NOMEM;
        }
        core->ipc_inflight = new_list;
        core->ipc_inflight_capacity = new_capacity;
    }
    core->ipc_inflight[core->ipc_inflight_count] = ptr;
    ++core->ipc_inflight_count;
    return COG_OK;
}

/**
 * @brief Remove an in-flight IPC payload copy from the tracking list.
 */
static void core_ipc_inflight_remove(cog_core_t *core, void *ptr)
{
    size_t i;

    for (i = 0U; i < core->ipc_inflight_count; ++i) {
        if (core->ipc_inflight[i] == ptr) {
            if (i + 1U < core->ipc_inflight_count) {
                (void)memmove(&core->ipc_inflight[i],
                              &core->ipc_inflight[i + 1U],
                              (core->ipc_inflight_count - i - 1U) *
                                  sizeof(core->ipc_inflight[0]));
            }
            --core->ipc_inflight_count;
            return;
        }
    }
}

/**
 * @brief Release an in-flight IPC payload copy (if tracked).
 */
static void core_ipc_inflight_free(cog_core_t *core, void *ptr)
{
    if (ptr == NULL) {
        return;
    }
    core_ipc_inflight_remove(core, ptr);
    free(ptr);
}

/**
 * @brief Track an accepted client endpoint (listener case).
 *
 * @return COG_OK on success, COG_ERROR_NOMEM if the list cannot grow.
 */
static cog_result_t core_ipc_client_add(cog_core_t *core,
                                        cog_ipc_endpoint_t *client)
{
    if (core->ipc_client_count >= core->ipc_client_capacity) {
        size_t new_capacity = core->ipc_client_capacity == 0U
                                  ? 4U
                                  : core->ipc_client_capacity * 2U;
        cog_ipc_endpoint_t **new_list = (cog_ipc_endpoint_t **)realloc(
            core->ipc_clients, new_capacity * sizeof(*new_list));

        if (new_list == NULL) {
            return COG_ERROR_NOMEM;
        }
        core->ipc_clients = new_list;
        core->ipc_client_capacity = new_capacity;
    }
    core->ipc_clients[core->ipc_client_count] = client;
    ++core->ipc_client_count;
    return COG_OK;
}

/**
 * @brief Remove and free an accepted client endpoint.
 *
 * Freeing the client disconnects it, which removes it from the owning
 * listener's peer table (see ipc.c).
 */
static void core_ipc_client_remove(cog_core_t *core,
                                   cog_ipc_endpoint_t *client)
{
    size_t i;

    for (i = 0U; i < core->ipc_client_count; ++i) {
        if (core->ipc_clients[i] == client) {
            if (i + 1U < core->ipc_client_count) {
                (void)memmove(&core->ipc_clients[i],
                              &core->ipc_clients[i + 1U],
                              (core->ipc_client_count - i - 1U) *
                                  sizeof(core->ipc_clients[0]));
            }
            --core->ipc_client_count;
            break;
        }
    }
    (void)cog_ipc_endpoint_free(client);
}

/**
 * @brief Enqueue a COG_EVENT_IPC_* event.
 *
 * Copies @p payload (if any) into a core-owned buffer so the message
 * bytes survive until the event is dispatched, and tracks that buffer in
 * the core's in-flight list so it is released after dispatch.  The
 * cog_event_ipc_t struct itself is snapshotted by the event queue (see
 * event.h / the event CONTRACT.md), so only the inner msg_payload needs
 * core-managed lifetime.
 *
 * @return COG_OK on success.
 * @return COG_ERROR_NOMEM if the payload copy or in-flight tracking fails.
 * @return COG_ERROR_OVERFLOW if the event queue is full (the payload copy
 *         is released; the caller should stop draining).
 */
static cog_result_t core_ipc_enqueue(cog_core_t *core,
                                     cog_event_kind_t kind,
                                     cog_peer_id_t peer,
                                     const void *payload,
                                     size_t payload_len)
{
    cog_event_t event;
    cog_event_ipc_t ipc;
    void *copy = NULL;
    cog_result_t rc;

    if (payload_len > 0U) {
        copy = malloc(payload_len);
        if (copy == NULL) {
            return COG_ERROR_NOMEM;
        }
        (void)memcpy(copy, payload, payload_len);
        rc = core_ipc_inflight_add(core, copy);
        if (rc != COG_OK) {
            free(copy);
            return rc;
        }
    }

    ipc.peer = peer;
    ipc.msg_type = 0U;
    ipc.msg_payload = copy;
    ipc.msg_payload_len = payload_len;

    event.id = (cog_event_id_t)kind;
    event.kind = kind;
    event.payload = &ipc;
    event.payload_len = sizeof(ipc);
    event.source_id = 0U;

    rc = cog_event_queue_push(core->event_queue, &event);
    if (rc != COG_OK) {
        if (copy != NULL) {
            core_ipc_inflight_remove(core, copy);
            free(copy);
        }
        return rc;
    }
    return COG_OK;
}

/**
 * @brief Poll and receive one message from a connected endpoint.
 *
 * @param[in,out] core  Core owning the endpoint and the receive buffer.
 * @param[in,out] ep    Connected endpoint to read from.
 * @param[in]     owned Non-zero if @p ep is a core-owned accepted client
 *                      that must be freed on disconnect; zero for the
 *                      core's own configured endpoint.
 * @return COG_OK if a message was received and enqueued, or if nothing
 *         was ready, or (owned == 0) if the endpoint disconnected.
 * @return COG_ERROR_IO if an owned client disconnected and was freed.
 * @return COG_ERROR_OVERFLOW if the event queue is full (stop draining).
 * @return COG_ERROR_NOMEM if a payload copy or tracking allocation fails.
 */
static cog_result_t core_ipc_recv_one(cog_core_t *core,
                                      cog_ipc_endpoint_t *ep,
                                      int owned)
{
    cog_result_t rc;
    int revents = 0;
    cog_peer_id_t peer_id = COG_PEER_ID_INVALID;

    rc = cog_ipc_endpoint_poll(ep, COG_IPC_POLL_IN, 0, &revents);
    if (rc == COG_ERROR_INVALID_STATE) {
        return COG_OK; /* endpoint already disconnected; nothing to do */
    }
    if (rc != COG_OK || (revents & (COG_IPC_POLL_IN | COG_IPC_POLL_ERR)) == 0) {
        return COG_OK; /* timeout or transient; nothing ready */
    }

    (void)cog_ipc_endpoint_get_peer_id(ep, &peer_id);

    rc = cog_ipc_endpoint_recv(ep, &core->ipc_recv);
    if (rc == COG_OK) {
        return core_ipc_enqueue(core, COG_EVENT_IPC_MESSAGE, peer_id,
                                core->ipc_recv.data, core->ipc_recv.len);
    }

    /* Disconnect or malformed frame: ipc.c has already closed the
     * connection.  Report the disconnect.  For an owned client, free it
     * (removing it from the listener's peer table). */
    if (owned) {
        core_ipc_client_remove(core, ep);
        (void)core_ipc_enqueue(core, COG_EVENT_IPC_DISCONNECTED, peer_id,
                               NULL, 0U);
        return COG_ERROR_IO;
    }
    (void)core_ipc_enqueue(core, COG_EVENT_IPC_DISCONNECTED, peer_id, NULL,
                           0U);
    return COG_OK;
}

/**
 * @brief Accept all pending connections on the core's listener.
 *
 * Each accepted client is tracked and reported with a
 * COG_EVENT_IPC_CONNECTED event.  Stops accepting when no connection is
 * pending or the event queue is full.
 *
 * @return COG_OK on success.
 * @return COG_ERROR_NOMEM if a client-list or event allocation fails.
 * @return COG_ERROR_IO if accept fails.
 * @return COG_ERROR_OVERFLOW if the event queue is full (stop draining).
 */
static cog_result_t core_ipc_accept_pending(cog_core_t *core)
{
    for (;;) {
        cog_ipc_endpoint_t *client = NULL;
        cog_peer_id_t peer_id = COG_PEER_ID_INVALID;
        cog_result_t rc;
        int revents = 0;

        rc = cog_ipc_endpoint_poll(core->ipc_endpoint, COG_IPC_POLL_IN, 0,
                                   &revents);
        if (rc != COG_OK || (revents & COG_IPC_POLL_IN) == 0) {
            break; /* no pending connection */
        }

        rc = cog_ipc_endpoint_accept(core->ipc_endpoint, &client, &peer_id);
        if (rc == COG_ERROR_AGAIN) {
            break; /* raced; no pending connection */
        }
        if (rc != COG_OK) {
            return rc; /* accept failed */
        }

        rc = core_ipc_client_add(core, client);
        if (rc != COG_OK) {
            (void)cog_ipc_endpoint_free(client);
            return rc;
        }

        rc = core_ipc_enqueue(core, COG_EVENT_IPC_CONNECTED, peer_id, NULL,
                             0U);
        if (rc != COG_OK) {
            /* Queue full or allocation failure: stop accepting.  The
             * client stays tracked and will be drained for messages on a
             * later pass. */
            return rc;
        }
    }
    return COG_OK;
}

/**
 * @brief Drain every accepted client of the core's listener.
 *
 * @return COG_OK on success.
 * @return COG_ERROR_NOMEM if a payload copy or tracking allocation fails.
 * @return COG_ERROR_OVERFLOW if the event queue is full (stop draining).
 */
static cog_result_t core_ipc_drain_clients(cog_core_t *core)
{
    size_t i = 0U;

    while (i < core->ipc_client_count) {
        cog_result_t rc = core_ipc_recv_one(core, core->ipc_clients[i], 1);

        if (rc == COG_ERROR_IO) {
            continue; /* client freed; the list shifted, @p i is current */
        }
        if (rc != COG_OK) {
            return rc;
        }
        ++i;
    }
    return COG_OK;
}

/**
 * @brief Drain the core's IPC fabric.
 *
 * Accepts pending connections and receives messages on the core's own
 * endpoint (connected case) or its accepted clients (listener case),
 * enqueuing COG_EVENT_IPC_* events.  A full event queue is not an error:
 * the drain stops and the loop dispatches what it has.
 *
 * @return COG_OK on success.
 * @return COG_ERROR_NOMEM if an allocation fails.
 * @return COG_ERROR_IO if accept fails.
 */
static cog_result_t core_ipc_drain(cog_core_t *core)
{
    cog_result_t rc;

    if (core->ipc_endpoint == NULL) {
        return COG_OK;
    }

    if (core->ipc_is_listener == 1) {
        rc = core_ipc_accept_pending(core);
        if (rc == COG_ERROR_OVERFLOW) {
            return COG_OK;
        }
        if (rc != COG_OK) {
            return rc;
        }
        rc = core_ipc_drain_clients(core);
        if (rc == COG_ERROR_OVERFLOW) {
            return COG_OK;
        }
        return rc;
    }

    if (core->ipc_is_listener == 2) {
        rc = core_ipc_recv_one(core, core->ipc_endpoint, 0);
        if (rc == COG_ERROR_OVERFLOW) {
            return COG_OK;
        }
        return rc;
    }

    return COG_OK; /* unbound: nothing to drain */
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

cog_result_t cog_core_new(cog_core_t **out, const cog_core_config_t *config)
{
    struct cog_core *core;
    size_t queue_capacity;
    cog_result_t rc;

    if (out == NULL || config == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    *out = NULL;

    if (config->session == NULL ||
        config->dispatch_table == NULL ||
        config->dispatch_table_size == 0U) {
        return COG_ERROR_INVALID_ARG;
    }

    core = (struct cog_core *)calloc(1U, sizeof(*core));
    if (core == NULL) {
        return COG_ERROR_NOMEM;
    }

    queue_capacity = config->event_queue_capacity;
    if (queue_capacity == 0U) {
        queue_capacity = COG_CORE_DEFAULT_EVENT_QUEUE_CAPACITY;
    }

    rc = cog_event_queue_new(&core->event_queue, queue_capacity);
    if (rc != COG_OK) {
        free(core);
        return rc;
    }

    core->pending = (cog_pending_msg_t *)calloc(
        COG_CORE_DEFAULT_PENDING_CAPACITY, sizeof(cog_pending_msg_t));
    if (core->pending == NULL) {
        cog_event_queue_free(core->event_queue);
        free(core);
        return COG_ERROR_NOMEM;
    }

    /* Accept and take ownership of an optional IPC endpoint.  The core
     * drains it during cog_core_run and destroys it in cog_core_free.
     * The receive buffer is sized to accept every valid frame. */
    if (config->ipc_endpoint != NULL) {
        rc = cog_ipc_message_new(&core->ipc_recv, COG_CORE_IPC_RECV_CAPACITY);
        if (rc != COG_OK) {
            free(core->pending);
            cog_event_queue_free(core->event_queue);
            free(core);
            return rc;
        }
        core->ipc_is_listener = core_ipc_probe_role(config->ipc_endpoint);
    }

    core->session = config->session;
    core->dispatch_table = config->dispatch_table;
    core->dispatch_table_size = config->dispatch_table_size;
    core->tool_table = config->tool_table;
    core->wait_fn = config->wait_fn;
    core->wait_arg = config->wait_arg;
    core->ipc_endpoint = config->ipc_endpoint;
    core->local_peer_id = config->local_peer_id;
    core->provider = config->provider;
    core->obs = config->obs;
    core->store = config->store;
    core->state = 0U;
    core->running = false;
    core->stop_requested = false;
    core->pending_capacity = COG_CORE_DEFAULT_PENDING_CAPACITY;
    core->pending_count = 0U;
    core->injected_payload = NULL;

    /* Attach the session so cog_session_reset refuses while the core is
     * alive (see session.c). */
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

    /* Detach the session so it can be reset/freed independently. */
    ((struct cog_session *)core->session)->attached = false;

    for (i = 0U; i < core->pending_count; ++i) {
        free(core->pending[i].session_id);
        free(core->pending[i].text);
    }
    free(core->pending);
    free(core->injected_payload);
    cog_event_queue_free(core->event_queue);

    /* Release every subagent record and its child session. */
    cog_agent_registry_reset(&core->agents);

    /* Release any in-flight IPC message payload copies that were never
     * dispatched (e.g. the core was freed with events still queued). */
    for (i = 0U; i < core->ipc_inflight_count; ++i) {
        free(core->ipc_inflight[i]);
    }
    free(core->ipc_inflight);

    /* Free accepted clients first so each disconnect removes itself from
     * the listener's peer table while the listener is still alive. */
    for (i = 0U; i < core->ipc_client_count; ++i) {
        (void)cog_ipc_endpoint_free(core->ipc_clients[i]);
    }
    free(core->ipc_clients);

    /* Destroy the core's own endpoint (listener or connected). */
    if (core->ipc_endpoint != NULL) {
        (void)cog_ipc_endpoint_free(core->ipc_endpoint);
    }

    /* Release the receive buffer (no-op when no endpoint was configured). */
    (void)cog_ipc_message_free(&core->ipc_recv);

    if (core->obs != NULL) {
        (void)cog_obs_log(core->obs, COG_OBS_INFO, "core.destroyed", NULL,
                          0U);
    }

    free(core);
}

/* ------------------------------------------------------------------ */
/* Run loop                                                            */
/* ------------------------------------------------------------------ */

cog_result_t cog_core_run(cog_core_t *core)
{
    cog_result_t rc;

    if (core == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    if (core->running) {
        return COG_ERROR_INVALID_STATE;
    }

    core->running = true;
    core->stop_requested = false;

    if (core->obs != NULL) {
        (void)cog_obs_log(core->obs, COG_OBS_INFO, "core.run_start", NULL,
                          0U);
    }

    rc = COG_OK;
    while (!core->stop_requested) {
        cog_event_t event;
        cog_result_t pop_rc;

        pop_rc = cog_event_queue_pop(core->event_queue, &event);
        if (pop_rc == COG_ERROR_AGAIN) {
            /* Queue empty.  Honour a pending stop request before doing any
             * further work: injecting a pending message here would dequeue
             * it and then drop it unprocessed on shutdown. */
            if (core->stop_requested) {
                break;
            }
            /* Drain the IPC fabric: accept new peers and receive messages,
             * enqueuing COG_EVENT_IPC_* events.  A drain failure (e.g.
             * allocation failure) is fatal to the loop and is returned; a
             * full event queue is not (the drain stops and the loop
             * dispatches what it has). */
            rc = core_ipc_drain(core);
            if (rc != COG_OK) {
                break;
            }
            if (core->stop_requested) {
                break;
            }
            /* If IPC produced events, pop them before waiting. */
            if (!cog_event_queue_is_empty(core->event_queue)) {
                continue;
            }
            /* Inject a pending message for our session, or wait. */
            if (core_inject_pending(core) == COG_OK) {
                continue;
            }
            if (core->stop_requested) {
                break;
            }
            if (core->wait_fn != NULL) {
                core->wait_fn(core->wait_arg);
            } else {
                core_yield();
            }
            continue;
        }
        if (pop_rc != COG_OK) {
            rc = pop_rc; /* unexpected queue error */
            break;
        }

        rc = core_dispatch(core, &event);
        /* Release the IPC message payload copy after dispatch.  The
         * cog_event_ipc_t struct itself is snapshotted by the event queue;
         * only the inner msg_payload needs core-managed lifetime. */
        if (event.kind == COG_EVENT_IPC_MESSAGE && event.payload != NULL) {
            const cog_event_ipc_t *ipc =
                (const cog_event_ipc_t *)event.payload;
            core_ipc_inflight_free(core, (void *)ipc->msg_payload);
        }
        free(core->injected_payload);
        core->injected_payload = NULL;
        if (rc != COG_OK) {
            break;
        }
    }

    core->running = false;
    if (core->obs != NULL) {
        (void)cog_obs_log(core->obs,
                          rc == COG_OK ? COG_OBS_INFO : COG_OBS_ERROR,
                          rc == COG_OK ? "core.run_stop" : "core.run_error",
                          NULL, 0U);
    }
    return rc;
}

void cog_core_stop(cog_core_t *core)
{
    if (core == NULL) {
        return;
    }
    core->stop_requested = true;
    core->running = false;
}

cog_result_t cog_core_enqueue(cog_core_t *core, const cog_event_t *event)
{
    if (core == NULL || event == NULL) {
        return COG_ERROR_NULL_PTR;
    }
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
    return (core != NULL) && core->running;
}

cog_ipc_endpoint_t *cog_core_ipc(cog_core_t *core)
{
    return (core != NULL) ? core->ipc_endpoint : NULL;
}

cog_provider_t *cog_core_provider(cog_core_t *core)
{
    return (core != NULL) ? core->provider : NULL;
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

/* ------------------------------------------------------------------ */
/* Pending message FIFO                                                */
/* ------------------------------------------------------------------ */

cog_result_t cog_core_queue_message(cog_core_t *core,
                                     const char *session_id,
                                     const char *text,
                                     size_t text_len)
{
    char *sid_copy;
    char *text_copy;

    if (core == NULL || session_id == NULL || text == NULL) {
        return COG_ERROR_NULL_PTR;
    }

    if (core->pending_count >= core->pending_capacity) {
        return COG_ERROR_OVERFLOW;
    }

    /* text_len + 1 must not wrap: a length of SIZE_MAX would make the
     * allocation size zero and then write past it.  Reject it as an
     * unsatisfiable allocation. */
    if (text_len == SIZE_MAX) {
        return COG_ERROR_NOMEM;
    }

    sid_copy = strdup(session_id);
    if (sid_copy == NULL) {
        return COG_ERROR_NOMEM;
    }
    text_copy = (char *)malloc(text_len + 1U);
    if (text_copy == NULL) {
        free(sid_copy);
        return COG_ERROR_NOMEM;
    }
    if (text_len > 0U) {
        (void)memcpy(text_copy, text, text_len);
    }
    text_copy[text_len] = '\0';

    core->pending[core->pending_count].session_id = sid_copy;
    core->pending[core->pending_count].text = text_copy;
    core->pending[core->pending_count].text_len = text_len;
    ++core->pending_count;
    return COG_OK;
}

cog_result_t cog_core_dequeue_message(cog_core_t *core,
                                       const char *session_id,
                                       char *out_buf,
                                       size_t buf_capacity,
                                       size_t *out_len)
{
    size_t i;
    cog_pending_msg_t *msg;

    if (core == NULL || session_id == NULL || out_buf == NULL) {
        return COG_ERROR_NULL_PTR;
    }

    for (i = 0U; i < core->pending_count; ++i) {
        if (strcmp(core->pending[i].session_id, session_id) == 0) {
            break;
        }
    }
    if (i == core->pending_count) {
        return COG_ERROR_NOT_FOUND;
    }

    msg = &core->pending[i];

    if (msg->text_len >= buf_capacity) {
        if (out_len != NULL) {
            *out_len = msg->text_len; /* required capacity, excluding NUL */
        }
        return COG_ERROR_OVERFLOW;
    }

    (void)memcpy(out_buf, msg->text, msg->text_len);
    out_buf[msg->text_len] = '\0';
    if (out_len != NULL) {
        *out_len = msg->text_len;
    }

    free(msg->session_id);
    free(msg->text);
    for (; i + 1U < core->pending_count; ++i) {
        core->pending[i] = core->pending[i + 1U];
    }
    --core->pending_count;
    return COG_OK;
}

cog_result_t cog_core_pending_count(const cog_core_t *core,
                                     const char *session_id,
                                     size_t *out_count)
{
    size_t i;
    size_t count;

    if (core == NULL || session_id == NULL || out_count == NULL) {
        return COG_ERROR_NULL_PTR;
    }

    count = 0U;
    for (i = 0U; i < core->pending_count; ++i) {
        if (strcmp(core->pending[i].session_id, session_id) == 0) {
            ++count;
        }
    }
    *out_count = count;
    return COG_OK;
}

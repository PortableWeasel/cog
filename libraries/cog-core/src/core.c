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
 * - The core owns its event queue and its pending-message FIFO.  The
 *   session, provider, obs, and store are borrowed (the caller retains
 *   ownership).  The IPC endpoint is owned by the core per the header,
 *   but its destructor (cog_ipc_endpoint_free) belongs to a later phase
 *   (cog-ipc is still header-only), so freeing the endpoint is deferred;
 *   the core stores the pointer for the accessor and does not release it.
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
 *   clears it on cog_core_free.  Because the flag lives in session.c's
 *   private struct (there is no public attach API), core.c mirrors the
 *   struct layout; see the note below.
 */
#define _POSIX_C_SOURCE 200809L

#include "cog/core.h"

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

/* ------------------------------------------------------------------ */
/* Session layout mirror                                               */
/* ------------------------------------------------------------------ */

/* The session's `attached` flag lives in session.c's private struct and
 * is documented there as "set by the core integration layer only".  The
 * public cog_session_t is opaque, so core.c mirrors the exact layout of
 * session.c's struct cog_session in order to set and clear that flag.
 * Both translation units define the same tag privately; the layouts must
 * stay in sync.  This is the standard opaque-type-sharing pattern and is
 * the only way to honour the header's session-attachment semantics
 * without a public attach API. */
struct cog_session_node;

struct cog_session {
    char *id;
    char *parent_id;
    size_t max_nodes;
    size_t node_count;
    uint64_t next_node_id;
    struct cog_session_node *root;
    struct cog_session_node *active;
    size_t prunable_leaf_count;
    bool attached;
    cog_message_t *history;
    size_t history_count;
    size_t history_capacity;
};

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
 * The session, provider, obs, and store are borrowed.  The event queue
 * and the pending FIFO are owned.  The IPC endpoint is owned per the
 * header but its release is deferred to the IPC phase.
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

    /* The IPC endpoint is owned by the core per the header, but its
     * destructor (cog_ipc_endpoint_free) is not yet implemented (cog-ipc
     * is a later phase); releasing it is deferred. */

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

    rc = COG_OK;
    while (!core->stop_requested) {
        cog_event_t event;
        cog_result_t pop_rc;

        pop_rc = cog_event_queue_pop(core->event_queue, &event);
        if (pop_rc == COG_ERROR_AGAIN) {
            /* Queue empty: inject a pending message for our session, or
             * wait. */
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
        free(core->injected_payload);
        core->injected_payload = NULL;
        if (rc != COG_OK) {
            break;
        }
    }

    core->running = false;
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

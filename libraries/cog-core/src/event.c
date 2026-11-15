/**
 * @file event.c
 * @brief Event queue implementation for cog-core.
 *
 * Events are the only inputs that advance the cog-core state machine.
 * This unit implements the bounded FIFO queue declared in @c cog/event.h:
 * construction/destruction, push/pop, and the count/empty/full
 * inspectors.
 *
 * Storage model (per header): all storage is allocated in a single heap
 * block by @c cog_event_queue_new — one allocation holding the queue
 * header and the ring of event slots — and released by
 * @c cog_event_queue_free.
 *
 * Payload model (per header):
 *  - a pushed @c cog_event_t is copied by value: every scalar field and
 *    the payload pointer are snapshotted at push time;
 *  - the payload pointee is referenced, not owned, for every event kind
 *    except the @c COG_EVENT_IPC_* kinds, which document that the queue
 *    copies the embedded @c cog_event_ipc_t struct.  For those kinds the
 *    queue snapshots the struct into queue-owned storage and the popped
 *    event's payload points at that snapshot;
 *  - the IPC message payload (@c cog_event_ipc_t::msg_payload) is always
 *    borrowed from the IPC layer and is never copied.
 *
 * Threading model (per header): single producer / single consumer.
 * @c cog_event_queue_push and @c cog_event_queue_pop must not be called
 * concurrently on the same queue; no locking is performed here.
 */
#include "cog/event.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

/**
 * @brief Kinds whose payload is a @c cog_event_ipc_t that the queue
 *        snapshots (see the header's @c cog_event_ipc_t documentation).
 *
 * Static const lookup table: indexer-friendly, no dynamic registration.
 */
static const cog_event_kind_t cog_event_ipc_kinds[] = {
    COG_EVENT_IPC_MESSAGE,
    COG_EVENT_IPC_CONNECTED,
    COG_EVENT_IPC_DISCONNECTED,
};

/**
 * @brief One ring-buffer slot: the queued event plus queue-owned storage
 *        for the copied @c cog_event_ipc_t used by COG_EVENT_IPC_* kinds.
 */
typedef struct cog_event_slot {
    cog_event_t event;        /**< Copied event (payload pointer included). */
    cog_event_ipc_t ipc;      /**< Snapshot; only valid for IPC kinds. */
} cog_event_slot_t;

/**
 * @brief Opaque queue layout (definition of @c cog_event_queue_t).
 *
 * The ring holds @c capacity slots; the tail index is
 * @c (head + count) % capacity.  The struct is allocated together with
 * the @c slots array in a single heap block.
 */
struct cog_event_queue {
    size_t capacity;          /**< Maximum number of queued events; > 0. */
    size_t count;              /**< Number of events currently queued. */
    size_t head;               /**< Index of the oldest queued event. */
    cog_event_slot_t slots[];  /**< Ring storage, @c capacity entries. */
};

/**
 * @brief Return true if @p kind carries a @c cog_event_ipc_t payload that
 *        the queue must snapshot.
 */
static bool cog_event_kind_is_ipc(cog_event_kind_t kind)
{
    size_t i;

    for (i = 0U;
         i < sizeof(cog_event_ipc_kinds) / sizeof(cog_event_ipc_kinds[0]);
         ++i) {
        if (cog_event_ipc_kinds[i] == kind) {
            return true;
        }
    }
    return false;
}

/**
 * @brief Create a bounded event queue.
 *
 * All storage (queue header plus the ring of @p capacity slots) is
 * allocated in a single heap block.  See @c cog/event.h for the full
 * contract.
 */
cog_result_t cog_event_queue_new(cog_event_queue_t **out, size_t capacity)
{
    struct cog_event_queue *q;
    const size_t header_bytes = offsetof(struct cog_event_queue, slots);
    const size_t slot_bytes = sizeof(cog_event_slot_t);

    if (out == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    *out = NULL;

    if (capacity == 0U) {
        return COG_ERROR_INVALID_ARG;
    }

    /* The single-block size computation must not wrap around.  A request
     * this large cannot be satisfied; report it as an allocation failure
     * rather than computing a truncated (too small) block. */
    if (capacity > (SIZE_MAX - header_bytes) / slot_bytes) {
        return COG_ERROR_NOMEM;
    }

    q = (struct cog_event_queue *)malloc(header_bytes + capacity * slot_bytes);
    if (q == NULL) {
        return COG_ERROR_NOMEM;
    }

    q->capacity = capacity;
    q->count = 0U;
    q->head = 0U;

    *out = q;
    return COG_OK;
}

/**
 * @brief Destroy an event queue.
 *
 * Releases the single heap block owned by @p q.  A NULL @p q is a no-op.
 * See @c cog/event.h for the full contract.
 */
void cog_event_queue_free(cog_event_queue_t *q)
{
    free(q);
}

/**
 * @brief Push an event onto the queue.
 *
 * The @c cog_event_t struct is copied by value.  The payload pointee is
 * borrowed except for the @c COG_EVENT_IPC_* kinds, whose embedded
 * @c cog_event_ipc_t struct is snapshotted into queue-owned storage.
 * See @c cog/event.h for the full contract.
 */
cog_result_t cog_event_queue_push(cog_event_queue_t *q,
                                  const cog_event_t *event)
{
    cog_event_slot_t *slot;
    size_t tail;

    if (q == NULL || event == NULL) {
        return COG_ERROR_NULL_PTR;
    }

    assert(q->capacity > 0U);
    assert(q->count <= q->capacity);

    if (q->count == q->capacity) {
        return COG_ERROR_OVERFLOW;
    }

    tail = (q->head + q->count) % q->capacity;
    slot = &q->slots[tail];

    slot->event = *event;

    if (cog_event_kind_is_ipc(event->kind) && event->payload != NULL) {
        /* Producers of IPC events supply a complete cog_event_ipc_t. */
        assert(event->payload_len >= sizeof(cog_event_ipc_t));
        if (event->payload_len >= sizeof(cog_event_ipc_t)) {
            slot->ipc = *(const cog_event_ipc_t *)event->payload;
            slot->event.payload = &slot->ipc;
        }
        /* payload_len is preserved verbatim from the producer. */
    }

    ++q->count;
    return COG_OK;
}

/**
 * @brief Pop the oldest event from the queue.
 *
 * On success @p out receives a copy of the oldest queued event; on
 * failure @p out is left unchanged.  See @c cog/event.h for the full
 * contract.
 */
cog_result_t cog_event_queue_pop(cog_event_queue_t *q, cog_event_t *out)
{
    cog_event_slot_t *slot;

    if (q == NULL || out == NULL) {
        return COG_ERROR_NULL_PTR;
    }

    assert(q->capacity > 0U);
    assert(q->count <= q->capacity);

    if (q->count == 0U) {
        return COG_ERROR_AGAIN;
    }

    slot = &q->slots[q->head];
    *out = slot->event;
    q->head = (q->head + 1U) % q->capacity;
    --q->count;
    return COG_OK;
}

/**
 * @brief Return the number of events currently in the queue.
 *
 * Returns zero when @p q is NULL.  See @c cog/event.h for the full
 * contract.
 */
size_t cog_event_queue_count(const cog_event_queue_t *q)
{
    if (q == NULL) {
        return 0U;
    }
    return q->count;
}

/**
 * @brief Return true if the queue contains no events.
 *
 * Returns true when @p q is NULL.  See @c cog/event.h for the full
 * contract.
 */
bool cog_event_queue_is_empty(const cog_event_queue_t *q)
{
    if (q == NULL) {
        return true;
    }
    return q->count == 0U;
}

/**
 * @brief Return true if the queue cannot accept another event.
 *
 * Returns false when @p q is NULL.  See @c cog/event.h for the full
 * contract.
 */
bool cog_event_queue_is_full(const cog_event_queue_t *q)
{
    if (q == NULL) {
        return false;
    }
    return q->count == q->capacity;
}

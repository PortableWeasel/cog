/**
 * @file event.h
 * @brief Event kinds, event payload, and event queue.
 *
 * Events are the only inputs that advance the cog-core state machine.
 * This header defines the event type, the event kind enumeration, and a
 * bounded FIFO queue used to feed events into a core instance.
 *
 * Threading model: a single producer / single consumer queue is assumed.
 * @c cog_event_queue_push and @c cog_event_queue_pop must not be called
 * concurrently on the same queue.  @c cog_core_enqueue is the intended
 * producer; @c cog_core_run is the intended consumer.  Multi-threaded
 * producers may be layered on top later with an external lock.
 */
#ifndef COG_EVENT_H
#define COG_EVENT_H

#include <cog/result.h>
#include <cog/ipc.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief State identifier used by the dispatch table.
 *
 * Values 0xFFFE and 0xFFFF are reserved (see @c COG_TRANSITION_SELF and
 * @c COG_TRANSITION_END).  Application states must use values in the
 * range [0, 0xFFFD].
 */
typedef uint16_t cog_state_t;

/**
 * @brief Event identifier used by the dispatch table.
 *
 * Values are unsigned 16-bit integers in the range [0, 0xFFFF].
 * No event id values are reserved by the core; semantic distinctions
 * (such as shutdown or system signals) are conveyed by
 * @c cog_event_kind_t, not by the id.
 */
typedef uint16_t cog_event_id_t;

/**
 * @brief Kinds of events that can advance the core state machine.
 */
typedef enum cog_event_kind {
    COG_EVENT_INVALID = 0,        /**< Uninitialised or sentinel value. */
    COG_EVENT_USER_INPUT,         /**< Free-form user message. */
    COG_EVENT_TOOL_RESULT,        /**< Result returned from a tool invocation. */
    COG_EVENT_SYSTEM_SIGNAL,      /**< Signal or notification from the runtime. */
    COG_EVENT_UI_MESSAGE,         /**< Message received from an attached UI process. */
    COG_EVENT_SUBAGENT_RESULT,    /**< Result returned from a subagent handler. */
    COG_EVENT_SUBAGENT_STATUS,    /**< Subagent progress/status notification. */
    COG_EVENT_LLM_CHUNK,          /**< Streaming chunk from LLM provider. */
    COG_EVENT_LLM_COMPLETE,       /**< LLM response finished. */
    COG_EVENT_PROVIDER_ERROR,     /**< Provider call failed (network, auth, etc). */
    COG_EVENT_NETWORK_STALL,      /**< Network connection stalled. */
    COG_EVENT_NETWORK_RECOVERED,  /**< Network connection recovered. */
    COG_EVENT_STATUS_UPDATE,      /**< Status update for UI display. */
    COG_EVENT_IPC_MESSAGE,        /**< Framed message received from a peer. */
    COG_EVENT_IPC_CONNECTED,      /**< New peer connected to the IPC fabric. */
    COG_EVENT_IPC_DISCONNECTED,   /**< Peer disconnected from the IPC fabric. */
    COG_EVENT_CONFIG_CHANGED,     /**< Configuration was reloaded. */
    COG_EVENT_SUBAGENT_TIMEOUT,   /**< Subagent exceeded its time limit. */
    COG_EVENT_SHUTDOWN,           /**< Request for graceful shutdown. */
} cog_event_kind_t;

/**
 * @brief Event passed through the state machine.
 *
 * The payload is referenced, not owned, unless the event kind documents
 * otherwise.  The caller must ensure the payload outlives the event.
 */
typedef struct cog_event {
    cog_event_id_t id;       /**< Dispatch id; often maps one-to-one to kind. */
    cog_event_kind_t kind;   /**< Semantic kind of the event. */
    const void *payload;     /**< Optional payload; may be NULL. */
    size_t payload_len;      /**< Payload size in bytes; zero when payload is NULL. */
    uint64_t source_id;      /**< Diagnostic source identifier; zero when unknown. */
} cog_event_t;

/**
 * @brief Payload for COG_EVENT_IPC_* events.
 *
 * Embedded in cog_event_t.payload.  The event queue copies this struct;
 * the message payload pointer refers to memory owned by the IPC layer
 * and is valid only during handler execution.
 */
typedef struct cog_event_ipc {
    cog_peer_id_t peer;       /**< Source or destination peer. */
    uint16_t msg_type;        /**< Application message type tag. */
    const void *msg_payload;  /**< Message payload (borrowed). */
    size_t msg_payload_len;   /**< Payload length in bytes. */
} cog_event_ipc_t;

/**
 * @brief Opaque event queue.
 *
 * Created with @c cog_event_queue_new and destroyed with
 * @c cog_event_queue_free.  All storage is allocated in a single heap
 * block by the constructor and released by the destructor.
 */
typedef struct cog_event_queue cog_event_queue_t;

/**
 * @brief Create a bounded event queue.
 *
 * @param[out] out   Uninitialised pointer; set to the new queue on success.
 * @param[in]  capacity Maximum number of events the queue can hold.
 *                     Must be greater than zero.
 * @return COG_OK on success.
 * @return COG_ERROR_NULL_PTR if @p out is NULL.
 * @return COG_ERROR_INVALID_ARG if @p capacity is zero.
 * @return COG_ERROR_NOMEM if allocation fails.
 *
 * @post On success, @p *out is a valid queue and the caller owns it.
 * @post On failure, @p *out is NULL and no resources are held.
 *
 * Threading: not thread-safe.  The queue must be fully created before
 * any other thread accesses it.
 */
cog_result_t cog_event_queue_new(cog_event_queue_t **out, size_t capacity);

/**
 * @brief Destroy an event queue.
 *
 * @param[in,out] q Queue to destroy; may be NULL (no-op).
 *
 * @post All storage owned by @p q is released.  @p q must not be used
 *       after this call.
 *
 * Threading: not thread-safe.  No other thread may access the queue
 * during or after destruction.
 */
void cog_event_queue_free(cog_event_queue_t *q);

/**
 * @brief Push an event onto the queue.
 *
 * @param[in,out] q     Queue to receive the event.
 * @param[in]     event Event to copy into the queue.
 * @return COG_OK on success.
 * @return COG_ERROR_NULL_PTR if @p q or @p event is NULL.
 * @return COG_ERROR_OVERFLOW if the queue is full.
 *
 * @post The queue contains a copy of @p event.
 *
 * Threading: single-producer.  Only one thread/caller may push at a time.
 */
cog_result_t cog_event_queue_push(cog_event_queue_t *q, const cog_event_t *event);

/**
 * @brief Pop an event from the queue.
 *
 * @param[in,out] q   Queue to read from.
 * @param[out]    out Receives the popped event on success.
 * @return COG_OK on success.
 * @return COG_ERROR_NULL_PTR if @p q or @p out is NULL.
 * @return COG_ERROR_AGAIN if the queue is empty.
 *
 * @post On success, @p out contains the oldest queued event.
 * @post On failure, @p out is unchanged.
 *
 * Threading: single-consumer.  Only one thread/caller may pop at a time.
 */
cog_result_t cog_event_queue_pop(cog_event_queue_t *q, cog_event_t *out);

/**
 * @brief Return the number of events currently in the queue.
 *
 * @param[in] q Queue to inspect; may be NULL.
 * @return Number of queued events, or zero if @p q is NULL.
 *
 * Threading: single-consumer for reliable results.
 */
size_t cog_event_queue_count(const cog_event_queue_t *q);

/**
 * @brief Return true if the queue contains no events.
 *
 * @param[in] q Queue to inspect; may be NULL.
 * @return true if the queue is empty or NULL, false otherwise.
 *
 * Threading: single-consumer for reliable results.
 */
bool cog_event_queue_is_empty(const cog_event_queue_t *q);

/**
 * @brief Return true if the queue cannot accept another event.
 *
 * @param[in] q Queue to inspect; may be NULL.
 * @return true if the queue is full, false if it is not full or is NULL.
 *
 * Threading: single-producer for reliable results.
 */
bool cog_event_queue_is_full(const cog_event_queue_t *q);

#ifdef __cplusplus
}
#endif

#endif /* COG_EVENT_H */

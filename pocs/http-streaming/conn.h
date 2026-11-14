/**
 * @file conn.h
 * @brief Connection lifecycle tracking for the HTTP streaming PoC.
 *
 * This module covers the Observability block from docs/DESIGN.md: every
 * in-flight network connection has an ID, a start time, a last-chunk
 * timestamp, byte/chunk counters, and a lifecycle state. Transports update
 * the tracker as chunks arrive; the tracker owns stall detection and can
 * render itself as a human-readable status snapshot.
 */

#ifndef COG_POC_HTTP_CONN_H
#define COG_POC_HTTP_CONN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Lifecycle states of a tracked connection. */
typedef enum conn_state {
    CONN_STATE_IDLE = 0,   /**< Created but never started. */
    CONN_STATE_CONNECTING, /**< TCP connect / request in flight. */
    CONN_STATE_STREAMING,  /**< Response body is flowing. */
    CONN_STATE_STALLED,    /**< No chunk arrived within the threshold. */
    CONN_STATE_DONE,       /**< Response fully received. */
    CONN_STATE_ERROR       /**< Terminated by an error. */
} conn_state_t;

/** Opaque connection tracker; created with conn_tracker_create(). */
typedef struct conn_tracker conn_tracker_t;

/**
 * @brief Create a connection tracker.
 *
 * Records @p conn_id and the current monotonic time as both the start time
 * and the last-chunk time. The initial state is CONN_STATE_CONNECTING.
 *
 * @param[in] conn_id Caller-chosen identifier for this connection.
 * @return Pointer to the tracker, or NULL on allocation failure.
 */
conn_tracker_t *conn_tracker_create(uint64_t conn_id);

/**
 * @brief Destroy a tracker created by conn_tracker_create().
 *
 * @param[in,out] t Tracker to destroy (may be NULL).
 */
void conn_tracker_destroy(conn_tracker_t *t);

/**
 * @brief Reset a tracker for a new attempt on the same connection ID.
 *
 * Clears counters and timestamps and returns the state to
 * CONN_STATE_CONNECTING. Used by transports that retry: each attempt gets
 * a fresh timing baseline while keeping the logical connection ID.
 *
 * @param[in,out] t Tracker to reset.
 */
void conn_tracker_reset(conn_tracker_t *t);

/**
 * @brief Record the arrival of a chunk of @p len bytes.
 *
 * Updates the last-chunk timestamp, byte and chunk counters, and promotes
 * the state from CONNECTING/STALLED to STREAMING.
 *
 * @param[in,out] t   Tracker to update.
 * @param[in]     len Chunk byte count.
 */
void conn_tracker_note_chunk(conn_tracker_t *t, size_t len);

/**
 * @brief Set the state explicitly (e.g. DONE, ERROR, STALLED).
 *
 * @param[in,out] t     Tracker to update.
 * @param[in]     state New state.
 */
void conn_tracker_set_state(conn_tracker_t *t, conn_state_t state);

/**
 * @brief Check whether the connection has stalled.
 *
 * A stall is when more than @p threshold_ms has elapsed since the last
 * chunk while the connection is still CONNECTING or STREAMING. On
 * detection the state is set to CONN_STATE_STALLED. Already-stalled or
 * finished connections never report a new stall.
 *
 * @param[in,out] t            Tracker to check.
 * @param[in]     now_ms       Current monotonic time (conn_tracker_now_ms()).
 * @param[in]     threshold_ms Stall threshold in milliseconds.
 * @return true if the connection is (now) stalled.
 */
bool conn_tracker_check_stall(conn_tracker_t *t, uint64_t now_ms,
                              uint64_t threshold_ms);

/**
 * @brief Current monotonic time in milliseconds (CLOCK_MONOTONIC).
 *
 * @return Milliseconds since an unspecified epoch; only deltas are meaningful.
 */
uint64_t conn_tracker_now_ms(void);

/**
 * @brief How long the connection has been alive, in milliseconds.
 *
 * @param[in] t      Tracker to query.
 * @param[in] now_ms Current monotonic time.
 * @return Age in milliseconds (saturating).
 */
uint64_t conn_tracker_age_ms(const conn_tracker_t *t, uint64_t now_ms);

/**
 * @brief Milliseconds since the last chunk arrived.
 *
 * @param[in] t      Tracker to query.
 * @param[in] now_ms Current monotonic time.
 * @return Silence duration in milliseconds (saturating).
 */
uint64_t conn_tracker_last_chunk_age_ms(const conn_tracker_t *t, uint64_t now_ms);

/** @brief Connection identifier of the tracker. */
uint64_t conn_tracker_conn_id(const conn_tracker_t *t);

/** @brief Monotonic start time of the tracker, in milliseconds. */
uint64_t conn_tracker_start_ms(const conn_tracker_t *t);

/** @brief Monotonic last-chunk time of the tracker, in milliseconds. */
uint64_t conn_tracker_last_chunk_ms(const conn_tracker_t *t);

/** @brief Total body bytes recorded so far. */
size_t conn_tracker_bytes(const conn_tracker_t *t);

/** @brief Total chunks recorded so far. */
size_t conn_tracker_chunks(const conn_tracker_t *t);

/** @brief Current state of the tracker. */
conn_state_t conn_tracker_state(const conn_tracker_t *t);

/**
 * @brief Human-readable name for a connection state.
 *
 * Backed by a static const string table — indexer-friendly, no dynamic
 * allocation.
 *
 * @param[in] s State to name.
 * @return Static string (never NULL).
 */
const char *conn_state_name(conn_state_t s);

/**
 * @brief Render a status snapshot into @p buf.
 *
 * Format:
 *     id=<id> state=<state> age=<ms>ms last_chunk=<ms>ms_ago bytes=<n> chunks=<n>
 * The output is always NUL-terminated and truncated to @p cap.
 *
 * @param[in]  t   Tracker to snapshot.
 * @param[out] buf Destination buffer.
 * @param[in]  cap Buffer capacity in bytes.
 */
void conn_tracker_snapshot(const conn_tracker_t *t, char *buf, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* COG_POC_HTTP_CONN_H */

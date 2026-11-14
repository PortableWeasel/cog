/**
 * @file conn.c
 * @brief Connection tracker implementation (see conn.h).
 *
 * All timestamps use CLOCK_MONOTONIC so they are immune to wall-clock
 * jumps. The tracker is caller-visible through accessors only; the struct
 * layout is private to this file, matching the project's opaque-typedef
 * convention.
 */

#define _POSIX_C_SOURCE 200809L

#include "conn.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

/** A connection tracker is an opaque struct; the layout is private. */
struct conn_tracker {
    uint64_t conn_id;
    uint64_t start_ms;
    uint64_t last_chunk_ms;
    size_t bytes;
    size_t chunks;
    conn_state_t state;
};

/** Static names for the connection states. See conn_state_name(). */
static const char *const k_state_names[] = {
    "idle", "connecting", "streaming", "stalled", "done", "error"
};

/** Number of entries in ::k_state_names. */
#define STATE_NAME_COUNT (sizeof k_state_names / sizeof k_state_names[0])

conn_tracker_t *conn_tracker_create(uint64_t conn_id)
{
    struct conn_tracker *t = calloc(1U, sizeof *t);
    if (t == NULL) {
        return NULL;
    }
    t->conn_id = conn_id;
    t->start_ms = conn_tracker_now_ms();
    t->last_chunk_ms = t->start_ms;
    t->state = CONN_STATE_CONNECTING;
    return t;
}

void conn_tracker_destroy(conn_tracker_t *t)
{
    free(t);
}

void conn_tracker_reset(conn_tracker_t *t)
{
    if (t == NULL) {
        return;
    }
    t->start_ms = conn_tracker_now_ms();
    t->last_chunk_ms = t->start_ms;
    t->bytes = 0U;
    t->chunks = 0U;
    t->state = CONN_STATE_CONNECTING;
}

void conn_tracker_note_chunk(conn_tracker_t *t, size_t len)
{
    if (t == NULL) {
        return;
    }
    t->last_chunk_ms = conn_tracker_now_ms();
    t->bytes += len;
    t->chunks += 1U;
    if (t->state != CONN_STATE_DONE && t->state != CONN_STATE_ERROR) {
        t->state = CONN_STATE_STREAMING;
    }
}

void conn_tracker_set_state(conn_tracker_t *t, conn_state_t state)
{
    if (t == NULL) {
        return;
    }
    t->state = state;
}

bool conn_tracker_check_stall(conn_tracker_t *t, uint64_t now_ms,
                              uint64_t threshold_ms)
{
    if (t == NULL) {
        return false;
    }
    if (t->state != CONN_STATE_CONNECTING && t->state != CONN_STATE_STREAMING) {
        return false;
    }
    if (now_ms >= t->last_chunk_ms &&
        now_ms - t->last_chunk_ms > threshold_ms) {
        t->state = CONN_STATE_STALLED;
        return true;
    }
    return false;
}

uint64_t conn_tracker_now_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0U;
    }
    return (uint64_t)ts.tv_sec * 1000U + (uint64_t)ts.tv_nsec / 1000000U;
}

uint64_t conn_tracker_age_ms(const conn_tracker_t *t, uint64_t now_ms)
{
    if (t == NULL || now_ms <= t->start_ms) {
        return 0U;
    }
    return now_ms - t->start_ms;
}

uint64_t conn_tracker_last_chunk_age_ms(const conn_tracker_t *t, uint64_t now_ms)
{
    if (t == NULL || now_ms <= t->last_chunk_ms) {
        return 0U;
    }
    return now_ms - t->last_chunk_ms;
}

uint64_t conn_tracker_conn_id(const conn_tracker_t *t)
{
    return t != NULL ? t->conn_id : 0U;
}

uint64_t conn_tracker_start_ms(const conn_tracker_t *t)
{
    return t != NULL ? t->start_ms : 0U;
}

uint64_t conn_tracker_last_chunk_ms(const conn_tracker_t *t)
{
    return t != NULL ? t->last_chunk_ms : 0U;
}

size_t conn_tracker_bytes(const conn_tracker_t *t)
{
    return t != NULL ? t->bytes : 0U;
}

size_t conn_tracker_chunks(const conn_tracker_t *t)
{
    return t != NULL ? t->chunks : 0U;
}

conn_state_t conn_tracker_state(const conn_tracker_t *t)
{
    return t != NULL ? t->state : CONN_STATE_IDLE;
}

const char *conn_state_name(conn_state_t s)
{
    size_t i = (size_t)s;
    if (i >= STATE_NAME_COUNT) {
        return "unknown";
    }
    return k_state_names[i];
}

void conn_tracker_snapshot(const conn_tracker_t *t, char *buf, size_t cap)
{
    if (buf == NULL || cap == 0U) {
        return;
    }
    uint64_t now = conn_tracker_now_ms();
    uint64_t age = conn_tracker_age_ms(t, now);
    uint64_t last = conn_tracker_last_chunk_age_ms(t, now);
    int n = snprintf(buf, cap,
                     "id=%llu state=%s age=%llums last_chunk=%llums_ago "
                     "bytes=%zu chunks=%zu",
                     (unsigned long long)conn_tracker_conn_id(t),
                     conn_state_name(conn_tracker_state(t)),
                     (unsigned long long)age, (unsigned long long)last,
                     conn_tracker_bytes(t), conn_tracker_chunks(t));
    (void)n;
}

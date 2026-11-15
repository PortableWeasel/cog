/**
 * @file test_event.c
 * @brief Contract test for cog/event.h: event kinds, payload structs,
 *        and the bounded FIFO event queue.
 *
 * This test pins the public contract of libraries/cog-core/include/cog/
 * event.h as implemented by libraries/cog-core/src/event.c:
 *
 *   - the 26 event kinds keep their declared order (0 = INVALID);
 *   - cog_event_t / cog_event_ipc_t field types are exact;
 *   - the queue constructor enforces capacity > 0, rejects a NULL out
 *     pointer, returns NOMEM when the single-block allocation cannot be
 *     satisfied, and leaves *out NULL on every failure;
 *   - free(NULL) is a no-op and a queue is freed without leaks;
 *   - push/pop validate NULL arguments and leave the queue unchanged;
 *   - pop of an empty queue returns COG_ERROR_AGAIN and leaves *out
 *     unchanged;
 *   - events round-trip by value: scalar fields and the payload pointer
 *     are snapshotted at push time;
 *   - FIFO ordering holds across fill, overflow, drain, wrap-around, and
 *     repeated full cycles;
 *   - the payload pointee is referenced, not owned: mutating the buffer
 *     after push is visible in the popped event;
 *   - COG_EVENT_IPC_* payloads are snapshotted by the queue (the
 *     cog_event_ipc_t struct is copied; msg_payload stays borrowed).
 *
 * Nothing is tested here that is not either stated in the header or
 * guaranteed by C11 semantics.
 *
 * Ticket: 2875a5a (Stage 12.1b — Implement event.c)
 */

#include "cog/event.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifndef COG_EVENT_H
#error "COG_EVENT_H include guard missing"
#endif

/* ------------------------------------------------------------------ */
/* Compile-time checks: kind values, struct shapes, field types.       */
/* ------------------------------------------------------------------ */

_Static_assert(COG_EVENT_INVALID == 0,
               "COG_EVENT_INVALID must be exactly 0 (sentinel)");

/* Field types: exact, verified via type-generic selection (unevaluated). */
#define FIELD_TYPE_ASSERT(strukt, field, type)                                 \
    _Static_assert(_Generic(((strukt){0}).field, type : 1, default : 0),        \
                   #strukt "." #field " must have type " #type)

FIELD_TYPE_ASSERT(cog_event_t, id, cog_event_id_t);
FIELD_TYPE_ASSERT(cog_event_t, kind, cog_event_kind_t);
FIELD_TYPE_ASSERT(cog_event_t, payload, const void *);
FIELD_TYPE_ASSERT(cog_event_t, payload_len, size_t);
FIELD_TYPE_ASSERT(cog_event_t, source_id, uint64_t);

FIELD_TYPE_ASSERT(cog_event_ipc_t, peer, cog_peer_id_t);
FIELD_TYPE_ASSERT(cog_event_ipc_t, msg_type, uint16_t);
FIELD_TYPE_ASSERT(cog_event_ipc_t, msg_payload, const void *);
FIELD_TYPE_ASSERT(cog_event_ipc_t, msg_payload_len, size_t);

#undef FIELD_TYPE_ASSERT

/* ------------------------------------------------------------------ */
/* Runtime checks.                                                     */
/* ------------------------------------------------------------------ */

/** All event kinds, in declared order. */
static const cog_event_kind_t all_kinds[] = {
    COG_EVENT_INVALID,
    COG_EVENT_USER_INPUT,
    COG_EVENT_TOOL_RESULT,
    COG_EVENT_SYSTEM_SIGNAL,
    COG_EVENT_UI_MESSAGE,
    COG_EVENT_SUBAGENT_RESULT,
    COG_EVENT_SUBAGENT_STATUS,
    COG_EVENT_LLM_CHUNK,
    COG_EVENT_LLM_COMPLETE,
    COG_EVENT_PROVIDER_ERROR,
    COG_EVENT_NETWORK_STALL,
    COG_EVENT_NETWORK_RECOVERED,
    COG_EVENT_STATUS_UPDATE,
    COG_EVENT_IPC_MESSAGE,
    COG_EVENT_IPC_CONNECTED,
    COG_EVENT_IPC_DISCONNECTED,
    COG_EVENT_CONFIG_CHANGED,
    COG_EVENT_SUBAGENT_TIMEOUT,
    COG_EVENT_SHUTDOWN,
    COG_EVENT_CANCEL,
    COG_EVENT_CANCELLED,
    COG_EVENT_QUEUE_MSG,
    COG_EVENT_MSG_QUEUED,
    COG_EVENT_RETRY,
    COG_EVENT_ABORT,
    COG_EVENT_ERROR,
};

/** The three IPC kinds whose payload the queue snapshots. */
static const cog_event_kind_t ipc_kinds[] = {
    COG_EVENT_IPC_MESSAGE,
    COG_EVENT_IPC_CONNECTED,
    COG_EVENT_IPC_DISCONNECTED,
};

/** Expected id of a synthetic event built with @c make_event. */
static cog_event_t make_event(cog_event_id_t id, cog_event_kind_t kind)
{
    cog_event_t ev;
    ev.id = id;
    ev.kind = kind;
    ev.payload = NULL;
    ev.payload_len = 0U;
    ev.source_id = 0U;
    return ev;
}

/** Verifies that every declared kind appears in order, starting at 0.
 *  This pins the values used by the state-machine dispatch tables. */
static int verify_kind_order(void)
{
    const size_t n = sizeof(all_kinds) / sizeof(all_kinds[0]);
    size_t i;

    for (i = 0U; i < n; ++i) {
        if ((int)all_kinds[i] != (int)i) {
            (void)fprintf(stderr, "all_kinds[%zu] = %d, expected %zu\n",
                          i, (int)all_kinds[i], i);
            return 1;
        }
    }
    return 0;
}

/** Map a kind to its index, as a dispatch table would. */
static int kind_index(cog_event_kind_t kind)
{
    switch (kind) {
    case COG_EVENT_INVALID:          return 0;
    case COG_EVENT_USER_INPUT:       return 1;
    case COG_EVENT_TOOL_RESULT:      return 2;
    case COG_EVENT_SYSTEM_SIGNAL:    return 3;
    case COG_EVENT_UI_MESSAGE:       return 4;
    case COG_EVENT_SUBAGENT_RESULT:   return 5;
    case COG_EVENT_SUBAGENT_STATUS:  return 6;
    case COG_EVENT_LLM_CHUNK:        return 7;
    case COG_EVENT_LLM_COMPLETE:     return 8;
    case COG_EVENT_PROVIDER_ERROR:   return 9;
    case COG_EVENT_NETWORK_STALL:    return 10;
    case COG_EVENT_NETWORK_RECOVERED: return 11;
    case COG_EVENT_STATUS_UPDATE:    return 12;
    case COG_EVENT_IPC_MESSAGE:      return 13;
    case COG_EVENT_IPC_CONNECTED:    return 14;
    case COG_EVENT_IPC_DISCONNECTED: return 15;
    case COG_EVENT_CONFIG_CHANGED:   return 16;
    case COG_EVENT_SUBAGENT_TIMEOUT: return 17;
    case COG_EVENT_SHUTDOWN:         return 18;
    case COG_EVENT_CANCEL:           return 19;
    case COG_EVENT_CANCELLED:        return 20;
    case COG_EVENT_QUEUE_MSG:        return 21;
    case COG_EVENT_MSG_QUEUED:       return 22;
    case COG_EVENT_RETRY:            return 23;
    case COG_EVENT_ABORT:            return 24;
    case COG_EVENT_ERROR:            return 25;
    default:                        return -1;
    }
}

static int verify_kind_switch(void)
{
    const size_t n = sizeof(all_kinds) / sizeof(all_kinds[0]);
    size_t i;

    for (i = 0U; i < n; ++i) {
        if (kind_index(all_kinds[i]) != (int)i) {
            return 1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Constructor / destructor.                                           */
/* ------------------------------------------------------------------ */

static int test_new_null_out(void)
{
    if (cog_event_queue_new(NULL, 4U) != COG_ERROR_NULL_PTR) {
        return 1;
    }
    return 0;
}

static int test_new_zero_capacity(void)
{
    cog_event_queue_t *q = NULL;
    cog_result_t r;

    q = (cog_event_queue_t *)(uintptr_t)1; /* poison: must be overwritten */
    r = cog_event_queue_new(&q, 0U);
    if (r != COG_ERROR_INVALID_ARG) {
        return 1;
    }
    if (q != NULL) {
        return 1;
    }
    return 0;
}

static int test_new_size_overflow(void)
{
    cog_event_queue_t *q = NULL;

    /* The single-block size computation (header + capacity * slot) wraps
     * for SIZE_MAX; the constructor must report NOMEM, not truncate. */
    if (cog_event_queue_new(&q, SIZE_MAX) != COG_ERROR_NOMEM) {
        return 1;
    }
    if (q != NULL) {
        return 1;
    }

    q = NULL;
    if (cog_event_queue_new(&q, SIZE_MAX / 2U) != COG_ERROR_NOMEM) {
        return 1;
    }
    if (q != NULL) {
        return 1;
    }
    return 0;
}

static int test_new_size_one(void)
{
    cog_event_queue_t *q = NULL;

    if (cog_event_queue_new(&q, 1U) != COG_OK) {
        return 1;
    }
    if (q == NULL) {
        return 1;
    }
    if (cog_event_queue_count(q) != 0U) {
        return 1;
    }
    if (!cog_event_queue_is_empty(q)) {
        return 1;
    }
    if (cog_event_queue_is_full(q)) {
        return 1;
    }
    cog_event_queue_free(q);
    return 0;
}

static int test_free_null(void)
{
    cog_event_queue_free(NULL); /* must be a no-op */
    return 0;
}

static int test_null_queue_queries(void)
{
    if (cog_event_queue_count(NULL) != 0U) {
        return 1;
    }
    if (!cog_event_queue_is_empty(NULL)) {
        return 1;
    }
    if (cog_event_queue_is_full(NULL)) {
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Push / pop argument validation and empty behavior.                 */
/* ------------------------------------------------------------------ */

static int test_push_null_args(void)
{
    cog_event_queue_t *q = NULL;
    cog_event_t ev = make_event(1U, COG_EVENT_USER_INPUT);

    if (cog_event_queue_new(&q, 2U) != COG_OK) {
        return 1;
    }
    if (cog_event_queue_push(NULL, &ev) != COG_ERROR_NULL_PTR) {
        cog_event_queue_free(q);
        return 1;
    }
    if (cog_event_queue_push(q, NULL) != COG_ERROR_NULL_PTR) {
        cog_event_queue_free(q);
        return 1;
    }
    if (cog_event_queue_count(q) != 0U) {
        cog_event_queue_free(q);
        return 1;
    }
    cog_event_queue_free(q);
    return 0;
}

static int test_pop_null_args(void)
{
    cog_event_queue_t *q = NULL;
    cog_event_t out;

    if (cog_event_queue_new(&q, 2U) != COG_OK) {
        return 1;
    }
    if (cog_event_queue_pop(NULL, &out) != COG_ERROR_NULL_PTR) {
        cog_event_queue_free(q);
        return 1;
    }
    if (cog_event_queue_pop(q, NULL) != COG_ERROR_NULL_PTR) {
        cog_event_queue_free(q);
        return 1;
    }
    if (cog_event_queue_count(q) != 0U) {
        cog_event_queue_free(q);
        return 1;
    }
    cog_event_queue_free(q);
    return 0;
}

static int test_pop_empty_unchanged(void)
{
    cog_event_queue_t *q = NULL;
    cog_event_t sentinel;
    cog_event_t out;
    cog_event_t out2;

    if (cog_event_queue_new(&q, 2U) != COG_OK) {
        return 1;
    }

    sentinel = make_event(0x1234U, COG_EVENT_SHUTDOWN);
    sentinel.payload = (const void *)&sentinel;
    sentinel.payload_len = 7U;
    sentinel.source_id = 99U;

    out = sentinel;
    if (cog_event_queue_pop(q, &out) != COG_ERROR_AGAIN) {
        cog_event_queue_free(q);
        return 1;
    }
    if (out.id != sentinel.id || out.kind != sentinel.kind ||
        out.payload != sentinel.payload || out.payload_len != sentinel.payload_len ||
        out.source_id != sentinel.source_id) {
        cog_event_queue_free(q);
        return 1;
    }

    /* Repeated empty pops stay COG_ERROR_AGAIN. */
    out2 = sentinel;
    if (cog_event_queue_pop(q, &out2) != COG_ERROR_AGAIN) {
        cog_event_queue_free(q);
        return 1;
    }
    if (out2.id != sentinel.id) {
        cog_event_queue_free(q);
        return 1;
    }

    cog_event_queue_free(q);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Value round-trip and copy semantics.                                */
/* ------------------------------------------------------------------ */

static int test_roundtrip_fields(void)
{
    cog_event_queue_t *q = NULL;
    char buf[8] = "payload";
    cog_event_t ev;
    cog_event_t out;

    if (cog_event_queue_new(&q, 1U) != COG_OK) {
        return 1;
    }

    ev.id = 42U;
    ev.kind = COG_EVENT_TOOL_RESULT;
    ev.payload = (const void *)buf;
    ev.payload_len = 7U;
    ev.source_id = 0x1122334455667788ULL;

    if (cog_event_queue_push(q, &ev) != COG_OK) {
        cog_event_queue_free(q);
        return 1;
    }
    if (cog_event_queue_count(q) != 1U) {
        cog_event_queue_free(q);
        return 1;
    }
    if (cog_event_queue_is_empty(q)) {
        cog_event_queue_free(q);
        return 1;
    }
    if (!cog_event_queue_is_full(q)) {
        cog_event_queue_free(q);
        return 1;
    }

    /* Copy semantics: mutating the source after push must not affect
     * the queued snapshot.  The payload pointer itself is copied; the
     * pointee stays referenced. */
    ev.id = 0U;
    ev.kind = COG_EVENT_INVALID;
    ev.payload = NULL;
    ev.payload_len = 0U;
    ev.source_id = 0U;

    if (cog_event_queue_pop(q, &out) != COG_OK) {
        cog_event_queue_free(q);
        return 1;
    }
    if (out.id != 42U || out.kind != COG_EVENT_TOOL_RESULT ||
        out.payload != (const void *)buf || out.payload_len != 7U ||
        out.source_id != 0x1122334455667788ULL) {
        cog_event_queue_free(q);
        return 1;
    }
    if (cog_event_queue_count(q) != 0U) {
        cog_event_queue_free(q);
        return 1;
    }
    if (!cog_event_queue_is_empty(q)) {
        cog_event_queue_free(q);
        return 1;
    }

    cog_event_queue_free(q);
    return 0;
}

static int test_null_payload_event(void)
{
    cog_event_queue_t *q = NULL;
    cog_event_t ev = make_event(1U, COG_EVENT_USER_INPUT);
    cog_event_t out;

    if (cog_event_queue_new(&q, 1U) != COG_OK) {
        return 1;
    }
    if (cog_event_queue_push(q, &ev) != COG_OK) {
        cog_event_queue_free(q);
        return 1;
    }
    if (cog_event_queue_pop(q, &out) != COG_OK) {
        cog_event_queue_free(q);
        return 1;
    }
    if (out.payload != NULL || out.payload_len != 0U || out.kind != COG_EVENT_USER_INPUT) {
        cog_event_queue_free(q);
        return 1;
    }
    cog_event_queue_free(q);
    return 0;
}

static int test_payload_reference_semantics(void)
{
    cog_event_queue_t *q = NULL;
    char buf[8] = "hello";
    cog_event_t ev;
    cog_event_t out;

    if (cog_event_queue_new(&q, 1U) != COG_OK) {
        return 1;
    }

    ev = make_event(1U, COG_EVENT_USER_INPUT);
    ev.payload = (const void *)buf;
    ev.payload_len = 5U;

    if (cog_event_queue_push(q, &ev) != COG_OK) {
        cog_event_queue_free(q);
        return 1;
    }

    /* The queue never copies the payload pointee: a mutation after push
     * is visible in the popped event. */
    buf[0] = 'H';

    if (cog_event_queue_pop(q, &out) != COG_OK) {
        cog_event_queue_free(q);
        return 1;
    }
    if (out.payload != (const void *)buf) {
        cog_event_queue_free(q);
        return 1;
    }
    if (out.payload_len != 5U) {
        cog_event_queue_free(q);
        return 1;
    }
    if (memcmp(out.payload, "Hello", 5U) != 0) {
        cog_event_queue_free(q);
        return 1;
    }

    cog_event_queue_free(q);
    return 0;
}

/* ------------------------------------------------------------------ */
/* FIFO ordering, fill, overflow.                                      */
/* ------------------------------------------------------------------ */

static int test_fifo_order(void)
{
    static const char *const texts[] = {"first", "second", "third", "fourth"};
    const size_t n = sizeof(texts) / sizeof(texts[0]);
    cog_event_queue_t *q = NULL;
    size_t i;

    if (n != 4U) {
        return 1;
    }
    if (cog_event_queue_new(&q, n) != COG_OK) {
        return 1;
    }

    for (i = 0U; i < n; ++i) {
        cog_event_t ev = make_event((cog_event_id_t)(uint16_t)(i + 1U),
                                    all_kinds[i]);
        ev.payload = (const void *)texts[i];
        ev.payload_len = strlen(texts[i]);
        ev.source_id = (uint64_t)(100U + i);
        if (cog_event_queue_push(q, &ev) != COG_OK) {
            cog_event_queue_free(q);
            return 1;
        }
        if (cog_event_queue_count(q) != i + 1U) {
            cog_event_queue_free(q);
            return 1;
        }
    }
    if (!cog_event_queue_is_full(q)) {
        cog_event_queue_free(q);
        return 1;
    }
    if (cog_event_queue_is_empty(q)) {
        cog_event_queue_free(q);
        return 1;
    }

    for (i = 0U; i < n; ++i) {
        cog_event_t out;
        if (cog_event_queue_pop(q, &out) != COG_OK) {
            cog_event_queue_free(q);
            return 1;
        }
        if (out.id != (cog_event_id_t)(uint16_t)(i + 1U) ||
            out.kind != all_kinds[i] ||
            out.payload != (const void *)texts[i] ||
            out.payload_len != strlen(texts[i]) ||
            out.source_id != (uint64_t)(100U + i)) {
            cog_event_queue_free(q);
            return 1;
        }
        if (cog_event_queue_count(q) != n - (i + 1U)) {
            cog_event_queue_free(q);
            return 1;
        }
    }
    if (!cog_event_queue_is_empty(q) || cog_event_queue_is_full(q)) {
        cog_event_queue_free(q);
        return 1;
    }

    cog_event_queue_free(q);
    return 0;
}

static int test_all_kinds_roundtrip(void)
{
    const size_t n = sizeof(all_kinds) / sizeof(all_kinds[0]);
    cog_event_queue_t *q = NULL;
    size_t i;

    /* Fill the queue to exactly capacity with every kind in order. */
    if (cog_event_queue_new(&q, n) != COG_OK) {
        return 1;
    }
    for (i = 0U; i < n; ++i) {
        cog_event_t ev = make_event((cog_event_id_t)(uint16_t)i, all_kinds[i]);
        ev.source_id = (uint64_t)i;
        if (cog_event_queue_push(q, &ev) != COG_OK) {
            cog_event_queue_free(q);
            return 1;
        }
    }
    if (cog_event_queue_count(q) != n || !cog_event_queue_is_full(q)) {
        cog_event_queue_free(q);
        return 1;
    }
    for (i = 0U; i < n; ++i) {
        cog_event_t out;
        if (cog_event_queue_pop(q, &out) != COG_OK) {
            cog_event_queue_free(q);
            return 1;
        }
        if (out.id != (cog_event_id_t)(uint16_t)i || out.kind != all_kinds[i] ||
            out.source_id != (uint64_t)i) {
            cog_event_queue_free(q);
            return 1;
        }
    }
    cog_event_queue_free(q);
    return 0;
}

static int test_fill_and_overflow(void)
{
    cog_event_queue_t *q = NULL;
    cog_event_t ev;
    size_t i;

    if (cog_event_queue_new(&q, 3U) != COG_OK) {
        return 1;
    }
    for (i = 0U; i < 3U; ++i) {
        ev = make_event((cog_event_id_t)(uint16_t)i, COG_EVENT_USER_INPUT);
        if (cog_event_queue_push(q, &ev) != COG_OK) {
            cog_event_queue_free(q);
            return 1;
        }
    }
    if (cog_event_queue_count(q) != 3U || !cog_event_queue_is_full(q)) {
        cog_event_queue_free(q);
        return 1;
    }

    /* Push beyond capacity: overflow, queue and counter unchanged. */
    ev = make_event(99U, COG_EVENT_USER_INPUT);
    if (cog_event_queue_push(q, &ev) != COG_ERROR_OVERFLOW) {
        cog_event_queue_free(q);
        return 1;
    }
    if (cog_event_queue_count(q) != 3U || !cog_event_queue_is_full(q)) {
        cog_event_queue_free(q);
        return 1;
    }

    /* Drain in FIFO order; ids 0,1,2 as pushed. */
    for (i = 0U; i < 3U; ++i) {
        cog_event_t out;
        if (cog_event_queue_pop(q, &out) != COG_OK || out.id != (cog_event_id_t)(uint16_t)i) {
            cog_event_queue_free(q);
            return 1;
        }
    }
    if (cog_event_queue_count(q) != 0U || !cog_event_queue_is_empty(q) ||
        cog_event_queue_is_full(q)) {
        cog_event_queue_free(q);
        return 1;
    }
    cog_event_queue_free(q);
    return 0;
}

static int test_overflow_recovery(void)
{
    cog_event_queue_t *q = NULL;
    cog_event_t ev;
    cog_event_t out;

    if (cog_event_queue_new(&q, 2U) != COG_OK) {
        return 1;
    }
    ev = make_event(1U, COG_EVENT_USER_INPUT);
    if (cog_event_queue_push(q, &ev) != COG_OK ||
        cog_event_queue_push(q, &ev) != COG_OK) {
        cog_event_queue_free(q);
        return 1;
    }
    ev.id = 99U;
    if (cog_event_queue_push(q, &ev) != COG_ERROR_OVERFLOW) {
        cog_event_queue_free(q);
        return 1;
    }

    /* Pop one, then push again: id 99 must land behind 2. */
    if (cog_event_queue_pop(q, &out) != COG_OK || out.id != 1U) {
        cog_event_queue_free(q);
        return 1;
    }
    ev.id = 99U;
    if (cog_event_queue_push(q, &ev) != COG_OK) {
        cog_event_queue_free(q);
        return 1;
    }
    if (cog_event_queue_pop(q, &out) != COG_OK || out.id != 1U) {
        cog_event_queue_free(q);
        return 1;
    }
    if (cog_event_queue_pop(q, &out) != COG_OK || out.id != 99U) {
        cog_event_queue_free(q);
        return 1;
    }
    if (cog_event_queue_pop(q, &out) != COG_ERROR_AGAIN) {
        cog_event_queue_free(q);
        return 1;
    }
    cog_event_queue_free(q);
    return 0;
}

static int test_interleaved_fifo(void)
{
    cog_event_queue_t *q = NULL;
    cog_event_t ev;
    cog_event_t out;
    const cog_event_id_t expected[] = {2U, 3U, 4U, 5U};
    size_t i;

    if (cog_event_queue_new(&q, 4U) != COG_OK) {
        return 1;
    }

    ev = make_event(1U, COG_EVENT_USER_INPUT);
    if (cog_event_queue_push(q, &ev) != COG_OK) { /* 1 */
        cog_event_queue_free(q);
        return 1;
    }
    ev.id = 2U;
    if (cog_event_queue_push(q, &ev) != COG_OK) { /* 1,2 */
        cog_event_queue_free(q);
        return 1;
    }
    if (cog_event_queue_pop(q, &out) != COG_OK || out.id != 1U) { /* 2 */
        cog_event_queue_free(q);
        return 1;
    }
    ev.id = 3U;
    if (cog_event_queue_push(q, &ev) != COG_OK) { /* 2,3 */
        cog_event_queue_free(q);
        return 1;
    }
    ev.id = 4U;
    if (cog_event_queue_push(q, &ev) != COG_OK) { /* 2,3,4 */
        cog_event_queue_free(q);
        return 1;
    }
    ev.id = 5U;
    if (cog_event_queue_push(q, &ev) != COG_OK) { /* 2,3,4,5 full */
        cog_event_queue_free(q);
        return 1;
    }
    ev.id = 6U;
    if (cog_event_queue_push(q, &ev) != COG_ERROR_OVERFLOW) {
        cog_event_queue_free(q);
        return 1;
    }

    for (i = 0U; i < 4U; ++i) {
        if (cog_event_queue_pop(q, &out) != COG_OK || out.id != expected[i]) {
            cog_event_queue_free(q);
            return 1;
        }
    }
    if (cog_event_queue_count(q) != 0U) {
        cog_event_queue_free(q);
        return 1;
    }

    /* Queue is usable again after full drain. */
    ev.id = 6U;
    if (cog_event_queue_push(q, &ev) != COG_OK ||
        cog_event_queue_pop(q, &out) != COG_OK || out.id != 6U) {
        cog_event_queue_free(q);
        return 1;
    }
    cog_event_queue_free(q);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Wrap-around and repeated cycles.                                    */
/* ------------------------------------------------------------------ */

static int test_single_slot_cycles(void)
{
    cog_event_queue_t *q = NULL;
    cog_event_t ev;
    cog_event_t out;
    cog_event_id_t i;

    if (cog_event_queue_new(&q, 1U) != COG_OK) {
        return 1;
    }
    for (i = 0U; i < 64U; ++i) {
        ev = make_event(i, COG_EVENT_QUEUE_MSG);
        if (cog_event_queue_push(q, &ev) != COG_OK) {
            cog_event_queue_free(q);
            return 1;
        }
        if (cog_event_queue_count(q) != 1U || !cog_event_queue_is_full(q)) {
            cog_event_queue_free(q);
            return 1;
        }
        ev.id = 0xFFFFU; /* full: rejected */
        if (cog_event_queue_push(q, &ev) != COG_ERROR_OVERFLOW) {
            cog_event_queue_free(q);
            return 1;
        }
        if (cog_event_queue_pop(q, &out) != COG_OK || out.id != i) {
            cog_event_queue_free(q);
            return 1;
        }
        if (cog_event_queue_count(q) != 0U || !cog_event_queue_is_empty(q)) {
            cog_event_queue_free(q);
            return 1;
        }
    }
    cog_event_queue_free(q);
    return 0;
}

static int test_repeated_cycles(void)
{
    cog_event_queue_t *q = NULL;
    cog_event_t ev;
    cog_event_t out;
    cog_event_id_t next_id = 0U;
    unsigned cycle;

    if (cog_event_queue_new(&q, 3U) != COG_OK) {
        return 1;
    }

    /* 1000 full push-3 / pop-3 cycles exercise every ring offset and the
     * head/tail wrap repeatedly; 3000 ids fits comfortably in uint16. */
    for (cycle = 0U; cycle < 1000U; ++cycle) {
        unsigned j;
        for (j = 0U; j < 3U; ++j) {
            ev = make_event(next_id, COG_EVENT_SYSTEM_SIGNAL);
            if (cog_event_queue_push(q, &ev) != COG_OK) {
                cog_event_queue_free(q);
                return 1;
            }
            ++next_id;
        }
        if (cog_event_queue_count(q) != 3U || !cog_event_queue_is_full(q)) {
            cog_event_queue_free(q);
            return 1;
        }
        /* Overflow must not corrupt state. */
        ev = make_event(0x7777U, COG_EVENT_SYSTEM_SIGNAL);
        if (cog_event_queue_push(q, &ev) != COG_ERROR_OVERFLOW) {
            cog_event_queue_free(q);
            return 1;
        }
        for (j = 0U; j < 3U; ++j) {
            if (cog_event_queue_pop(q, &out) != COG_OK) {
                cog_event_queue_free(q);
                return 1;
            }
            if (out.id != (cog_event_id_t)(uint16_t)(next_id - 3U + j)) {
                cog_event_queue_free(q);
                return 1;
            }
        }
        if (cog_event_queue_count(q) != 0U || !cog_event_queue_is_empty(q) ||
            cog_event_queue_is_full(q)) {
            cog_event_queue_free(q);
            return 1;
        }
    }
    if (next_id != 3000U) {
        cog_event_queue_free(q);
        return 1;
    }
    cog_event_queue_free(q);
    return 0;
}

/* ------------------------------------------------------------------ */
/* IPC payload snapshot semantics.                                     */
/* ------------------------------------------------------------------ */

static int verify_ipc_snapshot(cog_event_kind_t kind)
{
    static const uint8_t msgbuf[] = {0x01U, 0x02U, 0x03U, 0x04U, 0x05U};
    cog_event_queue_t *q = NULL;
    cog_event_ipc_t ipc;
    cog_event_t ev;
    cog_event_t out;
    const cog_event_ipc_t *out_ipc;
    int ok = 0;

    if (cog_event_queue_new(&q, 1U) != COG_OK) {
        return 1;
    }

    ipc.peer = 0x1122334455667788ULL;
    ipc.msg_type = 0xABCDU;
    ipc.msg_payload = (const void *)msgbuf;
    ipc.msg_payload_len = sizeof(msgbuf);

    ev = make_event(7U, kind);
    ev.payload = (const void *)&ipc;
    ev.payload_len = sizeof(ipc);

    if (cog_event_queue_push(q, &ev) != COG_OK) {
        cog_event_queue_free(q);
        return 1;
    }

    /* The queue snapshots the ipc struct at push time, so mutating the
     * producer's struct afterwards must not affect the queued event. */
    ipc.peer = 0xDEADBEEFDEADBEEFULL;
    ipc.msg_type = 0U;
    ipc.msg_payload = NULL;
    ipc.msg_payload_len = 0U;

    if (cog_event_queue_pop(q, &out) != COG_OK) {
        cog_event_queue_free(q);
        return 1;
    }
    if (out.id != 7U || out.kind != kind || out.payload_len != sizeof(ipc)) {
        cog_event_queue_free(q);
        return 1;
    }
    /* Popped payload must point at queue-owned storage, not at the
     * producer's struct. */
    if (out.payload == (const void *)&ipc || out.payload == NULL) {
        cog_event_queue_free(q);
        return 1;
    }
    out_ipc = (const cog_event_ipc_t *)out.payload;
    if (out_ipc->peer != 0x1122334455667788ULL ||
        out_ipc->msg_type != 0xABCDU ||
        out_ipc->msg_payload != (const void *)msgbuf ||
        out_ipc->msg_payload_len != sizeof(msgbuf)) {
        cog_event_queue_free(q);
        return 1;
    }

    ok = 1;
    cog_event_queue_free(q);
    return ok ? 0 : 1;
}

static int test_ipc_snapshot_all_kinds(void)
{
    const size_t n = sizeof(ipc_kinds) / sizeof(ipc_kinds[0]);
    size_t i;

    for (i = 0U; i < n; ++i) {
        if (verify_ipc_snapshot(ipc_kinds[i]) != 0) {
            return 1;
        }
    }
    return 0;
}

static int test_ipc_null_payload_no_snapshot(void)
{
    cog_event_queue_t *q = NULL;
    cog_event_t ev;
    cog_event_t out;

    /* An IPC-kind event with a NULL payload is stored as-is (payload
     * remains NULL); this must not crash or fabricate a payload. */
    if (cog_event_queue_new(&q, 1U) != COG_OK) {
        return 1;
    }
    ev = make_event(1U, COG_EVENT_IPC_MESSAGE);
    ev.payload_len = 0U;
    if (cog_event_queue_push(q, &ev) != COG_OK) {
        cog_event_queue_free(q);
        return 1;
    }
    if (cog_event_queue_pop(q, &out) != COG_OK) {
        cog_event_queue_free(q);
        return 1;
    }
    if (out.payload != NULL || out.payload_len != 0U ||
        out.kind != COG_EVENT_IPC_MESSAGE) {
        cog_event_queue_free(q);
        return 1;
    }
    cog_event_queue_free(q);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Const-correct inspectors on a live queue.                           */
/* ------------------------------------------------------------------ */

static int test_const_accessors(void)
{
    cog_event_queue_t *q = NULL;
    const cog_event_queue_t *cq;
    cog_event_t ev = make_event(1U, COG_EVENT_USER_INPUT);

    if (cog_event_queue_new(&q, 2U) != COG_OK) {
        return 1;
    }
    cq = q;
    if (cog_event_queue_count(cq) != 0U || !cog_event_queue_is_empty(cq) ||
        cog_event_queue_is_full(cq)) {
        cog_event_queue_free(q);
        return 1;
    }
    if (cog_event_queue_push(q, &ev) != COG_OK) {
        cog_event_queue_free(q);
        return 1;
    }
    if (cog_event_queue_count(cq) != 1U || cog_event_queue_is_empty(cq) ||
        cog_event_queue_is_full(cq)) {
        cog_event_queue_free(q);
        return 1;
    }
    cog_event_queue_free(q);
    return 0;
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int main(void)
{
    unsigned failures = 0U;

#define CHECK(label, expr)                                                   \
    do {                                                                     \
        if ((expr) != 0) {                                                   \
            (void)fprintf(stderr, "FAIL: %s\n", label);                      \
            ++failures;                                                      \
        }                                                                    \
    } while (0)

    CHECK("kind order", verify_kind_order());
    CHECK("kind switch", verify_kind_switch());

    CHECK("new NULL out", test_new_null_out());
    CHECK("new zero capacity", test_new_zero_capacity());
    CHECK("new size overflow", test_new_size_overflow());
    CHECK("new size one", test_new_size_one());
    CHECK("free NULL", test_free_null());
    CHECK("NULL queue queries", test_null_queue_queries());

    CHECK("push NULL args", test_push_null_args());
    CHECK("pop NULL args", test_pop_null_args());
    CHECK("pop empty unchanged", test_pop_empty_unchanged());

    CHECK("roundtrip fields", test_roundtrip_fields());
    CHECK("NULL payload event", test_null_payload_event());
    CHECK("payload reference semantics", test_payload_reference_semantics());

    CHECK("FIFO order", test_fifo_order());
    CHECK("all kinds roundtrip", test_all_kinds_roundtrip());
    CHECK("fill to capacity and overflow", test_fill_and_overflow());
    CHECK("overflow recovery", test_overflow_recovery());
    CHECK("interleaved FIFO", test_interleaved_fifo());

    CHECK("single slot cycles", test_single_slot_cycles());
    CHECK("repeated cycles", test_repeated_cycles());

    CHECK("IPC snapshot all kinds", test_ipc_snapshot_all_kinds());
    CHECK("IPC NULL payload", test_ipc_null_payload_no_snapshot());

    CHECK("const accessors", test_const_accessors());

#undef CHECK

    if (failures == 0U) {
        (void)printf("test_event: ok\n");
    }
    return failures == 0U ? 0 : 1;
}

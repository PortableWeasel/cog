/**
 * @file test_agent.c
 * @brief Contract test for cog/agent.h implemented by cog-core/src/agent.c.
 *
 * agent.h declares the subagent lifecycle: spawn, cancel, status,
 * notify_parent, result, session_id, and is_cancelled.
 *
 * This test verifies at run time:
 *
 *   - spawn validation (NULL core/config/out_id, NULL/empty task,
 *     UNAVAILABLE when the registry is full)
 *   - spawn happy path: unique non-zero ids, PENDING status, child
 *     session created and linked to the parent session, group id and
 *     timeout recorded
 *   - status / session_id / is_cancelled: NULL checks, NOT_FOUND, and
 *     correct values across the lifecycle
 *   - cancel: NULL/NOT_FOUND checks, PENDING and RUNNING cancellation,
 *     INVALID_STATE on terminal agents, cancellation idempotence
 *     (is_cancelled stays true, late results rejected)
 *   - notify_parent: NULL/NOT_FOUND checks, message recorded and
 *     replaced
 *   - result: NULL/NOT_FOUND/INVALID_STATE checks, round-trip, exact
 *     length, NUL termination, OVERFLOW sizing, empty result
 *   - internal status transitions (agent_internal.h): start/complete/
 *     fail/timeout, valid and invalid transitions, terminal-state
 *     locking, argument validation
 *   - registry isolation between two cores
 *   - cleanup under stress: many agents in mixed states, then
 *     cog_core_free (leak-checked by ASan/UBSan)
 *
 * The status-transition hooks (cog_agent_internal_*) are private to
 * cog-core; this white-box test includes agent_internal.h directly (the
 * src directory is added to the test's include path in CMake).
 *
 * Ticket: 6a025eb (Stage 12.2b — Implement agent.c)
 */

#define _POSIX_C_SOURCE 200809L

#include "cog/agent.h"
#include "cog/core.h"

#include "agent_internal.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifndef COG_AGENT_H
#error "COG_AGENT_H include guard missing"
#endif

/* ------------------------------------------------------------------ */
/* Test application vocabulary (per state.h/handler.h conventions).    */
/* ------------------------------------------------------------------ */

/** Dummy handler: the agent tests never run the dispatch loop. */
static cog_handler_result_t dummy_handler(cog_core_t *core,
                                          const cog_event_t *event)
{
    (void)core;
    (void)event;
    return (cog_handler_result_t){COG_TRANSITION_SELF, COG_OK};
}

static const cog_state_t dummy_transitions[] = {
    COG_TRANSITION_SELF,
    COG_TRANSITION_END,
};

static const cog_dispatch_entry_t dummy_table[] = {
    COG_DISPATCH_ENTRY(0U, 0U, dummy_handler, dummy_transitions),
};

/* ------------------------------------------------------------------ */
/* Small assertion helpers.                                            */
/* ------------------------------------------------------------------ */

static int failures = 0;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,     \
                    #cond);                                             \
            ++failures;                                                 \
        }                                                               \
    } while (0)

#define CHECK_EQ_INT(a, b)                                              \
    do {                                                                \
        long long _a = (long long)(a);                                  \
        long long _b = (long long)(b);                                  \
        if (_a != _b) {                                                 \
            fprintf(stderr, "FAIL %s:%d: %s == %s (%lld != %lld)\n",    \
                    __FILE__, __LINE__, #a, #b, _a, _b);                \
            ++failures;                                                 \
        }                                                               \
    } while (0)

#define CHECK_EQ_STR(a, b)                                              \
    do {                                                                \
        const char *_a = (a);                                           \
        const char *_b = (b);                                           \
        if (_a == NULL || _b == NULL || strcmp(_a, _b) != 0) {          \
            fprintf(stderr, "FAIL %s:%d: %s == %s (%s != %s)\n",        \
                    __FILE__, __LINE__, #a, #b,                         \
                    _a != NULL ? _a : "(null)",                         \
                    _b != NULL ? _b : "(null)");                        \
            ++failures;                                                 \
        }                                                               \
    } while (0)

/* ------------------------------------------------------------------ */
/* Core factory                                                        */
/* ------------------------------------------------------------------ */

/** Create a core with a fresh session; the session is returned through
 *  @p out_session (may be NULL) and must be freed by the caller. */
static cog_core_t *make_core(cog_session_t **out_session)
{
    cog_core_t *core = NULL;
    cog_session_t *session = NULL;
    cog_core_config_t cfg;

    if (cog_session_new(&session, NULL) != COG_OK) {
        return NULL;
    }
    memset(&cfg, 0, sizeof(cfg));
    cfg.session = session;
    cfg.dispatch_table = dummy_table;
    cfg.dispatch_table_size = 1U;
    if (cog_core_new(&core, &cfg) != COG_OK) {
        cog_session_free(session);
        return NULL;
    }
    if (out_session != NULL) {
        *out_session = session;
    }
    return core;
}

/* ------------------------------------------------------------------ */
/* Spawn                                                               */
/* ------------------------------------------------------------------ */

static void test_spawn_validation(void)
{
    cog_core_t *core;
    cog_session_t *session;
    cog_agent_config_t cfg;
    cog_agent_id_t id = 0U;

    core = make_core(&session);
    CHECK(core != NULL);

    memset(&cfg, 0, sizeof(cfg));
    cfg.task = "task";

    /* NULL core / config / out_id. */
    CHECK_EQ_INT(cog_agent_spawn(NULL, &cfg, &id), COG_ERROR_NULL_PTR);
    CHECK_EQ_INT(cog_agent_spawn(core, NULL, &id), COG_ERROR_NULL_PTR);
    CHECK_EQ_INT(cog_agent_spawn(core, &cfg, NULL), COG_ERROR_NULL_PTR);

    /* NULL / empty task. */
    cfg.task = NULL;
    CHECK_EQ_INT(cog_agent_spawn(core, &cfg, &id), COG_ERROR_INVALID_ARG);
    cfg.task = "";
    CHECK_EQ_INT(cog_agent_spawn(core, &cfg, &id), COG_ERROR_INVALID_ARG);

    cog_core_free(core);
    cog_session_free(session);
}

static void test_spawn_happy_path(void)
{
    cog_core_t *core;
    cog_session_t *session;
    cog_agent_config_t cfg;
    cog_agent_id_t id = 0U;
    cog_agent_status_t st;
    const char *sid = NULL;
    const cog_agent_entry_t *rec;

    core = make_core(&session);
    CHECK(core != NULL);

    memset(&cfg, 0, sizeof(cfg));
    cfg.task = "summarize the meeting";
    cfg.parent_session_id = "parent-1";
    cfg.timeout_ms = 5000U;
    cfg.group_id = 7U;
    cfg.force_local = true;

    CHECK_EQ_INT(cog_agent_spawn(core, &cfg, &id), COG_OK);
    CHECK(id != 0U);

    /* Fresh agent is PENDING with a generated session id. */
    CHECK_EQ_INT(cog_agent_status(core, id, &st), COG_OK);
    CHECK_EQ_INT((int)st, (int)COG_AGENT_STATUS_PENDING);
    CHECK_EQ_INT(cog_agent_session_id(core, id, &sid), COG_OK);
    CHECK(sid != NULL);
    CHECK(strlen(sid) > 0U);

    /* The internal record carries the config: child session linked to
     * the parent, group id, timeout, and no cancellation. */
    rec = cog_agent_internal_get(core, id);
    CHECK(rec != NULL);
    CHECK_EQ_INT((int)rec->status, (int)COG_AGENT_STATUS_PENDING);
    CHECK(rec->session != NULL);
    CHECK_EQ_STR(cog_session_id(rec->session), sid);
    CHECK_EQ_STR(cog_session_parent_id(rec->session), "parent-1");
    CHECK_EQ_STR(rec->parent_session_id, "parent-1");
    CHECK_EQ_INT((long long)rec->group_id, 7LL);
    CHECK_EQ_INT((long long)rec->timeout_ms, 5000LL);
    CHECK(rec->force_local == true);
    CHECK(rec->cancelled == false);
    CHECK(rec->result == NULL);

    cog_core_free(core);
    cog_session_free(session);
}

static void test_spawn_no_parent_and_distinct_ids(void)
{
    cog_core_t *core;
    cog_session_t *session;
    cog_agent_config_t cfg;
    cog_agent_id_t id1 = 0U, id2 = 0U, id3 = 0U;
    const char *sid1 = NULL, *sid2 = NULL;

    core = make_core(&session);
    CHECK(core != NULL);

    memset(&cfg, 0, sizeof(cfg));
    cfg.task = "t1";
    cfg.parent_session_id = NULL;
    CHECK_EQ_INT(cog_agent_spawn(core, &cfg, &id1), COG_OK);
    CHECK(cog_agent_internal_get(core, id1)->parent_session_id == NULL);
    CHECK(cog_session_parent_id(cog_agent_internal_get(core, id1)->session) == NULL);
    /* Zeroed config: force_local defaults to false (mirrors the true
     * assertion in test_spawn_happy_path). */
    CHECK(cog_agent_internal_get(core, id1)->force_local == false);

    cfg.task = "t2";
    CHECK_EQ_INT(cog_agent_spawn(core, &cfg, &id2), COG_OK);
    cfg.task = "t3";
    CHECK_EQ_INT(cog_agent_spawn(core, &cfg, &id3), COG_OK);

    /* Ids are unique and monotonically increasing from 1. */
    CHECK_EQ_INT((long long)id1, 1LL);
    CHECK_EQ_INT((long long)id2, 2LL);
    CHECK_EQ_INT((long long)id3, 3LL);
    CHECK(id1 != id2);
    CHECK(id2 != id3);

    /* Each agent gets its own child session id. */
    CHECK_EQ_INT(cog_agent_session_id(core, id1, &sid1), COG_OK);
    CHECK_EQ_INT(cog_agent_session_id(core, id2, &sid2), COG_OK);
    CHECK(sid1 != NULL && sid2 != NULL);
    CHECK(strcmp(sid1, sid2) != 0);

    cog_core_free(core);
    cog_session_free(session);
}

static void test_spawn_registry_full(void)
{
    cog_core_t *core;
    cog_session_t *session;
    cog_agent_config_t cfg;
    cog_agent_id_t id = 0U;
    size_t i;
    cog_result_t rc = COG_OK;

    core = make_core(&session);
    CHECK(core != NULL);

    memset(&cfg, 0, sizeof(cfg));
    cfg.task = "x";

    for (i = 0U; i < COG_AGENT_MAX_PER_CORE; ++i) {
        rc = cog_agent_spawn(core, &cfg, &id);
        if (rc != COG_OK) {
            break;
        }
    }
    CHECK_EQ_INT((long long)i, (long long)COG_AGENT_MAX_PER_CORE);
    CHECK_EQ_INT(rc, COG_OK);

    /* No core has capacity: the registry is full. */
    CHECK_EQ_INT(cog_agent_spawn(core, &cfg, &id), COG_ERROR_UNAVAILABLE);

    cog_core_free(core);
    cog_session_free(session);
}

/* ------------------------------------------------------------------ */
/* Status / session id / is_cancelled                                  */
/* ------------------------------------------------------------------ */

static void test_status_and_not_found(void)
{
    cog_core_t *core;
    cog_session_t *session;
    cog_agent_config_t cfg;
    cog_agent_id_t id = 0U;
    cog_agent_status_t st = COG_AGENT_STATUS_PENDING;

    core = make_core(&session);
    CHECK(core != NULL);

    /* NULL checks. */
    CHECK_EQ_INT(cog_agent_status(NULL, 1U, &st), COG_ERROR_NULL_PTR);
    CHECK_EQ_INT(cog_agent_status(core, 1U, NULL), COG_ERROR_NULL_PTR);

    /* NOT_FOUND: zero id and unknown id. */
    CHECK_EQ_INT(cog_agent_status(core, 0U, &st), COG_ERROR_NOT_FOUND);
    CHECK_EQ_INT(cog_agent_status(core, 999U, &st), COG_ERROR_NOT_FOUND);

    memset(&cfg, 0, sizeof(cfg));
    cfg.task = "t";
    CHECK_EQ_INT(cog_agent_spawn(core, &cfg, &id), COG_OK);
    CHECK_EQ_INT(cog_agent_status(core, id, &st), COG_OK);
    CHECK_EQ_INT((int)st, (int)COG_AGENT_STATUS_PENDING);

    cog_core_free(core);
    cog_session_free(session);
}

static void test_session_id_checks(void)
{
    cog_core_t *core;
    cog_session_t *session;
    cog_agent_config_t cfg;
    cog_agent_id_t id = 0U;
    const char *sid = NULL;

    core = make_core(&session);
    CHECK(core != NULL);

    CHECK_EQ_INT(cog_agent_session_id(NULL, 1U, &sid), COG_ERROR_NULL_PTR);
    CHECK_EQ_INT(cog_agent_session_id(core, 1U, NULL), COG_ERROR_NULL_PTR);
    CHECK_EQ_INT(cog_agent_session_id(core, 0U, &sid), COG_ERROR_NOT_FOUND);

    memset(&cfg, 0, sizeof(cfg));
    cfg.task = "t";
    CHECK_EQ_INT(cog_agent_spawn(core, &cfg, &id), COG_OK);
    CHECK_EQ_INT(cog_agent_session_id(core, id, &sid), COG_OK);
    CHECK(sid != NULL);
    CHECK_EQ_STR(sid, cog_session_id(cog_agent_internal_get(core, id)->session));

    cog_core_free(core);
    cog_session_free(session);
}

static void test_is_cancelled_checks(void)
{
    cog_core_t *core;
    cog_session_t *session;
    cog_agent_config_t cfg;
    cog_agent_id_t id = 0U;
    bool cancelled = true;

    core = make_core(&session);
    CHECK(core != NULL);

    CHECK_EQ_INT(cog_agent_is_cancelled(NULL, 1U, &cancelled), COG_ERROR_NULL_PTR);
    CHECK_EQ_INT(cog_agent_is_cancelled(core, 1U, NULL), COG_ERROR_NULL_PTR);
    CHECK_EQ_INT(cog_agent_is_cancelled(core, 0U, &cancelled), COG_ERROR_NOT_FOUND);

    memset(&cfg, 0, sizeof(cfg));
    cfg.task = "t";
    CHECK_EQ_INT(cog_agent_spawn(core, &cfg, &id), COG_OK);
    CHECK_EQ_INT(cog_agent_is_cancelled(core, id, &cancelled), COG_OK);
    CHECK(cancelled == false);

    cog_core_free(core);
    cog_session_free(session);
}

/* ------------------------------------------------------------------ */
/* Cancel                                                              */
/* ------------------------------------------------------------------ */

static void test_cancel(void)
{
    cog_core_t *core;
    cog_session_t *session;
    cog_agent_config_t cfg;
    cog_agent_id_t id = 0U;
    cog_agent_status_t st;
    bool cancelled = false;

    core = make_core(&session);
    CHECK(core != NULL);

    CHECK_EQ_INT(cog_agent_cancel(NULL, 1U), COG_ERROR_NULL_PTR);
    CHECK_EQ_INT(cog_agent_cancel(core, 0U), COG_ERROR_NOT_FOUND);
    CHECK_EQ_INT(cog_agent_cancel(core, 4242U), COG_ERROR_NOT_FOUND);

    memset(&cfg, 0, sizeof(cfg));
    cfg.task = "t";

    /* Cancel a PENDING agent. */
    CHECK_EQ_INT(cog_agent_spawn(core, &cfg, &id), COG_OK);
    CHECK_EQ_INT(cog_agent_cancel(core, id), COG_OK);
    CHECK_EQ_INT(cog_agent_status(core, id, &st), COG_OK);
    CHECK_EQ_INT((int)st, (int)COG_AGENT_STATUS_CANCELLED);
    CHECK_EQ_INT(cog_agent_is_cancelled(core, id, &cancelled), COG_OK);
    CHECK(cancelled == true);

    /* Cancellation idempotence: a second cancel is refused (terminal),
     * and the cancelled flag stays set. */
    CHECK_EQ_INT(cog_agent_cancel(core, id), COG_ERROR_INVALID_STATE);
    CHECK_EQ_INT(cog_agent_is_cancelled(core, id, &cancelled), COG_OK);
    CHECK(cancelled == true);

    /* Cancel a RUNNING agent. */
    CHECK_EQ_INT(cog_agent_spawn(core, &cfg, &id), COG_OK);
    CHECK_EQ_INT(cog_agent_internal_start(core, id), COG_OK);
    CHECK_EQ_INT(cog_agent_cancel(core, id), COG_OK);
    CHECK_EQ_INT(cog_agent_status(core, id, &st), COG_OK);
    CHECK_EQ_INT((int)st, (int)COG_AGENT_STATUS_CANCELLED);

    /* Cancel on every terminal state is refused. */
    CHECK_EQ_INT(cog_agent_spawn(core, &cfg, &id), COG_OK);
    CHECK_EQ_INT(cog_agent_internal_start(core, id), COG_OK);
    CHECK_EQ_INT(cog_agent_internal_complete(core, id, "done", 4U), COG_OK);
    CHECK_EQ_INT(cog_agent_cancel(core, id), COG_ERROR_INVALID_STATE);

    CHECK_EQ_INT(cog_agent_spawn(core, &cfg, &id), COG_OK);
    CHECK_EQ_INT(cog_agent_internal_start(core, id), COG_OK);
    CHECK_EQ_INT(cog_agent_internal_fail(core, id), COG_OK);
    CHECK_EQ_INT(cog_agent_cancel(core, id), COG_ERROR_INVALID_STATE);

    CHECK_EQ_INT(cog_agent_spawn(core, &cfg, &id), COG_OK);
    CHECK_EQ_INT(cog_agent_internal_start(core, id), COG_OK);
    CHECK_EQ_INT(cog_agent_internal_timeout(core, id), COG_OK);
    CHECK_EQ_INT(cog_agent_cancel(core, id), COG_ERROR_INVALID_STATE);

    cog_core_free(core);
    cog_session_free(session);
}

/* ------------------------------------------------------------------ */
/* Notify parent                                                       */
/* ------------------------------------------------------------------ */

static void test_notify_parent(void)
{
    cog_core_t *core;
    cog_session_t *session;
    cog_agent_config_t cfg;
    cog_agent_id_t id = 0U;
    const cog_agent_entry_t *rec;

    core = make_core(&session);
    CHECK(core != NULL);

    CHECK_EQ_INT(cog_agent_notify_parent(NULL, 1U, "m"), COG_ERROR_NULL_PTR);
    CHECK_EQ_INT(cog_agent_notify_parent(core, 1U, NULL), COG_ERROR_NULL_PTR);
    CHECK_EQ_INT(cog_agent_notify_parent(core, 0U, "m"), COG_ERROR_NOT_FOUND);

    memset(&cfg, 0, sizeof(cfg));
    cfg.task = "t";
    CHECK_EQ_INT(cog_agent_spawn(core, &cfg, &id), COG_OK);

    CHECK_EQ_INT(cog_agent_notify_parent(core, id, "thinking"), COG_OK);
    rec = cog_agent_internal_get(core, id);
    CHECK(rec != NULL);
    CHECK_EQ_STR(rec->status_msg, "thinking");

    /* A later message replaces the earlier one. */
    CHECK_EQ_INT(cog_agent_notify_parent(core, id, "writing"), COG_OK);
    rec = cog_agent_internal_get(core, id);
    CHECK_EQ_STR(rec->status_msg, "writing");

    cog_core_free(core);
    cog_session_free(session);
}

/* ------------------------------------------------------------------ */
/* Result                                                              */
/* ------------------------------------------------------------------ */

static void test_result(void)
{
    cog_core_t *core;
    cog_session_t *session;
    cog_agent_config_t cfg;
    cog_agent_id_t id = 0U;
    char buf[64];
    size_t len = 0U;
    const char *result_text = "final answer: 42";

    core = make_core(&session);
    CHECK(core != NULL);

    CHECK_EQ_INT(cog_agent_result(NULL, 1U, buf, sizeof(buf), &len),
                 COG_ERROR_NULL_PTR);
    CHECK_EQ_INT(cog_agent_result(core, 1U, NULL, sizeof(buf), &len),
                 COG_ERROR_NULL_PTR);
    CHECK_EQ_INT(cog_agent_result(core, 0U, buf, sizeof(buf), &len),
                 COG_ERROR_NOT_FOUND);

    memset(&cfg, 0, sizeof(cfg));
    cfg.task = "t";
    CHECK_EQ_INT(cog_agent_spawn(core, &cfg, &id), COG_OK);

    /* Not completed yet: INVALID_STATE in every non-terminal state. */
    CHECK_EQ_INT(cog_agent_result(core, id, buf, sizeof(buf), &len),
                 COG_ERROR_INVALID_STATE);
    CHECK_EQ_INT(cog_agent_internal_start(core, id), COG_OK);
    CHECK_EQ_INT(cog_agent_result(core, id, buf, sizeof(buf), &len),
                 COG_ERROR_INVALID_STATE);

    /* Complete and read back. */
    CHECK_EQ_INT(cog_agent_internal_complete(core, id, result_text,
                                             strlen(result_text)),
                 COG_OK);
    CHECK_EQ_INT(cog_agent_result(core, id, buf, sizeof(buf), &len), COG_OK);
    CHECK_EQ_INT((long long)len, (long long)strlen(result_text));
    CHECK_EQ_STR(buf, result_text);

    /* Overflow: buffer too small; *out_len receives the required size. */
    len = 0U;
    CHECK_EQ_INT(cog_agent_result(core, id, buf, 4U, &len), COG_ERROR_OVERFLOW);
    CHECK_EQ_INT((long long)len, (long long)strlen(result_text));

    /* Empty result: a 1-byte buffer holds just the terminator. */
    CHECK_EQ_INT(cog_agent_spawn(core, &cfg, &id), COG_OK);
    CHECK_EQ_INT(cog_agent_internal_start(core, id), COG_OK);
    CHECK_EQ_INT(cog_agent_internal_complete(core, id, NULL, 0U), COG_OK);
    len = 99U;
    CHECK_EQ_INT(cog_agent_result(core, id, buf, 1U, &len), COG_OK);
    CHECK_EQ_INT((long long)len, 0LL);
    CHECK_EQ_INT((int)buf[0], 0); /* NUL-terminated */

    /* A zero-capacity buffer cannot hold even the terminator. */
    CHECK_EQ_INT(cog_agent_result(core, id, buf, 0U, &len), COG_ERROR_OVERFLOW);

    /* Result on a failed agent: INVALID_STATE. */
    CHECK_EQ_INT(cog_agent_spawn(core, &cfg, &id), COG_OK);
    CHECK_EQ_INT(cog_agent_internal_start(core, id), COG_OK);
    CHECK_EQ_INT(cog_agent_internal_fail(core, id), COG_OK);
    CHECK_EQ_INT(cog_agent_result(core, id, buf, sizeof(buf), &len),
                 COG_ERROR_INVALID_STATE);

    cog_core_free(core);
    cog_session_free(session);
}

/* ------------------------------------------------------------------ */
/* Internal status transitions                                         */
/* ------------------------------------------------------------------ */

static void test_internal_transitions(void)
{
    cog_core_t *core;
    cog_session_t *session;
    cog_agent_config_t cfg;
    cog_agent_id_t id = 0U;
    cog_agent_status_t st;

    core = make_core(&session);
    CHECK(core != NULL);

    /* NULL / NOT_FOUND on every hook. */
    CHECK_EQ_INT(cog_agent_internal_start(NULL, 1U), COG_ERROR_NULL_PTR);
    CHECK_EQ_INT(cog_agent_internal_start(core, 0U), COG_ERROR_NOT_FOUND);
    CHECK_EQ_INT(cog_agent_internal_complete(NULL, 1U, "r", 1U),
                 COG_ERROR_NULL_PTR);
    CHECK_EQ_INT(cog_agent_internal_complete(core, 0U, "r", 1U),
                 COG_ERROR_NOT_FOUND);
    CHECK_EQ_INT(cog_agent_internal_fail(NULL, 1U), COG_ERROR_NULL_PTR);
    CHECK_EQ_INT(cog_agent_internal_fail(core, 0U), COG_ERROR_NOT_FOUND);
    CHECK_EQ_INT(cog_agent_internal_timeout(NULL, 1U), COG_ERROR_NULL_PTR);
    CHECK_EQ_INT(cog_agent_internal_timeout(core, 0U), COG_ERROR_NOT_FOUND);
    CHECK(cog_agent_internal_get(NULL, 1U) == NULL);
    CHECK(cog_agent_internal_get(core, 0U) == NULL);

    memset(&cfg, 0, sizeof(cfg));
    cfg.task = "t";

    /* PENDING -> RUNNING -> COMPLETED. */
    CHECK_EQ_INT(cog_agent_spawn(core, &cfg, &id), COG_OK);
    CHECK_EQ_INT(cog_agent_internal_start(core, id), COG_OK);
    CHECK_EQ_INT(cog_agent_status(core, id, &st), COG_OK);
    CHECK_EQ_INT((int)st, (int)COG_AGENT_STATUS_RUNNING);
    CHECK_EQ_INT(cog_agent_internal_start(core, id), COG_ERROR_INVALID_STATE);
    CHECK_EQ_INT(cog_agent_internal_complete(core, id, "ok", 2U), COG_OK);
    CHECK_EQ_INT(cog_agent_status(core, id, &st), COG_OK);
    CHECK_EQ_INT((int)st, (int)COG_AGENT_STATUS_COMPLETED);
    /* Terminal: no further transitions. */
    CHECK_EQ_INT(cog_agent_internal_complete(core, id, "again", 5U),
                 COG_ERROR_INVALID_STATE);
    CHECK_EQ_INT(cog_agent_internal_fail(core, id), COG_ERROR_INVALID_STATE);
    CHECK_EQ_INT(cog_agent_internal_timeout(core, id), COG_ERROR_INVALID_STATE);

    /* PENDING -> RUNNING -> FAILED. */
    CHECK_EQ_INT(cog_agent_spawn(core, &cfg, &id), COG_OK);
    CHECK_EQ_INT(cog_agent_internal_start(core, id), COG_OK);
    CHECK_EQ_INT(cog_agent_internal_fail(core, id), COG_OK);
    CHECK_EQ_INT(cog_agent_status(core, id, &st), COG_OK);
    CHECK_EQ_INT((int)st, (int)COG_AGENT_STATUS_FAILED);

    /* PENDING -> RUNNING -> TIMEOUT. */
    CHECK_EQ_INT(cog_agent_spawn(core, &cfg, &id), COG_OK);
    CHECK_EQ_INT(cog_agent_internal_start(core, id), COG_OK);
    CHECK_EQ_INT(cog_agent_internal_timeout(core, id), COG_OK);
    CHECK_EQ_INT(cog_agent_status(core, id, &st), COG_OK);
    CHECK_EQ_INT((int)st, (int)COG_AGENT_STATUS_TIMEOUT);

    /* Complete from PENDING (never started) is invalid. */
    CHECK_EQ_INT(cog_agent_spawn(core, &cfg, &id), COG_OK);
    CHECK_EQ_INT(cog_agent_internal_complete(core, id, "x", 1U),
                 COG_ERROR_INVALID_STATE);

    /* Complete argument validation. */
    CHECK_EQ_INT(cog_agent_spawn(core, &cfg, &id), COG_OK);
    CHECK_EQ_INT(cog_agent_internal_start(core, id), COG_OK);
    CHECK_EQ_INT(cog_agent_internal_complete(core, id, NULL, 1U),
                 COG_ERROR_INVALID_ARG);
    CHECK_EQ_INT(cog_agent_internal_complete(core, id, "x", SIZE_MAX),
                 COG_ERROR_NOMEM);

    cog_core_free(core);
    cog_session_free(session);
}

static void test_cancellation_cooperation(void)
{
    cog_core_t *core;
    cog_session_t *session;
    cog_agent_config_t cfg;
    cog_agent_id_t id = 0U;
    cog_agent_status_t st;
    bool cancelled = false;

    core = make_core(&session);
    CHECK(core != NULL);

    memset(&cfg, 0, sizeof(cfg));
    cfg.task = "t";
    CHECK_EQ_INT(cog_agent_spawn(core, &cfg, &id), COG_OK);
    CHECK_EQ_INT(cog_agent_internal_start(core, id), COG_OK);
    CHECK_EQ_INT(cog_agent_cancel(core, id), COG_OK);
    CHECK_EQ_INT(cog_agent_is_cancelled(core, id, &cancelled), COG_OK);
    CHECK(cancelled == true);

    /* A late result must not overwrite the cancelled state. */
    CHECK_EQ_INT(cog_agent_internal_complete(core, id, "late", 4U),
                 COG_ERROR_INVALID_STATE);
    CHECK_EQ_INT(cog_agent_status(core, id, &st), COG_OK);
    CHECK_EQ_INT((int)st, (int)COG_AGENT_STATUS_CANCELLED);
    CHECK(cog_agent_internal_get(core, id)->result == NULL);

    cog_core_free(core);
    cog_session_free(session);
}

/* ------------------------------------------------------------------ */
/* Isolation and stress                                                */
/* ------------------------------------------------------------------ */

static void test_core_isolation(void)
{
    cog_core_t *core_a;
    cog_core_t *core_b;
    cog_session_t *sa;
    cog_session_t *sb;
    cog_agent_config_t cfg;
    cog_agent_id_t id_a1 = 0U, id_a2 = 0U, id_a3 = 0U, id_b1 = 0U;
    cog_agent_status_t st;
    const char *sid_a = NULL, *sid_b = NULL;

    core_a = make_core(&sa);
    core_b = make_core(&sb);
    CHECK(core_a != NULL);
    CHECK(core_b != NULL);

    memset(&cfg, 0, sizeof(cfg));
    cfg.task = "t";
    CHECK_EQ_INT(cog_agent_spawn(core_a, &cfg, &id_a1), COG_OK);
    CHECK_EQ_INT(cog_agent_spawn(core_a, &cfg, &id_a2), COG_OK);
    CHECK_EQ_INT(cog_agent_spawn(core_a, &cfg, &id_a3), COG_OK);
    CHECK_EQ_INT(cog_agent_spawn(core_b, &cfg, &id_b1), COG_OK);

    /* Both cores start their id counters at 1. */
    CHECK_EQ_INT((long long)id_a1, 1LL);
    CHECK_EQ_INT((long long)id_a2, 2LL);
    CHECK_EQ_INT((long long)id_a3, 3LL);
    CHECK_EQ_INT((long long)id_b1, 1LL);

    /* Id 1 on core A and id 1 on core B are different agents: their
     * session ids differ, and each core only sees its own records. */
    CHECK_EQ_INT(cog_agent_session_id(core_a, id_a1, &sid_a), COG_OK);
    CHECK_EQ_INT(cog_agent_session_id(core_b, id_b1, &sid_b), COG_OK);
    CHECK(sid_a != NULL && sid_b != NULL);
    CHECK(strcmp(sid_a, sid_b) != 0);

    /* An id that exists on one core is invisible on the other: core B
     * has only id 1, so ids 2 and 3 are NOT_FOUND there, while core A
     * still owns them. */
    CHECK_EQ_INT(cog_agent_status(core_a, id_b1, &st), COG_OK);
    CHECK_EQ_INT(cog_agent_status(core_b, id_a2, &st), COG_ERROR_NOT_FOUND);
    CHECK_EQ_INT(cog_agent_status(core_b, id_a3, &st), COG_ERROR_NOT_FOUND);
    CHECK_EQ_INT(cog_agent_cancel(core_b, id_a2), COG_ERROR_NOT_FOUND);
    CHECK(cog_agent_internal_get(core_b, id_a2) == NULL);
    CHECK(cog_agent_internal_get(core_b, id_a3) == NULL);

    /* Each core still sees its own agents. */
    CHECK_EQ_INT(cog_agent_status(core_a, id_a1, &st), COG_OK);
    CHECK_EQ_INT(cog_agent_status(core_a, id_a3, &st), COG_OK);
    CHECK_EQ_INT(cog_agent_status(core_b, id_b1, &st), COG_OK);

    cog_core_free(core_a);
    cog_core_free(core_b);
    cog_session_free(sa);
    cog_session_free(sb);
}

static void test_stress_cleanup(void)
{
    /* Spawn a large number of agents, drive them into every state, and
     * free the core: ASan/UBSan verify no leaks or invalid accesses. */
    cog_core_t *core;
    cog_session_t *session;
    cog_agent_config_t cfg;
    cog_agent_id_t id = 0U;
    size_t i;
    char buf[32];
    size_t len = 0U;
    cog_agent_status_t st;

    core = make_core(&session);
    CHECK(core != NULL);

    memset(&cfg, 0, sizeof(cfg));
    cfg.task = "stress";
    cfg.group_id = 3U;
    cfg.timeout_ms = 1000U;

    for (i = 0U; i < 200U; ++i) {
        CHECK_EQ_INT(cog_agent_spawn(core, &cfg, &id), COG_OK);
        switch (i % 5U) {
        case 0U: /* complete */
            CHECK_EQ_INT(cog_agent_internal_start(core, id), COG_OK);
            CHECK_EQ_INT(cog_agent_internal_complete(core, id, "r", 1U), COG_OK);
            CHECK_EQ_INT(cog_agent_result(core, id, buf, sizeof(buf), &len),
                         COG_OK);
            CHECK_EQ_INT((long long)len, 1LL);
            break;
        case 1U: /* fail */
            CHECK_EQ_INT(cog_agent_internal_start(core, id), COG_OK);
            CHECK_EQ_INT(cog_agent_internal_fail(core, id), COG_OK);
            break;
        case 2U: /* timeout */
            CHECK_EQ_INT(cog_agent_internal_start(core, id), COG_OK);
            CHECK_EQ_INT(cog_agent_internal_timeout(core, id), COG_OK);
            break;
        case 3U: /* cancel */
            CHECK_EQ_INT(cog_agent_cancel(core, id), COG_OK);
            break;
        default: /* leave running, with a status message */
            CHECK_EQ_INT(cog_agent_internal_start(core, id), COG_OK);
            CHECK_EQ_INT(cog_agent_notify_parent(core, id, "working"), COG_OK);
            break;
        }
        CHECK_EQ_INT(cog_agent_status(core, id, &st), COG_OK);
        CHECK(st != COG_AGENT_STATUS_PENDING);
    }

    /* Every agent is still queryable after the stress run. */
    CHECK_EQ_INT(cog_agent_status(core, 1U, &st), COG_OK);
    CHECK_EQ_INT(cog_agent_status(core, 200U, &st), COG_OK);

    cog_core_free(core);
    cog_session_free(session);
}

/* ------------------------------------------------------------------ */
/* Critic regression tests                                             */
/* ------------------------------------------------------------------ */

static void test_result_null_len(void)
{
    /* cog_agent_result must tolerate a NULL out_result_len on both the
     * success and the overflow paths (the header does not mark the
     * parameter optional, but the implementation accepts NULL). */
    cog_core_t *core;
    cog_session_t *session;
    cog_agent_config_t cfg;
    cog_agent_id_t id = 0U;
    char buf[64];
    const char *result_text = "0123456789";

    core = make_core(&session);
    CHECK(core != NULL);

    memset(&cfg, 0, sizeof(cfg));
    cfg.task = "t";
    CHECK_EQ_INT(cog_agent_spawn(core, &cfg, &id), COG_OK);
    CHECK_EQ_INT(cog_agent_internal_start(core, id), COG_OK);
    CHECK_EQ_INT(cog_agent_internal_complete(core, id, result_text,
                                             strlen(result_text)),
                 COG_OK);

    /* Success path with NULL out_result_len. */
    CHECK_EQ_INT(cog_agent_result(core, id, buf, sizeof(buf), NULL), COG_OK);
    CHECK_EQ_STR(buf, result_text);

    /* Overflow path with NULL out_result_len: still OVERFLOW, no write. */
    CHECK_EQ_INT(cog_agent_result(core, id, buf, 4U, NULL),
                 COG_ERROR_OVERFLOW);

    cog_core_free(core);
    cog_session_free(session);
}

static void test_notify_parent_terminal(void)
{
    /* notify_parent is not gated on status: a cancelled (terminal) agent
     * may still record a final status message.  The message is owned and
     * released with the record. */
    cog_core_t *core;
    cog_session_t *session;
    cog_agent_config_t cfg;
    cog_agent_id_t id = 0U;
    const cog_agent_entry_t *rec;

    core = make_core(&session);
    CHECK(core != NULL);

    memset(&cfg, 0, sizeof(cfg));
    cfg.task = "t";
    CHECK_EQ_INT(cog_agent_spawn(core, &cfg, &id), COG_OK);
    CHECK_EQ_INT(cog_agent_cancel(core, id), COG_OK);

    CHECK_EQ_INT(cog_agent_notify_parent(core, id, "cancelled by user"),
                 COG_OK);
    rec = cog_agent_internal_get(core, id);
    CHECK(rec != NULL);
    CHECK_EQ_STR(rec->status_msg, "cancelled by user");
    CHECK_EQ_INT((int)rec->status, (int)COG_AGENT_STATUS_CANCELLED);

    cog_core_free(core);
    cog_session_free(session);
}

static void test_registry_growth_boundaries(void)
{
    /* The registry grows lazily (initial capacity 8, doubling up to the
     * 256 maximum).  Verify ids stay contiguous and correct across every
     * growth boundary, and that the registry-full boundary is exact. */
    cog_core_t *core;
    cog_session_t *session;
    cog_agent_config_t cfg;
    cog_agent_id_t id = 0U;
    size_t i;

    core = make_core(&session);
    CHECK(core != NULL);

    memset(&cfg, 0, sizeof(cfg));
    cfg.task = "x";

    for (i = 0U; i < COG_AGENT_MAX_PER_CORE; ++i) {
        CHECK_EQ_INT(cog_agent_spawn(core, &cfg, &id), COG_OK);
        /* Ids are 1-based and contiguous. */
        CHECK_EQ_INT((long long)id, (long long)(i + 1U));
    }

    /* Exactly at the boundary the registry is full. */
    CHECK_EQ_INT(cog_agent_spawn(core, &cfg, &id), COG_ERROR_UNAVAILABLE);

    cog_core_free(core);
    cog_session_free(session);
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int main(void)
{
    test_spawn_validation();
    test_spawn_happy_path();
    test_spawn_no_parent_and_distinct_ids();
    test_spawn_registry_full();
    test_status_and_not_found();
    test_session_id_checks();
    test_is_cancelled_checks();
    test_cancel();
    test_notify_parent();
    test_result();
    test_internal_transitions();
    test_cancellation_cooperation();
    test_core_isolation();
    test_stress_cleanup();
    test_result_null_len();
    test_notify_parent_terminal();
    test_registry_growth_boundaries();

    if (failures != 0) {
        fprintf(stderr, "test_agent: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_agent: all checks passed\n");
    return 0;
}

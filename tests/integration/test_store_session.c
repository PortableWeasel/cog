/**
 * @file test_store_session.c
 * @brief Cross-module integration test for session_store.c + session.c +
 *        core.c.
 *
 * This test wires the compiled cog-core modules together along the real
 * persistence path:
 *
 *   session_store.c — the file / null backends and the public store API;
 *   session.c       — the hierarchical conversation tree being persisted;
 *   core.c          — the borrowed-store contract in cog/core.h
 *                     (config.store is accepted, cog_core_store returns it,
 *                     and the core never owns or frees the store).
 *
 * It verifies the cross-module effects a unit test of session_store.c
 * alone cannot:
 *
 *   - a full save -> load -> append -> load cycle through two independent
 *     store instances on the same base directory;
 *   - the store survives a core that borrows it: after cog_core_free the
 *     store is still usable (the core must not free borrowed state);
 *   - a null-backend store is accepted by the core and returned by
 *     cog_core_store;
 *   - a core configured with no store reports NULL.
 *
 * Ticket: 0efef06 (Stage 12.2d — Implement session_store.c)
 */

#define _POSIX_C_SOURCE 200809L

#include "cog/core.h"

#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* Test application vocabulary (minimal dispatch table for the core).  */
/* ------------------------------------------------------------------ */

enum {
    ST_IDLE = 0,
    ST_RUNNING = 1,
};

enum {
    EV_USER_INPUT = COG_EVENT_USER_INPUT,
    EV_LLM_COMPLETE = COG_EVENT_LLM_COMPLETE,
    EV_LLM_CHUNK = COG_EVENT_LLM_CHUNK,
};

static cog_handler_result_t on_user_input(cog_core_t *core,
                                          const cog_event_t *event)
{
    cog_handler_result_t r = {ST_RUNNING, COG_OK};

    (void)core;
    (void)event;
    return r;
}

static cog_handler_result_t on_llm_complete(cog_core_t *core,
                                            const cog_event_t *event)
{
    cog_handler_result_t r = {ST_IDLE, COG_OK};

    (void)core;
    (void)event;
    return r;
}

static const cog_state_t idle_transitions[] = {
    ST_RUNNING,
    COG_TRANSITION_SELF,
    COG_TRANSITION_END,
};

static const cog_state_t running_transitions[] = {
    ST_IDLE,
    COG_TRANSITION_SELF,
    COG_TRANSITION_END,
};

static const cog_dispatch_entry_t dispatch_table[] = {
    COG_DISPATCH_ENTRY(ST_IDLE, EV_USER_INPUT, on_user_input, idle_transitions),
    COG_DISPATCH_ENTRY(ST_RUNNING, EV_LLM_COMPLETE, on_llm_complete,
                       running_transitions),
};

/* ------------------------------------------------------------------ */
/* Auto-save dispatch table: handlers build a real conversation tree   */
/* so a saved session is loadable, plus an LLM_CHUNK row that stays in */
/* RUNNING (to prove chunks never trigger a save).                     */
/* ------------------------------------------------------------------ */

static cog_handler_result_t as_on_user_input(cog_core_t *core,
                                             const cog_event_t *event)
{
    cog_message_t msg;
    cog_handler_result_t r = {ST_RUNNING, COG_OK};

    (void)event;
    msg.role = COG_MESSAGE_ROLE_USER;
    msg.content = "q";
    msg.content_len = 1U;
    msg.name = NULL;
    if (cog_session_append(cog_core_session(core), &msg, NULL) != COG_OK) {
        r.status = COG_ERROR_IO;
    }
    return r;
}

static cog_handler_result_t as_on_chunk(cog_core_t *core,
                                        const cog_event_t *event)
{
    cog_handler_result_t r = {COG_TRANSITION_SELF, COG_OK};

    (void)core;
    (void)event;
    return r;
}

static cog_handler_result_t as_on_llm_complete(cog_core_t *core,
                                               const cog_event_t *event)
{
    cog_message_t msg;
    cog_handler_result_t r = {ST_IDLE, COG_OK};

    (void)event;
    msg.role = COG_MESSAGE_ROLE_ASSISTANT;
    msg.content = "a";
    msg.content_len = 1U;
    msg.name = NULL;
    if (cog_session_append(cog_core_session(core), &msg, NULL) != COG_OK) {
        r.status = COG_ERROR_IO;
    }
    return r;
}

static const cog_dispatch_entry_t autosave_table[] = {
    COG_DISPATCH_ENTRY(ST_IDLE, EV_USER_INPUT, as_on_user_input,
                       idle_transitions),
    COG_DISPATCH_ENTRY(ST_RUNNING, EV_LLM_CHUNK, as_on_chunk,
                       running_transitions),
    COG_DISPATCH_ENTRY(ST_RUNNING, EV_LLM_COMPLETE, as_on_llm_complete,
                       running_transitions),
};

/* ------------------------------------------------------------------ */
/* Failing backend: every save returns COG_ERROR_IO; other ops are     */
/* benign so the store is constructible and usable for the failure     */
/* propagation test.                                                   */
/* ------------------------------------------------------------------ */

static cog_result_t fail_save(void *store, const cog_session_t *session)
{
    (void)store;
    (void)session;
    return COG_ERROR_IO;
}

static cog_result_t fail_load(void *store, const char *id,
                              cog_session_t **out)
{
    (void)store;
    (void)id;
    *out = NULL;
    return COG_OK;
}

static cog_result_t fail_append(void *store, const cog_session_t *session,
                                const cog_session_node_t *node)
{
    (void)store;
    (void)session;
    (void)node;
    return COG_OK;
}

static cog_result_t fail_delete(void *store, const char *id)
{
    (void)store;
    (void)id;
    return COG_OK;
}

static cog_result_t fail_list(void *store, char ***out_ids,
                              size_t *out_count)
{
    (void)store;
    *out_ids = NULL;
    *out_count = 0U;
    return COG_OK;
}

static bool fail_exists(void *store, const char *id)
{
    (void)store;
    (void)id;
    return false;
}

static const cog_store_vtable_t failing_backend = {
    "failing",
    fail_save,
    fail_load,
    fail_append,
    fail_delete,
    fail_list,
    fail_exists,
};

/* Free a cog_store_list result. */
static void free_ids(char **ids, size_t count)
{
    size_t i;

    if (ids == NULL) {
        return;
    }
    for (i = 0U; i < count; ++i) {
        free(ids[i]);
    }
    free(ids);
}

/* ------------------------------------------------------------------ */
/* Assertion helpers.                                                  */
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

/* ------------------------------------------------------------------ */
/* Temp-directory helpers.                                             */
/* ------------------------------------------------------------------ */

static char *path_join(const char *a, const char *b)
{
    size_t la = strlen(a);
    size_t lb = strlen(b);
    char *p = malloc(la + 1U + lb + 1U);

    if (p == NULL) {
        return NULL;
    }
    memcpy(p, a, la);
    p[la] = '/';
    memcpy(p + la + 1U, b, lb + 1U);
    return p;
}

static void rmtree(const char *dir)
{
    DIR *dp = opendir(dir);
    struct dirent *de;

    if (dp == NULL) {
        return;
    }
    while ((de = readdir(dp)) != NULL) {
        struct stat sb;
        char *p;

        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) {
            continue;
        }
        p = path_join(dir, de->d_name);
        if (p == NULL) {
            continue;
        }
        if (lstat(p, &sb) == 0 && S_ISDIR(sb.st_mode)) {
            rmtree(p);
        } else {
            (void)unlink(p);
        }
        free(p);
    }
    (void)closedir(dp);
    (void)rmdir(dir);
}

static char *make_tmp(void)
{
    char templ[] = "/tmp/cog-store-int-XXXXXX";
    char *dir = strdup(templ);

    if (dir == NULL) {
        return NULL;
    }
    if (mkdtemp(dir) == NULL) {
        free(dir);
        return NULL;
    }
    return dir;
}

/* ------------------------------------------------------------------ */
/* Full persistence cycle through two independent store instances.     */
/* ------------------------------------------------------------------ */

static void test_full_cycle(void)
{
    char *base = make_tmp();
    cog_store_config_t cfg;
    cog_store_t *st1 = NULL;
    cog_store_t *st2 = NULL;
    cog_session_t *s = NULL;
    cog_session_t *loaded = NULL;
    cog_session_node_t *n = NULL;
    cog_session_config_t scfg;
    cog_timing_t timing;
    char **ids = NULL;
    size_t count = 0U;
    size_t i;

    CHECK(base != NULL);
    memset(&cfg, 0, sizeof(cfg));
    cfg.base_dir = base;
    CHECK(cog_store_new(&st1, &cfg) == COG_OK);
    CHECK(cog_store_new(&st2, &cfg) == COG_OK);

    /* Build a small conversation and persist it through st1. */
    memset(&scfg, 0, sizeof(scfg));
    scfg.id = "int-sess";
    CHECK(cog_session_new(&s, &scfg) == COG_OK);
    CHECK(cog_session_append(s,
                             &(cog_message_t){COG_MESSAGE_ROLE_USER, "q1",
                                              2U, NULL},
                             &n) == COG_OK);
    CHECK(cog_session_append(s,
                             &(cog_message_t){COG_MESSAGE_ROLE_ASSISTANT,
                                              "a1", 2U, NULL},
                             NULL) == COG_OK);
    memset(&timing, 0, sizeof(timing));
    timing.duration_ms = 7U;
    CHECK(cog_session_set_timing(s, n, &timing) == COG_OK);
    CHECK(cog_store_save(st1, s) == COG_OK);

    /* Load through a different store instance on the same base_dir. */
    CHECK(cog_store_load(st2, "int-sess", &loaded) == COG_OK);
    CHECK(loaded != NULL);
    CHECK(strcmp(cog_session_id(loaded), "int-sess") == 0);
    CHECK(cog_session_root(loaded) != NULL);
    CHECK(cog_node_data(cog_session_root(loaded))->role == COG_NODE_USER);
    CHECK(cog_node_data(cog_session_root(loaded))->content_len == 2U);
    CHECK(memcmp(cog_node_data(cog_session_root(loaded))->content, "q1",
                 2U) == 0);
    CHECK(cog_node_child_count(cog_session_root(loaded)) == 1U);
    /* Timing was set on the root (the node captured at append time). */
    CHECK(cog_node_data(cog_session_root(loaded))->timing.duration_ms ==
          7U);

    /* Append a third node through st2 and reload through st1. */
    CHECK(cog_session_append(loaded,
                             &(cog_message_t){COG_MESSAGE_ROLE_USER, "q2",
                                              2U, NULL},
                             &n) == COG_OK);
    CHECK(cog_store_append(st2, loaded, n) == COG_OK);
    cog_session_free(loaded);
    loaded = NULL;
    CHECK(cog_store_load(st1, "int-sess", &loaded) == COG_OK);
    CHECK(loaded != NULL);
    /* q2 was appended to the active leaf (a1), so the chain is
     * root(q1) -> a1 -> q2. */
    CHECK(cog_node_child_count(cog_session_root(loaded)) == 1U);
    {
        cog_session_node_t *a1 =
            cog_node_first_child(cog_session_root(loaded));

        CHECK(a1 != NULL);
        CHECK(cog_node_child_count(a1) == 1U);
        CHECK(cog_node_data(cog_node_first_child(a1))->content_len == 2U);
        CHECK(memcmp(cog_node_data(cog_node_first_child(a1))->content,
                     "q2", 2U) == 0);
    }

    /* List and delete through st2. */
    CHECK(cog_store_list(st2, &ids, &count) == COG_OK);
    CHECK(count == 1U);
    CHECK(strcmp(ids[0], "int-sess") == 0);
    for (i = 0U; i < count; i++) {
        free(ids[i]);
    }
    free(ids);
    ids = NULL;
    CHECK(cog_store_delete(st2, "int-sess") == COG_OK);
    CHECK(!cog_store_exists(st1, "int-sess"));

    cog_session_free(s);
    cog_session_free(loaded);
    cog_store_free(st1);
    cog_store_free(st2);
    rmtree(base);
    free(base);
}

/* ------------------------------------------------------------------ */
/* Core borrows the store (never owns or frees it).                    */
/* ------------------------------------------------------------------ */

static void test_core_borrows_store(void)
{
    char *base = make_tmp();
    cog_store_config_t cfg;
    cog_store_t *store = NULL;
    cog_core_t *core = NULL;
    cog_core_config_t ccfg;
    cog_session_t *session = NULL;
    cog_session_t *loaded = NULL;
    cog_session_config_t scfg;

    CHECK(base != NULL);
    memset(&cfg, 0, sizeof(cfg));
    cfg.base_dir = base;
    CHECK(cog_store_new(&store, &cfg) == COG_OK);
    CHECK(cog_session_new(&session, NULL) == COG_OK);

    memset(&ccfg, 0, sizeof(ccfg));
    ccfg.session = session;
    ccfg.dispatch_table = dispatch_table;
    ccfg.dispatch_table_size =
        sizeof(dispatch_table) / sizeof(dispatch_table[0]);
    ccfg.store = store;
    CHECK(cog_core_new(&core, &ccfg) == COG_OK);
    CHECK(cog_core_store(core) == store);

    /* The core borrows the store: freeing the core must not free it. */
    cog_core_free(core);
    core = NULL;

    /* The store is still fully usable after the core is gone. */
    memset(&scfg, 0, sizeof(scfg));
    scfg.id = "borrowed";
    CHECK(cog_session_new(&loaded, &scfg) == COG_OK);
    CHECK(cog_store_save(store, loaded) == COG_OK);
    CHECK(cog_store_exists(store, "borrowed"));
    cog_session_free(loaded);
    loaded = NULL;

    cog_session_free(session);
    cog_store_free(store);
    rmtree(base);
    free(base);
}

/* ------------------------------------------------------------------ */
/* Auto-save trigger (gate 9b130b7 / design e6249b6)                   */
/*                                                                     */
/* With auto_save enabled the core saves the session exactly once      */
/* after a successfully handled COG_EVENT_LLM_COMPLETE exchange,       */
/* immediately after the valid state transition is applied.  With      */
/* auto_save disabled nothing is saved.  Save errors propagate from    */
/* cog_core_run.  Chunks never save.  The store is borrowed.           */
/* ------------------------------------------------------------------ */

/* Wait hook: stop the loop on the first wait (the queue is drained). */
typedef struct autosave_wait_ctx {
    cog_core_t *core;
} autosave_wait_ctx_t;

static void autosave_stop_wait(void *arg)
{
    autosave_wait_ctx_t *ctx = (autosave_wait_ctx_t *)arg;
    cog_core_stop(ctx->core);
}

/* Build a core over the auto-save table with a stop-on-first-wait hook. */
static cog_core_t *make_autosave_core(cog_session_t *session,
                                      cog_store_t *store,
                                      autosave_wait_ctx_t *wctx)
{
    cog_core_t *core = NULL;
    cog_core_config_t ccfg;

    memset(&ccfg, 0, sizeof(ccfg));
    ccfg.session = session;
    ccfg.dispatch_table = autosave_table;
    ccfg.dispatch_table_size =
        sizeof(autosave_table) / sizeof(autosave_table[0]);
    ccfg.store = store;
    ccfg.wait_fn = autosave_stop_wait;
    ccfg.wait_arg = wctx;
    if (cog_core_new(&core, &ccfg) != COG_OK) {
        return NULL;
    }
    wctx->core = core;
    return core;
}

/* Enqueue one USER_INPUT turn followed by LLM_COMPLETE. */
static void enqueue_turn(cog_core_t *core)
{
    cog_event_t ev;

    memset(&ev, 0, sizeof(ev));
    ev.id = (cog_event_id_t)EV_USER_INPUT;
    ev.kind = COG_EVENT_USER_INPUT;
    (void)cog_core_enqueue(core, &ev);
    memset(&ev, 0, sizeof(ev));
    ev.id = (cog_event_id_t)EV_LLM_COMPLETE;
    ev.kind = COG_EVENT_LLM_COMPLETE;
    (void)cog_core_enqueue(core, &ev);
}

/* auto_save=false: a full exchange leaves the store empty. */
static void test_core_no_auto_save(void)
{
    char *base = make_tmp();
    cog_store_config_t cfg;
    cog_store_t *store = NULL;
    cog_core_t *core = NULL;
    cog_session_t *session = NULL;
    autosave_wait_ctx_t wctx;
    char **ids = NULL;
    size_t count = 0U;

    if (base == NULL) {
        ++failures;
        return;
    }
    memset(&cfg, 0, sizeof(cfg));
    cfg.base_dir = base;
    cfg.auto_save = false;
    CHECK(cog_store_new(&store, &cfg) == COG_OK);
    CHECK(cog_session_new(&session, NULL) == COG_OK);
    memset(&wctx, 0, sizeof(wctx));
    core = make_autosave_core(session, store, &wctx);
    CHECK(core != NULL);

    enqueue_turn(core);
    CHECK(cog_core_run(core) == COG_OK);

    /* auto_save disabled: nothing is persisted. */
    CHECK(cog_store_list(store, &ids, &count) == COG_OK);
    CHECK(count == 0U);
    free_ids(ids, count);

    cog_core_free(core);
    cog_session_free(session);
    cog_store_free(store);
    rmtree(base);
    free(base);
}

/* auto_save=true: exactly one saved session, loadable tree. */
static void test_core_auto_save_saves(void)
{
    char *base = make_tmp();
    cog_store_config_t cfg;
    cog_store_t *store = NULL;
    cog_store_t *verify = NULL;
    cog_core_t *core = NULL;
    cog_session_t *session = NULL;
    cog_session_t *loaded = NULL;
    autosave_wait_ctx_t wctx;
    const char *sid;
    char **ids = NULL;
    size_t count = 0U;

    if (base == NULL) {
        ++failures;
        return;
    }
    memset(&cfg, 0, sizeof(cfg));
    cfg.base_dir = base;
    cfg.auto_save = true;
    CHECK(cog_store_new(&store, &cfg) == COG_OK);
    CHECK(cog_session_new(&session, NULL) == COG_OK);
    sid = cog_session_id(session);
    CHECK(sid != NULL);
    memset(&wctx, 0, sizeof(wctx));
    core = make_autosave_core(session, store, &wctx);
    CHECK(core != NULL);

    enqueue_turn(core);
    CHECK(cog_core_run(core) == COG_OK);

    /* Exactly one saved session after one exchange. */
    CHECK(cog_store_list(store, &ids, &count) == COG_OK);
    CHECK(count == 1U);
    if (count == 1U) {
        CHECK(strcmp(ids[0], sid) == 0);
    }
    free_ids(ids, count);
    ids = NULL;
    CHECK(cog_store_exists(store, sid));

    /* Load it back through an independent store: a 2-node tree
     * (user "q" -> assistant "a"). */
    CHECK(cog_store_new(&verify, &cfg) == COG_OK);
    CHECK(cog_store_load(verify, sid, &loaded) == COG_OK);
    CHECK(loaded != NULL);
    if (loaded != NULL) {
        cog_session_node_t *root = cog_session_root(loaded);

        CHECK(root != NULL);
        if (root != NULL) {
            const cog_node_data_t *rd = cog_node_data(root);
            cog_session_node_t *child = cog_node_first_child(root);

            CHECK(rd->role == COG_NODE_USER);
            CHECK(rd->content_len == 1U);
            CHECK(memcmp(rd->content, "q", 1U) == 0);
            CHECK(child != NULL);
            if (child != NULL) {
                CHECK(cog_node_data(child)->role == COG_NODE_ASSISTANT);
                CHECK(memcmp(cog_node_data(child)->content, "a", 1U) == 0);
            }
        }
    }

    cog_session_free(loaded);
    cog_core_free(core);
    cog_session_free(session);
    cog_store_free(store);
    cog_store_free(verify);
    rmtree(base);
    free(base);
}

/* auto_save=true with a failing backend: the save error propagates from
 * cog_core_run; the session tree is intact. */
static void test_core_auto_save_failure(void)
{
    cog_store_config_t cfg;
    cog_store_t *store = NULL;
    cog_core_t *core = NULL;
    cog_session_t *session = NULL;
    autosave_wait_ctx_t wctx;

    memset(&cfg, 0, sizeof(cfg));
    cfg.backend = &failing_backend;
    cfg.auto_save = true;
    CHECK(cog_store_new(&store, &cfg) == COG_OK);
    CHECK(cog_session_new(&session, NULL) == COG_OK);
    memset(&wctx, 0, sizeof(wctx));
    core = make_autosave_core(session, store, &wctx);
    CHECK(core != NULL);

    enqueue_turn(core);
    /* The save failure (COG_ERROR_IO) propagates out of the run loop. */
    CHECK(cog_core_run(core) == COG_ERROR_IO);

    /* The state transition was applied before the save attempt, and the
     * session tree is intact (user + assistant). */
    CHECK(cog_core_state(core) == ST_IDLE);
    {
        const cog_message_t *msgs = NULL;
        size_t count = 0U;

        CHECK(cog_session_get_history(session, cog_session_active(session),
                                      &msgs, &count) == COG_OK);
        CHECK(count == 2U);
    }

    cog_core_free(core);
    cog_session_free(session);
    cog_store_free(store);
}

/* auto_save=true: LLM_CHUNK events between USER_INPUT and LLM_COMPLETE
 * never trigger a save; only LLM_COMPLETE does. */
static void test_core_chunks_do_not_save(void)
{
    char *base = make_tmp();
    cog_store_config_t cfg;
    cog_store_t *store = NULL;
    cog_core_t *core = NULL;
    cog_session_t *session = NULL;
    autosave_wait_ctx_t wctx;
    cog_event_t ev;
    char **ids = NULL;
    size_t count = 0U;

    if (base == NULL) {
        ++failures;
        return;
    }
    memset(&cfg, 0, sizeof(cfg));
    cfg.base_dir = base;
    cfg.auto_save = true;
    CHECK(cog_store_new(&store, &cfg) == COG_OK);
    CHECK(cog_session_new(&session, NULL) == COG_OK);
    memset(&wctx, 0, sizeof(wctx));
    core = make_autosave_core(session, store, &wctx);
    CHECK(core != NULL);

    /* USER_INPUT, then two LLM_CHUNKs, then LLM_COMPLETE. */
    memset(&ev, 0, sizeof(ev));
    ev.id = (cog_event_id_t)EV_USER_INPUT;
    ev.kind = COG_EVENT_USER_INPUT;
    CHECK(cog_core_enqueue(core, &ev) == COG_OK);
    memset(&ev, 0, sizeof(ev));
    ev.id = (cog_event_id_t)EV_LLM_CHUNK;
    ev.kind = COG_EVENT_LLM_CHUNK;
    CHECK(cog_core_enqueue(core, &ev) == COG_OK);
    CHECK(cog_core_enqueue(core, &ev) == COG_OK);
    memset(&ev, 0, sizeof(ev));
    ev.id = (cog_event_id_t)EV_LLM_COMPLETE;
    ev.kind = COG_EVENT_LLM_COMPLETE;
    CHECK(cog_core_enqueue(core, &ev) == COG_OK);
    CHECK(cog_core_run(core) == COG_OK);

    /* Only the LLM_COMPLETE saved: exactly one session, saved once. */
    CHECK(cog_store_list(store, &ids, &count) == COG_OK);
    CHECK(count == 1U);
    free_ids(ids, count);

    cog_core_free(core);
    cog_session_free(session);
    cog_store_free(store);
    rmtree(base);
    free(base);
}

/* auto_save=true: the borrowed store outlives the core and stays usable. */
static void test_core_store_outlives_core(void)
{
    char *base = make_tmp();
    cog_store_config_t cfg;
    cog_store_t *store = NULL;
    cog_core_t *core = NULL;
    cog_session_t *session = NULL;
    cog_session_t *loaded = NULL;
    autosave_wait_ctx_t wctx;
    char **ids = NULL;
    size_t count = 0U;

    if (base == NULL) {
        ++failures;
        return;
    }
    memset(&cfg, 0, sizeof(cfg));
    cfg.base_dir = base;
    cfg.auto_save = true;
    CHECK(cog_store_new(&store, &cfg) == COG_OK);
    CHECK(cog_session_new(&session, NULL) == COG_OK);
    memset(&wctx, 0, sizeof(wctx));
    core = make_autosave_core(session, store, &wctx);
    CHECK(core != NULL);

    enqueue_turn(core);
    CHECK(cog_core_run(core) == COG_OK);

    /* Free the core; the borrowed store must remain usable. */
    cog_core_free(core);
    core = NULL;

    CHECK(cog_store_list(store, &ids, &count) == COG_OK);
    CHECK(count == 1U);
    free_ids(ids, count);
    ids = NULL;

    /* The store still loads and saves after the core is gone. */
    CHECK(cog_store_load(store, cog_session_id(session), &loaded) == COG_OK);
    CHECK(loaded != NULL);
    cog_session_free(loaded);
    loaded = NULL;

    cog_session_free(session);
    cog_store_free(store);
    rmtree(base);
    free(base);
}

/* ------------------------------------------------------------------ */
/* Core with a null-backend store, and with no store at all.           */
/* ------------------------------------------------------------------ */

static void test_core_null_and_no_store(void)
{
    cog_store_config_t cfg;
    cog_store_t *null_store = NULL;
    cog_core_t *core = NULL;
    cog_core_config_t ccfg;
    cog_session_t *session = NULL;

    CHECK(cog_session_new(&session, NULL) == COG_OK);

    /* Null-backend store is accepted and returned. */
    memset(&cfg, 0, sizeof(cfg));
    cfg.backend = &cog_store_null_backend;
    CHECK(cog_store_new(&null_store, &cfg) == COG_OK);
    memset(&ccfg, 0, sizeof(ccfg));
    ccfg.session = session;
    ccfg.dispatch_table = dispatch_table;
    ccfg.dispatch_table_size =
        sizeof(dispatch_table) / sizeof(dispatch_table[0]);
    ccfg.store = null_store;
    CHECK(cog_core_new(&core, &ccfg) == COG_OK);
    CHECK(cog_core_store(core) == null_store);
    cog_core_free(core);
    core = NULL;
    cog_store_free(null_store);

    /* No store configured -> cog_core_store returns NULL. */
    memset(&ccfg, 0, sizeof(ccfg));
    ccfg.session = session;
    ccfg.dispatch_table = dispatch_table;
    ccfg.dispatch_table_size =
        sizeof(dispatch_table) / sizeof(dispatch_table[0]);
    CHECK(cog_core_new(&core, &ccfg) == COG_OK);
    CHECK(cog_core_store(core) == NULL);
    cog_core_free(core);
    core = NULL;

    cog_session_free(session);
}

int main(void)
{
    test_full_cycle();
    test_core_borrows_store();
    test_core_null_and_no_store();
    test_core_no_auto_save();
    test_core_auto_save_saves();
    test_core_auto_save_failure();
    test_core_chunks_do_not_save();
    test_core_store_outlives_core();

    if (failures == 0U) {
        printf("test_store_session: ok\n");
    }
    return failures == 0U ? 0 : 1;
}

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

    if (failures == 0U) {
        printf("test_store_session: ok\n");
    }
    return failures == 0U ? 0 : 1;
}

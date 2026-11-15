/**
 * @file test_13_session_persistence.c
 * @brief Scenario 13 — session persistence (save/load tree fidelity).
 *
 * The scenario doc (docs/scenarios/13-session-persistence.md) covers the
 * store vtable (save/load/append/delete/list/exists) and the file
 * backend.  This test drives the real @c cog_store_* and @c cog_session_*
 * APIs against the file backend in a temp directory.
 *
 * What is verified:
 *  1. Save: a session with a stable id and a multi-node tree is saved.
 *  2. Exists: the store reports the session present.
 *  3. Load: a fresh session loaded back has identical history (roles,
 *     content, lengths) and stable node ids.
 *  4. Append: an incremental node append is persisted and visible on a
 *     reload.
 *  5. List: the session id appears in the store listing.
 *  6. Delete: durable lock anchors remain protected from deletion.
 */
#define _POSIX_C_SOURCE 200809L

#include "cog/session.h"
#include "cog/session_store.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures = 0;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            (void)fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__,         \
                          __LINE__, #cond);                             \
            ++failures;                                                 \
        }                                                               \
    } while (0)

#define CHECK_EQ_INT(a, b)                                              \
    do {                                                                \
        long long _a = (long long)(a);                                  \
        long long _b = (long long)(b);                                  \
        if (_a != _b) {                                                 \
            (void)fprintf(stderr, "FAIL %s:%d: %s == %s (%lld != %lld)\n", \
                          __FILE__, __LINE__, #a, #b, _a, _b);          \
            ++failures;                                                 \
        }                                                               \
    } while (0)

/* Test-only teardown after all stores/guards are destroyed. */
static void remove_tree(const char *path)
{
    DIR *directory = opendir(path);
    struct dirent *entry;
    CHECK(directory != NULL);
    if (directory == NULL) return;
    while ((entry = readdir(directory)) != NULL) {
        char child[512];
        struct stat info;
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        CHECK(snprintf(child, sizeof(child), "%s/%s", path, entry->d_name) > 0);
        CHECK(lstat(child, &info) == 0);
        if (S_ISDIR(info.st_mode)) remove_tree(child);
        else CHECK(unlink(child) == 0);
    }
    CHECK(closedir(directory) == 0);
    CHECK(rmdir(path) == 0);
}

/** Build a session with a user + assistant node (stable id). */
static cog_session_t *make_session(const char *id)
{
    cog_session_t *s = NULL;
    cog_session_config_t cfg;
    cog_message_t m;

    (void)memset(&cfg, 0, sizeof(cfg));
    cfg.id = id;
    if (cog_session_new(&s, &cfg) != COG_OK) {
        return NULL;
    }

    (void)memset(&m, 0, sizeof(m));
    m.role = COG_MESSAGE_ROLE_USER;
    m.content = "hello";
    m.content_len = 5U;
    if (cog_session_append(s, &m, NULL) != COG_OK) {
        cog_session_free(s);
        return NULL;
    }

    (void)memset(&m, 0, sizeof(m));
    m.role = COG_MESSAGE_ROLE_ASSISTANT;
    m.content = "hi there";
    m.content_len = 8U;
    if (cog_session_append(s, &m, NULL) != COG_OK) {
        cog_session_free(s);
        return NULL;
    }
    return s;
}

/** Read history of @p s into @p out (max @p cap); returns count. */
static size_t read_history(cog_session_t *s, cog_message_t *out, size_t cap)
{
    const cog_message_t *msgs = NULL;
    size_t count = 0U;
    size_t i;

    if (cog_session_get_history(s, cog_session_active(s), &msgs, &count) !=
        COG_OK) {
        return 0U;
    }
    if (count > cap) {
        count = cap;
    }
    for (i = 0U; i < count; ++i) {
        out[i] = msgs[i];
    }
    return count;
}

static void scenario_persistence(void)
{
    char dir[] = "/tmp/cog-sc13-store-XXXXXX";
    cog_store_config_t store_cfg;
    cog_store_t *store = NULL;
    cog_session_t *s = NULL;
    cog_session_t *loaded = NULL;
    cog_message_t a[8];
    cog_message_t b[8];
    size_t na;
    size_t nb;
    size_t i;
    char **ids = NULL;
    size_t id_count = 0U;
    size_t found = 0U;
    cog_session_node_t *active;
    cog_message_t m;
    cog_session_node_t *appended = NULL;

    if (mkdtemp(dir) == NULL) {
        (void)fprintf(stderr, "mkdtemp failed\n");
        ++failures;
        return;
    }

    (void)memset(&store_cfg, 0, sizeof(store_cfg));
    store_cfg.base_dir = dir;
    store_cfg.auto_save = true;
    CHECK(cog_store_new(&store, &store_cfg) == COG_OK);

    /* Build + save. */
    s = make_session("sc13-session");
    CHECK(s != NULL);
    CHECK(cog_store_save(store, s) == COG_OK);
    CHECK(cog_store_exists(store, "sc13-session"));

    /* Load into a fresh session and compare tree fidelity. */
    CHECK(cog_store_load(store, "sc13-session", &loaded) == COG_OK);
    CHECK(loaded != NULL);
    CHECK(strcmp(cog_session_id(loaded), "sc13-session") == 0);

    na = read_history(s, a, 8U);
    nb = read_history(loaded, b, 8U);
    CHECK_EQ_INT((long long)na, 2LL);
    CHECK_EQ_INT((long long)nb, 2LL);
    for (i = 0U; i < na && i < nb; ++i) {
        CHECK(a[i].role == b[i].role);
        CHECK(a[i].content_len == b[i].content_len);
        CHECK(memcmp(a[i].content, b[i].content, a[i].content_len) == 0);
    }

    /* Stable node ids across save/load. */
    {
        const cog_session_node_t *na_node = cog_session_active(s);
        const cog_session_node_t *nb_node = cog_session_active(loaded);

        CHECK(na_node != NULL);
        CHECK(nb_node != NULL);
        CHECK(cog_node_data(na_node)->node_id ==
              cog_node_data(nb_node)->node_id);
    }

    /* Append an incremental node and verify it persists on reload. */
    active = cog_session_active(loaded);
    (void)memset(&m, 0, sizeof(m));
    m.role = COG_MESSAGE_ROLE_ASSISTANT;
    m.content = "more";
    m.content_len = 4U;
    CHECK(cog_session_add_message(loaded, active, &m, &appended) == COG_OK);
    CHECK(cog_store_append(store, loaded, appended) == COG_OK);

    cog_session_free(loaded);
    loaded = NULL;
    CHECK(cog_store_load(store, "sc13-session", &loaded) == COG_OK);
    nb = read_history(loaded, b, 8U);
    CHECK_EQ_INT((long long)nb, 3LL);
    CHECK(b[2].role == COG_MESSAGE_ROLE_ASSISTANT);
    CHECK(b[2].content_len == 4U);
    CHECK(memcmp(b[2].content, "more", 4U) == 0);

    /* List: the session id is present. */
    CHECK(cog_store_list(store, &ids, &id_count) == COG_OK);
    for (i = 0U; i < id_count; ++i) {
        if (strcmp(ids[i], "sc13-session") == 0) {
            found++;
        }
        free(ids[i]);
    }
    free(ids);
    CHECK_EQ_INT((long long)found, 1LL);

    /* Publication creates a stable ownership anchor, even while unowned. */
    CHECK(cog_store_delete(store, "sc13-session") == COG_ERROR_UNSUPPORTED);
    CHECK(cog_store_exists(store, "sc13-session"));

    cog_session_free(loaded);
    cog_session_free(s);
    cog_store_free(store);

    remove_tree(dir);
}

/* One assistant call and its correlated result survive durable admission. */
static void scenario_tool_identity_roundtrip(void)
{
    char dir[] = "/tmp/cog-sc13-tool-XXXXXX";
    cog_store_config_t store_cfg;
    cog_store_t *store = NULL;
    cog_session_t *s = NULL;
    cog_session_t *loaded = NULL;
    cog_session_node_t *root = NULL;
    cog_session_node_t *tc = NULL;
    cog_session_node_t *tr = NULL;
    cog_message_t m;
    const cog_message_t *msgs = NULL;
    size_t count = 0U;
    const cog_message_tool_call_t call = {"search-1", "search", "{\"q\":\"X\"}", 9U};

    if (mkdtemp(dir) == NULL) {
        (void)fprintf(stderr, "mkdtemp failed\n");
        ++failures;
        return;
    }

    (void)memset(&store_cfg, 0, sizeof(store_cfg));
    store_cfg.base_dir = dir;
    CHECK(cog_store_new(&store, &store_cfg) == COG_OK);

    /* Persist STARTED before accepting the simulated tool's result. */
    CHECK(cog_session_new(&s, &(cog_session_config_t){.id = "sc13-tool"}) ==
          COG_OK);
    (void)memset(&m, 0, sizeof(m));
    m.role = COG_MESSAGE_ROLE_USER;
    m.content = "q";
    m.content_len = 1U;
    CHECK(cog_session_add_message(s, NULL, &m, &root) == COG_OK);
    (void)memset(&m, 0, sizeof(m));
    m.role = COG_MESSAGE_ROLE_ASSISTANT;
    m.tool_calls = &call;
    m.tool_call_count = 1U;
    CHECK(cog_session_add_message(s, root, &m, &tc) == COG_OK);
    CHECK(cog_session_tool_transition(s, tc, "search-1",
          COG_TOOL_EXECUTION_PROPOSED, COG_TOOL_EXECUTION_STARTED) == COG_OK);
    CHECK(cog_store_save(store, s) == COG_OK);
    (void)memset(&m, 0, sizeof(m));
    m.role = COG_MESSAGE_ROLE_TOOL;
    m.tool_call_id = "search-1";
    m.name = "search";
    m.content = "results for X";
    m.content_len = sizeof("results for X") - 1U;
    CHECK(cog_session_append(s, &m, &tr) == COG_OK);
    CHECK(cog_session_tool_transition(s, tc, "search-1",
          COG_TOOL_EXECUTION_STARTED, COG_TOOL_EXECUTION_SUCCEEDED) == COG_OK);

    /* Descriptive names are separate from the stable call ID. */
    CHECK(cog_node_data(tr)->tool_name != NULL);
    CHECK(strcmp(cog_node_data(tr)->tool_name, "search") == 0);

    CHECK(cog_store_save(store, s) == COG_OK);
    CHECK(cog_store_load(store, "sc13-tool", &loaded) == COG_OK);
    CHECK(loaded != NULL);

    /* History identity survives the round-trip. */
    CHECK(cog_session_get_history(loaded, cog_session_active(loaded),
                                  &msgs, &count) == COG_OK);
    CHECK_EQ_INT((long long)count, 3LL);
    CHECK(msgs[1].role == COG_MESSAGE_ROLE_ASSISTANT);
    CHECK(msgs[1].tool_call_count == 1U);
    CHECK(strcmp(msgs[1].tool_calls[0].id, "search-1") == 0);
    CHECK(strcmp(msgs[1].tool_calls[0].name, "search") == 0);
    CHECK(msgs[2].role == COG_MESSAGE_ROLE_TOOL);
    CHECK(msgs[2].name != NULL);
    CHECK(strcmp(msgs[2].name, "search") == 0);
    CHECK(strcmp(msgs[2].tool_call_id, "search-1") == 0);
    CHECK(msgs[2].content != NULL);
    CHECK(strcmp(msgs[2].content, "results for X") == 0);

    cog_session_free(loaded);
    cog_session_free(s);
    cog_store_free(store);

    remove_tree(dir);
}

int main(void)
{
    scenario_persistence();
    scenario_tool_identity_roundtrip();

    if (failures != 0) {
        (void)fprintf(stderr, "test_13_session_persistence: %d failure(s)\n",
                      failures);
        return 1;
    }
    (void)printf("test_13_session_persistence: all checks passed\n");
    return 0;
}

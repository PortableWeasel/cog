/**
 * @file test_17_session_migration.c
 * @brief Scenario 17: serialize / transfer / rebuild a session on
 *        another core/store, update routing, preserve history, avoid
 *        double-free.
 *
 * The scenario doc (docs/scenarios/17-session-migration.md) names
 * cog_session_serialize / cog_session_deserialize and
 * cog_ipc_endpoint_register as the migration transport.  Those APIs are
 * **not yet implemented** in this phase (the headers declare neither
 * function), so this test uses the honest current boundary: the file
 * session store is the transfer format.
 *
 *   - serialize:  cog_store_save writes the session tree to the file
 *                 backend under <store_dir>/<session_id>/.
 *   - transfer:   cog_store_load reads it back into a fresh session
 *                 owned by a second core/store (a different app).
 *   - rebuild:    the loaded session's history matches the source.
 *   - routing:    the store's session-id -> file mapping is the routing
 *                 table; the migrated session is addressable by the same
 *                 id on the destination store.
 *   - no double-free: the source session and the migrated session are
 *                 each freed exactly once (ASan-clean).
 *
 * Gaps documented: cog_session_serialize/deserialize and the IPC routing
 * update are not yet implemented; the file store is the transfer medium.
 */
#define _POSIX_C_SOURCE 200809L

#include "cog_cli/cli.h"
#include "cog_cli/mock_provider.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

/** Build a single-prompt app from options (fills parse defaults). */
static cog_cli_app_t *make_app(cog_cli_options_t *opts)
{
    cog_cli_app_t *app = NULL;

    if (opts->model_id == NULL) {
        opts->model_id = "mock-model";
    }
    if (opts->endpoint_name == NULL) {
        opts->endpoint_name = "mock";
    }
    opts->interactive = false;
    opts->backend = COG_CLI_BACKEND_MOCK;
    opts->explicit_options |= COG_CLI_OPT_BACKEND | COG_CLI_OPT_MODEL | COG_CLI_OPT_ENDPOINT;
    if (opts->store_dir != NULL) {
        opts->store_mode = COG_CLI_STORE_DURABLE;
        opts->explicit_options |= COG_CLI_OPT_STORE_MODE | COG_CLI_OPT_STORE_DIR;
    }
    if (cog_cli_app_new(&app, opts) != COG_OK) {
        return NULL;
    }
    return app;
}

/** Read the session history into a caller array (max @p cap). */
static size_t read_history(cog_session_t *session, cog_message_t *out,
                           size_t cap)
{
    const cog_message_t *msgs = NULL;
    size_t count = 0U;
    size_t i;

    if (cog_session_get_history(session, cog_session_active(session),
                                &msgs, &count) != COG_OK) {
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

static void test_migrate_via_store(void)
{
    char dir[] = "/tmp/cog-17-store-XXXXXX";
    cog_cli_options_t opts;
    cog_cli_app_t *src = NULL;   /* source core/store */
    cog_cli_app_t *dst = NULL;   /* destination core/store */
    cog_session_t *migrated = NULL;
    const char *sid;
    cog_message_t src_hist[8];
    cog_message_t dst_hist[8];
    size_t src_n;
    size_t dst_n;
    size_t i;

    if (mkdtemp(dir) == NULL) {
        (void)fprintf(stderr, "mkdtemp failed\n");
        ++failures;
        return;
    }

    /* Source: run one turn and persist it. */
    (void)memset(&opts, 0, sizeof(opts));
    opts.prompt = "migrate me";
    opts.chunks = "migrated answer";
    opts.store_dir = dir;
    src = make_app(&opts);
    CHECK(src != NULL);
    CHECK(cog_cli_app_run(src) == COG_OK);

    sid = cog_session_id(cog_cli_app_session(src));
    CHECK(sid != NULL);
    src_n = read_history(cog_cli_app_session(src), src_hist, 8U);
    CHECK_EQ_INT((long long)src_n, 2LL);

    /* Serialize: the store auto-saved on LLM_COMPLETE; save explicitly
     * to make the transfer boundary explicit. */
    CHECK(cog_store_save(cog_cli_app_store(src),
                         cog_cli_app_session(src)) == COG_OK);
    CHECK(cog_store_exists(cog_cli_app_store(src), sid));

    /* Destination: a second app sharing the same store directory. */
    (void)memset(&opts, 0, sizeof(opts));
    opts.prompt = NULL; /* no auto prompt; we load instead */
    opts.store_dir = dir;
    dst = make_app(&opts);
    CHECK(dst != NULL);

    /* Transfer + rebuild: load the session from the store into a fresh
     * session owned by the destination store. */
    CHECK(cog_store_load(cog_cli_app_store(dst), sid, &migrated) == COG_OK);
    CHECK(migrated != NULL);
    CHECK(strcmp(cog_session_id(migrated), sid) == 0);

    /* Routing: the migrated session is addressable by the same id on the
     * destination store. */
    CHECK(cog_store_exists(cog_cli_app_store(dst), sid));

    /* History preserved across the transfer. */
    dst_n = read_history(migrated, dst_hist, 8U);
    CHECK_EQ_INT((long long)dst_n, (long long)src_n);
    for (i = 0U; i < dst_n; ++i) {
        CHECK(dst_hist[i].role == src_hist[i].role);
        CHECK_EQ_INT((long long)dst_hist[i].content_len,
                     (long long)src_hist[i].content_len);
        CHECK(memcmp(dst_hist[i].content, src_hist[i].content,
                     src_hist[i].content_len) == 0);
    }

    /* No double-free: the migrated session is freed once here; the source
     * session is freed once by cog_cli_app_free.  ASan verifies. */
    cog_session_free(migrated);
    migrated = NULL;

    /* Clean up the store files so the temp dir can be removed.  Do this
     * before freeing the apps (which free their stores). */
    (void)cog_store_delete(cog_cli_app_store(src), sid);

    cog_cli_app_free(dst);
    cog_cli_app_free(src);

    (void)rmdir(dir);
}

int main(void)
{
    test_migrate_via_store();

    if (failures != 0) {
        (void)fprintf(stderr, "test_17_session_migration: %d failure(s)\n",
                      failures);
        return 1;
    }
    (void)printf("test_17_session_migration: all checks passed\n");
    return 0;
}

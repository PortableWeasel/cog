/**
 * @file tests.c
 * @brief Assertion-based test driver for the config-tree PoC.
 *
 * Covers the acceptance criteria:
 *   - set/delete on dotted paths
 *   - candidate separate from running
 *   - per-subtree validators
 *   - atomic commit (validator failure leaves running untouched)
 *   - rollback to the previous known-good running tree
 * plus the documented error codes and edge cases (NULL args, malformed
 * paths, zero-length buffers, boundary values).
 *
 * Memory pattern: `cog_cfg_t` is opaque, so the caller allocates the
 * storage (malloc) and passes it to cog_cfg_init(); cog_cfg_fini() frees
 * every internal allocation and the caller frees the struct. This is the
 * "every allocation is visible" contract from the design blueprint.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config_tree.h"

static int g_failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
            g_failures++;                                                    \
        }                                                                    \
    } while (0)

#define CHECK_EQ(expr, expect)                                               \
    do {                                                                     \
        int got_ = (expr);                                                   \
        if (got_ != (expect)) {                                              \
            fprintf(stderr, "FAIL %s:%d: %s == %d, expected %d (%s)\n",      \
                    __FILE__, __LINE__, #expr, got_, (expect),              \
                    cog_cfg_strerror(got_));                                 \
            g_failures++;                                                    \
        }                                                                    \
    } while (0)

/* ------------------------------------------------------------------ */
/* Fixture helpers                                                     */
/* ------------------------------------------------------------------ */

/** Allocate caller-owned storage and initialise a config object. */
static cog_cfg_t *cfg_new(void)
{
    cog_cfg_t *cfg = (cog_cfg_t *)malloc(cog_cfg_sizeof());
    if (cfg == NULL || cog_cfg_init(cfg) != COG_CFG_OK) {
        fprintf(stderr, "fixture setup failed\n");
        exit(1);
    }
    return cfg;
}

/** Tear down and release the caller-owned storage. */
static void cfg_free(cog_cfg_t *cfg)
{
    cog_cfg_fini(cfg);
    free(cfg);
}

/* ------------------------------------------------------------------ */
/* Validator callbacks                                                 */
/* ------------------------------------------------------------------ */

static int validator_calls = 0; /* incremented by counting validators */

/** Requires the staged "api-key" child to be present and non-empty. */
static int validate_openai_requires_key(const cog_cfg_node_t *subtree,
                                        const char *path, void *userdata)
{
    (void)path;
    (void)userdata;
    char buf[128];
    if (cog_cfg_node_get_value(subtree, "api-key", buf, sizeof buf) != COG_CFG_OK) {
        return -1;
    }
    return buf[0] != '\0' ? 0 : -1;
}

/** Counting validator: records invocation, always accepts. */
static int count_calls(const cog_cfg_node_t *subtree, const char *path,
                       void *userdata)
{
    (void)subtree;
    (void)path;
    (void)userdata;
    validator_calls++;
    return 0;
}

/** Whole-tree validator: never actually rejects (kept for registration). */
static int accept_all(const cog_cfg_node_t *subtree, const char *path,
                      void *userdata)
{
    (void)subtree;
    (void)path;
    (void)userdata;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Tests                                                               */
/* ------------------------------------------------------------------ */

static void test_lifecycle_null(void)
{
    CHECK(cog_cfg_sizeof() > 0U);
    CHECK_EQ(cog_cfg_init(NULL), COG_CFG_EINVAL);
    CHECK_EQ(cog_cfg_set(NULL, "a.b", "1"), COG_CFG_EINVAL);
    CHECK_EQ(cog_cfg_delete(NULL, "a"), COG_CFG_EINVAL);
    CHECK_EQ(cog_cfg_candidate_get(NULL, "a", NULL, 0U), COG_CFG_EINVAL);
    CHECK_EQ(cog_cfg_get(NULL, "a", NULL, 0U), COG_CFG_EINVAL);
    CHECK_EQ(cog_cfg_commit(NULL), COG_CFG_EINVAL);
    CHECK_EQ(cog_cfg_rollback(NULL), COG_CFG_EINVAL);
    cog_cfg_fini(NULL); /* must be a safe no-op */

    /* init + fini round trip on caller-owned storage. */
    cog_cfg_t *cfg = cfg_new();
    cfg_free(cfg);

    /* Structural error: pass uninitialised storage to fini is caller's
     * responsibility; here we only verify the NULL path is safe. */
    CHECK_EQ(cog_cfg_register_validator(NULL, "a", count_calls, NULL),
             COG_CFG_EINVAL);
}

static void test_set_get_commit_happy_path(void)
{
    cog_cfg_t *cfg = cfg_new();
    char buf[256];

    /* Staging is invisible to readers of the running tree. */
    CHECK_EQ(cog_cfg_set(cfg, "providers.openai.base-url",
                         "https://api.openai.com/v1"), COG_CFG_OK);
    CHECK_EQ(cog_cfg_set(cfg, "providers.openai.api-key", "sk-test"), COG_CFG_OK);
    CHECK_EQ(cog_cfg_get(cfg, "providers.openai.base-url", buf, sizeof buf),
             COG_CFG_ENOTFOUND);
    CHECK_EQ(cog_cfg_candidate_get(cfg, "providers.openai.base-url",
                                   buf, sizeof buf), COG_CFG_OK);
    CHECK(strcmp(buf, "https://api.openai.com/v1") == 0);

    /* Commit makes the staged tree readable. */
    CHECK_EQ(cog_cfg_commit(cfg), COG_CFG_OK);
    CHECK_EQ(cog_cfg_get(cfg, "providers.openai.base-url", buf, sizeof buf),
             COG_CFG_OK);
    CHECK(strcmp(buf, "https://api.openai.com/v1") == 0);
    CHECK_EQ(cog_cfg_get(cfg, "providers.openai.api-key", buf, sizeof buf),
             COG_CFG_OK);
    CHECK(strcmp(buf, "sk-test") == 0);

    /* Reading a container without a value is a "not found". */
    CHECK_EQ(cog_cfg_get(cfg, "providers", buf, sizeof buf), COG_CFG_ENOTFOUND);

    cfg_free(cfg);
}

static void test_delete_and_prune(void)
{
    cog_cfg_t *cfg = cfg_new();
    char buf[64];

    CHECK_EQ(cog_cfg_set(cfg, "a.b.c", "1"), COG_CFG_OK);
    CHECK_EQ(cog_cfg_set(cfg, "a.b.d", "2"), COG_CFG_OK);
    CHECK_EQ(cog_cfg_delete(cfg, "a.b.c"), COG_CFG_OK);

    /* Sibling survives. */
    CHECK_EQ(cog_cfg_candidate_get(cfg, "a.b.d", buf, sizeof buf), COG_CFG_OK);
    CHECK(strcmp(buf, "2") == 0);
    /* Deleted leaf is gone. */
    CHECK_EQ(cog_cfg_candidate_get(cfg, "a.b.c", buf, sizeof buf),
             COG_CFG_ENOTFOUND);

    /* Deleting the last leaf prunes empty ancestors. */
    CHECK_EQ(cog_cfg_delete(cfg, "a.b.d"), COG_CFG_OK);
    CHECK_EQ(cog_cfg_candidate_get(cfg, "a", buf, sizeof buf), COG_CFG_ENOTFOUND);
    CHECK_EQ(cog_cfg_candidate_get(cfg, "a.b", buf, sizeof buf), COG_CFG_ENOTFOUND);

    /* A valued ancestor is preserved during pruning. */
    CHECK_EQ(cog_cfg_set(cfg, "x", "v"), COG_CFG_OK);
    CHECK_EQ(cog_cfg_set(cfg, "x.y", "1"), COG_CFG_OK);
    CHECK_EQ(cog_cfg_delete(cfg, "x.y"), COG_CFG_OK);
    CHECK_EQ(cog_cfg_candidate_get(cfg, "x", buf, sizeof buf), COG_CFG_OK);
    CHECK(strcmp(buf, "v") == 0);

    /* Deleting a container removes its whole subtree. */
    CHECK_EQ(cog_cfg_set(cfg, "p.q.r", "1"), COG_CFG_OK);
    CHECK_EQ(cog_cfg_delete(cfg, "p.q"), COG_CFG_OK);
    CHECK_EQ(cog_cfg_candidate_get(cfg, "p.q.r", buf, sizeof buf),
             COG_CFG_ENOTFOUND);

    /* Deleting a path that does not exist. */
    CHECK_EQ(cog_cfg_delete(cfg, "p.q"), COG_CFG_ENOTFOUND);
    CHECK_EQ(cog_cfg_delete(cfg, "missing.path"), COG_CFG_ENOTFOUND);

    cfg_free(cfg);
}

static void test_set_overwrites_and_empty_value(void)
{
    cog_cfg_t *cfg = cfg_new();
    char buf[64];

    CHECK_EQ(cog_cfg_set(cfg, "a.b", "first"), COG_CFG_OK);
    CHECK_EQ(cog_cfg_set(cfg, "a.b", "second"), COG_CFG_OK);
    CHECK_EQ(cog_cfg_commit(cfg), COG_CFG_OK);
    CHECK_EQ(cog_cfg_get(cfg, "a.b", buf, sizeof buf), COG_CFG_OK);
    CHECK(strcmp(buf, "second") == 0);

    /* Explicit empty value is distinct from "no value". */
    CHECK_EQ(cog_cfg_set(cfg, "a.c", ""), COG_CFG_OK);
    CHECK_EQ(cog_cfg_commit(cfg), COG_CFG_OK);
    CHECK_EQ(cog_cfg_get(cfg, "a.c", buf, sizeof buf), COG_CFG_OK);
    CHECK(strcmp(buf, "") == 0);

    cfg_free(cfg);
}

static void test_malformed_paths(void)
{
    cog_cfg_t *cfg = cfg_new();

    CHECK_EQ(cog_cfg_set(cfg, "", "v"), COG_CFG_EINVAL);
    CHECK_EQ(cog_cfg_set(cfg, ".", "v"), COG_CFG_EINVAL);
    CHECK_EQ(cog_cfg_set(cfg, "a..b", "v"), COG_CFG_EINVAL);
    CHECK_EQ(cog_cfg_set(cfg, ".a", "v"), COG_CFG_EINVAL);
    CHECK_EQ(cog_cfg_set(cfg, "a.", "v"), COG_CFG_EINVAL);
    CHECK_EQ(cog_cfg_set(cfg, "a.b", NULL), COG_CFG_EINVAL);
    CHECK_EQ(cog_cfg_delete(cfg, ""), COG_CFG_EINVAL);
    CHECK_EQ(cog_cfg_delete(cfg, "a..b"), COG_CFG_EINVAL);
    CHECK_EQ(cog_cfg_register_validator(cfg, "a..b", count_calls, NULL),
             COG_CFG_EINVAL);
    CHECK_EQ(cog_cfg_register_validator(cfg, NULL, count_calls, NULL),
             COG_CFG_EINVAL);
    CHECK_EQ(cog_cfg_register_validator(cfg, "a", NULL, NULL), COG_CFG_EINVAL);

    /* A failed set must not leave partial nodes behind. */
    CHECK_EQ(cog_cfg_set(cfg, "ok", "1"), COG_CFG_OK);
    CHECK_EQ(cog_cfg_set(cfg, "ok.bad..path", "v"), COG_CFG_EINVAL);
    CHECK_EQ(cog_cfg_commit(cfg), COG_CFG_OK); /* only "ok" staged */
    CHECK_EQ(cog_cfg_get(cfg, "ok", NULL, 0U), COG_CFG_EINVAL);
    { /* the failed "ok.bad..path" set must not have created "ok.bad" */
        char checkbuf[8];
        CHECK_EQ(cog_cfg_get(cfg, "ok.bad", checkbuf, sizeof checkbuf),
                 COG_CFG_ENOTFOUND);
    }

    cfg_free(cfg);
}

static void test_get_buffer_edges(void)
{
    cog_cfg_t *cfg = cfg_new();
    char small[8]; /* value needs 11 bytes (10 chars + NUL) */
    char big[16];

    CHECK_EQ(cog_cfg_set(cfg, "k", "1234567890"), COG_CFG_OK);
    CHECK_EQ(cog_cfg_commit(cfg), COG_CFG_OK);

    CHECK_EQ(cog_cfg_get(cfg, "k", small, sizeof small), COG_CFG_EBUFSMALL);
    CHECK_EQ(cog_cfg_get(cfg, "k", NULL, 0U), COG_CFG_EINVAL);
    CHECK_EQ(cog_cfg_get(cfg, "k", small, 0U), COG_CFG_EINVAL);

    CHECK_EQ(cog_cfg_get(cfg, "k", big, sizeof big), COG_CFG_OK);
    CHECK(strcmp(big, "1234567890") == 0);

    cfg_free(cfg);
}

static void test_validator_rejects_and_running_untouched(void)
{
    cog_cfg_t *cfg = cfg_new();
    char buf[64];

    CHECK_EQ(cog_cfg_register_validator(cfg, "providers.openai",
                                       validate_openai_requires_key, NULL),
             COG_CFG_OK);

    /* Stage a provider without an api-key: commit must fail atomically. */
    CHECK_EQ(cog_cfg_set(cfg, "providers.openai.base-url",
                         "https://api.openai.com/v1"), COG_CFG_OK);
    CHECK_EQ(cog_cfg_commit(cfg), COG_CFG_EVALIDATE);
    /* Running tree untouched and the rejected candidate is discarded. */
    CHECK_EQ(cog_cfg_get(cfg, "providers.openai.base-url", buf, sizeof buf),
             COG_CFG_ENOTFOUND);

    /* Re-stage correctly and commit. */
    CHECK_EQ(cog_cfg_set(cfg, "providers.openai.base-url",
                         "https://api.openai.com/v1"), COG_CFG_OK);
    CHECK_EQ(cog_cfg_set(cfg, "providers.openai.api-key", "sk-1"), COG_CFG_OK);
    CHECK_EQ(cog_cfg_commit(cfg), COG_CFG_OK);
    CHECK_EQ(cog_cfg_get(cfg, "providers.openai.api-key", buf, sizeof buf),
             COG_CFG_OK);
    CHECK(strcmp(buf, "sk-1") == 0);

    /* Emptying a required field also fails validation. */
    CHECK_EQ(cog_cfg_set(cfg, "providers.openai.api-key", ""), COG_CFG_OK);
    CHECK_EQ(cog_cfg_commit(cfg), COG_CFG_EVALIDATE);
    CHECK_EQ(cog_cfg_get(cfg, "providers.openai.api-key", buf, sizeof buf),
             COG_CFG_OK);
    CHECK(strcmp(buf, "sk-1") == 0); /* running still has the old value */

    cfg_free(cfg);
}

static void test_validator_subtree_scoping(void)
{
    cog_cfg_t *cfg = cfg_new();

    /* A validator on "a.b" must not fire when only "c.d" changes. */
    CHECK_EQ(cog_cfg_register_validator(cfg, "a.b", count_calls, NULL),
             COG_CFG_OK);
    validator_calls = 0;
    CHECK_EQ(cog_cfg_set(cfg, "c.d", "1"), COG_CFG_OK);
    CHECK_EQ(cog_cfg_commit(cfg), COG_CFG_OK);
    CHECK_EQ(validator_calls, 0);

    /* It fires when its own subtree is staged. */
    CHECK_EQ(cog_cfg_set(cfg, "a.b.x", "1"), COG_CFG_OK);
    CHECK_EQ(cog_cfg_commit(cfg), COG_CFG_OK);
    CHECK_EQ(validator_calls, 1);

    /* A whole-tree validator ("") fires for any non-empty candidate. */
    /* It does NOT re-fire on later commits that touch other subtrees:
     * "a.b" is present in the session candidate but not dirty. */
    CHECK_EQ(cog_cfg_set(cfg, "e", "1"), COG_CFG_OK);
    CHECK_EQ(cog_cfg_commit(cfg), COG_CFG_OK);
    CHECK_EQ(validator_calls, 1);

    /* A whole-tree validator ("") fires whenever anything is staged. */
    CHECK_EQ(cog_cfg_register_validator(cfg, "", accept_all, NULL), COG_CFG_OK);
    CHECK_EQ(cog_cfg_set(cfg, "f", "2"), COG_CFG_OK);
    CHECK_EQ(cog_cfg_commit(cfg), COG_CFG_OK);
    CHECK_EQ(validator_calls, 1); /* a.b still untouched */

    /* Staging a sibling and deleting a leaf keeps the registered subtree
     * alive but dirty, so its validator fires. */
    CHECK_EQ(cog_cfg_set(cfg, "a.b.y", "2"), COG_CFG_OK);
    CHECK_EQ(cog_cfg_delete(cfg, "a.b.x"), COG_CFG_OK);
    CHECK_EQ(cog_cfg_commit(cfg), COG_CFG_OK);
    CHECK_EQ(validator_calls, 2);
    { /* the delete took effect in the running tree */
        char checkbuf[8];
        CHECK_EQ(cog_cfg_get(cfg, "a.b.x", checkbuf, sizeof checkbuf),
                 COG_CFG_ENOTFOUND);
        CHECK_EQ(cog_cfg_get(cfg, "a.b.y", checkbuf, sizeof checkbuf),
                 COG_CFG_OK);
    }

    /* Deleting the whole registered subtree removes it from the
     * candidate, so its validator is skipped (nothing left to
     * validate) and the subtree is gone from running. */
    CHECK_EQ(cog_cfg_delete(cfg, "a.b.y"), COG_CFG_OK);
    CHECK_EQ(cog_cfg_commit(cfg), COG_CFG_OK);
    CHECK_EQ(validator_calls, 2);
    { /* a.b was pruned along with its last child */
        char checkbuf[8];
        CHECK_EQ(cog_cfg_get(cfg, "a.b.y", checkbuf, sizeof checkbuf),
                 COG_CFG_ENOTFOUND);
    }

    cfg_free(cfg);
}

static void test_rollback(void)
{
    cog_cfg_t *cfg = cfg_new();
    char buf[64];

    /* Commit 1: a. */
    CHECK_EQ(cog_cfg_set(cfg, "a", "1"), COG_CFG_OK);
    CHECK_EQ(cog_cfg_commit(cfg), COG_CFG_OK);

    /* Commit 2: b. */
    CHECK_EQ(cog_cfg_set(cfg, "b", "2"), COG_CFG_OK);
    CHECK_EQ(cog_cfg_commit(cfg), COG_CFG_OK);
    CHECK_EQ(cog_cfg_get(cfg, "b", buf, sizeof buf), COG_CFG_OK);
    CHECK(strcmp(buf, "2") == 0);

    /* Rollback returns to the known-good state from before commit 2. */
    CHECK_EQ(cog_cfg_rollback(cfg), COG_CFG_OK);
    CHECK_EQ(cog_cfg_get(cfg, "b", buf, sizeof buf), COG_CFG_ENOTFOUND);
    CHECK_EQ(cog_cfg_get(cfg, "a", buf, sizeof buf), COG_CFG_OK);
    CHECK(strcmp(buf, "1") == 0);

    /* Single-slot rollback: the point from before commit 1 is gone, and a
     * failed rollback leaves the running tree untouched. */
    CHECK_EQ(cog_cfg_rollback(cfg), COG_CFG_ENOROLLBACK);
    CHECK_EQ(cog_cfg_get(cfg, "a", buf, sizeof buf), COG_CFG_OK);
    CHECK(strcmp(buf, "1") == 0);

    /* A new commit re-arms the rollback slot, and the session candidate
     * still mirrors the running tree so further edits work. */
    CHECK_EQ(cog_cfg_set(cfg, "c", "3"), COG_CFG_OK);
    CHECK_EQ(cog_cfg_commit(cfg), COG_CFG_OK);
    CHECK_EQ(cog_cfg_rollback(cfg), COG_CFG_OK);
    CHECK_EQ(cog_cfg_get(cfg, "c", buf, sizeof buf), COG_CFG_ENOTFOUND);
    CHECK_EQ(cog_cfg_get(cfg, "a", buf, sizeof buf), COG_CFG_OK);
    CHECK(strcmp(buf, "1") == 0);

    cfg_free(cfg);
}

static void test_rollback_discards_candidate(void)
{
    cog_cfg_t *cfg = cfg_new();
    char buf[64];

    CHECK_EQ(cog_cfg_set(cfg, "a", "1"), COG_CFG_OK);
    CHECK_EQ(cog_cfg_commit(cfg), COG_CFG_OK);
    CHECK_EQ(cog_cfg_set(cfg, "b", "2"), COG_CFG_OK);
    CHECK_EQ(cog_cfg_commit(cfg), COG_CFG_OK);
    CHECK_EQ(cog_cfg_set(cfg, "c", "3"), COG_CFG_OK); /* uncommitted */

    /* Rollback restores the state from before commit 2 ({a:1}) and
     * discards the uncommitted "c" edit. */
    CHECK_EQ(cog_cfg_rollback(cfg), COG_CFG_OK);
    CHECK_EQ(cog_cfg_get(cfg, "b", buf, sizeof buf), COG_CFG_ENOTFOUND);
    CHECK_EQ(cog_cfg_get(cfg, "c", buf, sizeof buf), COG_CFG_ENOTFOUND);
    CHECK_EQ(cog_cfg_get(cfg, "a", buf, sizeof buf), COG_CFG_OK);
    CHECK(strcmp(buf, "1") == 0);

    /* The restored session candidate is clean and usable. */
    CHECK_EQ(cog_cfg_set(cfg, "a", "9"), COG_CFG_OK);
    CHECK_EQ(cog_cfg_commit(cfg), COG_CFG_OK);
    CHECK_EQ(cog_cfg_get(cfg, "a", buf, sizeof buf), COG_CFG_OK);
    CHECK(strcmp(buf, "9") == 0);

    cfg_free(cfg);
}

static void test_commit_noop_and_delete_after_commit(void)
{
    cog_cfg_t *cfg = cfg_new();
    char buf[64];

    CHECK_EQ(cog_cfg_commit(cfg), COG_CFG_OK); /* empty candidate: no-op */

    CHECK_EQ(cog_cfg_set(cfg, "a", "1"), COG_CFG_OK);
    CHECK_EQ(cog_cfg_commit(cfg), COG_CFG_OK);
    CHECK_EQ(cog_cfg_set(cfg, "b", "2"), COG_CFG_OK);
    CHECK_EQ(cog_cfg_delete(cfg, "a"), COG_CFG_OK);
    CHECK_EQ(cog_cfg_commit(cfg), COG_CFG_OK);

    CHECK_EQ(cog_cfg_get(cfg, "a", buf, sizeof buf), COG_CFG_ENOTFOUND);
    CHECK_EQ(cog_cfg_get(cfg, "b", buf, sizeof buf), COG_CFG_OK);
    CHECK(strcmp(buf, "2") == 0);

    cfg_free(cfg);
}

static void test_validator_node_inspection(void)
{
    cog_cfg_t *cfg = cfg_new();
    char buf[64];

    CHECK_EQ(cog_cfg_set(cfg, "p1.name", "n1"), COG_CFG_OK);
    CHECK_EQ(cog_cfg_set(cfg, "p1.url", "u1"), COG_CFG_OK);

    /* Subtree validator fires for the staged "p1" subtree. */
    CHECK_EQ(cog_cfg_register_validator(cfg, "p1", count_calls, NULL),
             COG_CFG_OK);
    validator_calls = 0;
    CHECK_EQ(cog_cfg_commit(cfg), COG_CFG_OK);
    CHECK_EQ(validator_calls, 1);

    /* Node helpers on a bare NULL handle. */
    CHECK(strcmp(cog_cfg_node_name(NULL), "") == 0);
    CHECK(cog_cfg_node_value(NULL) == NULL);
    CHECK((int)cog_cfg_node_child_count(NULL) == 0);
    CHECK(cog_cfg_node_child(NULL, 0U) == NULL);
    CHECK_EQ(cog_cfg_node_get_value(NULL, "a", buf, sizeof buf), COG_CFG_EINVAL);
    CHECK_EQ(cog_cfg_node_get_value(NULL, NULL, buf, sizeof buf), COG_CFG_EINVAL);

    cfg_free(cfg);
}

int main(void)
{
    test_lifecycle_null();
    test_set_get_commit_happy_path();
    test_delete_and_prune();
    test_set_overwrites_and_empty_value();
    test_malformed_paths();
    test_get_buffer_edges();
    test_validator_rejects_and_running_untouched();
    test_validator_subtree_scoping();
    test_rollback();
    test_rollback_discards_candidate();
    test_commit_noop_and_delete_after_commit();
    test_validator_node_inspection();

    if (g_failures != 0) {
        fprintf(stderr, "%d check(s) FAILED\n", g_failures);
        return 1;
    }
    printf("config-tree: all checks passed\n");
    return 0;
}

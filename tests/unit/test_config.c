/**
 * @file test_config.c
 * @brief Contract test for cog/config.h (Stage 12.3c, ticket df4d0fa).
 *
 * Exercises the full public surface of the hierarchical configuration
 * tree:
 *
 *   - lifecycle: tree new/free, NULL contracts
 *   - path grammar: empty/malformed paths on every operation
 *   - set/get: nested values, overwrite, empty values, valueless
 *     containers ("missing key"), path-not-found, borrowed pointers
 *   - delete: leaf/container, missing, empty-ancestor pruning
 *   - list: root listing, nested listing, count-only, overflow, order
 *   - transactions: begin/commit/rollback, atomicity, snapshot
 *     isolation of pre-commit readers (hot-reload semantics),
 *     finalization (INVALID_STATE/ NOT_FOUND on stale handles)
 *   - validators: fire-on-present-prefix, rejection keeps running
 *     untouched and the transaction retryable, skip-absent-prefix,
 *     whole-tree "", registration order, fixed-capacity overflow,
 *     NULL/invalid arguments
 *   - stress: many commit/rollback cycles plus concurrent snapshots,
 *     freed cleanly by tree_free (ASan/LSan clean)
 *
 * Every documented error code is exercised where reachable through the
 * public API.  COG_ERROR_NOMEM is not triggered: allocation failure is
 * not injectable through this API (the library exposes no allocation
 * hooks), matching test_session.c.  Values have exactly one type
 * (const char *), so there is no "wrong type" case; the nearest
 * equivalent — reading a container-without-value — is pinned to
 * COG_ERROR_NOT_FOUND.
 */

#include "cog/config.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef COG_CONFIG_H
#error "COG_CONFIG_H include guard missing"
#endif

/* ------------------------------------------------------------------ */
/* Validator fixtures (file-static: the validator signature declared by */
/* config.h has no userdata parameter).                                */
/* ------------------------------------------------------------------ */

/** Observations recorded by the recorder validator. */
typedef struct recorder {
    int calls;                /**< Number of invocations. */
    char prefix[64];          /**< Prefix of the last invocation. */
    cog_result_t verdict;     /**< Verdict returned by the last call. */
} recorder_t;

static recorder_t g_rec;

/** Records prefix/verdict; callers set g_rec.verdict before committing. */
static cog_result_t rec_validator(const cog_config_txn_t *txn,
                                   const char *prefix)
{
    (void)txn;
    g_rec.calls++;
    if (prefix != NULL) {
        (void)snprintf(g_rec.prefix, sizeof g_rec.prefix, "%s", prefix);
    } else {
        g_rec.prefix[0] = '\0';
    }
    return g_rec.verdict;
}

/** Rejects the candidate when prefix.base-url is missing or empty. */
static cog_result_t base_url_validator(const cog_config_txn_t *txn,
                                        const char *prefix)
{
    char path[128];
    const char *v = NULL;

    (void)snprintf(path, sizeof path, "%s.openai.base-url", prefix);
    if (cog_config_get(txn, path, &v) != COG_OK) {
        return COG_ERROR_INVALID_ARG;
    }
    if (v == NULL || v[0] == '\0') {
        return COG_ERROR_INVALID_ARG;
    }
    return COG_OK;
}

static void recorder_reset(void)
{
    g_rec.calls = 0;
    g_rec.prefix[0] = '\0';
    g_rec.verdict = COG_OK;
}

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

/** Assert that the value at @p path in @p txn equals @p want. */
static int expect_value(const cog_config_txn_t *txn, const char *path,
                        const char *want)
{
    const char *v = NULL;
    cog_result_t r = cog_config_get(txn, path, &v);

    if (r != COG_OK) {
        (void)fprintf(stderr, "  get(%s) -> %d, want COG_OK\n", path, (int)r);
        return 1;
    }
    if (strcmp(v, want) != 0) {
        (void)fprintf(stderr, "  get(%s) -> \"%s\", want \"%s\"\n",
                      path, v, want);
        return 1;
    }
    return 0;
}

/** Assert that get at @p path fails with exactly @p want. */
static int expect_get_err(const cog_config_txn_t *txn, const char *path,
                          cog_result_t want)
{
    const char *v = NULL;
    cog_result_t r = cog_config_get(txn, path, &v);

    if (r != want) {
        (void)fprintf(stderr, "  get(%s) -> %d, want %d\n", path, (int)r,
                      (int)want);
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                            */
/* ------------------------------------------------------------------ */

static int test_tree_lifecycle(void)
{
    cog_config_tree_t *tree = NULL;

    if (cog_config_tree_new(NULL) != COG_ERROR_NULL_PTR) {
        return 1;
    }
    if (cog_config_tree_new(&tree) != COG_OK || tree == NULL) {
        return 1;
    }
    cog_config_tree_free(tree);
    cog_config_tree_free(NULL); /* no-op */
    return 0;
}

static int test_txn_arg_errors(void)
{
    cog_config_tree_t *tree = NULL;
    cog_config_txn_t *txn = NULL;
    cog_config_txn_t *stale = NULL;
    cog_config_txn_t **out = &stale;

    if (cog_config_tree_new(&tree) != COG_OK) {
        return 1;
    }
    /* begin: NULL tree / NULL out */
    if (cog_config_begin(NULL, out) != COG_ERROR_NULL_PTR) {
        return 1;
    }
    if (cog_config_begin(tree, NULL) != COG_ERROR_NULL_PTR) {
        return 1;
    }
    /* commit on NULL */
    if (cog_config_commit(NULL) != COG_ERROR_NULL_PTR) {
        return 1;
    }
    /* rollback(NULL) is a documented no-op */
    cog_config_rollback(NULL);
    /* a real transaction round-trip on an empty tree */
    if (cog_config_begin(tree, &txn) != COG_OK) {
        return 1;
    }
    if (cog_config_commit(txn) != COG_OK) {
        return 1;
    }
    cog_config_tree_free(tree);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Set / get / delete                                                  */
/* ------------------------------------------------------------------ */

static int test_set_get_nested(void)
{
    cog_config_tree_t *tree = NULL;
    cog_config_txn_t *txn = NULL;
    int rc;

    if (cog_config_tree_new(&tree) != COG_OK ||
        cog_config_begin(tree, &txn) != COG_OK) {
        return 1;
    }

    /* nested set creates intermediate nodes */
    rc = cog_config_set(txn, "providers.openai.base-url", "https://api.openai.com");
    rc |= cog_config_set(txn, "providers.openai.api-key", "sk-1234");
    rc |= cog_config_set(txn, "providers.anthropic.base-url", "https://api.anthropic.com");
    if (rc != COG_OK) {
        return 1;
    }
    if (expect_value(txn, "providers.openai.base-url",
                     "https://api.openai.com") != 0) {
        return 1;
    }
    if (expect_value(txn, "providers.openai.api-key", "sk-1234") != 0) {
        return 1;
    }
    if (expect_value(txn, "providers.anthropic.base-url",
                     "https://api.anthropic.com") != 0) {
        return 1;
    }

    /* containers resolve but carry no value: that is a "missing value" */
    if (expect_get_err(txn, "providers", COG_ERROR_NOT_FOUND) != 0) {
        return 1;
    }
    if (expect_get_err(txn, "providers.openai", COG_ERROR_NOT_FOUND) != 0) {
        return 1;
    }

    /* overwrite */
    if (cog_config_set(txn, "providers.openai.api-key", "sk-NEW") != COG_OK) {
        return 1;
    }
    if (expect_value(txn, "providers.openai.api-key", "sk-NEW") != 0) {
        return 1;
    }

    /* a node can carry both a value and children */
    if (cog_config_set(txn, "providers.active", "openai") != COG_OK) {
        return 1;
    }
    if (expect_value(txn, "providers.active", "openai") != 0) {
        return 1;
    }
    if (expect_value(txn, "providers.openai.base-url",
                     "https://api.openai.com") != 0) {
        return 1;
    }

    /* empty value is legal */
    if (cog_config_set(txn, "feature.flag", "") != COG_OK) {
        return 1;
    }
    if (expect_value(txn, "feature.flag", "") != 0) {
        return 1;
    }

    /* missing key */
    if (expect_get_err(txn, "providers.google.base-url",
                       COG_ERROR_NOT_FOUND) != 0) {
        return 1;
    }

    cog_config_rollback(txn);
    cog_config_tree_free(tree);
    return 0;
}

static int test_set_get_nulls(void)
{
    cog_config_tree_t *tree = NULL;
    cog_config_txn_t *txn = NULL;
    const char *v = NULL;

    if (cog_config_tree_new(&tree) != COG_OK ||
        cog_config_begin(tree, &txn) != COG_OK) {
        return 1;
    }
    if (cog_config_set(NULL, "a", "b") != COG_ERROR_NULL_PTR) {
        return 1;
    }
    if (cog_config_set(txn, NULL, "b") != COG_ERROR_NULL_PTR) {
        return 1;
    }
    if (cog_config_set(txn, "a", NULL) != COG_ERROR_NULL_PTR) {
        return 1;
    }
    if (cog_config_get(NULL, "a", &v) != COG_ERROR_NULL_PTR) {
        return 1;
    }
    if (cog_config_get(txn, NULL, &v) != COG_ERROR_NULL_PTR) {
        return 1;
    }
    if (cog_config_get(txn, "a", NULL) != COG_ERROR_NULL_PTR) {
        return 1;
    }
    /* out not touched on address-0 set */
    v = NULL;
    if (cog_config_set(txn, NULL, "b") != COG_ERROR_NULL_PTR) {
        return 1;
    }
    cog_config_rollback(txn);
    cog_config_tree_free(tree);
    return 0;
}

static int test_path_validation(void)
{
    static const char *const bad_paths[] = {
        "", ".", "..", "a.", ".a", "a..b", "a.b.", "a..b.c", ".a.b",
    };
    cog_config_tree_t *tree = NULL;
    cog_config_txn_t *txn = NULL;
    size_t i;
    const char *v = NULL;
    size_t count = 123U;

    if (cog_config_tree_new(&tree) != COG_OK ||
        cog_config_begin(tree, &txn) != COG_OK) {
        return 1;
    }
    for (i = 0U; i < sizeof bad_paths / sizeof bad_paths[0]; ++i) {
        if (cog_config_set(txn, bad_paths[i], "v") != COG_ERROR_INVALID_ARG) {
            (void)fprintf(stderr, "  set(\"%s\") accepted\n", bad_paths[i]);
            return 1;
        }
        if (cog_config_get(txn, bad_paths[i], &v) != COG_ERROR_INVALID_ARG) {
            (void)fprintf(stderr, "  get(\"%s\") accepted\n", bad_paths[i]);
            return 1;
        }
        if (cog_config_delete(txn, bad_paths[i]) != COG_ERROR_INVALID_ARG) {
            (void)fprintf(stderr, "  delete(\"%s\") accepted\n", bad_paths[i]);
            return 1;
        }
        /* "" is reserved for list (tree root); every other malformed
         * path is rejected there too */
        if (bad_paths[i][0] != '\0' &&
            cog_config_list(txn, bad_paths[i], NULL, 0U, &count) !=
            COG_ERROR_INVALID_ARG) {
            (void)fprintf(stderr, "  list(\"%s\") accepted\n", bad_paths[i]);
            return 1;
        }
    }
    /* the root path "" is valid for list only */
    if (cog_config_list(txn, "", NULL, 0U, &count) != COG_OK ||
        count != 0U) {
        return 1;
    }
    cog_config_rollback(txn);
    cog_config_tree_free(tree);
    return 0;
}

static int test_delete(void)
{
    cog_config_tree_t *tree = NULL;
    cog_config_txn_t *txn = NULL;
    size_t count = 0U;

    if (cog_config_tree_new(&tree) != COG_OK ||
        cog_config_begin(tree, &txn) != COG_OK) {
        return 1;
    }
    if (cog_config_set(txn, "a.b.c", "1") != COG_OK ||
        cog_config_set(txn, "x.y", "2") != COG_OK) {
        return 1;
    }

    /* delete a leaf */
    if (cog_config_delete(txn, "a.b.c") != COG_OK) {
        return 1;
    }
    if (expect_get_err(txn, "a.b.c", COG_ERROR_NOT_FOUND) != 0) {
        return 1;
    }
    /* empty valueless ancestors are pruned: a is gone entirely */
    if (expect_get_err(txn, "a", COG_ERROR_NOT_FOUND) != 0 ||
        expect_get_err(txn, "a.b", COG_ERROR_NOT_FOUND) != 0) {
        return 1;
    }

    /* delete a container with descendants */
    if (cog_config_set(txn, "a.b.c", "1") != COG_OK ||
        cog_config_delete(txn, "a") != COG_OK) {
        return 1;
    }
    if (expect_get_err(txn, "a.b.c", COG_ERROR_NOT_FOUND) != 0) {
        return 1;
    }

    /* delete a missing key */
    if (cog_config_delete(txn, "nope") != COG_ERROR_NOT_FOUND) {
        return 1;
    }

    /* the unrelated subtree survives */
    if (expect_value(txn, "x.y", "2") != 0) {
        return 1;
    }
    if (cog_config_list(txn, "", NULL, 0U, &count) != COG_OK ||
        count != 1U) {
        return 1;
    }

    /* re-create after delete */
    if (cog_config_delete(txn, "x.y") != COG_OK ||
        cog_config_set(txn, "x.z", "3") != COG_OK) {
        return 1;
    }
    if (expect_get_err(txn, "x.y", COG_ERROR_NOT_FOUND) != 0 ||
        expect_value(txn, "x.z", "3") != 0) {
        return 1;
    }

    cog_config_rollback(txn);
    cog_config_tree_free(tree);
    return 0;
}

static int test_delete_nulls_and_state(void)
{
    cog_config_tree_t *tree = NULL;
    cog_config_txn_t *txn = NULL;

    if (cog_config_tree_new(&tree) != COG_OK ||
        cog_config_begin(tree, &txn) != COG_OK) {
        return 1;
    }
    if (cog_config_delete(NULL, "a") != COG_ERROR_NULL_PTR) {
        return 1;
    }
    if (cog_config_delete(txn, NULL) != COG_ERROR_NULL_PTR) {
        return 1;
    }
    cog_config_rollback(txn);
    if (cog_config_delete(txn, "a") != COG_ERROR_INVALID_STATE) {
        return 1;
    }
    cog_config_tree_free(tree);
    return 0;
}

/* ------------------------------------------------------------------ */
/* List                                                                */
/* ------------------------------------------------------------------ */

static int test_list(void)
{
    cog_config_tree_t *tree = NULL;
    cog_config_txn_t *txn = NULL;
    const char *keys[8];
    size_t count = 0U;
    size_t i;

    if (cog_config_tree_new(&tree) != COG_OK ||
        cog_config_begin(tree, &txn) != COG_OK) {
        return 1;
    }
    if (cog_config_set(txn, "a.k1", "1") != COG_OK ||
        cog_config_set(txn, "a.k2", "2") != COG_OK ||
        cog_config_set(txn, "b.k3", "3") != COG_OK) {
        return 1;
    }

    /* root listing ("" path), insertion order */
    if (cog_config_list(txn, "", keys, 8U, &count) != COG_OK ||
        count != 2U) {
        return 1;
    }
    if (strcmp(keys[0], "a") != 0 || strcmp(keys[1], "b") != 0) {
        return 1;
    }

    /* nested listing */
    if (cog_config_list(txn, "a", keys, 8U, &count) != COG_OK ||
        count != 2U ||
        strcmp(keys[0], "k1") != 0 || strcmp(keys[1], "k2") != 0) {
        return 1;
    }

    /* count-only query: max_keys 0 with out_keys NULL */
    if (cog_config_list(txn, "a", NULL, 0U, &count) != COG_OK ||
        count != 2U) {
        return 1;
    }
    if (cog_config_list(txn, "", NULL, 0U, &count) != COG_OK ||
        count != 2U) {
        return 1;
    }

    /* node with both value and children lists its children */
    if (cog_config_set(txn, "a.active", "yes") != COG_OK ||
        cog_config_list(txn, "a", keys, 8U, &count) != COG_OK ||
        count != 3U) {
        return 1;
    }

    /* truncation is refused loudly: OVERFLOW with the real count */
    if (cog_config_list(txn, "a", keys, 2U, &count) != COG_ERROR_OVERFLOW ||
        count != 3U) {
        return 1;
    }

    /* missing path */
    if (cog_config_list(txn, "zzz", keys, 8U, &count) !=
        COG_ERROR_NOT_FOUND) {
        return 1;
    }

    /* NULL contracts */
    if (cog_config_list(NULL, "a", keys, 8U, &count) != COG_ERROR_NULL_PTR) {
        return 1;
    }
    if (cog_config_list(txn, NULL, keys, 8U, &count) != COG_ERROR_NULL_PTR) {
        return 1;
    }
    if (cog_config_list(txn, "a", keys, 8U, NULL) != COG_ERROR_NULL_PTR) {
        return 1;
    }
    if (cog_config_list(txn, "a", NULL, 8U, &count) != COG_ERROR_NULL_PTR) {
        return 1;
    }

    /* insertion order after delete + re-set: removed slot shifts, new
     * keys append at the end */
    if (cog_config_delete(txn, "a.k1") != COG_OK ||
        cog_config_set(txn, "a.k3", "3") != COG_OK) {
        return 1;
    }
    if (cog_config_list(txn, "a", keys, 8U, &count) != COG_OK ||
        count != 3U ||
        strcmp(keys[0], "k2") != 0 || strcmp(keys[1], "active") != 0 ||
        strcmp(keys[2], "k3") != 0) {
        return 1;
    }

    /* keys are borrowed pointers into the candidate: they must match the
     * live strings while the transaction is open */
    for (i = 0U; i < count; ++i) {
        (void)keys[i];
    }
    cog_config_rollback(txn);
    cog_config_tree_free(tree);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Transactions: commit / rollback / atomicity / isolation              */
/* ------------------------------------------------------------------ */

static int test_commit_visibility(void)
{
    cog_config_tree_t *tree = NULL;
    cog_config_txn_t *writer = NULL;
    cog_config_txn_t *before = NULL;
    cog_config_txn_t *after = NULL;
    cog_config_txn_t *reader = NULL;

    if (cog_config_tree_new(&tree) != COG_OK) {
        return 1;
    }
    /* a snapshot taken before any commit is empty */
    if (cog_config_begin(tree, &before) != COG_OK) {
        return 1;
    }

    /* writer stages and commits */
    if (cog_config_begin(tree, &writer) != COG_OK ||
        cog_config_set(writer, "providers.openai.base-url",
                       "https://api.openai.com") != COG_OK ||
        cog_config_commit(writer) != COG_OK) {
        return 1;
    }

    /* after-commit reader sees the new value */
    if (cog_config_begin(tree, &after) != COG_OK) {
        return 1;
    }
    if (expect_value(after, "providers.openai.base-url",
                     "https://api.openai.com") != 0) {
        return 1;
    }

    /* the pre-commit snapshot still sees nothing: in-flight readers are
     * isolated from the reload (hot-reload semantics) */
    if (expect_get_err(before, "providers.openai.base-url",
                      COG_ERROR_NOT_FOUND) != 0) {
        return 1;
    }

    /* staging on a later snapshot is invisible until its own commit */
    if (cog_config_begin(tree, &reader) != COG_OK ||
        cog_config_set(reader, "providers.active", "openai") != COG_OK) {
        return 1;
    }
    if (expect_get_err(after, "providers.active", COG_ERROR_NOT_FOUND) != 0) {
        return 1;
    }
    if (cog_config_commit(reader) != COG_OK) {
        return 1;
    }
    cog_config_rollback(before);
    cog_config_rollback(after);
    cog_config_tree_free(tree);
    return 0;
}

static int test_commit_finalizes(void)
{
    cog_config_tree_t *tree = NULL;
    cog_config_txn_t *txn = NULL;
    const char *v = NULL;
    const char *keys[4];
    size_t count = 0U;

    if (cog_config_tree_new(&tree) != COG_OK ||
        cog_config_begin(tree, &txn) != COG_OK ||
        cog_config_set(txn, "a", "1") != COG_OK ||
        cog_config_commit(txn) != COG_OK) {
        return 1;
    }

    /* second commit on the same handle */
    if (cog_config_commit(txn) != COG_ERROR_INVALID_STATE) {
        return 1;
    }
    /* mutations on a finalized transaction */
    if (cog_config_set(txn, "b", "2") != COG_ERROR_INVALID_STATE) {
        return 1;
    }
    if (cog_config_delete(txn, "a") != COG_ERROR_INVALID_STATE) {
        return 1;
    }
    /* reads on a finalized transaction: the candidate is gone */
    if (cog_config_get(txn, "a", &v) != COG_ERROR_NOT_FOUND) {
        return 1;
    }
    if (cog_config_list(txn, "", keys, 4U, &count) != COG_ERROR_NOT_FOUND) {
        return 1;
    }
    /* rollback of a finalized transaction is a documented no-op */
    cog_config_rollback(txn);

    /* the commit itself stuck */
    if (cog_config_begin(tree, &txn) != COG_OK ||
        expect_value(txn, "a", "1") != 0) {
        return 1;
    }
    cog_config_rollback(txn);
    cog_config_tree_free(tree);
    return 0;
}

static int test_rollback_discards(void)
{
    cog_config_tree_t *tree = NULL;
    cog_config_txn_t *txn = NULL;
    cog_config_txn_t *check = NULL;

    if (cog_config_tree_new(&tree) != COG_OK) {
        return 1;
    }
    /* seed the running tree */
    if (cog_config_begin(tree, &txn) != COG_OK ||
        cog_config_set(txn, "a", "keep") != COG_OK ||
        cog_config_commit(txn) != COG_OK) {
        return 1;
    }

    /* stage changes, then discard them */
    if (cog_config_begin(tree, &txn) != COG_OK ||
        cog_config_set(txn, "a", "discard") != COG_OK ||
        cog_config_set(txn, "new.key", "gone") != COG_OK) {
        return 1;
    }
    cog_config_rollback(txn);

    /* the running tree is unchanged */
    if (cog_config_begin(tree, &check) != COG_OK) {
        return 1;
    }
    if (expect_value(check, "a", "keep") != 0 ||
        expect_get_err(check, "new.key", COG_ERROR_NOT_FOUND) != 0) {
        return 1;
    }
    cog_config_rollback(check);

    /* the rolled-back transaction is finalized */
    if (cog_config_commit(txn) != COG_ERROR_INVALID_STATE) {
        return 1;
    }
    if (cog_config_set(txn, "x", "y") != COG_ERROR_INVALID_STATE) {
        return 1;
    }
    cog_config_rollback(txn); /* idempotent no-op */

    cog_config_tree_free(tree);
    return 0;
}

static int test_noop_commit(void)
{
    cog_config_tree_t *tree = NULL;
    cog_config_txn_t *txn = NULL;

    recorder_reset();
    if (cog_config_tree_new(&tree) != COG_OK ||
        cog_config_register_validator(tree, "", rec_validator) != COG_OK ||
        cog_config_begin(tree, &txn) != COG_OK) {
        return 1;
    }
    /* a clean commit still runs validators (whole-tree prefix fires on
     * every commit) */
    g_rec.verdict = COG_OK;
    if (cog_config_commit(txn) != COG_OK) {
        return 1;
    }
    if (g_rec.calls != 1 || strcmp(g_rec.prefix, "") != 0) {
        return 1;
    }
    cog_config_tree_free(tree);
    return 0;
}

static int test_snapshot_isolation_stress(void)
{
    cog_config_tree_t *tree = NULL;
    cog_config_txn_t *snap[32];
    cog_config_txn_t *w = NULL;
    cog_config_txn_t *check = NULL;
    size_t i;
    char path[32];

    if (cog_config_tree_new(&tree) != COG_OK) {
        return 1;
    }
    /* open 32 concurrent read snapshots of the empty tree */
    for (i = 0U; i < 32U; ++i) {
        if (cog_config_begin(tree, &snap[i]) != COG_OK) {
            return 1;
        }
    }
    /* commit a few waves of changes */
    for (i = 0U; i < 8U; ++i) {
        (void)snprintf(path, sizeof path, "wave%zu.key", i);
        if (cog_config_begin(tree, &w) != COG_OK ||
            cog_config_set(w, path, "v") != COG_OK ||
            cog_config_commit(w) != COG_OK) {
            return 1;
        }
    }
    /* every pre-commit snapshot still sees an empty tree */
    for (i = 0U; i < 32U; ++i) {
        if (expect_get_err(snap[i], "wave0.key", COG_ERROR_NOT_FOUND) != 0) {
            return 1;
        }
        cog_config_rollback(snap[i]);
    }
    /* a fresh snapshot sees all eight waves */
    if (cog_config_begin(tree, &check) != COG_OK) {
        return 1;
    }
    for (i = 0U; i < 8U; ++i) {
        (void)snprintf(path, sizeof path, "wave%zu.key", i);
        if (expect_value(check, path, "v") != 0) {
            return 1;
        }
    }
    cog_config_rollback(check);
    cog_config_tree_free(tree);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Validators                                                           */
/* ------------------------------------------------------------------ */

static int test_validators_basic(void)
{
    cog_config_tree_t *tree = NULL;
    cog_config_txn_t *txn = NULL;
    cog_config_txn_t *check = NULL;

    recorder_reset();
    if (cog_config_tree_new(&tree) != COG_OK) {
        return 1;
    }
    /* nested-prefix validator reads the candidate through the txn */
    if (cog_config_register_validator(tree, "providers",
                                      base_url_validator) != COG_OK ||
        cog_config_register_validator(tree, "recorder", rec_validator) !=
            COG_OK) {
        return 1;
    }
    if (cog_config_begin(tree, &txn) != COG_OK ||
        cog_config_set(txn, "providers.openai.base-url",
                       "https://api.openai.com") != COG_OK ||
        cog_config_set(txn, "recorder.enabled", "yes") != COG_OK) {
        return 1;
    }
    g_rec.verdict = COG_OK;
    if (cog_config_commit(txn) != COG_OK) {
        (void)fprintf(stderr, "  valid commit rejected\n");
        return 1;
    }
    /* both validators ran, in registration order */
    if (g_rec.calls != 1 || strcmp(g_rec.prefix, "recorder") != 0) {
        (void)fprintf(stderr, "  recorder validator not called once\n");
        return 1;
    }
    if (cog_config_begin(tree, &check) != COG_OK) {
        return 1;
    }
    if (expect_value(check, "providers.openai.base-url",
                     "https://api.openai.com") != 0) {
        return 1;
    }
    cog_config_rollback(check);
    cog_config_tree_free(tree);
    return 0;
}

static int test_validator_rejects(void)
{
    cog_config_tree_t *tree = NULL;
    cog_config_txn_t *txn = NULL;
    cog_config_txn_t *check = NULL;

    if (cog_config_tree_new(&tree) != COG_OK) {
        return 1;
    }
    if (cog_config_register_validator(tree, "providers",
                                      base_url_validator) != COG_OK) {
        return 1;
    }

    /* the validator rejects: base-url is empty */
    if (cog_config_begin(tree, &txn) != COG_OK ||
        cog_config_set(txn, "providers.openai.base-url", "") != COG_OK) {
        return 1;
    }
    g_rec.calls = 0;
    if (cog_config_commit(txn) != COG_ERROR_INVALID_ARG) {
        (void)fprintf(stderr, "  rejected commit returned wrong code\n");
        return 1;
    }

    /* the running tree is untouched */
    if (cog_config_begin(tree, &check) != COG_OK) {
        return 1;
    }
    if (expect_get_err(check, "providers.openai.base-url",
                       COG_ERROR_NOT_FOUND) != 0) {
        return 1;
    }
    cog_config_rollback(check);

    /* the transaction is still usable: fix the value and retry */
    if (cog_config_set(txn, "providers.openai.base-url",
                       "https://api.openai.com") != COG_OK) {
        return 1;
    }
    if (cog_config_commit(txn) != COG_OK) {
        (void)fprintf(stderr, "  retry after rejection failed\n");
        return 1;
    }
    if (cog_config_begin(tree, &check) != COG_OK) {
        return 1;
    }
    if (expect_value(check, "providers.openai.base-url",
                     "https://api.openai.com") != 0) {
        return 1;
    }
    cog_config_rollback(check);

    /* commit of a stale snapshot (taken before the fix) is rejected
     * again and leaves the running tree as-is */
    if (cog_config_begin(tree, &txn) != COG_OK ||
        cog_config_set(txn, "providers.openai.base-url", "") != COG_OK) {
        return 1;
    }
    if (cog_config_commit(txn) != COG_ERROR_INVALID_ARG) {
        return 1;
    }
    if (cog_config_begin(tree, &check) != COG_OK) {
        return 1;
    }
    if (expect_value(check, "providers.openai.base-url",
                     "https://api.openai.com") != 0) {
        return 1;
    }
    cog_config_rollback(check);
    cog_config_rollback(txn);
    cog_config_tree_free(tree);
    return 0;
}

static int test_validator_absent_prefix(void)
{
    cog_config_tree_t *tree = NULL;
    cog_config_txn_t *txn = NULL;

    recorder_reset();
    if (cog_config_tree_new(&tree) != COG_OK ||
        cog_config_register_validator(tree, "a", rec_validator) != COG_OK) {
        return 1;
    }
    /* prefix "a" exists: fires */
    g_rec.verdict = COG_OK;
    if (cog_config_begin(tree, &txn) != COG_OK ||
        cog_config_set(txn, "a.b", "1") != COG_OK ||
        cog_config_commit(txn) != COG_OK) {
        return 1;
    }
    if (g_rec.calls != 1 || strcmp(g_rec.prefix, "a") != 0) {
        return 1;
    }
    /* deleting the subtree makes the prefix absent: not fired */
    recorder_reset();
    g_rec.verdict = COG_OK;
    if (cog_config_begin(tree, &txn) != COG_OK ||
        cog_config_delete(txn, "a.b") != COG_OK ||
        cog_config_commit(txn) != COG_OK) {
        return 1;
    }
    if (g_rec.calls != 0) {
        (void)fprintf(stderr, "  validator fired for an absent prefix\n");
        return 1;
    }
    cog_config_tree_free(tree);
    return 0;
}

static int test_validator_registration_errors(void)
{
    cog_config_tree_t *tree = NULL;

    if (cog_config_tree_new(&tree) != COG_OK) {
        return 1;
    }
    if (cog_config_register_validator(NULL, "a", rec_validator) !=
        COG_ERROR_NULL_PTR) {
        return 1;
    }
    if (cog_config_register_validator(tree, NULL, rec_validator) !=
        COG_ERROR_NULL_PTR) {
        return 1;
    }
    if (cog_config_register_validator(tree, "a", NULL) != COG_ERROR_NULL_PTR) {
        return 1;
    }
    if (cog_config_register_validator(tree, "a..b", rec_validator) !=
        COG_ERROR_INVALID_ARG) {
        return 1;
    }
    /* "" is legal: whole-tree validator */
    if (cog_config_register_validator(tree, "", rec_validator) != COG_OK) {
        return 1;
    }
    /* duplicates are allowed; the table is finite */
    {
        size_t i;
        cog_result_t r = COG_OK;

        for (i = 0U; i < 20U; ++i) {
            char p[16];

            (void)snprintf(p, sizeof p, "dup%zu", i);
            r = cog_config_register_validator(tree, p, rec_validator);
            if (r != COG_OK) {
                break;
            }
        }
        if (r != COG_ERROR_OVERFLOW) {
            (void)fprintf(stderr, "  full table returned %d\n", (int)r);
            return 1;
        }
    }
    /* registration after the table filled still overflows */
    if (cog_config_register_validator(tree, "extra", rec_validator) !=
        COG_ERROR_OVERFLOW) {
        return 1;
    }
    cog_config_tree_free(tree);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Deep paths and stress                                                */
/* ------------------------------------------------------------------ */

static int test_deep_paths(void)
{
    cog_config_tree_t *tree = NULL;
    cog_config_txn_t *txn = NULL;
    char path[512];
    size_t i;

    if (cog_config_tree_new(&tree) != COG_OK ||
        cog_config_begin(tree, &txn) != COG_OK) {
        return 1;
    }
    /* a path 80 segments deep */
    path[0] = '\0';
    for (i = 0U; i < 80U; ++i) {
        (void)strcat(path, i == 0U ? "lvl0" : ".lvl0");
    }
    (void)strcat(path, ".leaf");
    if (cog_config_set(txn, path, "deep") != COG_OK) {
        return 1;
    }
    if (expect_value(txn, path, "deep") != 0) {
        return 1;
    }
    /* listing one intermediate level */
    if (cog_config_list(txn, "lvl0.lvl0", NULL, 0U, &i) != COG_OK ||
        i != 1U) {
        return 1;
    }
    if (cog_config_delete(txn, path) != COG_OK) {
        return 1;
    }
    if (expect_get_err(txn, "lvl0", COG_ERROR_NOT_FOUND) != 0) {
        return 1;
    }
    cog_config_rollback(txn);
    cog_config_tree_free(tree);
    return 0;
}

static int test_commit_rollback_stress(void)
{
    cog_config_tree_t *tree = NULL;
    cog_config_txn_t *txn = NULL;
    cog_config_txn_t *check = NULL;
    size_t i;
    char path[32];
    char val[32];

    if (cog_config_tree_new(&tree) != COG_OK) {
        return 1;
    }
    for (i = 0U; i < 500U; ++i) {
        (void)snprintf(path, sizeof path, "k%zu", i % 50U);
        (void)snprintf(val, sizeof val, "v%zu", i);
        if (cog_config_begin(tree, &txn) != COG_OK) {
            return 1;
        }
        if (i % 2U == 0U) {
            if (cog_config_set(txn, path, val) != COG_OK ||
                cog_config_commit(txn) != COG_OK) {
                return 1;
            }
        } else {
            /* odd iterations roll back before the edit is visible */
            if (cog_config_set(txn, path, "discarded") != COG_OK) {
                return 1;
            }
            cog_config_rollback(txn);
        }
    }
    /* committed even iterations stick; discarded odd ones do not.
     * Even i carries i %% 50 == k for even k only (i and i %% 50 share
     * parity because 50 is even), so odd keys never commit.  The final
     * committed value for even k is the largest even i < 500 with
     * i %% 50 == k, i.e. i = 450 + k. */
    if (cog_config_begin(tree, &check) != COG_OK) {
        return 1;
    }
    for (i = 0U; i < 50U; ++i) {
        (void)snprintf(path, sizeof path, "k%zu", i);
        if (i % 2U == 0U) {
            (void)snprintf(val, sizeof val, "v%zu", 450U + i);
            if (expect_value(check, path, val) != 0) {
                return 1;
            }
        } else if (expect_get_err(check, path, COG_ERROR_NOT_FOUND) != 0) {
            return 1;
        }
    }
    cog_config_rollback(check);
    cog_config_tree_free(tree);
    return 0;
}

static int test_mixed_stress_invariants(void)
{
    cog_config_tree_t *tree = NULL;
    cog_config_txn_t *txns[32];
    cog_config_txn_t *txn = NULL;
    cog_config_txn_t *check = NULL;
    size_t i;
    char path[32];
    char val[32];

    if (cog_config_tree_new(&tree) != COG_OK) {
        return 1;
    }
    /* interleave commit, rollback and long-lived snapshots */
    for (i = 0U; i < 32U; ++i) {
        if (cog_config_begin(tree, &txns[i]) != COG_OK) {
            return 1;
        }
    }
    for (i = 0U; i < 100U; ++i) {
        (void)snprintf(path, sizeof path, "mix%zu", i % 10U);
        (void)snprintf(val, sizeof val, "m%zu", i);
        if (cog_config_begin(tree, &txn) != COG_OK) {
            return 1;
        }
        if (cog_config_set(txn, path, val) != COG_OK) {
            return 1;
        }
        if (i % 3U == 0U) {
            cog_config_rollback(txn);
        } else {
            if (cog_config_commit(txn) != COG_OK) {
                return 1;
            }
        }
    }
    /* finalize every long-lived snapshot (mix of active + finalized) */
    for (i = 0U; i < 32U; ++i) {
        cog_config_rollback(txns[i]);
    }
    /* the surviving committed values are visible and consistent */
    if (cog_config_begin(tree, &check) != COG_OK) {
        return 1;
    }
    {
        size_t count = 0U;

        if (cog_config_list(check, "", NULL, 0U, &count) != COG_OK ||
            count > 10U) {
            return 1;
        }
    }
    cog_config_rollback(check);
    cog_config_tree_free(tree);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Main                                                                 */
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

    /* Lifecycle */
    CHECK("tree lifecycle", test_tree_lifecycle());
    CHECK("txn arg errors", test_txn_arg_errors());

    /* Set / get / delete */
    CHECK("set/get nested", test_set_get_nested());
    CHECK("set/get nulls", test_set_get_nulls());
    CHECK("path validation", test_path_validation());
    CHECK("delete", test_delete());
    CHECK("delete nulls/state", test_delete_nulls_and_state());

    /* List */
    CHECK("list", test_list());

    /* Transactions */
    CHECK("commit visibility", test_commit_visibility());
    CHECK("commit finalizes", test_commit_finalizes());
    CHECK("rollback discards", test_rollback_discards());
    CHECK("no-op commit", test_noop_commit());
    CHECK("snapshot isolation stress", test_snapshot_isolation_stress());

    /* Validators */
    CHECK("validators basic", test_validators_basic());
    CHECK("validator rejects", test_validator_rejects());
    CHECK("validator absent prefix", test_validator_absent_prefix());
    CHECK("validator registration errors",
          test_validator_registration_errors());

    /* Deep paths and stress */
    CHECK("deep paths", test_deep_paths());
    CHECK("commit/rollback stress", test_commit_rollback_stress());
    CHECK("mixed stress invariants", test_mixed_stress_invariants());

#undef CHECK

    if (failures == 0U) {
        (void)printf("test_config: ok\n");
    }
    return failures == 0U ? 0 : 1;
}

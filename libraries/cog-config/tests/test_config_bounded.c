/** @file tests_config_bounded.c @brief Aggregate capacity and atomic failure. */
#include <cog/config.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x)                                                                       \
    do {                                                                               \
        if (!(x)) {                                                                    \
            fprintf(stderr, "line %d: %s\n", __LINE__, #x);                            \
            return 1;                                                                  \
        }                                                                              \
    } while (0)
static cog_result_t reject(const cog_config_txn_t *t, const char *prefix)
{
    (void)t;
    (void)prefix;
    return COG_ERROR_INVALID_ARG;
}
static int capacity(void)
{
    cog_config_tree_t *t = NULL;
    cog_config_txn_t *a = NULL, *b = NULL, *snap = NULL;
    cog_config_limits_t l = {4U, 8U, 5U, 1U};
    const char *v = NULL, *key = NULL;
    size_t count = 0U;
    CHECK(cog_config_tree_new_ex(&t, &l) == COG_OK);
    CHECK(cog_config_begin(t, &a) == COG_OK);
    CHECK(cog_config_set(a, "a", "x") == COG_OK); /* roots 2 + leaf 1, 4 bytes */
    CHECK(cog_config_set(a, "b.c", "z") == COG_ERROR_LIMIT);
    CHECK(cog_config_get(a, "b", &v) == COG_ERROR_NOT_FOUND);
    CHECK(cog_config_commit(a) == COG_OK);
    CHECK(cog_config_begin(t, &snap) == COG_OK); /* four aggregate nodes, eight bytes */
    CHECK(cog_config_get(snap, "a", &v) == COG_OK);
    CHECK(cog_config_list(snap, "", &key, 1U, &count) == COG_OK);
    CHECK(cog_config_begin(t, &b) == COG_ERROR_LIMIT && b == NULL);
    CHECK(strcmp(v, "x") == 0 && strcmp(key, "a") == 0);
    CHECK(cog_config_set(snap, "a", "too long") == COG_ERROR_LIMIT);
    CHECK(cog_config_get(snap, "a", &v) == COG_OK && strcmp(v, "x") == 0);
    CHECK(cog_config_set(snap, "a", v) == COG_OK); /* aliased replacement at ceiling */
    cog_config_rollback(snap);
    CHECK(cog_config_begin(t, &b) == COG_OK);
    CHECK(cog_config_set(a, "a", "stale") == COG_ERROR_INVALID_STATE);
    cog_config_rollback(b);
    CHECK(cog_config_begin(t, &b) == COG_OK);
    cog_config_rollback(b);
    CHECK(cog_config_begin(t, &b) == COG_OK);
    cog_config_rollback(b);
    CHECK(cog_config_begin(t, &b) == COG_ERROR_LIMIT && b == NULL);
    cog_config_tree_free(t);
    return 0;
}
static int strings_and_reset(void)
{
    const char *v = NULL;
    cog_config_tree_t *t = NULL;
    cog_config_txn_t *a = NULL, *b = NULL;
    cog_config_limits_t l = {10U, 8U, 8U, 2U};
    CHECK(cog_config_tree_new_ex(&t, &l) == COG_OK);
    CHECK(cog_config_begin(t, &a) == COG_OK);
    CHECK(cog_config_set(a, "a", "x") == COG_OK);
    CHECK(cog_config_commit(a) == COG_OK);
    CHECK(cog_config_begin(t, &a) == COG_OK);
    CHECK(cog_config_register_validator(t, "a", reject) == COG_ERROR_LIMIT);
    cog_config_rollback(a);
    CHECK(cog_config_register_validator(t, "a", reject) ==
          COG_OK); /* prefix now fits */
    CHECK(cog_config_register_validator(t, "b", reject) == COG_OK);
    CHECK(cog_config_register_validator(t, "c", reject) == COG_ERROR_OVERFLOW);
    CHECK(cog_config_begin(t, &a) == COG_ERROR_LIMIT);
    cog_config_tree_free(t);
    l.string_bytes = 128U;
    l.max_nodes = 7U;
    CHECK(cog_config_tree_new_ex(&t, &l) == COG_OK);
    CHECK(cog_config_register_validator(t, "", reject) == COG_OK);
    CHECK(cog_config_begin(t, &a) == COG_OK);           /* empty snapshot, 2 nodes */
    CHECK(cog_config_begin(t, &b) == COG_OK);           /* another empty candidate */
    CHECK(cog_config_set(b, "a.b.c.d", "v") == COG_OK); /* all seven nodes */
    /* Rejection resets b to empty; rejection is still reusable. */
    CHECK(cog_config_commit(b) == COG_ERROR_INVALID_ARG);
    CHECK(cog_config_get(b, "a", &v) == COG_ERROR_NOT_FOUND);
    cog_config_rollback(a);
    cog_config_rollback(b);
    cog_config_tree_free(t);
    return 0;
}
static int rejected_reset_limit(void)
{
    cog_config_tree_t *t = NULL;
    cog_config_txn_t *old = NULL, *w = NULL, *b = NULL;
    cog_config_limits_t l = {8U, 128U, 10U, 1U};
    CHECK(cog_config_tree_new_ex(&t, &l) == COG_OK);
    CHECK(cog_config_begin(t, &old) == COG_OK); /* old empty root */
    CHECK(cog_config_begin(t, &w) == COG_OK);
    CHECK(cog_config_set(w, "a.b.c.d.e", "v") == COG_OK); /* total eight */
    CHECK(cog_config_commit(w) == COG_OK);                /* six running + one old */
    CHECK(cog_config_register_validator(t, "", reject) == COG_OK);
    CHECK(cog_config_commit(old) == COG_ERROR_LIMIT); /* six-node reset cannot fit */
    CHECK(cog_config_set(old, "x", "y") == COG_ERROR_INVALID_STATE);
    CHECK(cog_config_begin(t, &b) == COG_ERROR_LIMIT);
    cog_config_tree_free(t);
    return 0;
}
static int pointer_isolation(void)
{
    cog_config_tree_t *t = NULL;
    cog_config_txn_t *a = NULL, *b = NULL, *c = NULL;
    const char *v = NULL, *key = NULL;
    size_t n;
    CHECK(cog_config_tree_new(&t) == COG_OK);
    CHECK(cog_config_begin(t, &a) == COG_OK);
    CHECK(cog_config_set(a, "first", "stable") == COG_OK);
    CHECK(cog_config_commit(a) == COG_OK);
    CHECK(cog_config_begin(t, &a) == COG_OK);
    CHECK(cog_config_get(a, "first", &v) == COG_OK);
    CHECK(cog_config_list(a, "", &key, 1U, &n) == COG_OK);
    CHECK(cog_config_begin(t, &b) == COG_OK);
    CHECK(cog_config_set(b, "later", "new") == COG_OK);
    CHECK(cog_config_commit(b) == COG_OK);
    CHECK(cog_config_begin(t, &c) == COG_OK);
    CHECK(strcmp(v, "stable") == 0 && strcmp(key, "first") == 0);
    cog_config_rollback(c);
    cog_config_rollback(a);
    cog_config_tree_free(t);
    return 0;
}
int main(void)
{
    cog_config_tree_t *t = (cog_config_tree_t *)(void *)&t;
    cog_config_limits_t impossible = {SIZE_MAX, 0U, 0U, 0U};
    CHECK(cog_config_tree_new_ex(&t, &impossible) == COG_ERROR_INVALID_ARG &&
          t == NULL);
    CHECK(capacity() == 0);
    CHECK(strings_and_reset() == 0);
    CHECK(rejected_reset_limit() == 0);
    CHECK(pointer_isolation() == 0);
    puts("bounded configuration: ok");
    return 0;
}

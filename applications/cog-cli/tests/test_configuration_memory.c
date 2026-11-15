/** @file test_configuration_memory.c @brief Link-local allocation failure audit. */
#define _POSIX_C_SOURCE 200809L
#include "configuration.h"
#include "options_internal.h"
#include <cog/config.h>
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
void *__real_malloc(size_t size);
void *__real_calloc(size_t count, size_t size);
void __real_free(void *ptr);
void *__wrap_malloc(size_t size);
void *__wrap_calloc(size_t count, size_t size);
void __wrap_free(void *ptr);
static size_t calls, fail_at, live;
static void *watched;
static size_t watched_bytes;
static bool watch_next, uncleared;
void *__wrap_malloc(size_t size)
{
    void *p;
    ++calls;
    if (fail_at != 0U && calls == fail_at)
        return NULL;
    p = __real_malloc(size);
    if (p != NULL)
        ++live;
    return p;
}
void *__wrap_calloc(size_t count, size_t size)
{
    void *p;
    ++calls;
    if (fail_at != 0U && calls == fail_at)
        return NULL;
    p = __real_calloc(count, size);
    if (p != NULL)
        ++live;
    if (watch_next && p != NULL) {
        watched = p;
        watched_bytes = count * size;
        watch_next = false;
    }
    return p;
}
void __wrap_free(void *p)
{
    if (p == watched && p != NULL) {
        const unsigned char *bytes = p;
        size_t i;
        for (i = 0U; i < watched_bytes; ++i)
            if (bytes[i] != 0U)
                uncleared = true;
        watched = NULL;
    }
    if (p != NULL)
        --live;
    __real_free(p);
}
static cog_result_t reject(const cog_config_txn_t *txn, const char *prefix)
{
    (void)txn;
    (void)prefix;
    return COG_ERROR_INVALID_ARG;
}
static cog_result_t secret_fail(void *arg, const char *ref, char *out, size_t cap,
                                size_t *required)
{
    (void)arg;
    (void)ref;
    *required = 16U;
    if (cap < 16U)
        return COG_ERROR_OVERFLOW;
    memset(out, 's', 15U);
    out[15] = '\0';
    return COG_ERROR_DENIED;
}
int main(void)
{
    cog_config_limits_t limits = {32U, 256U, 16U, 2U};
    cog_config_tree_t *tree = NULL;
    cog_config_txn_t *a = NULL, *b = NULL;
    size_t i, n, initial;
    const char *v = NULL;
    cog_cli_options_t o;
    cog_cli_configuration_t *c = NULL;
    for (i = 1U; i <= 9U; ++i) {
        calls = 0U;
        fail_at = i;
        CHECK(cog_config_tree_new_ex(&tree, &limits) == COG_ERROR_NOMEM &&
              tree == NULL);
        CHECK(live == 0U);
    }
    fail_at = 0U;
    calls = 0U;
    CHECK(cog_config_tree_new_ex(&tree, &limits) == COG_OK);
    initial = calls;
    CHECK(initial == 9U);
    fail_at = calls + 1U; /* every runtime allocation would fail */
    CHECK(cog_config_register_validator(tree, "", reject) == COG_OK);
    CHECK(cog_config_begin(tree, &a) == COG_OK);
    CHECK(cog_config_begin(tree, &b) == COG_OK);
    CHECK(cog_config_set(a, "a.b", "value") == COG_OK);
    CHECK(cog_config_get(a, "a.b", &v) == COG_OK && strcmp(v, "value") == 0);
    CHECK(cog_config_list(a, "", NULL, 0U, &n) == COG_OK && n == 1U);
    CHECK(cog_config_delete(a, "a.b") == COG_OK);
    CHECK(cog_config_commit(a) == COG_ERROR_INVALID_ARG);
    cog_config_rollback(a);
    cog_config_rollback(b);
    CHECK(calls == initial);
    cog_config_tree_free(tree);
    CHECK(live == 0U);
    cog_cli_options_defaults(&o);
    o.backend = COG_CLI_BACKEND_MOCK;
    o.explicit_options = COG_CLI_OPT_BACKEND;
    calls = 0U;
    fail_at = 1U;
    CHECK(cog_cli_configuration_new(&c, &o, 1U) == COG_ERROR_NOMEM && c == NULL &&
          live == 0U);
    fail_at = 0U;
    watch_next = true;
    CHECK(cog_cli_configuration_new(&c, &o, 1U) == COG_OK);
    cog_cli_configuration_free(c);
    CHECK(!uncleared && watched == NULL && live == 0U);
    o.backend = COG_CLI_BACKEND_REAL;
    o.store_mode = COG_CLI_STORE_DISABLED;
    o.model_id = "synthetic/model";
    o.credential_ref = "test:synthetic";
    o.credential_read = secret_fail;
    o.explicit_options |=
        COG_CLI_OPT_MODEL | COG_CLI_OPT_CREDENTIAL | COG_CLI_OPT_STORE_MODE;
    watch_next = true;
    CHECK(cog_cli_configuration_new(&c, &o, 2U) == COG_ERROR_DENIED && c == NULL);
    CHECK(!uncleared && watched == NULL && live == 0U);
    puts("configuration allocation and secret clearing: ok");
    return 0;
}

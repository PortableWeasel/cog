#define _POSIX_C_SOURCE 200809L
#include <cog/net.h>
#include <curl/curl.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>

static size_t call_count, fail_at;
static void *test_malloc(size_t n) { if (++call_count == fail_at) return NULL; return malloc(n); }
static void test_free(void *p) { free(p); }
static void *test_realloc(void *p, size_t n) { if (++call_count == fail_at) return NULL; return realloc(p, n); }
static char *test_strdup(const char *s)
{
    size_t n = strlen(s) + 1U;
    char *p = test_malloc(n);
    if (p != NULL) memcpy(p, s, n);
    return p;
}
static void *test_calloc(size_t n, size_t s) { if (++call_count == fail_at) return NULL; return calloc(n, s); }
int main(void)
{
    size_t point;
    for (point = 1U; point <= 100U; ++point) {
        cog_net_ctx_t *ctx = NULL;
        cog_net_config_t config = {0};
        cog_request_id_t id = 0U;
        cog_result_t rc;
        fail_at = 0U; call_count = 0U;
        assert(curl_global_init_mem(CURL_GLOBAL_DEFAULT, test_malloc, test_free,
                                    test_realloc, test_strdup, test_calloc) == CURLE_OK);
        config.host_runtime_initialized = true;
        config.dependency_memory = COG_NET_DEPENDENCY_MEMORY_DYNAMIC;
        call_count = 0U; fail_at = point;
        rc = cog_net_new(&ctx, &config);
        assert(rc == COG_OK || rc == COG_ERROR_NOMEM);
        if (rc == COG_OK) {
            rc = cog_net_request(ctx, "GET", "https://localhost/", "X-Test: value", NULL, 0U, &id);
            assert(rc == COG_OK || rc == COG_ERROR_NOMEM);
            if (rc == COG_OK) assert(cog_net_request_release(ctx, id) == COG_OK);
            cog_net_free(ctx);
        } else assert(ctx == NULL);
        fail_at = 0U;
        curl_global_cleanup();
    }
    for (point = 1U; point <= 100U; ++point) {
        cog_net_ctx_t *ctx = NULL;
        cog_net_config_t config = {0};
        cog_request_id_t id = 0U;
        uint64_t next;
        cog_result_t rc;
        fail_at = 0U; call_count = 0U;
        assert(curl_global_init_mem(CURL_GLOBAL_DEFAULT, test_malloc, test_free,
                                    test_realloc, test_strdup, test_calloc) == CURLE_OK);
        config.host_runtime_initialized = true;
        config.dependency_memory = COG_NET_DEPENDENCY_MEMORY_DYNAMIC;
        assert(cog_net_new(&ctx, &config) == COG_OK);
        assert(cog_net_request(ctx, "GET", "https://127.0.0.1:1/", NULL, NULL, 0U, &id) == COG_OK);
        call_count = 0U; fail_at = point;
        rc = cog_net_service(ctx, 100U, &next);
        assert(rc == COG_OK || rc == COG_ERROR_NOMEM || rc == COG_ERROR_IO);
        fail_at = 0U;
        assert(cog_net_request_release(ctx, id) == COG_OK);
        cog_net_free(ctx);
        curl_global_cleanup();
    }
    return 0;
}

#include <cog/net.h>
#include <curl/curl.h>
#include <assert.h>
#include <stddef.h>
#include <stdlib.h>

void *__real_malloc(size_t size);
void *__real_calloc(size_t n, size_t size);
void *__wrap_malloc(size_t size);
void *__wrap_calloc(size_t n, size_t size);
static size_t allocation, fail_at;
void *__wrap_malloc(size_t size)
{
    if (++allocation == fail_at) return NULL;
    return __real_malloc(size);
}
void *__wrap_calloc(size_t n, size_t size)
{
    if (++allocation == fail_at) return NULL;
    return __real_calloc(n, size);
}
int main(void)
{
    size_t point;
    assert(curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK);
    for (point = 1U; point <= 32U; ++point) {
        cog_net_ctx_t *ctx = NULL;
        cog_net_config_t config = {0};
        cog_result_t rc;
        config.host_runtime_initialized = true;
        config.dependency_memory = COG_NET_DEPENDENCY_MEMORY_DYNAMIC;
        config.ca_bundle = "/a/copied/path";
        allocation = 0U; fail_at = point;
        rc = cog_net_new(&ctx, &config);
        fail_at = 0U;
        assert(rc == COG_OK || rc == COG_ERROR_NOMEM);
        if (rc == COG_OK) cog_net_free(ctx);
        else assert(ctx == NULL);
    }
    curl_global_cleanup();
    return 0;
}

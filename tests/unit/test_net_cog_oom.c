#include <cog/net.h>
#include <curl/curl.h>
#include <assert.h>
#include <stddef.h>
#include <stdlib.h>

void *__real_malloc(size_t size);
void *__real_calloc(size_t n, size_t size);
void *__wrap_malloc(size_t size);
void *__wrap_calloc(size_t n, size_t size);
/* Observe linker-wrapper effects across builtin/libc leaf assumptions. */
static volatile size_t allocation, fail_at;
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
/* Positive control: optimized direct allocator calls must reach wrappers,
 * update the real oracle, and honor its fault injection before use. */
static void allocation_oracle_control(void)
{
    size_t before = allocation;
    unsigned char *bytes = malloc(8U);
    assert(bytes != NULL && allocation == before + 1U);
    bytes[0] = 0x5aU;
    free(bytes);
    before = allocation;
    bytes = calloc(8U, 1U);
    assert(bytes != NULL && allocation == before + 1U);
    assert(bytes[0] == 0U && bytes[7] == 0U);
    free(bytes);
    before = allocation;
    bytes = malloc(8U);
    free(bytes);
    assert(allocation == before + 1U); /* Detect optimized-away allocator pairs. */
    before = allocation;
    fail_at = allocation + 1U;
    bytes = malloc(8U);
    assert(bytes == NULL && allocation == before + 1U);
    fail_at = 0U;
    allocation = 0U;
}

int main(void)
{
    allocation_oracle_control();
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

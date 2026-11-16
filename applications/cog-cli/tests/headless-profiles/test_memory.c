/** @file test_memory.c @brief Observable O3 allocator/source-read/whole-owner wipe oracles. */
#define _POSIX_C_SOURCE 200809L
#include "child_profile.h"
#include "host_options.h"
#include "options_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); return 1; } } while (0)
void *__real_malloc(size_t n);
void *__real_calloc(size_t n, size_t bytes);
void *__real_realloc(void *p, size_t n);
void __real_free(void *p);
char *__real_getenv(const char *name);
FILE *__real_fopen(const char *path, const char *mode);
void *__wrap_malloc(size_t n);
void *__wrap_calloc(size_t n, size_t bytes);
void *__wrap_realloc(void *p, size_t n);
void __wrap_free(void *p);
char *__wrap_getenv(const char *name);
FILE *__wrap_fopen(const char *path, const char *mode);
static volatile size_t calls, fail_at, live, forbidden_reads;
static volatile bool forbid_sources;
static bool watch_next, dirty_free;
static void *watched;
static size_t watched_bytes;
void *__wrap_malloc(size_t n)
{
    ++calls; if (fail_at != 0U && calls == fail_at) return NULL;
    void *p = __real_malloc(n); if (p != NULL) ++live; return p;
}
void *__wrap_calloc(size_t n, size_t bytes)
{
    ++calls; if (fail_at != 0U && calls == fail_at) return NULL;
    void *p = __real_calloc(n, bytes);
    if (p != NULL) {
        ++live;
        if (watch_next) { watched = p; watched_bytes = n * bytes; watch_next = false; }
    }
    return p;
}
void *__wrap_realloc(void *p, size_t n)
{
    ++calls; if (fail_at != 0U && calls == fail_at) return NULL;
    void *r = __real_realloc(p, n); if (r != NULL && p == NULL) ++live; return r;
}
void __wrap_free(void *p)
{
    if (p != NULL) {
        if (p == watched) {
            const unsigned char *bytes = p;
            for (size_t i = 0U; i < watched_bytes; ++i) if (bytes[i] != 0U) dirty_free = true;
            watched = NULL;
        }
        --live;
    }
    __real_free(p);
}
char *__wrap_getenv(const char *name)
{
    if (forbid_sources) { ++forbidden_reads; return NULL; }
    return __real_getenv(name);
}
FILE *__wrap_fopen(const char *path, const char *mode)
{
    if (forbid_sources) { ++forbidden_reads; return NULL; }
    return __real_fopen(path, mode);
}
static volatile unsigned resolves;
static cog_result_t secret(void *arg, const char *reference, char *output, size_t capacity, size_t *required)
{
    (void)arg; (void)reference;
    ++resolves; *required = sizeof("SYNTHETIC_MEMORY_KEY");
    if (capacity < *required) return COG_ERROR_OVERFLOW;
    memcpy(output, "SYNTHETIC_MEMORY_KEY", *required); return COG_OK;
}
static int oracle(void)
{
    size_t before = calls;
    void *p = malloc(8U); CHECK(p != NULL && calls == before + 1U && live == 1U); free(p);
    before = calls; p = calloc(2U, 4U); CHECK(p != NULL && calls == before + 1U); free(p);
    before = calls; p = malloc(8U); free(p); CHECK(calls == before + 1U && live == 0U);
    before = calls; fail_at = calls + 1U; p = malloc(8U); CHECK(p == NULL && calls == before + 1U); fail_at = 0U;
    forbid_sources = true;
    CHECK(getenv("HOME") == NULL && fopen("/not-read", "rb") == NULL && forbidden_reads == 2U);
    forbid_sources = false; forbidden_reads = 0U;
    return 0;
}
int main(void)
{
    CHECK(oracle() == 0);
    for (size_t i = 0U; i < cog_cli_field_count; ++i)
        if (cog_cli_fields[i].environment != NULL) CHECK(unsetenv(cog_cli_fields[i].environment) == 0);
    cog_cli_options_t options; cog_cli_options_defaults(&options);
    options.model_id = "synthetic/model"; options.credential_ref = "host:memory";
    options.credential_read = secret; options.workspace_root = "/work"; options.store_dir = "/state";
    options.explicit_options = COG_CLI_OPT_MODEL | COG_CLI_OPT_CREDENTIAL | COG_CLI_OPT_WORKSPACE | COG_CLI_OPT_STORE_DIR;
    cog_cli_configuration_t *parent = NULL, *child = NULL, *decoded = NULL;
    watch_next = true;
    CHECK(cog_cli_configuration_new(&parent, &options, 7U) == COG_OK && resolves == 1U && live == 1U && watched == parent);
    size_t owner_bytes = watched_bytes;
    unsigned char *snapshot = __real_malloc(owner_bytes); CHECK(snapshot != NULL);
    memcpy(snapshot, parent, owner_bytes);
    forbid_sources = true;
    size_t before = calls;
    CHECK(cog_cli_configuration_finish_host(parent, (cog_net_ctx_t *)(void *)&options, "/work", "/bad/../path") == COG_ERROR_INVALID_ARG);
    CHECK(memcmp(parent, snapshot, owner_bytes) == 0 && calls == before);
    CHECK(cog_cli_configuration_finish_host(parent, (cog_net_ctx_t *)(void *)&options, "/work", "/state") == COG_OK && calls == before);
    __real_free(snapshot);
    cog_cli_child_profile_t profile = {.tools = 15U, .max_retries = 1U};
    uint64_t deadline = 777U;
    fail_at = calls + 1U;
    CHECK(cog_cli_configuration_child_new(&child, parent, &profile, 7U, 100U, 1000U,
        (cog_net_ctx_t *)(void *)&options, &deadline) == COG_ERROR_NOMEM && child == NULL && deadline == 777U && live == 1U);
    fail_at = 0U; watched = NULL; watch_next = true;
    CHECK(cog_cli_configuration_child_new(&child, parent, &profile, 7U, 100U, 1000U,
        (cog_net_ctx_t *)(void *)&options, &deadline) == COG_OK && watched == child && live == 2U);
    cog_cli_bootstrap_probe_t probe = {.session_id = "root", .owner = {1U, 2U}, .checkpoint = {3U}}, copied, probe_before;
    cog_net_config_t transport = {.host_runtime_initialized = true, .dependency_memory = COG_NET_DEPENDENCY_MEMORY_DYNAMIC};
    unsigned char frame[32768]; size_t length = 0U;
    before = calls;
    CHECK(cog_cli_configuration_bootstrap_encode(child, &probe, &transport, frame, sizeof(frame), &length) == COG_OK && calls == before);
    cog_net_config_t decoded_transport;
    size_t required = 777U;
    CHECK(cog_cli_bootstrap_transport_decode(frame, length, &decoded_transport, NULL, 0U, &required) == COG_OK && required == 0U && calls == before);
    memset(&copied, 0x55, sizeof(copied)); probe_before = copied;
    fail_at = calls + 1U;
    CHECK(cog_cli_configuration_bootstrap_new(&decoded, frame, length, (cog_net_ctx_t *)(void *)&options, &copied) == COG_ERROR_NOMEM && decoded == NULL && live == 2U);
    CHECK(memcmp(&copied, &probe_before, sizeof(copied)) == 0);
    fail_at = 0U;
    cog_cli_configuration_free(child); CHECK(!dirty_free && watched == NULL && live == 1U);
    watch_next = true;
    CHECK(cog_cli_configuration_bootstrap_new(&decoded, frame, length, (cog_net_ctx_t *)(void *)&options, &copied) == COG_OK && watched == decoded && live == 2U);
    /* Decoded owner outlives and no longer aliases the full wire reservation. */
    memset(frame, 0x77, sizeof(frame));
    CHECK(strcmp(cog_cli_configuration_options(decoded)->model_id, "synthetic/model") == 0);
    cog_cli_configuration_free(decoded); CHECK(!dirty_free && watched == NULL && live == 1U);
    watched = parent; watched_bytes = owner_bytes;
    cog_cli_configuration_free(parent); CHECK(!dirty_free && watched == NULL && live == 0U);
    CHECK(resolves == 1U && forbidden_reads == 0U);
    cog_cli_host_options_t host = {0}; int exit_code = 0;
    char *argv[] = {(char *)"cog", (char *)"--attach=/ui"};
    before = calls; fail_at = calls + 1U;
    CHECK(cog_cli_host_options_open(&host, 2, argv, &exit_code) == COG_ERROR_NOMEM && host.owned_parse_storage == NULL && live == 0U && calls == before + 1U);
    fail_at = 0U; before = calls;
    CHECK(cog_cli_host_options_open(&host, 2, argv, &exit_code) == COG_OK && calls == before + 1U && live == 1U);
    cog_cli_host_options_close(&host); cog_cli_host_options_close(&host);
    CHECK(live == 0U && forbidden_reads == 0U && resolves == 1U);
    forbid_sources = false;
    puts("O3 allocation/source-read/zero-before-free controls: ok"); return 0;
}

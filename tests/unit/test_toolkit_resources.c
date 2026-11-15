/** @file test_toolkit_resources.c @brief Constructor faults and no runtime growth. */
#define _POSIX_C_SOURCE 200809L
#include <cog/toolkit.h>
#include "fixtures/toolkit_core.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void *__real_malloc(size_t size);
void *__real_calloc(size_t count, size_t size);
void *__real_realloc(void *memory, size_t size);
void *__wrap_malloc(size_t size);
void *__wrap_calloc(size_t count, size_t size);
void *__wrap_realloc(void *memory, size_t size);
/* Observe linker-wrapper effects across builtin/libc leaf assumptions. */
static volatile long fail_after = -1L;
static volatile size_t forbidden_allocations;
static bool fail_all;
static bool fail_allocation(void)
{
    if (fail_all) { forbidden_allocations++; return true; }
    if (fail_after < 0L) { return false; }
    if (fail_after == 0L) { return true; }
    fail_after--;
    return false;
}
void *__wrap_malloc(size_t size) { return fail_allocation() ? NULL : __real_malloc(size); }
void *__wrap_calloc(size_t count, size_t size) { return fail_allocation() ? NULL : __real_calloc(count, size); }
void *__wrap_realloc(void *memory, size_t size) { return fail_allocation() ? NULL : __real_realloc(memory, size); }

static size_t effects;
static cog_result_t effect(cog_core_t *core, const char *request, size_t request_len,
    char *output, size_t capacity, size_t *length)
{
    assert(core == toolkit_test_core && request_len == 3U && memcmp(request, "arg", 3U) == 0);
    effects++;
    *length = 12U;
    assert(capacity >= 13U);
    memcpy(output, "effect-value", 13U);
    return COG_OK;
}
static cog_result_t overflow_effect(cog_core_t *core, const char *request, size_t request_len,
    char *output, size_t capacity, size_t *length)
{
    (void)core; (void)request; (void)request_len; (void)output; (void)capacity;
    effects++;
    *length = 131072U;
    return COG_ERROR_OVERFLOW;
}
static const cog_tool_t descriptors[] = {
    { .name = "effect", .handler = effect },
    { .name = "overflow", .handler = overflow_effect }
};
static const cog_tool_table_t table = { descriptors, 2U };
static void retained_legacy(void)
{
    char request[] = "arg";
    cog_toolkit_call_t *call = NULL;
    assert(cog_toolkit_call_new(&call, "effect", request, 3U) == COG_OK);
    memset(request, 'x', 3U);
    char bytes[64] = "untouched";
    cog_toolkit_result_t result = { .output = bytes };
    fail_all = true;
    assert(cog_toolkit_execute(toolkit_test_core, &table, call, &result, 2U) == COG_ERROR_OVERFLOW);
    assert(result.output_len == 12U && strcmp(bytes, "untouched") == 0 && effects == 1U);
    assert(cog_toolkit_execute(toolkit_test_core, &table, call, &result, sizeof(bytes)) == COG_OK);
    assert(strcmp(bytes, "effect-value") == 0 && effects == 1U);
    assert(cog_toolkit_call_cancel(call) == COG_OK);
    assert(cog_toolkit_execute(toolkit_test_core, &table, call, &result, sizeof(bytes)) == COG_OK);
    fail_all = false;
    assert(forbidden_allocations == 0U);
    cog_toolkit_call_free(call);
    assert(cog_toolkit_call_new(&call, "overflow", NULL, 0U) == COG_OK);
    assert(cog_toolkit_execute(toolkit_test_core, &table, call, &result, sizeof(bytes)) == COG_ERROR_OVERFLOW);
    assert(effects == 2U && result.truncated && result.side_effects_possible && result.output_len == 131072U);
    assert(cog_toolkit_execute(toolkit_test_core, &table, call, &result, sizeof(bytes)) == COG_ERROR_OVERFLOW && effects == 2U);
    cog_toolkit_call_free(call);
}
static void constructors(const char *root)
{
    cog_workspace_config_t config = { .root = root, .max_file_bytes = 4096U,
        .max_search_entries = 8U, .service_quantum = 1U };
    size_t failures = 0U;
    for (long i = 0L; i < 32L; i++) {
        cog_workspace_t *workspace = NULL;
        fail_after = i;
        cog_result_t r = cog_workspace_new(&workspace, &config);
        fail_after = -1L;
        if (r == COG_OK) { assert(cog_workspace_free(workspace) == COG_OK); break; }
        assert(r == COG_ERROR_NOMEM && workspace == NULL);
        failures++;
    }
    assert(failures >= 6U);
    cog_workspace_t *workspace = NULL;
    assert(cog_workspace_new(&workspace, &config) == COG_OK);
    cog_tool_async_config_t limits = { .max_output_bytes = 64U, .timeout_ms = 1000U, .user = workspace };
    failures = 0U;
    for (long i = 0L; i < 32L; i++) {
        cog_toolkit_execution_t *execution = NULL;
        fail_after = i;
        cog_result_t r = cog_toolkit_execution_new(&execution, toolkit_test_core, &cog_workspace_tools,
            "id", "read_file", "{\"path\":\"file\"}", 15U, &limits);
        fail_after = -1L;
        if (r == COG_OK) { assert(cog_toolkit_execution_free(execution) == COG_OK); break; }
        assert(r == COG_ERROR_NOMEM && execution == NULL);
        failures++;
    }
    assert(failures >= 5U);
    cog_toolkit_execution_t *execution = NULL;
    assert(cog_toolkit_execution_new(&execution, toolkit_test_core, &cog_workspace_tools,
        "id", "read_file", "{\"path\":\"file\"}", 15U, &limits) == COG_OK);
    fail_all = true;
    assert(cog_toolkit_execution_start(execution, COG_TOOLKIT_POLICY_ALLOW, 0U) == COG_OK);
    bool done = false;
    uint64_t deadline;
    for (size_t i = 0U; i < 32U && !done; i++) {
        assert(cog_toolkit_execution_poll(execution, i, &done, &deadline) == COG_OK);
    }
    assert(done);
    char output[64];
    cog_toolkit_result_t result = {0};
    size_t required;
    assert(cog_toolkit_execution_result(execution, &result, output, sizeof(output), &required) == COG_OK);
    assert(result.status == COG_OK && strcmp(output, "fixture") == 0);
    assert(cog_toolkit_execution_free(execution) == COG_OK);
    fail_all = false;
    assert(forbidden_allocations == 0U);
    assert(cog_workspace_free(workspace) == COG_OK);
}
static void batch_setup_faults(void)
{
    size_t before = effects;
    for (long i = 0L; i < 4L; i++) {
        cog_toolkit_call_t *call = NULL;
        assert(cog_toolkit_call_new(&call, "effect", "arg", 3U) == COG_OK);
        cog_toolkit_call_t *calls[] = {call};
        cog_toolkit_result_collection_t *collection = NULL;
        fail_after = i;
        cog_result_t r = cog_toolkit_execute_all(toolkit_test_core, &table, calls, 1U, NULL, &collection);
        fail_after = -1L;
        if (r == COG_OK) { assert(effects == before + 1U); cog_toolkit_result_collection_free(collection); }
        else { assert(r == COG_ERROR_NOMEM && collection == NULL && effects == before); }
        cog_toolkit_call_free(call);
    }
}
/* Countdowns and the no-allocation deny counter must survive optimization. */
static void allocation_oracle_control(void)
{
    fail_after = 1L;
    unsigned char *bytes = malloc(8U);
    assert(bytes != NULL && fail_after == 0L);
    bytes[0] = 0x5aU;
    fail_after = 1L;
    unsigned char *grown = realloc(bytes, 16U);
    assert(grown != NULL && fail_after == 0L && grown[0] == 0x5aU);
    free(grown);
    fail_after = 1L;
    bytes = calloc(8U, 1U);
    assert(bytes != NULL && fail_after == 0L && bytes[0] == 0U && bytes[7] == 0U);
    free(bytes);
    fail_after = 1L;
    bytes = malloc(8U);
    free(bytes);
    assert(fail_after == 0L); /* Detect an optimized-away allocator pair. */
    fail_all = true;
    size_t before = forbidden_allocations;
    bytes = malloc(8U);
    assert(bytes == NULL && forbidden_allocations == before + 1U);
    fail_all = false;
    fail_after = -1L;
    forbidden_allocations = 0U;
}
int main(void)
{
    allocation_oracle_control();
    toolkit_core_create();
    retained_legacy(); batch_setup_faults();
    char root[] = "/tmp/cog-toolkit-resources-XXXXXX";
    assert(mkdtemp(root) != NULL);
    char path[4096];
    int n = snprintf(path, sizeof(path), "%s/file", root);
    assert(n > 0 && (size_t)n < sizeof(path));
    FILE *file = fopen(path, "w");
    assert(file != NULL && fputs("fixture", file) >= 0 && fclose(file) == 0);
    constructors(root);
    assert(unlink(path) == 0 && rmdir(root) == 0);
    toolkit_core_destroy();
    puts("test_toolkit_resources: ok");
    return 0;
}

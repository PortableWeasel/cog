/** @file test_session_allocations.c
 * @brief Linker-wrapped constructor OOM and mutation-allocation contract tests.
 * Link with --wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=strdup.
 */
#define _POSIX_C_SOURCE 200809L
#include "cog/session.h"
#include "session_internal.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>

#define CHECK(expr) do { if (!(expr)) { \
    (void)fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); abort(); \
    } } while (0)

void *__real_malloc(size_t size);
void *__real_calloc(size_t count, size_t size);
void *__real_realloc(void *ptr, size_t size);
char *__real_strdup(const char *text);
void *__wrap_malloc(size_t size);
void *__wrap_calloc(size_t count, size_t size);
void *__wrap_realloc(void *ptr, size_t size);
char *__wrap_strdup(const char *text);
static size_t attempts;
static size_t failure_at;
static bool deny;
static bool fail(void)
{
    ++attempts;
    return deny || (failure_at != 0U && attempts == failure_at);
}
void *__wrap_malloc(size_t size) { return fail() ? NULL : __real_malloc(size); }
void *__wrap_calloc(size_t count, size_t size) { return fail() ? NULL : __real_calloc(count, size); }
void *__wrap_realloc(void *ptr, size_t size) { return fail() ? NULL : __real_realloc(ptr, size); }
char *__wrap_strdup(const char *text) { return fail() ? NULL : __real_strdup(text); }

/* Positive control: optimized direct allocator calls must reach wrappers,
 * update the real oracle, and honor its fault injection before use. */
static void allocation_oracle_control(void)
{
    size_t before = attempts;
    unsigned char *bytes = malloc(8U);
    assert(bytes != NULL && attempts == before + 1U);
    bytes[0] = 0x5aU;
    before = attempts;
    unsigned char *grown = realloc(bytes, 16U);
    assert(grown != NULL && attempts == before + 1U && grown[0] == 0x5aU);
    bytes = grown;
    free(bytes);
    before = attempts;
    bytes = calloc(8U, 1U);
    assert(bytes != NULL && attempts == before + 1U);
    assert(bytes[0] == 0U && bytes[7] == 0U);
    free(bytes);
    before = attempts;
    char *text = strdup("oracle");
    assert(text != NULL && attempts == before + 1U && strcmp(text, "oracle") == 0);
    free(text);
    before = attempts;
    bytes = malloc(8U);
    free(bytes);
    assert(attempts == before + 1U); /* Detect optimized-away allocator pairs. */
    before = attempts;
    failure_at = attempts + 1U;
    bytes = malloc(8U);
    assert(bytes == NULL && attempts == before + 1U);
    failure_at = 0U;
    attempts = 0U;
}

int main(void)
{
    allocation_oracle_control();
    cog_session_t *s = NULL;
    const cog_session_config_t cfg = {.id = "alloc", .max_nodes = 16U,
        .max_payload_bytes = 16384U, .max_tool_calls_per_message = 2U};
    CHECK(cog_session_new(&s, &cfg) == COG_OK);
    const size_t constructor_allocations = attempts;
    CHECK(constructor_allocations != 0U);
    cog_session_free(s);
    for (size_t k = 1U; k <= constructor_allocations; ++k) {
        attempts = 0U; failure_at = k; s = (cog_session_t *)(void *)&attempts;
        CHECK(cog_session_new(&s, &cfg) == COG_ERROR_NOMEM && s == NULL);
    }
    attempts = 0U; failure_at = 0U;
    CHECK(cog_session_new(&s, &cfg) == COG_OK);
    const size_t before = attempts;
    deny = true;
    CHECK(cog_session_set_parent(s, "parent") == COG_OK);
    const cog_message_t user = {.role = COG_MESSAGE_ROLE_USER, .content = "input", .content_len = 5U};
    cog_session_node_t *root, *assistant, *draft;
    CHECK(cog_session_append(s, &user, &root) == COG_OK);
    CHECK(cog_session_append(s, &user, &draft) == COG_OK);
    CHECK(cog_session_discard_node(s, draft) == COG_OK);
    const cog_message_tool_call_t call = {"id", "tool", "{}", 2U};
    const cog_message_t proposal = {.role = COG_MESSAGE_ROLE_ASSISTANT, .tool_calls = &call, .tool_call_count = 1U};
    CHECK(cog_session_append(s, &proposal, &assistant) == COG_OK);
    CHECK(cog_session_tool_transition(s, assistant, "id", COG_TOOL_EXECUTION_PROPOSED, COG_TOOL_EXECUTION_STARTED) == COG_OK);
    CHECK(cog_session_tool_transition(s, assistant, "id", COG_TOOL_EXECUTION_STARTED, COG_TOOL_EXECUTION_INDETERMINATE) == COG_OK);
    const cog_message_t result = {.role = COG_MESSAGE_ROLE_TOOL, .content = "unknown outcome acknowledged", .content_len = 28U, .tool_call_id = "id"};
    CHECK(cog_session_reconcile_tool(s, assistant, "id", COG_TOOL_EXECUTION_ACKNOWLEDGED, &result, NULL) == COG_OK);
    cog_message_t history[16]; size_t count;
    CHECK(cog_session_copy_history(s, NULL, history, 16U, &count) == COG_OK && count == 3U);
    const cog_message_t *borrowed;
    CHECK(cog_session_get_history(s, cog_session_active(s), &borrowed, &count) == COG_OK);
    cog_session_snapshot_t snapshot;
    CHECK(cog_session_snapshot_view(s, &snapshot) == COG_OK);
    CHECK(cog_session_snapshot_validate(&snapshot) == COG_OK);
    CHECK(cog_session_fork(s, root, NULL) == COG_OK);
    CHECK(attempts == before);
    deny = false;
    CHECK(cog_session_snapshot_view(s, &snapshot) == COG_OK);
    for (size_t k = 1U; k <= constructor_allocations; ++k) {
        attempts = 0U; failure_at = k;
        cog_session_t *restored = NULL;
        CHECK(cog_session_restore(&restored, &cfg, &snapshot) == COG_ERROR_NOMEM && restored == NULL);
    }
    failure_at = 0U;
    cog_session_t *restored = NULL;
    CHECK(cog_session_restore(&restored, &cfg, &snapshot) == COG_OK);
    cog_session_free(restored); cog_session_free(s);
    puts("session constructor OOM and zero mutation allocation contracts passed");
    return 0;
}

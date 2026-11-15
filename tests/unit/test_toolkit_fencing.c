/** @file test_toolkit_fencing.c @brief Real guard admission and late uncertainty. */
#define _XOPEN_SOURCE 700
#include <cog/toolkit.h>
#include <cog/session_store.h>
#include "fixtures/toolkit_core.h"
#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static int cleanup(const char *path, const struct stat *st, int type, struct FTW *state)
{
    (void)st; (void)type; (void)state;
    return remove(path);
}
static cog_toolkit_execution_t *execution_new(cog_workspace_t *workspace,
    cog_store_owner_guard_t *guard, cog_result_t expected)
{
    cog_tool_async_config_t config = { .max_output_bytes = 64U, .timeout_ms = 100U,
        .owner_guard = guard, .user = workspace };
    static const char arguments[] = "{\"path\":\"file\",\"old\":\"before\",\"new\":\"after\"}";
    cog_toolkit_execution_t *execution = NULL;
    assert(cog_toolkit_execution_new(&execution, toolkit_test_core, &cog_workspace_tools,
        "fenced", "edit_file", arguments, sizeof(arguments) - 1U, &config) == expected);
    if (expected != COG_OK) { assert(execution == NULL); }
    return execution;
}
int main(void)
{
    char root[] = "/tmp/cog-toolkit-fencing-XXXXXX";
    assert(mkdtemp(root) != NULL);
    char path[4096];
    int n = snprintf(path, sizeof(path), "%s/file", root);
    assert(n > 0 && (size_t)n < sizeof(path));
    FILE *file = fopen(path, "w");
    assert(file != NULL && fputs("before", file) >= 0 && fclose(file) == 0);
    cog_store_t *store = NULL;
    cog_store_config_t config = { .base_dir = root, .require_owner = true, .max_record_bytes = 1048576U };
    assert(cog_store_new(&store, &config) == COG_OK);
    cog_store_owner_t owner = {1U, 55U};
    assert(cog_store_owner_compare_exchange(store, "toolkit-test", (cog_store_owner_t){0}, owner,
        (cog_store_checkpoint_t){0}) == COG_OK);
    cog_store_owner_guard_t *guard = NULL;
    assert(cog_store_owner_acquire(store, "toolkit-test", owner, &guard) == COG_OK);
    toolkit_test_store = store;
    toolkit_core_create();
    cog_workspace_t *workspace = NULL;
    cog_workspace_config_t workspace_config = { .root = root, .max_file_bytes = 4096U,
        .max_search_entries = 16U, .service_quantum = 1U };
    assert(cog_workspace_new(&workspace, &workspace_config) == COG_OK);
    (void)execution_new(workspace, NULL, COG_ERROR_INVALID_STATE);
    assert(cog_store_owner_compare_exchange(store, "other", (cog_store_owner_t){0}, owner,
        (cog_store_checkpoint_t){0}) == COG_OK);
    cog_store_owner_guard_t *wrong = NULL;
    assert(cog_store_owner_acquire(store, "other", owner, &wrong) == COG_OK);
    (void)execution_new(workspace, wrong, COG_ERROR_INVALID_STATE);
    assert(cog_store_owner_release(wrong) == COG_OK);
    cog_toolkit_call_t *legacy = NULL;
    assert(cog_toolkit_call_new(&legacy, "edit_file", "{}", 2U) == COG_OK);
    char output[64] = "preserved";
    cog_toolkit_result_t result = { .output = output };
    assert(cog_toolkit_execute(toolkit_test_core, &cog_workspace_tools, legacy, &result,
        sizeof(output)) == COG_ERROR_UNSUPPORTED);
    cog_toolkit_call_t *calls[] = {legacy};
    cog_toolkit_result_collection_t *collection = NULL;
    assert(cog_toolkit_execute_all(toolkit_test_core, &cog_workspace_tools, calls, 1U, NULL,
        &collection) == COG_ERROR_UNSUPPORTED && collection == NULL);
    cog_toolkit_call_free(legacy);
    cog_toolkit_execution_t *prepared = execution_new(workspace, guard, COG_OK);
    assert(cog_store_owner_release(guard) == COG_ERROR_INVALID_STATE);
    const cog_message_tool_call_t call = { .id = "fenced", .name = "edit_file", .arguments = "{}", .arguments_len = 2U };
    const cog_message_t assistant = { .role = COG_MESSAGE_ROLE_ASSISTANT, .tool_calls = &call, .tool_call_count = 1U };
    cog_session_node_t *node = NULL;
    assert(cog_session_append(toolkit_test_session, &assistant, &node) == COG_OK);
    assert(cog_session_tool_transition(toolkit_test_session, node, "fenced",
        COG_TOOL_EXECUTION_PROPOSED, COG_TOOL_EXECUTION_STARTED) == COG_OK);
    assert(cog_store_save(store, toolkit_test_session) == COG_OK);
    /* Admission must revalidate current state, even with a still-live guard and
       an execution constructor that succeeded before uncertainty arose. */
    assert(cog_session_tool_transition(toolkit_test_session, node, "fenced",
        COG_TOOL_EXECUTION_STARTED, COG_TOOL_EXECUTION_INDETERMINATE) == COG_OK);
    assert(cog_toolkit_execution_start(prepared, COG_TOOLKIT_POLICY_ALLOW, 0U) == COG_ERROR_INVALID_STATE);
    (void)execution_new(workspace, guard, COG_ERROR_INVALID_STATE);
    assert(cog_toolkit_execution_free(prepared) == COG_OK);
    assert(cog_store_owner_release(guard) == COG_OK);
    assert(cog_workspace_free(workspace) == COG_OK);
    toolkit_core_destroy();
    cog_store_free(store);
    file = fopen(path, "r");
    assert(file != NULL && fgets(output, sizeof(output), file) != NULL && fclose(file) == 0);
    assert(strcmp(output, "before") == 0);
    assert(nftw(root, cleanup, 16, FTW_DEPTH | FTW_PHYS) == 0);
    puts("test_toolkit_fencing: ok");
    return 0;
}

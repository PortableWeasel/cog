/** @file test_session_codec.c @brief Explicit persistence wire boundary tests. */
#include "session_store_codec.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(expr) do { if (!(expr)) { \
    (void)fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); abort(); \
    } } while (0)

static void test_v2(void)
{
    cog_session_t *s = NULL, *loaded = NULL;
    const cog_session_config_t cfg = {.id = "codec", .max_nodes = 8U,
        .max_payload_bytes = 16384U, .max_tool_calls_per_message = 2U};
    CHECK(cog_session_new(&s, &cfg) == COG_OK);
    const unsigned char opaque_data[] = {0U, 255U, 3U, 0U};
    const cog_message_opaque_t opaque = {"provider", "version1", opaque_data, sizeof(opaque_data)};
    const cog_message_tool_call_t calls[] = {{"call", "write", "{}", 2U}};
    const cog_message_t assistant = {.role = COG_MESSAGE_ROLE_ASSISTANT, .content = "display", .content_len = 7U,
        .tool_calls = calls, .tool_call_count = 1U, .opaque = &opaque, .opaque_count = 1U,
        .usage = {.tokens_known = true, .input_tokens = 123U, .output_tokens = 31U,
            .cost_known = true, .cost = 0.25, .currency = "USD"}};
    cog_session_node_t *a;
    CHECK(cog_session_append(s, &assistant, &a) == COG_OK);
    CHECK(cog_session_tool_transition(s, a, "call", COG_TOOL_EXECUTION_PROPOSED, COG_TOOL_EXECUTION_STARTED) == COG_OK);
    cog_session_snapshot_t snapshot;
    CHECK(cog_session_snapshot_view(s, &snapshot) == COG_OK);
    unsigned char data[16384], scratch[16384], second[16384];
    size_t size = 0U, size2 = 0U;
    CHECK(cog_store_codec_encode(&snapshot, data, sizeof(data), &size) == COG_OK);
    CHECK(memcmp(data, "COGSNAP2", 8U) == 0);
    cog_store_codec_workspace_t workspace = {scratch, sizeof(scratch), 0U};
    cog_session_snapshot_t decoded = {0};
    CHECK(cog_store_codec_decode(data, size, &workspace, &decoded) == COG_OK);
    CHECK(decoded.ledger[0].state == COG_TOOL_EXECUTION_STARTED);
    CHECK(decoded.audit_high_water == 1U && decoded.ledger[0].audit[0].sequence == 1U);
    CHECK(decoded.nodes[0].message->usage.tokens_known && decoded.nodes[0].message->usage.input_tokens == 123U);
    CHECK(decoded.nodes[0].message->usage.cost == 0.25);
    CHECK(memcmp(decoded.nodes[0].message->opaque[0].data, opaque_data, sizeof(opaque_data)) == 0);
    CHECK(cog_store_codec_encode(&decoded, second, sizeof(second), &size2) == COG_OK);
    CHECK(size == size2 && memcmp(data, second, size) == 0);
    CHECK(cog_session_restore(&loaded, &cfg, &decoded) == COG_OK);
    cog_tool_execution_state_t state;
    CHECK(cog_session_tool_state(loaded, cog_session_root(loaded), "call", &state) == COG_OK);
    CHECK(state == COG_TOOL_EXECUTION_INDETERMINATE);
    cog_session_free(loaded);
    for (size_t k = 0U; k < size; ++k) {
        workspace.used = 0U;
        decoded.node_count = 999U;
        CHECK(cog_store_codec_decode(data, k, &workspace, &decoded) != COG_OK);
        CHECK(decoded.node_count == 999U);
    }
    workspace = (cog_store_codec_workspace_t){scratch, 1U, 0U};
    CHECK(cog_store_codec_decode(data, size, &workspace, &decoded) == COG_ERROR_LIMIT);
    CHECK(decoded.node_count == 999U);
    size2 = 888U;
    CHECK(cog_store_codec_encode(&snapshot, second, size - 1U, &size2) == COG_ERROR_LIMIT && size2 == 888U);
    data[7] = '9'; workspace = (cog_store_codec_workspace_t){scratch, sizeof(scratch), 0U};
    CHECK(cog_store_codec_decode(data, size, &workspace, &decoded) == COG_ERROR_UNSUPPORTED);
    data[7] = '2'; data[size] = 0U;
    CHECK(cog_store_codec_decode(data, size + 1U, &workspace, &decoded) == COG_ERROR_PROTOCOL);
    cog_session_free(s);
}

static const char metadata[] = "{\"version\":1,\"id\":\"bGVnYWN5\",\"parent_id\":null,\"max_nodes\":4,\"node_count\":1,\"next_node_id\":11,\"active_node_id\":10}";
static const char record[] = "{\"node_id\":10,\"parent_id\":0,\"role\":1,\"content\":\"aGVsbG8=\",\"tool_name\":null,\"tool_args\":null,\"tool_result\":null,\"raw_request\":null,\"raw_response\":null,\"chunk_count\":0,\"timing\":{\"request_sent_ms\":0,\"first_token_ms\":0,\"last_token_ms\":0,\"duration_ms\":0,\"ttft_ms\":0,\"tokens_per_second\":0},\"cost\":{\"input_tokens\":0,\"output_tokens\":0,\"cost_per_input_token\":0,\"cost_per_output_token\":0,\"total_cost\":0},\"model\":null,\"provider\":null}\n";
static void test_v1(void)
{
    unsigned char scratch[16384];
    cog_store_codec_workspace_t workspace = {scratch, sizeof(scratch), 0U};
    cog_session_snapshot_t decoded = {0};
    cog_store_load_report_t report = {0};
    CHECK(cog_store_codec_decode_v1("legacy", (const unsigned char *)metadata, strlen(metadata),
        (const unsigned char *)record, strlen(record), COG_STORE_LOAD_REPLAY, false, &workspace, &decoded, &report) == COG_OK);
    CHECK(decoded.nodes[0].node_id == 10U && decoded.node_id_high_water == 10U);
    CHECK(decoded.nodes[0].content_len == 5U && strcmp(decoded.nodes[0].content, "hello") == 0);
    CHECK(report.source_version == 1U && report.migrated_in_memory && report.replay_safe);
    cog_session_t *loaded = NULL;
    CHECK(cog_session_restore(&loaded, NULL, &decoded) == COG_OK);
    cog_message_t history[1]; size_t count;
    CHECK(cog_session_copy_history(loaded, NULL, history, 1U, &count) == COG_OK && count == 1U);
    cog_session_free(loaded);
    char tail[2048];
    (void)snprintf(tail, sizeof(tail), "%s{\"node_id\":", record);
    workspace.used = 0U;
    CHECK(cog_store_codec_decode_v1("legacy", (const unsigned char *)metadata, strlen(metadata),
        (const unsigned char *)tail, strlen(tail), COG_STORE_LOAD_REPLAY, false, &workspace, &decoded, &report) == COG_ERROR_IO);
    workspace.used = 0U;
    CHECK(cog_store_codec_decode_v1("legacy", (const unsigned char *)metadata, strlen(metadata),
        (const unsigned char *)tail, strlen(tail), COG_STORE_LOAD_REPLAY, true, &workspace, &decoded, &report) == COG_OK);
    CHECK(report.recovered_truncated_tail && report.discarded_tail_bytes == strlen(tail) - strlen(record));
    workspace.used = 0U;
    CHECK(cog_store_codec_decode_v1("legacy", (const unsigned char *)metadata, strlen(metadata),
        (const unsigned char *)record, strlen(record) - 1U, COG_STORE_LOAD_REPLAY, true, &workspace, &decoded, &report) == COG_ERROR_IO);
    (void)snprintf(tail, sizeof(tail), "%s{}\n", record);
    workspace.used = 0U;
    CHECK(cog_store_codec_decode_v1("legacy", (const unsigned char *)metadata, strlen(metadata),
        (const unsigned char *)tail, strlen(tail), COG_STORE_LOAD_REPLAY, true, &workspace, &decoded, &report) == COG_ERROR_IO);
    (void)snprintf(tail, sizeof(tail), "%s", record);
    char *role = strstr(tail, "\"role\":1"); CHECK(role != NULL); role[7] = '3';
    workspace.used = 0U;
    CHECK(cog_store_codec_decode_v1("legacy", (const unsigned char *)metadata, strlen(metadata),
        (const unsigned char *)tail, strlen(tail), COG_STORE_LOAD_REPLAY, false, &workspace, &decoded, &report) == COG_ERROR_UNSUPPORTED);
    workspace.used = 0U;
    CHECK(cog_store_codec_decode_v1("legacy", (const unsigned char *)metadata, strlen(metadata),
        (const unsigned char *)tail, strlen(tail), COG_STORE_LOAD_INSPECT, false, &workspace, &decoded, &report) == COG_OK);
    CHECK(!report.replay_safe && decoded.inspection_only);
    CHECK(cog_session_restore(&loaded, NULL, &decoded) == COG_OK);
    CHECK(cog_session_copy_history(loaded, NULL, history, 1U, &count) == COG_ERROR_INVALID_STATE);
    cog_session_free(loaded);
    char meta[512]; (void)snprintf(meta, sizeof(meta), "%s", metadata);
    char *version = strstr(meta, "\"version\":1"); CHECK(version != NULL); version[10] = '9';
    workspace.used = 0U;
    CHECK(cog_store_codec_decode_v1("legacy", (const unsigned char *)meta, strlen(meta),
        (const unsigned char *)record, strlen(record), COG_STORE_LOAD_REPLAY, false, &workspace, &decoded, &report) == COG_ERROR_UNSUPPORTED);
}
int main(void)
{
    test_v2(); test_v1(); puts("bounded session wire codec contracts passed"); return 0;
}

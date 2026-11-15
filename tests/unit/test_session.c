/** @file test_session.c
 * @brief Bounded structured sessions, identities, recovery and branch contracts.
 */
#include "cog/session.h"
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define CHECK(expr) do { if (!(expr)) { \
    (void)fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); \
    abort(); } } while (0)


static cog_message_t text(cog_message_role_t role, const char *content)
{
    cog_message_t m = {0};
    m.role = role; m.content = content; m.content_len = strlen(content);
    return m;
}
static cog_session_t *session(size_t nodes, size_t payload)
{
    cog_session_t *s = NULL;
    const cog_session_config_t cfg = {.id = "test", .max_nodes = nodes,
        .max_payload_bytes = payload, .max_tool_calls_per_message = 2U};
    CHECK(cog_session_new(&s, &cfg) == COG_OK);
    return s;
}
static cog_session_snapshot_t snapshot(const cog_session_t *s)
{
    cog_session_snapshot_t snap = {0};
    CHECK(cog_session_snapshot_view(s, &snap) == COG_OK);
    return snap;
}
static void state(const cog_session_t *s, const cog_session_node_t *n,
    const char *id, cog_tool_execution_state_t expected)
{
    cog_tool_execution_state_t actual;
    CHECK(cog_session_tool_state(s, n, id, &actual) == COG_OK);
    CHECK(actual == expected);
}
static cog_session_node_t *assistant(cog_session_t *s, const char *id)
{
    cog_session_node_t *node = NULL;
    const cog_message_tool_call_t call = {id, "write", "{\"x\":1}", 7U};
    const cog_message_t m = {.role = COG_MESSAGE_ROLE_ASSISTANT,
        .tool_calls = &call, .tool_call_count = 1U};
    CHECK(cog_session_append(s, &m, &node) == COG_OK);
    return node;
}
static cog_message_t result(const char *id)
{
    cog_message_t m = text(COG_MESSAGE_ROLE_TOOL, "verified outcome; no retry");
    m.tool_call_id = id;
    return m;
}

static void test_capacity_identity(void)
{
    cog_session_t *s = session(2U, 1024U);
    cog_message_t m = text(COG_MESSAGE_ROLE_USER, "first");
    cog_session_node_t *a, *b;
    CHECK(cog_session_append(s, &m, &a) == COG_OK);
    CHECK(cog_session_append(s, &m, &b) == COG_OK);
    const uint64_t last = cog_node_data(b)->node_id;
    CHECK(cog_session_append(s, &m, NULL) == COG_ERROR_LIMIT);
    CHECK(cog_session_active(s) == b);
    CHECK(snapshot(s).node_count == 2U);
    CHECK(cog_session_discard_node(s, a) == COG_ERROR_INVALID_STATE);
    CHECK(cog_session_discard_node(s, b) == COG_OK);
    CHECK(cog_session_append(s, &m, &b) == COG_OK);
    CHECK(cog_node_data(b)->node_id > last);
    CHECK(cog_session_reset(s) == COG_OK);
    CHECK(cog_session_is_empty(s));
    CHECK(snapshot(s).node_id_high_water > last);
    CHECK(cog_session_append(s, &m, &a) == COG_OK);
    CHECK(cog_node_data(a)->node_id > last + 1U);
    cog_session_free(s);

    s = session(8U, 256U);
    CHECK(cog_session_append(s, &m, &a) == COG_OK);
    char big[1024]; (void)memset(big, 'x', sizeof(big));
    m.content = big; m.content_len = sizeof(big);
    cog_session_snapshot_t before = snapshot(s);
    CHECK(cog_session_append(s, &m, NULL) == COG_ERROR_LIMIT);
    cog_session_snapshot_t after = snapshot(s);
    CHECK(before.node_count == after.node_count && before.node_id_high_water == after.node_id_high_water);
    CHECK(cog_session_discard_node(s, a) == COG_OK);
    m = text(COG_MESSAGE_ROLE_USER, "reclaimed");
    for (size_t i = 0U; i < 20U; ++i) {
        CHECK(cog_session_append(s, &m, &a) == COG_OK);
        CHECK(cog_session_discard_node(s, a) == COG_OK);
    }
    cog_session_free(s);
}

static void test_deep_copy(void)
{
    cog_session_t *s = session(8U, 8192U);
    char id[] = "identity", name[] = "write", args[] = "{}", visible[] = "hello";
    char namespace_[] = "provider", format[] = "json-v1", currency[] = "USD";
    unsigned char bytes[] = {0U, 255U, 17U};
    cog_message_tool_call_t call = {id, name, args, 2U};
    cog_message_opaque_t opaque = {namespace_, format, bytes, sizeof(bytes)};
    cog_message_t m = {.role = COG_MESSAGE_ROLE_ASSISTANT, .content = visible, .content_len = 5U,
        .tool_calls = &call, .tool_call_count = 1U, .opaque = &opaque, .opaque_count = 1U,
        .usage = {.tokens_known = true, .input_tokens = 7U, .output_tokens = 3U,
            .reasoning_tokens_known = true, .reasoning_tokens = 2U, .cost_known = true,
            .cost = 0.5, .currency = currency}};
    cog_session_node_t *a;
    CHECK(cog_session_append(s, &m, &a) == COG_OK);
    id[0] = 'X'; name[0] = 'X'; args[0] = 'X'; visible[0] = 'X';
    namespace_[0] = 'X'; format[0] = 'X'; currency[0] = 'X'; bytes[0] = 42U;
    const cog_message_t *owned = cog_node_data(a)->message;
    CHECK(strcmp(owned->tool_calls[0].id, "identity") == 0);
    CHECK(strcmp(owned->tool_calls[0].name, "write") == 0);
    CHECK(strcmp(owned->tool_calls[0].arguments, "{}") == 0);
    CHECK(memcmp(owned->content, "hello", 5U) == 0);
    CHECK(strcmp(owned->opaque[0].namespace_id, "provider") == 0);
    CHECK(strcmp(owned->opaque[0].format, "json-v1") == 0);
    CHECK(owned->opaque[0].data[0] == 0U && owned->opaque[0].data[1] == 255U);
    CHECK(strcmp(owned->usage.currency, "USD") == 0 && owned->usage.reasoning_tokens == 2U);
    CHECK(cog_session_reset(s) == COG_ERROR_INVALID_STATE);
    CHECK(cog_session_discard_node(s, a) == COG_ERROR_INVALID_STATE);
    cog_message_t r = result("identity");
    CHECK(cog_session_append(s, &r, NULL) == COG_ERROR_INVALID_STATE);
    CHECK(cog_session_tool_transition(s, a, "identity", COG_TOOL_EXECUTION_PROPOSED, COG_TOOL_EXECUTION_DENIED) == COG_OK);
    CHECK(cog_session_append(s, &r, NULL) == COG_OK);
    cog_message_t history[2]; size_t count;
    CHECK(cog_session_copy_history(s, NULL, history, 2U, &count) == COG_OK && count == 2U);
    history[0].content = "sentinel";
    CHECK(cog_session_copy_history(s, NULL, history, 1U, &count) == COG_ERROR_OVERFLOW && count == 2U);
    CHECK(strcmp(history[0].content, "sentinel") == 0);
    cog_session_free(s);
}

static void test_crash_reconciliation(void)
{
    cog_session_t *s = session(1U, 4096U);
    cog_session_node_t *a = assistant(s, "call");
    cog_message_t r = result("call");
    CHECK(cog_session_tool_transition(s, a, "call", COG_TOOL_EXECUTION_PROPOSED, COG_TOOL_EXECUTION_STARTED) == COG_OK);
    cog_session_snapshot_t snap = snapshot(s);
    CHECK(snap.ledger_count == 1U && snap.ledger[0].audit_count == 1U);
    cog_session_config_t small = {.max_nodes = 1U, .max_payload_bytes = 8192U};
    cog_session_t *restored = NULL;
    CHECK(cog_session_restore(&restored, &small, &snap) == COG_OK);
    cog_session_node_t *ra = cog_session_root(restored);
    state(restored, ra, "call", COG_TOOL_EXECUTION_INDETERMINATE);
    cog_session_snapshot_t rs = snapshot(restored);
    CHECK(rs.ledger[0].audit_count == 2U && rs.audit_high_water == 2U);
    CHECK(rs.ledger[0].audit[0].next == COG_TOOL_EXECUTION_STARTED);
    CHECK(rs.ledger[0].audit[1].previous == COG_TOOL_EXECUTION_STARTED);
    CHECK(cog_session_reconcile_tool(restored, ra, "call", COG_TOOL_EXECUTION_ACKNOWLEDGED, &r, NULL) == COG_ERROR_LIMIT);
    CHECK(snapshot(restored).audit_high_water == 2U && snapshot(restored).node_count == 1U);
    state(restored, ra, "call", COG_TOOL_EXECUTION_INDETERMINATE);
    cog_session_config_t large = {.max_nodes = 4U, .max_payload_bytes = 8192U};
    cog_session_t *larger = NULL;
    CHECK(cog_session_restore(&larger, &large, &rs) == COG_OK);
    ra = cog_session_root(larger);
    CHECK(snapshot(larger).audit_high_water == 2U);
    CHECK(cog_session_append(larger, &r, NULL) == COG_ERROR_INVALID_STATE);
    CHECK(cog_session_tool_transition(larger, ra, "call", COG_TOOL_EXECUTION_INDETERMINATE, COG_TOOL_EXECUTION_STARTED) == COG_ERROR_INVALID_STATE);
    CHECK(cog_session_reconcile_tool(larger, ra, "call", COG_TOOL_EXECUTION_ACKNOWLEDGED, &r, NULL) == COG_OK);
    cog_session_snapshot_t closed = snapshot(larger);
    CHECK(closed.ledger[0].audit_count == 3U && closed.audit_high_water == 3U);
    const cog_tool_audit_record_t *audit = &closed.ledger[0].audit[2];
    CHECK(audit->explicit_reconciliation && audit->evidence_len == r.content_len);
    CHECK(memcmp(audit->evidence, r.content, r.content_len) == 0);
    cog_session_t *roundtrip = NULL;
    CHECK(cog_session_restore(&roundtrip, &large, &closed) == COG_OK);
    cog_session_free(larger);
    cog_message_t history[4]; size_t count = 0U;
    CHECK(cog_session_copy_history(roundtrip, NULL, history, 4U, &count) == COG_OK && count == 2U);
    CHECK(snapshot(roundtrip).ledger[0].audit[2].explicit_reconciliation);
    CHECK(memcmp(snapshot(roundtrip).ledger[0].audit[2].evidence, r.content, r.content_len) == 0);
    CHECK(cog_session_discard_node(roundtrip, cog_session_active(roundtrip)) == COG_ERROR_INVALID_STATE);
    CHECK(cog_session_reset(roundtrip) == COG_ERROR_INVALID_STATE);
    cog_session_free(roundtrip); cog_session_free(restored); cog_session_free(s);
}

static void test_branch_and_associations(void)
{
    cog_session_t *s = session(16U, 16384U);
    cog_message_t root = text(COG_MESSAGE_ROLE_USER, "root");
    cog_message_t answer = text(COG_MESSAGE_ROLE_ASSISTANT, "answer");
    cog_message_t next = text(COG_MESSAGE_ROLE_USER, "other");
    cog_session_node_t *base, *leaf, *point, *other;
    CHECK(cog_session_append(s, &root, &base) == COG_OK);
    CHECK(cog_session_append(s, &answer, &leaf) == COG_OK);
    CHECK(cog_session_fork(s, base, &point) == COG_OK && point == base);
    CHECK(cog_session_active(s) == leaf);
    cog_session_snapshot_t snap = snapshot(s);
    CHECK(snap.active_node_id == cog_node_data(leaf)->node_id);
    CHECK(cog_session_add_message(s, point, &next, &other) == COG_OK);
    cog_session_node_t *a = assistant(s, "same");
    cog_session_snapshot_t offpath = snapshot(s);
    offpath.active_node_id = cog_node_data(leaf)->node_id;
    cog_session_t *invalid = NULL;
    CHECK(cog_session_restore(&invalid, NULL, &offpath) == COG_ERROR_PROTOCOL && invalid == NULL);
    CHECK(cog_session_fork(s, leaf, NULL) == COG_ERROR_INVALID_STATE);
    CHECK(cog_session_add_message(s, leaf, &next, NULL) == COG_ERROR_INVALID_STATE);
    cog_message_t wrong = result("unknown");
    CHECK(cog_session_append(s, &wrong, NULL) == COG_ERROR_INVALID_ARG);
    cog_message_t r = result("same");
    CHECK(cog_session_tool_transition(s, a, "same", COG_TOOL_EXECUTION_PROPOSED, COG_TOOL_EXECUTION_STARTED) == COG_OK);
    CHECK(cog_session_append(s, &r, NULL) == COG_OK);
    CHECK(cog_session_append(s, &r, NULL) == COG_ERROR_INVALID_ARG);
    CHECK(cog_session_tool_transition(s, a, "same", COG_TOOL_EXECUTION_STARTED, COG_TOOL_EXECUTION_SUCCEEDED) == COG_OK);
    CHECK(cog_session_fork(s, a, &point) == COG_ERROR_INVALID_STATE);
    CHECK(cog_session_fork(s, cog_session_active(s), &point) == COG_OK);
    CHECK(cog_session_tool_transition(s, a, "same", COG_TOOL_EXECUTION_SUCCEEDED, COG_TOOL_EXECUTION_STARTED) == COG_ERROR_INVALID_STATE);
    CHECK(cog_session_fork(s, leaf, &point) == COG_OK && point == leaf);
    CHECK(cog_session_append(s, &next, NULL) == COG_OK);
    cog_session_node_t *a2 = assistant(s, "same");
    state(s, a2, "same", COG_TOOL_EXECUTION_PROPOSED);
    state(s, a, "same", COG_TOOL_EXECUTION_SUCCEEDED);
    CHECK(cog_session_tool_transition(s, a2, "same", COG_TOOL_EXECUTION_PROPOSED, COG_TOOL_EXECUTION_DENIED) == COG_OK);
    CHECK(cog_session_append(s, &r, NULL) == COG_OK);
    cog_message_t history[16]; size_t count;
    CHECK(cog_session_copy_history(s, NULL, history, 16U, &count) == COG_OK && count == 5U);
    CHECK(strcmp(history[1].content, "answer") == 0);
    CHECK(cog_node_parent(other) == base);
    cog_session_free(s);
}

static void test_snapshot_validation(void)
{
    const cog_message_t m = {.role = COG_MESSAGE_ROLE_USER, .content = "x", .content_len = 1U};
    cog_node_data_t nodes[] = {
        {.node_id = 10U, .role = COG_NODE_USER, .content = "x", .content_len = 1U, .message = &m},
        {.node_id = 90U, .parent_id = 10U, .role = COG_NODE_USER, .message = &m},
        {.node_id = 42U, .parent_id = 10U, .role = COG_NODE_USER, .message = &m}};
    cog_session_snapshot_t sn = {.session_id = "sparse", .nodes = nodes, .node_count = 3U,
        .active_node_id = 90U, .node_id_high_water = 101U, .audit_high_water = 70U};
    cog_session_config_t cfg = {.max_nodes = 5U, .max_payload_bytes = 8192U};
    cog_session_t *s = NULL;
    CHECK(cog_session_restore(&s, &cfg, &sn) == COG_OK);
    cog_session_snapshot_t seen = snapshot(s);
    CHECK(seen.nodes[0].node_id == 10U && seen.nodes[1].node_id == 90U && seen.nodes[2].node_id == 42U);
    CHECK(seen.active_node_id == 90U && seen.node_id_high_water == 101U && seen.audit_high_water == 70U);
    cog_session_node_t *newnode;
    CHECK(cog_session_append(s, &m, &newnode) == COG_OK);
    CHECK(cog_node_data(newnode)->node_id == 102U);
    CHECK(cog_session_discard_node(s, cog_node_first_child(cog_session_root(s))) == COG_ERROR_INVALID_STATE);
    cog_session_free(s);
    nodes[1].node_id = 10U;
    CHECK(cog_session_restore(&s, &cfg, &sn) == COG_ERROR_PROTOCOL && s == NULL);
    nodes[1].node_id = 90U;
    sn.active_node_id = 10U;
    CHECK(cog_session_restore(&s, &cfg, &sn) == COG_ERROR_PROTOCOL && s == NULL);
    sn.active_node_id = 90U; sn.node_id_high_water = 89U;
    CHECK(cog_session_restore(&s, &cfg, &sn) == COG_ERROR_PROTOCOL);
    sn.node_id_high_water = UINT64_MAX;
    CHECK(cog_session_restore(&s, &cfg, &sn) == COG_ERROR_LIMIT);
    sn.node_id_high_water = 101U; nodes[1].parent_id = 42U;
    CHECK(cog_session_restore(&s, &cfg, &sn) == COG_ERROR_PROTOCOL);
    nodes[1].parent_id = 10U; cfg.id = "wrong";
    CHECK(cog_session_restore(&s, &cfg, &sn) == COG_ERROR_INVALID_ARG);

    cog_session_t *source = session(4U, 4096U);
    cog_session_node_t *a = assistant(source, "id");
    CHECK(cog_session_tool_transition(source, a, "id", COG_TOOL_EXECUTION_PROPOSED, COG_TOOL_EXECUTION_STARTED) == COG_OK);
    sn = snapshot(source);
    cog_tool_ledger_record_t ledger = sn.ledger[0];
    cog_tool_audit_record_t audit = ledger.audit[0];
    ledger.audit = &audit; sn.ledger = &ledger;
    cfg.id = NULL;
    audit.previous = COG_TOOL_EXECUTION_DENIED;
    CHECK(cog_session_restore(&s, &cfg, &sn) == COG_ERROR_PROTOCOL);
    audit.previous = COG_TOOL_EXECUTION_PROPOSED; audit.sequence = sn.audit_high_water + 1U;
    CHECK(cog_session_restore(&s, &cfg, &sn) == COG_ERROR_PROTOCOL);
    audit.sequence = 1U; ledger.call_id = "other";
    CHECK(cog_session_restore(&s, &cfg, &sn) == COG_ERROR_PROTOCOL);
    cog_session_free(source);

    source = session(4U, 4096U);
    a = assistant(source, "id");
    sn = snapshot(source);
    cog_node_data_t illicit[2] = {sn.nodes[0], {.node_id = 2U, .parent_id = 1U,
        .role = COG_NODE_USER, .content = "x", .content_len = 1U, .message = &m}};
    sn.nodes = illicit; sn.node_count = 2U; sn.active_node_id = 2U; sn.node_id_high_water = 2U;
    CHECK(cog_session_restore(&s, &cfg, &sn) == COG_ERROR_PROTOCOL);
    cog_session_free(source);
}

static void test_legacy_and_incomplete(void)
{
    cog_session_t *s = session(8U, 8192U);
    cog_session_node_t *root, *call;
    cog_message_t m = text(COG_MESSAGE_ROLE_USER, "safe");
    CHECK(cog_session_append(s, &m, &root) == COG_OK);
    CHECK(cog_session_add_tool_call(s, root, "tool", "{}", &call) == COG_OK);
    size_t count;
    CHECK(cog_session_copy_history(s, NULL, NULL, 0U, &count) == COG_ERROR_INVALID_STATE);
    cog_session_snapshot_t sn = snapshot(s);
    cog_session_t *restored;
    CHECK(cog_session_restore(&restored, NULL, &sn) == COG_ERROR_UNSUPPORTED);
    sn.inspection_only = true;
    CHECK(cog_session_restore(&restored, NULL, &sn) == COG_OK);
    CHECK(cog_session_append(restored, &m, NULL) == COG_ERROR_INVALID_STATE);
    cog_session_free(restored); cog_session_free(s);
    s = session(4U, 4096U);
    CHECK(cog_session_append(s, &m, &root) == COG_OK);
    m = text(COG_MESSAGE_ROLE_ASSISTANT, "partial"); m.status = COG_MESSAGE_INCOMPLETE;
    CHECK(cog_session_append(s, &m, &call) == COG_OK);
    CHECK(cog_session_fork(s, call, NULL) == COG_ERROR_INVALID_STATE);
    CHECK(cog_session_fork(s, root, NULL) == COG_OK);
    CHECK(cog_session_active(s) == call);
    m = text(COG_MESSAGE_ROLE_USER, "explicit continuation");
    CHECK(cog_session_add_message(s, root, &m, NULL) == COG_OK);
    cog_session_free(s);
}

static void test_branch_local_result_coverage(void)
{
    cog_session_t *s = session(16U, 16384U);
    cog_session_node_t *a = assistant(s, "call");
    const cog_message_t user = text(COG_MESSAGE_ROLE_USER, "next");
    cog_message_t r = result("call");
    cog_session_node_t *complete;
    CHECK(cog_session_tool_transition(s, a, "call", COG_TOOL_EXECUTION_PROPOSED, COG_TOOL_EXECUTION_STARTED) == COG_OK);
    CHECK(cog_session_append(s, &r, &complete) == COG_OK);
    CHECK(cog_session_tool_transition(s, a, "call", COG_TOOL_EXECUTION_STARTED, COG_TOOL_EXECUTION_SUCCEEDED) == COG_OK);
    cog_session_snapshot_t before = snapshot(s);
    cog_message_t history[16]; size_t count = 123U;
    CHECK(cog_session_copy_history(s, a, history, 16U, &count) == COG_ERROR_INVALID_STATE);
    CHECK(count == 123U);
    CHECK(cog_session_fork(s, a, NULL) == COG_ERROR_INVALID_STATE);
    CHECK(cog_session_add_message(s, a, &user, NULL) == COG_ERROR_INVALID_STATE);
    cog_session_snapshot_t after = snapshot(s);
    CHECK(after.active_node_id == before.active_node_id && after.node_count == before.node_count &&
        after.node_id_high_water == before.node_id_high_water);
    CHECK(cog_session_active(s) == complete);
    cog_node_data_t malformed[3] = {after.nodes[0], after.nodes[1],
        {.node_id = 3U, .parent_id = 1U, .role = COG_NODE_USER, .message = &user,
         .content = user.content, .content_len = user.content_len}};
    cog_session_snapshot_t bad = after;
    bad.nodes = malformed; bad.node_count = 3U; bad.node_id_high_water = 3U; bad.active_node_id = 3U;
    cog_session_t *restored = NULL;
    CHECK(cog_session_restore(&restored, NULL, &bad) == COG_ERROR_PROTOCOL && restored == NULL);
    /* A safe active selection cannot conceal an unsafe retained sibling. */
    bad.active_node_id = after.active_node_id;
    CHECK(cog_session_restore(&restored, NULL, &bad) == COG_ERROR_PROTOCOL && restored == NULL);
    CHECK(cog_session_fork(s, complete, NULL) == COG_OK);
    CHECK(cog_session_append(s, &user, NULL) == COG_OK);
    CHECK(cog_session_copy_history(s, NULL, history, 16U, &count) == COG_OK && count == 3U);
    CHECK(history[1].role == COG_MESSAGE_ROLE_TOOL);
    cog_session_free(s);

    s = session(16U, 16384U);
    const cog_message_tool_call_t calls[] = {{"one", "tool", "{}", 2U}, {"two", "tool", "{}", 2U}};
    const cog_message_t proposal = {.role = COG_MESSAGE_ROLE_ASSISTANT, .tool_calls = calls, .tool_call_count = 2U};
    cog_session_node_t *partial;
    CHECK(cog_session_append(s, &proposal, &a) == COG_OK);
    CHECK(cog_session_tool_transition(s, a, "one", COG_TOOL_EXECUTION_PROPOSED, COG_TOOL_EXECUTION_DENIED) == COG_OK);
    r = result("one");
    CHECK(cog_session_append(s, &r, &partial) == COG_OK);
    /* The one current policy phase may retain a partially resolved turn. */
    after = snapshot(s);
    CHECK(cog_session_restore(&restored, NULL, &after) == COG_OK);
    cog_session_free(restored); restored = NULL;
    CHECK(cog_session_tool_transition(s, a, "two", COG_TOOL_EXECUTION_PROPOSED, COG_TOOL_EXECUTION_DENIED) == COG_OK);
    r = result("two");
    CHECK(cog_session_append(s, &r, &complete) == COG_OK);
    CHECK(cog_session_copy_history(s, partial, history, 16U, &count) == COG_ERROR_INVALID_STATE);
    CHECK(cog_session_fork(s, partial, NULL) == COG_ERROR_INVALID_STATE);
    CHECK(cog_session_add_message(s, partial, &user, NULL) == COG_ERROR_INVALID_STATE);
    after = snapshot(s);
    cog_node_data_t split[4] = {after.nodes[0], after.nodes[1], after.nodes[2],
        {.node_id = 4U, .parent_id = 2U, .role = COG_NODE_USER, .message = &user}};
    bad = after; bad.nodes = split; bad.node_count = 4U; bad.node_id_high_water = 4U; bad.active_node_id = 4U;
    CHECK(cog_session_restore(&restored, NULL, &bad) == COG_ERROR_PROTOCOL && restored == NULL);
    /* Splitting final TOOL messages across siblings is also invalid. */
    split[2].parent_id = 1U; bad.node_count = 3U; bad.active_node_id = 3U;
    CHECK(cog_session_restore(&restored, NULL, &bad) == COG_ERROR_PROTOCOL && restored == NULL);
    CHECK(cog_session_copy_history(s, complete, history, 16U, &count) == COG_OK && count == 3U);
    cog_session_free(s);
}

static void test_divergent_discard_leaf(void)
{
    cog_session_t *s = session(8U, 8192U);
    const cog_message_t user = text(COG_MESSAGE_ROLE_USER, "text");
    cog_session_node_t *root, *first, *draft;
    CHECK(cog_session_append(s, &user, &root) == COG_OK);
    CHECK(cog_session_append(s, &user, &first) == COG_OK);
    CHECK(cog_session_fork(s, root, NULL) == COG_OK);
    CHECK(cog_session_add_message(s, root, &user, &draft) == COG_OK);
    cog_session_snapshot_t before = snapshot(s);
    CHECK(cog_session_discard_node(s, draft) == COG_ERROR_INVALID_STATE);
    cog_session_snapshot_t after = snapshot(s);
    CHECK(after.active_node_id == before.active_node_id && after.node_count == before.node_count &&
        after.node_id_high_water == before.node_id_high_water);
    CHECK(cog_session_active(s) == draft && cog_node_child_count(root) == 2U);
    CHECK(cog_node_first_child(root) == first && cog_node_next_sibling(first) == draft);
    cog_session_t *restored = NULL;
    CHECK(cog_session_restore(&restored, NULL, &after) == COG_OK);
    cog_session_free(restored);
    /* Removing an unrelated safe draft leaves the selected leaf intact. */
    CHECK(cog_session_discard_node(s, first) == COG_OK);
    CHECK(cog_session_active(s) == draft);
    CHECK(cog_session_discard_node(s, draft) == COG_OK);
    CHECK(cog_session_active(s) == root && cog_node_child_count(root) == 0U);
    after = snapshot(s);
    CHECK(after.node_id_high_water == before.node_id_high_water);
    CHECK(cog_session_restore(&restored, NULL, &after) == COG_OK);
    cog_session_free(restored); cog_session_free(s);
}

int main(void)
{
    test_capacity_identity(); test_deep_copy(); test_crash_reconciliation();
    test_branch_and_associations(); test_snapshot_validation(); test_legacy_and_incomplete();
    test_branch_local_result_coverage(); test_divergent_discard_leaf();
    puts("bounded structured session contracts passed");
    return 0;
}

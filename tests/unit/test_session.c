/**
 * @file test_session.c
 * @brief Contract test for cog/session.h (Stage 12.1a, ticket 0f055f5).
 *
 * Exercises the full public surface of the conversation tree:
 *
 *   - lifecycle: new/free/id/reset/is_empty, NULL contracts
 *   - tree navigation: root/active/parent/first_child/next_sibling/child_count
 *   - mutation: add_message/append/add_tool_call/add_tool_result/fork
 *   - history building: root-to-node message arrays, tool mapping
 *   - timing/billing: set_timing/set_cost, cumulative sums
 *   - parent session metadata: parent_id get/set
 *   - cancellation: discard_node leaf/non-leaf/active/foreign/root
 *   - capacity: max_nodes pruning (oldest leaves, root/active preserved)
 *   - stable ids and node data views
 *
 * Every documented error code is exercised where it is reachable through
 * the public API.  COG_ERROR_NOMEM is not triggered (allocation failure
 * is not injectable through this API); COG_ERROR_INVALID_STATE from
 * cog_session_reset is reserved for the core integration layer, which
 * owns the session->attached flag (see CONTRACT.md).
 *
 * The header declares no persistence functions: save/load/serialize live
 * in cog/session_store.h (Stage 12.2d), so no round-trip tests apply
 * here.
 */

#include "cog/session.h"

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#ifndef COG_SESSION_H
#error "COG_SESSION_H include guard missing"
#endif

/* ------------------------------------------------------------------ */
/* Compile-time checks                                                 */
/* ------------------------------------------------------------------ */

_Static_assert(COG_NODE_SYSTEM == 0, "COG_NODE_SYSTEM must be exactly 0");
_Static_assert(COG_NODE_USER == 1, "COG_NODE_USER must be exactly 1");
_Static_assert(COG_NODE_ASSISTANT == 2,
               "COG_NODE_ASSISTANT must be exactly 2");
_Static_assert(COG_NODE_TOOL_CALL == 3,
               "COG_NODE_TOOL_CALL must be exactly 3");
_Static_assert(COG_NODE_TOOL_RESULT == 4,
               "COG_NODE_TOOL_RESULT must be exactly 4");

/* Field types, verified via type-generic selection (unevaluated). */
#define FIELD_TYPE_ASSERT(stype, field, type)                              \
    _Static_assert(                                                        \
        _Generic(((stype){0}).field, type : 1, default : 0),               \
        #stype "." #field " must have type " #type)

FIELD_TYPE_ASSERT(cog_timing_t, request_sent_ms, uint64_t);
FIELD_TYPE_ASSERT(cog_timing_t, first_token_ms, uint64_t);
FIELD_TYPE_ASSERT(cog_timing_t, last_token_ms, uint64_t);
FIELD_TYPE_ASSERT(cog_timing_t, duration_ms, uint64_t);
FIELD_TYPE_ASSERT(cog_timing_t, ttft_ms, uint64_t);
FIELD_TYPE_ASSERT(cog_timing_t, tokens_per_second, double);

FIELD_TYPE_ASSERT(cog_cost_t, input_tokens, uint64_t);
FIELD_TYPE_ASSERT(cog_cost_t, output_tokens, uint64_t);
FIELD_TYPE_ASSERT(cog_cost_t, cost_per_input_token, double);
FIELD_TYPE_ASSERT(cog_cost_t, cost_per_output_token, double);
FIELD_TYPE_ASSERT(cog_cost_t, total_cost, double);

FIELD_TYPE_ASSERT(cog_node_data_t, node_id, uint64_t);
FIELD_TYPE_ASSERT(cog_node_data_t, parent_id, uint64_t);
FIELD_TYPE_ASSERT(cog_node_data_t, role, cog_node_role_t);
FIELD_TYPE_ASSERT(cog_node_data_t, content, const char *);
FIELD_TYPE_ASSERT(cog_node_data_t, content_len, size_t);
FIELD_TYPE_ASSERT(cog_node_data_t, tool_name, const char *);
FIELD_TYPE_ASSERT(cog_node_data_t, tool_args, const char *);
FIELD_TYPE_ASSERT(cog_node_data_t, tool_result, const char *);
FIELD_TYPE_ASSERT(cog_node_data_t, raw_request, const char *);
FIELD_TYPE_ASSERT(cog_node_data_t, raw_response, const char *);
FIELD_TYPE_ASSERT(cog_node_data_t, chunk_count, size_t);
FIELD_TYPE_ASSERT(cog_node_data_t, model, const char *);
FIELD_TYPE_ASSERT(cog_node_data_t, provider, const char *);

FIELD_TYPE_ASSERT(cog_session_config_t, id, const char *);
FIELD_TYPE_ASSERT(cog_session_config_t, max_nodes, size_t);

#undef FIELD_TYPE_ASSERT

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static int approx_eq(double a, double b)
{
    return fabs(a - b) < 1e-9;
}

/* Message builders: compound literals, so MSG_USER("x") is a valid
 * pointer to an automatic-storage message (addressable rvalue). */
#define MSG_SYSTEM(t)                                                       \
    (&(cog_message_t){COG_MESSAGE_ROLE_SYSTEM, (t),                         \
                      (t) != NULL ? strlen(t) : 0U, NULL})
#define MSG_USER(t)                                                         \
    (&(cog_message_t){COG_MESSAGE_ROLE_USER, (t),                           \
                      (t) != NULL ? strlen(t) : 0U, NULL})
#define MSG_ASSISTANT(t)                                                    \
    (&(cog_message_t){COG_MESSAGE_ROLE_ASSISTANT, (t),                      \
                      (t) != NULL ? strlen(t) : 0U, NULL})

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

static int test_new_defaults(void)
{
    cog_session_t *s = NULL;
    cog_result_t rc = cog_session_new(&s, NULL);

    if (rc != COG_OK || s == NULL) {
        return 1;
    }
    if (!cog_session_is_empty(s)) {
        return 1;
    }
    if (cog_session_id(s) == NULL || cog_session_id(s)[0] == '\0') {
        return 1;
    }
    if (cog_session_root(s) != NULL || cog_session_active(s) != NULL) {
        return 1;
    }
    if (cog_session_parent_id(s) != NULL) {
        return 1;
    }
    cog_session_free(s);
    return 0;
}

static int test_new_with_id(void)
{
    cog_session_t *s = NULL;
    cog_session_config_t cfg = {.id = "sess-test-42", .max_nodes = 0U};
    cog_result_t rc = cog_session_new(&s, &cfg);

    if (rc != COG_OK || s == NULL) {
        return 1;
    }
    if (cog_session_id(s) == NULL ||
        strcmp(cog_session_id(s), "sess-test-42") != 0) {
        return 1;
    }
    cog_session_free(s);
    return 0;
}

static int test_new_null_out(void)
{
    if (cog_session_new(NULL, NULL) != COG_ERROR_NULL_PTR) {
        return 1;
    }
    return 0;
}

static int test_free_null(void)
{
    cog_session_free(NULL); /* must be a no-op */
    return 0;
}

static int test_id_null_session(void)
{
    if (cog_session_id(NULL) != NULL) {
        return 1;
    }
    if (!cog_session_is_empty(NULL)) {
        return 1;
    }
    if (cog_session_parent_id(NULL) != NULL) {
        return 1;
    }
    return 0;
}

static int test_reset(void)
{
    cog_session_t *s = NULL;
    cog_session_node_t *n1 = NULL;
    cog_session_node_t *n2 = NULL;
    cog_session_config_t cfg = {.id = "sess-reset", .max_nodes = 0U};
    const char *id_before;

    if (cog_session_new(&s, &cfg) != COG_OK) {
        return 1;
    }
    id_before = cog_session_id(s);
    if (cog_session_add_message(s, NULL, MSG_USER("hi"), &n1) != COG_OK) {
        return 1;
    }
    if (cog_session_append(s, MSG_ASSISTANT("yo"), &n2) != COG_OK) {
        return 1;
    }
    if (cog_session_set_parent(s, "sess-parent") != COG_OK) {
        return 1;
    }
    if (cog_session_reset(s) != COG_OK) {
        return 1;
    }
    if (!cog_session_is_empty(s)) {
        return 1;
    }
    if (cog_session_root(s) != NULL || cog_session_active(s) != NULL) {
        return 1;
    }
    /* The session retains its id; parent metadata is retained too. */
    if (cog_session_id(s) != id_before ||
        strcmp(cog_session_id(s), "sess-reset") != 0) {
        return 1;
    }
    if (cog_session_parent_id(s) == NULL ||
        strcmp(cog_session_parent_id(s), "sess-parent") != 0) {
        return 1;
    }
    /* A fresh add after reset works and restarts node ids at 1. */
    if (cog_session_add_message(s, NULL, MSG_USER("again"), &n1) != COG_OK) {
        return 1;
    }
    if (cog_node_data(n1)->node_id != 1U) {
        return 1;
    }
    cog_session_free(s);
    return 0;
}

static int test_reset_null(void)
{
    if (cog_session_reset(NULL) != COG_ERROR_NULL_PTR) {
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Add message                                                         */
/* ------------------------------------------------------------------ */

static int test_add_message_root(void)
{
    cog_session_t *s = NULL;
    cog_session_node_t *n = NULL;
    const cog_node_data_t *d;

    if (cog_session_new(&s, NULL) != COG_OK) {
        return 1;
    }
    if (cog_session_add_message(s, NULL, MSG_USER("hello"), &n) != COG_OK) {
        return 1;
    }
    if (n == NULL || cog_session_root(s) != n || cog_session_active(s) != n) {
        return 1;
    }
    if (cog_session_is_empty(s)) {
        return 1;
    }
    d = cog_node_data(n);
    if (d == NULL || d->role != COG_NODE_USER || d->node_id != 1U ||
        d->parent_id != 0U) {
        return 1;
    }
    if (d->content == NULL || strcmp(d->content, "hello") != 0 ||
        d->content_len != 5U) {
        return 1;
    }
    if (cog_node_parent(n) != NULL || cog_node_child_count(n) != 0U) {
        return 1;
    }
    cog_session_free(s);
    return 0;
}

static int test_add_message_child(void)
{
    cog_session_t *s = NULL;
    cog_session_node_t *root = NULL;
    cog_session_node_t *child = NULL;
    const cog_node_data_t *d;

    if (cog_session_new(&s, NULL) != COG_OK) {
        return 1;
    }
    if (cog_session_add_message(s, NULL, MSG_USER("q"), &root) != COG_OK) {
        return 1;
    }
    if (cog_session_add_message(s, root, MSG_ASSISTANT("a"), &child) !=
        COG_OK) {
        return 1;
    }
    d = cog_node_data(child);
    if (d == NULL || d->parent_id != cog_node_data(root)->node_id) {
        return 1;
    }
    if (cog_node_parent(child) != root) {
        return 1;
    }
    if (cog_node_first_child(root) != child) {
        return 1;
    }
    if (cog_node_child_count(root) != 1U) {
        return 1;
    }
    if (cog_session_active(s) != child) {
        return 1;
    }
    cog_session_free(s);
    return 0;
}

static int test_add_message_siblings(void)
{
    cog_session_t *s = NULL;
    cog_session_node_t *root = NULL;
    cog_session_node_t *a = NULL;
    cog_session_node_t *b = NULL;

    if (cog_session_new(&s, NULL) != COG_OK) {
        return 1;
    }
    if (cog_session_add_message(s, NULL, MSG_USER("root"), &root) != COG_OK) {
        return 1;
    }
    if (cog_session_add_message(s, root, MSG_USER("a"), &a) != COG_OK) {
        return 1;
    }
    if (cog_session_add_message(s, root, MSG_USER("b"), &b) != COG_OK) {
        return 1;
    }
    if (cog_node_child_count(root) != 2U) {
        return 1;
    }
    if (cog_node_first_child(root) != a || cog_node_next_sibling(a) != b ||
        cog_node_next_sibling(b) != NULL) {
        return 1;
    }
    if (cog_node_parent(a) != root || cog_node_parent(b) != root) {
        return 1;
    }
    cog_session_free(s);
    return 0;
}

static int test_add_message_nulls(void)
{
    cog_session_t *s = NULL;
    cog_session_node_t *n = NULL;

    if (cog_session_new(&s, NULL) != COG_OK) {
        return 1;
    }
    if (cog_session_add_message(NULL, NULL, MSG_USER("x"), &n) !=
        COG_ERROR_NULL_PTR) {
        return 1;
    }
    if (cog_session_add_message(s, NULL, NULL, &n) != COG_ERROR_NULL_PTR) {
        return 1;
    }
    if (cog_session_add_message(NULL, NULL, NULL, NULL) !=
        COG_ERROR_NULL_PTR) {
        return 1;
    }
    cog_session_free(s);
    return 0;
}

static int test_add_message_out_null(void)
{
    cog_session_t *s = NULL;

    if (cog_session_new(&s, NULL) != COG_OK) {
        return 1;
    }
    if (cog_session_add_message(s, NULL, MSG_USER("x"), NULL) != COG_OK) {
        return 1;
    }
    if (cog_session_is_empty(s)) {
        return 1;
    }
    cog_session_free(s);
    return 0;
}

static int test_add_message_embedded_nul(void)
{
    static const char payload[] = {'a', '\0', '\0', 'b'};
    cog_session_t *s = NULL;
    cog_session_node_t *n = NULL;
    cog_message_t m = {0};
    const cog_node_data_t *d;

    m.role = COG_MESSAGE_ROLE_USER;
    m.content = payload;
    m.content_len = sizeof(payload);

    if (cog_session_new(&s, NULL) != COG_OK) {
        return 1;
    }
    if (cog_session_add_message(s, NULL, &m, &n) != COG_OK) {
        return 1;
    }
    d = cog_node_data(n);
    if (d == NULL || d->content_len != 4U || d->content == NULL) {
        return 1;
    }
    if (d->content[0] != 'a' || d->content[1] != '\0' ||
        d->content[2] != '\0' || d->content[3] != 'b') {
        return 1;
    }
    cog_session_free(s);
    return 0;
}

static int test_add_message_null_content(void)
{
    cog_session_t *s = NULL;
    cog_session_node_t *n = NULL;
    cog_message_t m = {0};
    const cog_node_data_t *d;

    m.role = COG_MESSAGE_ROLE_ASSISTANT; /* content NULL, len 0 */
    if (cog_session_new(&s, NULL) != COG_OK) {
        return 1;
    }
    if (cog_session_add_message(s, NULL, &m, &n) != COG_OK) {
        return 1;
    }
    d = cog_node_data(n);
    if (d == NULL || d->content != NULL || d->content_len != 0U) {
        return 1;
    }
    cog_session_free(s);
    return 0;
}

static int test_add_message_tool_role(void)
{
    cog_session_t *s = NULL;
    cog_session_node_t *n = NULL;
    cog_message_t m = {0};
    const cog_node_data_t *d;

    m.role = COG_MESSAGE_ROLE_TOOL;
    m.content = "{\"ok\": true}";
    m.content_len = strlen(m.content);
    m.name = "get_weather";

    if (cog_session_new(&s, NULL) != COG_OK) {
        return 1;
    }
    if (cog_session_add_message(s, NULL, &m, &n) != COG_OK) {
        return 1;
    }
    d = cog_node_data(n);
    if (d == NULL || d->role != COG_NODE_TOOL_RESULT) {
        return 1;
    }
    if (d->tool_name == NULL || strcmp(d->tool_name, "get_weather") != 0) {
        return 1;
    }
    if (d->tool_result == NULL ||
        strcmp(d->tool_result, "{\"ok\": true}") != 0) {
        return 1;
    }
    if (d->content != NULL) {
        return 1;
    }
    cog_session_free(s);
    return 0;
}

static int test_add_message_invalid_role(void)
{
    cog_session_t *s = NULL;
    cog_message_t m = {0};

    m.role = (cog_message_role_t)99;
    m.content = "x";
    m.content_len = 1U;
    if (cog_session_new(&s, NULL) != COG_OK) {
        return 1;
    }
    if (cog_session_add_message(s, NULL, &m, NULL) != COG_ERROR_INVALID_ARG) {
        return 1;
    }
    if (!cog_session_is_empty(s)) {
        return 1;
    }
    cog_session_free(s);
    return 0;
}

static int test_add_message_foreign_parent(void)
{
    cog_session_t *s1 = NULL;
    cog_session_t *s2 = NULL;
    cog_session_node_t *n1 = NULL;
    cog_session_node_t *n2 = NULL;

    if (cog_session_new(&s1, NULL) != COG_OK ||
        cog_session_new(&s2, NULL) != COG_OK) {
        return 1;
    }
    if (cog_session_add_message(s1, NULL, MSG_USER("a"), &n1) != COG_OK) {
        return 1;
    }
    if (cog_session_add_message(s2, n1, MSG_USER("b"), &n2) !=
        COG_ERROR_NOT_FOUND) {
        return 1;
    }
    if (!cog_session_is_empty(s2)) {
        return 1;
    }
    cog_session_free(s1);
    cog_session_free(s2);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Append                                                              */
/* ------------------------------------------------------------------ */

static int test_append_chain(void)
{
    cog_session_t *s = NULL;
    cog_session_node_t *u = NULL;
    cog_session_node_t *a = NULL;

    if (cog_session_new(&s, NULL) != COG_OK) {
        return 1;
    }
    if (cog_session_append(s, MSG_USER("q"), &u) != COG_OK) {
        return 1;
    }
    if (cog_session_append(s, MSG_ASSISTANT("a"), &a) != COG_OK) {
        return 1;
    }
    if (cog_session_root(s) != u || cog_node_parent(a) != u) {
        return 1;
    }
    if (cog_session_active(s) != a) {
        return 1;
    }
    cog_session_free(s);
    return 0;
}

static int test_append_nulls(void)
{
    cog_session_t *s = NULL;

    if (cog_session_new(&s, NULL) != COG_OK) {
        return 1;
    }
    if (cog_session_append(NULL, MSG_USER("x"), NULL) != COG_ERROR_NULL_PTR) {
        return 1;
    }
    if (cog_session_append(s, NULL, NULL) != COG_ERROR_NULL_PTR) {
        return 1;
    }
    cog_session_free(s);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Tool call / result                                                  */
/* ------------------------------------------------------------------ */

static int test_add_tool_call(void)
{
    cog_session_t *s = NULL;
    cog_session_node_t *root = NULL;
    cog_session_node_t *tc = NULL;
    const cog_node_data_t *d;

    if (cog_session_new(&s, NULL) != COG_OK) {
        return 1;
    }
    if (cog_session_add_message(s, NULL, MSG_USER("q"), &root) != COG_OK) {
        return 1;
    }
    if (cog_session_add_tool_call(s, root, "get_weather",
                                  "{\"city\": \"Kyoto\"}", &tc) != COG_OK) {
        return 1;
    }
    d = cog_node_data(tc);
    if (d == NULL || d->role != COG_NODE_TOOL_CALL) {
        return 1;
    }
    if (d->tool_name == NULL || strcmp(d->tool_name, "get_weather") != 0) {
        return 1;
    }
    if (d->tool_args == NULL ||
        strcmp(d->tool_args, "{\"city\": \"Kyoto\"}") != 0) {
        return 1;
    }
    if (d->content != NULL || d->tool_result != NULL) {
        return 1;
    }
    if (cog_session_active(s) != tc) {
        return 1;
    }
    cog_session_free(s);
    return 0;
}

static int test_add_tool_call_nulls(void)
{
    cog_session_t *s = NULL;
    cog_session_node_t *root = NULL;

    if (cog_session_new(&s, NULL) != COG_OK) {
        return 1;
    }
    if (cog_session_add_message(s, NULL, MSG_USER("q"), &root) != COG_OK) {
        return 1;
    }
    if (cog_session_add_tool_call(NULL, root, "t", "{}", NULL) !=
        COG_ERROR_NULL_PTR) {
        return 1;
    }
    if (cog_session_add_tool_call(s, root, NULL, "{}", NULL) !=
        COG_ERROR_NULL_PTR) {
        return 1;
    }
    if (cog_session_add_tool_call(s, root, "t", NULL, NULL) !=
        COG_ERROR_NULL_PTR) {
        return 1;
    }
    cog_session_free(s);
    return 0;
}

static int test_add_tool_result(void)
{
    cog_session_t *s = NULL;
    cog_session_node_t *root = NULL;
    cog_session_node_t *tc = NULL;
    cog_session_node_t *tr = NULL;
    const cog_node_data_t *d;

    if (cog_session_new(&s, NULL) != COG_OK) {
        return 1;
    }
    if (cog_session_add_message(s, NULL, MSG_USER("q"), &root) != COG_OK) {
        return 1;
    }
    if (cog_session_add_tool_call(s, root, "get_weather", "{}", &tc) !=
        COG_OK) {
        return 1;
    }
    if (cog_session_add_tool_result(s, tc, "18C", &tr) != COG_OK) {
        return 1;
    }
    d = cog_node_data(tr);
    if (d == NULL || d->role != COG_NODE_TOOL_RESULT) {
        return 1;
    }
    if (d->tool_result == NULL || strcmp(d->tool_result, "18C") != 0) {
        return 1;
    }
    if (cog_node_parent(tr) != tc || cog_node_child_count(tc) != 1U) {
        return 1;
    }
    cog_session_free(s);
    return 0;
}

static int test_add_tool_result_nulls(void)
{
    cog_session_t *s = NULL;
    cog_session_node_t *root = NULL;

    if (cog_session_new(&s, NULL) != COG_OK) {
        return 1;
    }
    if (cog_session_add_message(s, NULL, MSG_USER("q"), &root) != COG_OK) {
        return 1;
    }
    if (cog_session_add_tool_result(NULL, root, "r", NULL) !=
        COG_ERROR_NULL_PTR) {
        return 1;
    }
    if (cog_session_add_tool_result(s, NULL, "r", NULL) !=
        COG_ERROR_NULL_PTR) {
        return 1;
    }
    if (cog_session_add_tool_result(s, root, NULL, NULL) !=
        COG_ERROR_NULL_PTR) {
        return 1;
    }
    cog_session_free(s);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Navigation                                                          */
/* ------------------------------------------------------------------ */

static int test_navigation_multi_level(void)
{
    cog_session_t *s = NULL;
    cog_session_node_t *root = NULL;
    cog_session_node_t *a = NULL;
    cog_session_node_t *b = NULL;
    cog_session_node_t *c = NULL;

    if (cog_session_new(&s, NULL) != COG_OK) {
        return 1;
    }
    if (cog_session_add_message(s, NULL, MSG_USER("r"), &root) != COG_OK) {
        return 1;
    }
    if (cog_session_add_message(s, root, MSG_USER("a"), &a) != COG_OK) {
        return 1;
    }
    if (cog_session_add_message(s, a, MSG_USER("b"), &b) != COG_OK) {
        return 1;
    }
    if (cog_session_add_message(s, b, MSG_USER("c"), &c) != COG_OK) {
        return 1;
    }
    if (cog_node_parent(c) != b || cog_node_parent(b) != a ||
        cog_node_parent(a) != root || cog_node_parent(root) != NULL) {
        return 1;
    }
    if (cog_node_first_child(root) != a || cog_node_first_child(a) != b ||
        cog_node_first_child(b) != c || cog_node_first_child(c) != NULL) {
        return 1;
    }
    if (cog_node_child_count(root) != 1U || cog_node_child_count(a) != 1U ||
        cog_node_child_count(b) != 1U || cog_node_child_count(c) != 0U) {
        return 1;
    }
    if (cog_session_root(s) != root || cog_session_active(s) != c) {
        return 1;
    }
    cog_session_free(s);
    return 0;
}

static int test_navigation_null_node(void)
{
    if (cog_node_parent(NULL) != NULL || cog_node_first_child(NULL) != NULL ||
        cog_node_next_sibling(NULL) != NULL || cog_node_child_count(NULL) != 0U ||
        cog_node_data(NULL) != NULL) {
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Node data                                                           */
/* ------------------------------------------------------------------ */

static int test_node_data_fields(void)
{
    cog_session_t *s = NULL;
    cog_session_node_t *root = NULL;
    cog_session_node_t *tc = NULL;
    const cog_node_data_t *d;

    if (cog_session_new(&s, NULL) != COG_OK) {
        return 1;
    }
    if (cog_session_add_message(s, NULL, MSG_USER("q"), &root) != COG_OK) {
        return 1;
    }
    if (cog_session_add_tool_call(s, root, "t", "{}", &tc) != COG_OK) {
        return 1;
    }
    d = cog_node_data(tc);
    if (d == NULL) {
        return 1;
    }
    if (d->node_id != 2U || d->parent_id != 1U) {
        return 1;
    }
    /* Reserved fields: no public setter exists in session.h, so they
     * must be NULL / zero (documented in CONTRACT.md). */
    if (d->raw_request != NULL || d->raw_response != NULL ||
        d->model != NULL || d->provider != NULL || d->chunk_count != 0U) {
        return 1;
    }
    if (d->timing.request_sent_ms != 0U || d->timing.duration_ms != 0U ||
        d->cost.input_tokens != 0U || d->cost.total_cost != 0.0) {
        return 1;
    }
    cog_session_free(s);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Fork                                                                */
/* ------------------------------------------------------------------ */

static int test_fork_leaf(void)
{
    cog_session_t *s = NULL;
    cog_session_node_t *root = NULL;
    cog_session_node_t *a = NULL;
    cog_session_node_t *copy = NULL;
    const cog_node_data_t *da;
    const cog_node_data_t *dc;

    if (cog_session_new(&s, NULL) != COG_OK) {
        return 1;
    }
    if (cog_session_add_message(s, NULL, MSG_USER("r"), &root) != COG_OK) {
        return 1;
    }
    if (cog_session_add_message(s, root, MSG_USER("a"), &a) != COG_OK) {
        return 1;
    }
    if (cog_session_fork(s, a, &copy) != COG_OK) {
        return 1;
    }
    da = cog_node_data(a);
    dc = cog_node_data(copy);
    if (copy == a || dc == NULL) {
        return 1;
    }
    /* The copy is the next sibling of the source, same parent. */
    if (cog_node_parent(copy) != root || cog_node_next_sibling(a) != copy ||
        cog_node_next_sibling(copy) != NULL) {
        return 1;
    }
    if (cog_node_child_count(root) != 2U) {
        return 1;
    }
    /* Data is copied; identity is fresh. */
    if (dc->role != da->role || dc->node_id == da->node_id ||
        dc->parent_id != da->parent_id) {
        return 1;
    }
    if (dc->content == NULL || strcmp(dc->content, "a") != 0) {
        return 1;
    }
    /* The copy has its own (empty) children. */
    if (cog_node_child_count(copy) != 0U ||
        cog_node_first_child(copy) != NULL) {
        return 1;
    }
    if (cog_session_active(s) != copy) {
        return 1;
    }
    cog_session_free(s);
    return 0;
}

static int test_fork_midtree(void)
{
    cog_session_t *s = NULL;
    cog_session_node_t *root = NULL;
    cog_session_node_t *a = NULL;
    cog_session_node_t *b = NULL;
    cog_session_node_t *copy = NULL;

    if (cog_session_new(&s, NULL) != COG_OK) {
        return 1;
    }
    if (cog_session_add_message(s, NULL, MSG_USER("r"), &root) != COG_OK) {
        return 1;
    }
    if (cog_session_add_message(s, root, MSG_USER("a"), &a) != COG_OK) {
        return 1;
    }
    if (cog_session_add_message(s, a, MSG_USER("b"), &b) != COG_OK) {
        return 1;
    }
    /* Fork at the mid-tree node a, which already has a child. */
    if (cog_session_fork(s, a, &copy) != COG_OK) {
        return 1;
    }
    /* The copy shares ancestors (same parent) but has no children. */
    if (cog_node_parent(copy) != root) {
        return 1;
    }
    if (cog_node_child_count(copy) != 0U) {
        return 1;
    }
    /* The original branch is untouched. */
    if (cog_node_child_count(a) != 1U || cog_node_first_child(a) != b) {
        return 1;
    }
    cog_session_free(s);
    return 0;
}

static int test_fork_root(void)
{
    cog_session_t *s = NULL;
    cog_session_node_t *root = NULL;
    cog_session_node_t *out = NULL;

    if (cog_session_new(&s, NULL) != COG_OK) {
        return 1;
    }
    if (cog_session_add_message(s, NULL, MSG_USER("r"), &root) != COG_OK) {
        return 1;
    }
    if (cog_session_fork(s, root, &out) != COG_OK) {
        return 1;
    }
    /* Forking the root is a no-op: the root is the shared ancestor. */
    if (out != root) {
        return 1;
    }
    if (cog_node_child_count(root) != 0U) {
        return 1;
    }
    cog_session_free(s);
    return 0;
}

static int test_fork_nulls(void)
{
    cog_session_t *s = NULL;
    cog_session_node_t *root = NULL;

    if (cog_session_new(&s, NULL) != COG_OK) {
        return 1;
    }
    if (cog_session_add_message(s, NULL, MSG_USER("r"), &root) != COG_OK) {
        return 1;
    }
    if (cog_session_fork(NULL, root, NULL) != COG_ERROR_NULL_PTR) {
        return 1;
    }
    if (cog_session_fork(s, NULL, NULL) != COG_ERROR_NULL_PTR) {
        return 1;
    }
    cog_session_free(s);
    return 0;
}

static int test_fork_foreign(void)
{
    cog_session_t *s1 = NULL;
    cog_session_t *s2 = NULL;
    cog_session_node_t *n1 = NULL;

    if (cog_session_new(&s1, NULL) != COG_OK ||
        cog_session_new(&s2, NULL) != COG_OK) {
        return 1;
    }
    if (cog_session_add_message(s1, NULL, MSG_USER("a"), &n1) != COG_OK) {
        return 1;
    }
    if (cog_session_fork(s2, n1, NULL) != COG_ERROR_NOT_FOUND) {
        return 1;
    }
    cog_session_free(s1);
    cog_session_free(s2);
    return 0;
}

static int test_fork_branches(void)
{
    cog_session_t *s = NULL;
    cog_session_node_t *root = NULL;
    cog_session_node_t *a1 = NULL;
    cog_session_node_t *a2 = NULL;
    cog_session_node_t *copy = NULL;
    cog_session_node_t *b1 = NULL;
    const cog_message_t *msgs = NULL;
    size_t count = 0U;

    if (cog_session_new(&s, NULL) != COG_OK) {
        return 1;
    }
    if (cog_session_add_message(s, NULL, MSG_USER("q"), &root) != COG_OK) {
        return 1;
    }
    if (cog_session_append(s, MSG_ASSISTANT("a1"), &a1) != COG_OK) {
        return 1;
    }
    if (cog_session_append(s, MSG_ASSISTANT("a2"), &a2) != COG_OK) {
        return 1;
    }
    /* Branch B forks at a1 and continues independently. */
    if (cog_session_fork(s, a1, &copy) != COG_OK) {
        return 1;
    }
    if (cog_session_append(s, MSG_ASSISTANT("b1"), &b1) != COG_OK) {
        return 1;
    }

    /* Branch A history: root, a1, a2. */
    if (cog_session_get_history(s, a2, &msgs, &count) != COG_OK) {
        return 1;
    }
    if (count != 3U || msgs[0].role != COG_MESSAGE_ROLE_USER ||
        msgs[1].role != COG_MESSAGE_ROLE_ASSISTANT ||
        msgs[2].role != COG_MESSAGE_ROLE_ASSISTANT) {
        return 1;
    }
    if (strcmp(msgs[1].content, "a1") != 0 ||
        strcmp(msgs[2].content, "a2") != 0) {
        return 1;
    }

    /* Branch B history: root, copy(a1), b1 — shared root, own continuation. */
    if (cog_session_get_history(s, b1, &msgs, &count) != COG_OK) {
        return 1;
    }
    if (count != 3U || msgs[0].role != COG_MESSAGE_ROLE_USER ||
        msgs[1].role != COG_MESSAGE_ROLE_ASSISTANT ||
        msgs[2].role != COG_MESSAGE_ROLE_ASSISTANT) {
        return 1;
    }
    if (strcmp(msgs[1].content, "a1") != 0 ||
        strcmp(msgs[2].content, "b1") != 0) {
        return 1;
    }
    cog_session_free(s);
    return 0;
}

static int test_fork_preserves_timing_cost(void)
{
    cog_session_t *s = NULL;
    cog_session_node_t *root = NULL;
    cog_session_node_t *a = NULL;
    cog_session_node_t *copy = NULL;
    cog_timing_t timing = {0};
    cog_cost_t cost = {0};
    const cog_node_data_t *dc;

    timing.request_sent_ms = 10U;
    timing.duration_ms = 500U;
    timing.ttft_ms = 120U;
    timing.tokens_per_second = 12.5;
    cost.input_tokens = 100U;
    cost.output_tokens = 50U;
    cost.total_cost = 0.0042;

    if (cog_session_new(&s, NULL) != COG_OK) {
        return 1;
    }
    if (cog_session_add_message(s, NULL, MSG_USER("r"), &root) != COG_OK) {
        return 1;
    }
    if (cog_session_add_message(s, root, MSG_USER("a"), &a) != COG_OK) {
        return 1;
    }
    if (cog_session_set_timing(s, a, &timing) != COG_OK ||
        cog_session_set_cost(s, a, &cost) != COG_OK) {
        return 1;
    }
    if (cog_session_fork(s, a, &copy) != COG_OK) {
        return 1;
    }
    dc = cog_node_data(copy);
    if (dc->timing.duration_ms != 500U || dc->timing.ttft_ms != 120U ||
        !approx_eq(dc->timing.tokens_per_second, 12.5)) {
        return 1;
    }
    if (dc->cost.input_tokens != 100U || dc->cost.output_tokens != 50U ||
        !approx_eq(dc->cost.total_cost, 0.0042)) {
        return 1;
    }
    cog_session_free(s);
    return 0;
}

/* ------------------------------------------------------------------ */
/* History                                                             */
/* ------------------------------------------------------------------ */

static int test_history_single(void)
{
    cog_session_t *s = NULL;
    cog_session_node_t *n = NULL;
    const cog_message_t *msgs = NULL;
    size_t count = 0U;

    if (cog_session_new(&s, NULL) != COG_OK) {
        return 1;
    }
    if (cog_session_add_message(s, NULL, MSG_USER("hi"), &n) != COG_OK) {
        return 1;
    }
    if (cog_session_get_history(s, n, &msgs, &count) != COG_OK) {
        return 1;
    }
    if (count != 1U || msgs == NULL) {
        return 1;
    }
    if (msgs[0].role != COG_MESSAGE_ROLE_USER ||
        msgs[0].content == NULL || strcmp(msgs[0].content, "hi") != 0 ||
        msgs[0].content_len != 2U || msgs[0].name != NULL) {
        return 1;
    }
    cog_session_free(s);
    return 0;
}

static int test_history_chain(void)
{
    cog_session_t *s = NULL;
    cog_session_node_t *sys = NULL;
    cog_session_node_t *u = NULL;
    cog_session_node_t *a = NULL;
    const cog_message_t *msgs = NULL;
    size_t count = 0U;

    if (cog_session_new(&s, NULL) != COG_OK) {
        return 1;
    }
    if (cog_session_add_message(s, NULL, MSG_SYSTEM("be brief"), &sys) !=
        COG_OK) {
        return 1;
    }
    if (cog_session_append(s, MSG_USER("q"), &u) != COG_OK) {
        return 1;
    }
    if (cog_session_append(s, MSG_ASSISTANT("a"), &a) != COG_OK) {
        return 1;
    }
    if (cog_session_get_history(s, a, &msgs, &count) != COG_OK) {
        return 1;
    }
    if (count != 3U) {
        return 1;
    }
    if (msgs[0].role != COG_MESSAGE_ROLE_SYSTEM ||
        strcmp(msgs[0].content, "be brief") != 0) {
        return 1;
    }
    if (msgs[1].role != COG_MESSAGE_ROLE_USER ||
        strcmp(msgs[1].content, "q") != 0) {
        return 1;
    }
    if (msgs[2].role != COG_MESSAGE_ROLE_ASSISTANT ||
        strcmp(msgs[2].content, "a") != 0) {
        return 1;
    }
    cog_session_free(s);
    return 0;
}

static int test_history_tool_nodes(void)
{
    cog_session_t *s = NULL;
    cog_session_node_t *root = NULL;
    cog_session_node_t *tc = NULL;
    cog_session_node_t *tr = NULL;
    const cog_message_t *msgs = NULL;
    size_t count = 0U;

    if (cog_session_new(&s, NULL) != COG_OK) {
        return 1;
    }
    if (cog_session_add_message(s, NULL, MSG_USER("q"), &root) != COG_OK) {
        return 1;
    }
    if (cog_session_add_tool_call(s, root, "get_weather",
                                  "{\"city\": \"Kyoto\"}", &tc) != COG_OK) {
        return 1;
    }
    if (cog_session_add_tool_result(s, tc, "18C", &tr) != COG_OK) {
        return 1;
    }
    if (cog_session_get_history(s, tr, &msgs, &count) != COG_OK) {
        return 1;
    }
    if (count != 3U) {
        return 1;
    }
    /* The tool call maps to COG_MESSAGE_ROLE_TOOL with the tool name
     * preserved.  The result node was created via add_tool_result, which
     * takes no name, so its message carries no tool name (CONTRACT.md). */
    if (msgs[1].role != COG_MESSAGE_ROLE_TOOL ||
        msgs[1].name == NULL || strcmp(msgs[1].name, "get_weather") != 0 ||
        msgs[1].content == NULL ||
        strcmp(msgs[1].content, "{\"city\": \"Kyoto\"}") != 0) {
        return 1;
    }
    if (msgs[2].role != COG_MESSAGE_ROLE_TOOL ||
        msgs[2].name != NULL ||
        msgs[2].content == NULL || strcmp(msgs[2].content, "18C") != 0) {
        return 1;
    }
    cog_session_free(s);
    return 0;
}

static int test_history_nulls(void)
{
    cog_session_t *s = NULL;
    cog_session_node_t *n = NULL;
    const cog_message_t *msgs = NULL;
    size_t count = 0U;

    if (cog_session_new(&s, NULL) != COG_OK) {
        return 1;
    }
    if (cog_session_add_message(s, NULL, MSG_USER("x"), &n) != COG_OK) {
        return 1;
    }
    if (cog_session_get_history(NULL, n, &msgs, &count) !=
        COG_ERROR_NULL_PTR) {
        return 1;
    }
    if (cog_session_get_history(s, NULL, &msgs, &count) !=
        COG_ERROR_NULL_PTR) {
        return 1;
    }
    if (cog_session_get_history(s, n, NULL, &count) != COG_ERROR_NULL_PTR) {
        return 1;
    }
    if (cog_session_get_history(s, n, &msgs, NULL) != COG_ERROR_NULL_PTR) {
        return 1;
    }
    cog_session_free(s);
    return 0;
}

static int test_history_foreign(void)
{
    cog_session_t *s1 = NULL;
    cog_session_t *s2 = NULL;
    cog_session_node_t *n1 = NULL;
    const cog_message_t *msgs = NULL;
    size_t count = 0U;

    if (cog_session_new(&s1, NULL) != COG_OK ||
        cog_session_new(&s2, NULL) != COG_OK) {
        return 1;
    }
    if (cog_session_add_message(s1, NULL, MSG_USER("x"), &n1) != COG_OK) {
        return 1;
    }
    if (cog_session_get_history(s2, n1, &msgs, &count) !=
        COG_ERROR_NOT_FOUND) {
        return 1;
    }
    cog_session_free(s1);
    cog_session_free(s2);
    return 0;
}

static int test_history_rebuild_overwrites(void)
{
    cog_session_t *s = NULL;
    cog_session_node_t *root = NULL;
    cog_session_node_t *a = NULL;
    cog_session_node_t *b = NULL;
    const cog_message_t *msgs = NULL;
    size_t count = 0U;

    if (cog_session_new(&s, NULL) != COG_OK) {
        return 1;
    }
    if (cog_session_add_message(s, NULL, MSG_USER("r"), &root) != COG_OK) {
        return 1;
    }
    if (cog_session_add_message(s, root, MSG_USER("a"), &a) != COG_OK) {
        return 1;
    }
    if (cog_session_add_message(s, root, MSG_USER("b"), &b) != COG_OK) {
        return 1;
    }
    /* Querying a different node rebuilds the shared per-session buffer. */
    if (cog_session_get_history(s, a, &msgs, &count) != COG_OK ||
        count != 2U || strcmp(msgs[1].content, "a") != 0) {
        return 1;
    }
    if (cog_session_get_history(s, b, &msgs, &count) != COG_OK ||
        count != 2U || strcmp(msgs[1].content, "b") != 0) {
        return 1;
    }
    cog_session_free(s);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Timing and billing                                                  */
/* ------------------------------------------------------------------ */

static int test_set_timing_cost(void)
{
    cog_session_t *s = NULL;
    cog_session_node_t *n = NULL;
    cog_timing_t timing = {0};
    cog_cost_t cost = {0};
    const cog_node_data_t *d;

    timing.request_sent_ms = 1U;
    timing.first_token_ms = 2U;
    timing.last_token_ms = 3U;
    timing.duration_ms = 4U;
    timing.ttft_ms = 5U;
    timing.tokens_per_second = 6.5;
    cost.input_tokens = 7U;
    cost.output_tokens = 8U;
    cost.cost_per_input_token = 0.1;
    cost.cost_per_output_token = 0.2;
    cost.total_cost = 0.3;

    if (cog_session_new(&s, NULL) != COG_OK) {
        return 1;
    }
    if (cog_session_add_message(s, NULL, MSG_USER("x"), &n) != COG_OK) {
        return 1;
    }
    if (cog_session_set_timing(s, n, &timing) != COG_OK ||
        cog_session_set_cost(s, n, &cost) != COG_OK) {
        return 1;
    }
    d = cog_node_data(n);
    if (d->timing.request_sent_ms != 1U || d->timing.first_token_ms != 2U ||
        d->timing.last_token_ms != 3U || d->timing.duration_ms != 4U ||
        d->timing.ttft_ms != 5U || !approx_eq(d->timing.tokens_per_second, 6.5)) {
        return 1;
    }
    if (d->cost.input_tokens != 7U || d->cost.output_tokens != 8U ||
        !approx_eq(d->cost.cost_per_input_token, 0.1) ||
        !approx_eq(d->cost.cost_per_output_token, 0.2) ||
        !approx_eq(d->cost.total_cost, 0.3)) {
        return 1;
    }
    cog_session_free(s);
    return 0;
}

static int test_set_timing_cost_nulls(void)
{
    cog_session_t *s = NULL;
    cog_session_node_t *n = NULL;
    cog_timing_t timing = {0};
    cog_cost_t cost = {0};

    if (cog_session_new(&s, NULL) != COG_OK) {
        return 1;
    }
    if (cog_session_add_message(s, NULL, MSG_USER("x"), &n) != COG_OK) {
        return 1;
    }
    if (cog_session_set_timing(NULL, n, &timing) != COG_ERROR_NULL_PTR) {
        return 1;
    }
    if (cog_session_set_timing(s, NULL, &timing) != COG_ERROR_NULL_PTR) {
        return 1;
    }
    if (cog_session_set_timing(s, n, NULL) != COG_ERROR_NULL_PTR) {
        return 1;
    }
    if (cog_session_set_cost(NULL, n, &cost) != COG_ERROR_NULL_PTR) {
        return 1;
    }
    if (cog_session_set_cost(s, NULL, &cost) != COG_ERROR_NULL_PTR) {
        return 1;
    }
    if (cog_session_set_cost(s, n, NULL) != COG_ERROR_NULL_PTR) {
        return 1;
    }
    cog_session_free(s);
    return 0;
}

static int test_set_timing_cost_foreign(void)
{
    cog_session_t *s1 = NULL;
    cog_session_t *s2 = NULL;
    cog_session_node_t *n1 = NULL;
    cog_timing_t timing = {0};
    cog_cost_t cost = {0};

    if (cog_session_new(&s1, NULL) != COG_OK ||
        cog_session_new(&s2, NULL) != COG_OK) {
        return 1;
    }
    if (cog_session_add_message(s1, NULL, MSG_USER("x"), &n1) != COG_OK) {
        return 1;
    }
    if (cog_session_set_timing(s2, n1, &timing) != COG_ERROR_NOT_FOUND) {
        return 1;
    }
    if (cog_session_set_cost(s2, n1, &cost) != COG_ERROR_NOT_FOUND) {
        return 1;
    }
    cog_session_free(s1);
    cog_session_free(s2);
    return 0;
}

static int test_cumulative_cost(void)
{
    cog_session_t *s = NULL;
    cog_session_node_t *root = NULL;
    cog_session_node_t *a = NULL;
    cog_session_node_t *b = NULL;
    cog_cost_t cost = {0};

    if (cog_session_new(&s, NULL) != COG_OK) {
        return 1;
    }
    if (cog_session_add_message(s, NULL, MSG_USER("r"), &root) != COG_OK) {
        return 1;
    }
    if (cog_session_add_message(s, root, MSG_USER("a"), &a) != COG_OK) {
        return 1;
    }
    if (cog_session_add_message(s, a, MSG_USER("b"), &b) != COG_OK) {
        return 1;
    }
    cost.total_cost = 0.001;
    if (cog_session_set_cost(s, root, &cost) != COG_OK) {
        return 1;
    }
    cost.total_cost = 0.002;
    if (cog_session_set_cost(s, a, &cost) != COG_OK) {
        return 1;
    }
    cost.total_cost = 0.003;
    if (cog_session_set_cost(s, b, &cost) != COG_OK) {
        return 1;
    }
    if (!approx_eq(cog_session_cumulative_cost(s, b), 0.006)) {
        return 1;
    }
    if (!approx_eq(cog_session_cumulative_cost(s, a), 0.003)) {
        return 1;
    }
    if (!approx_eq(cog_session_cumulative_cost(s, root), 0.001)) {
        return 1;
    }
    /* NULL node and foreign node both yield 0.0. */
    if (cog_session_cumulative_cost(s, NULL) != 0.0) {
        return 1;
    }
    cog_session_free(s);
    return 0;
}

static int test_cumulative_cost_foreign(void)
{
    cog_session_t *s1 = NULL;
    cog_session_t *s2 = NULL;
    cog_session_node_t *n1 = NULL;

    if (cog_session_new(&s1, NULL) != COG_OK ||
        cog_session_new(&s2, NULL) != COG_OK) {
        return 1;
    }
    if (cog_session_add_message(s1, NULL, MSG_USER("x"), &n1) != COG_OK) {
        return 1;
    }
    if (cog_session_cumulative_cost(s2, n1) != 0.0) {
        return 1;
    }
    cog_session_free(s1);
    cog_session_free(s2);
    return 0;
}

static int test_cumulative_timing(void)
{
    cog_session_t *s = NULL;
    cog_session_node_t *root = NULL;
    cog_session_node_t *a = NULL;
    cog_session_node_t *b = NULL;
    cog_timing_t timing = {0};
    cog_timing_t acc = {0};

    if (cog_session_new(&s, NULL) != COG_OK) {
        return 1;
    }
    if (cog_session_add_message(s, NULL, MSG_USER("r"), &root) != COG_OK) {
        return 1;
    }
    if (cog_session_add_message(s, root, MSG_USER("a"), &a) != COG_OK) {
        return 1;
    }
    if (cog_session_add_message(s, a, MSG_USER("b"), &b) != COG_OK) {
        return 1;
    }
    timing.duration_ms = 100U;
    timing.ttft_ms = 10U;
    timing.tokens_per_second = 1.5;
    if (cog_session_set_timing(s, root, &timing) != COG_OK) {
        return 1;
    }
    timing.duration_ms = 200U;
    timing.ttft_ms = 20U;
    timing.tokens_per_second = 2.5;
    if (cog_session_set_timing(s, a, &timing) != COG_OK) {
        return 1;
    }
    timing.duration_ms = 300U;
    timing.ttft_ms = 30U;
    timing.tokens_per_second = 3.5;
    if (cog_session_set_timing(s, b, &timing) != COG_OK) {
        return 1;
    }
    if (cog_session_cumulative_timing(s, b, &acc) != COG_OK) {
        return 1;
    }
    if (acc.duration_ms != 600U || acc.ttft_ms != 60U ||
        !approx_eq(acc.tokens_per_second, 7.5)) {
        return 1;
    }
    cog_session_free(s);
    return 0;
}

static int test_cumulative_timing_nulls(void)
{
    cog_session_t *s = NULL;
    cog_session_node_t *n = NULL;
    cog_timing_t acc = {0};

    if (cog_session_new(&s, NULL) != COG_OK) {
        return 1;
    }
    if (cog_session_add_message(s, NULL, MSG_USER("x"), &n) != COG_OK) {
        return 1;
    }
    if (cog_session_cumulative_timing(NULL, n, &acc) != COG_ERROR_NULL_PTR) {
        return 1;
    }
    if (cog_session_cumulative_timing(s, NULL, &acc) != COG_ERROR_NULL_PTR) {
        return 1;
    }
    if (cog_session_cumulative_timing(s, n, NULL) != COG_ERROR_NULL_PTR) {
        return 1;
    }
    cog_session_free(s);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Parent session                                                      */
/* ------------------------------------------------------------------ */

static int test_parent_id(void)
{
    cog_session_t *s = NULL;

    if (cog_session_new(&s, NULL) != COG_OK) {
        return 1;
    }
    if (cog_session_parent_id(s) != NULL) {
        return 1;
    }
    if (cog_session_set_parent(s, "sess-parent-1") != COG_OK) {
        return 1;
    }
    if (cog_session_parent_id(s) == NULL ||
        strcmp(cog_session_parent_id(s), "sess-parent-1") != 0) {
        return 1;
    }
    /* Replacing the parent frees the old copy. */
    if (cog_session_set_parent(s, "sess-parent-2") != COG_OK) {
        return 1;
    }
    if (cog_session_parent_id(s) == NULL ||
        strcmp(cog_session_parent_id(s), "sess-parent-2") != 0) {
        return 1;
    }
    /* NULL clears the parent. */
    if (cog_session_set_parent(s, NULL) != COG_OK) {
        return 1;
    }
    if (cog_session_parent_id(s) != NULL) {
        return 1;
    }
    if (cog_session_set_parent(NULL, "x") != COG_ERROR_NULL_PTR) {
        return 1;
    }
    cog_session_free(s);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Discard (cancellation)                                              */
/* ------------------------------------------------------------------ */

static int test_discard_leaf(void)
{
    cog_session_t *s = NULL;
    cog_session_node_t *root = NULL;
    cog_session_node_t *a = NULL;
    cog_session_node_t *b = NULL;

    if (cog_session_new(&s, NULL) != COG_OK) {
        return 1;
    }
    if (cog_session_add_message(s, NULL, MSG_USER("r"), &root) != COG_OK) {
        return 1;
    }
    if (cog_session_add_message(s, root, MSG_USER("a"), &a) != COG_OK) {
        return 1;
    }
    if (cog_session_add_message(s, root, MSG_USER("b"), &b) != COG_OK) {
        return 1;
    }
    if (cog_session_discard_node(s, a) != COG_OK) {
        return 1;
    }
    if (cog_node_child_count(root) != 1U) {
        return 1;
    }
    if (cog_node_first_child(root) != b || cog_node_next_sibling(b) != NULL) {
        return 1;
    }
    cog_session_free(s);
    return 0;
}

static int test_discard_active(void)
{
    cog_session_t *s = NULL;
    cog_session_node_t *root = NULL;
    cog_session_node_t *u = NULL;
    cog_session_node_t *a = NULL;

    if (cog_session_new(&s, NULL) != COG_OK) {
        return 1;
    }
    if (cog_session_add_message(s, NULL, MSG_USER("q"), &root) != COG_OK) {
        return 1;
    }
    if (cog_session_append(s, MSG_USER("u"), &u) != COG_OK) {
        return 1;
    }
    if (cog_session_append(s, MSG_ASSISTANT("partial"), &a) != COG_OK) {
        return 1;
    }
    if (cog_session_active(s) != a) {
        return 1;
    }
    /* Scenario 20: cancel mid-stream discards the partial response. */
    if (cog_session_discard_node(s, a) != COG_OK) {
        return 1;
    }
    if (cog_session_active(s) != u) {
        return 1;
    }
    /* The conversation continues from the preserved user message. */
    if (cog_session_append(s, MSG_ASSISTANT("retry"), NULL) != COG_OK) {
        return 1;
    }
    if (cog_node_child_count(u) != 1U) {
        return 1;
    }
    cog_session_free(s);
    return 0;
}

static int test_discard_nonleaf(void)
{
    cog_session_t *s = NULL;
    cog_session_node_t *root = NULL;
    cog_session_node_t *a = NULL;
    cog_session_node_t *b = NULL;

    if (cog_session_new(&s, NULL) != COG_OK) {
        return 1;
    }
    if (cog_session_add_message(s, NULL, MSG_USER("r"), &root) != COG_OK) {
        return 1;
    }
    if (cog_session_add_message(s, root, MSG_USER("a"), &a) != COG_OK) {
        return 1;
    }
    if (cog_session_add_message(s, a, MSG_USER("b"), &b) != COG_OK) {
        return 1;
    }
    if (cog_session_discard_node(s, a) != COG_ERROR_INVALID_STATE) {
        return 1;
    }
    cog_session_free(s);
    return 0;
}

static int test_discard_foreign(void)
{
    cog_session_t *s1 = NULL;
    cog_session_t *s2 = NULL;
    cog_session_node_t *n1 = NULL;

    if (cog_session_new(&s1, NULL) != COG_OK ||
        cog_session_new(&s2, NULL) != COG_OK) {
        return 1;
    }
    if (cog_session_add_message(s1, NULL, MSG_USER("x"), &n1) != COG_OK) {
        return 1;
    }
    if (cog_session_discard_node(s2, n1) != COG_ERROR_NOT_FOUND) {
        return 1;
    }
    cog_session_free(s1);
    cog_session_free(s2);
    return 0;
}

static int test_discard_nulls(void)
{
    cog_session_t *s = NULL;
    cog_session_node_t *n = NULL;

    if (cog_session_new(&s, NULL) != COG_OK) {
        return 1;
    }
    if (cog_session_add_message(s, NULL, MSG_USER("x"), &n) != COG_OK) {
        return 1;
    }
    if (cog_session_discard_node(NULL, n) != COG_ERROR_NULL_PTR) {
        return 1;
    }
    if (cog_session_discard_node(s, NULL) != COG_ERROR_NULL_PTR) {
        return 1;
    }
    cog_session_free(s);
    return 0;
}

static int test_discard_root(void)
{
    cog_session_t *s = NULL;
    cog_session_node_t *root = NULL;

    if (cog_session_new(&s, NULL) != COG_OK) {
        return 1;
    }
    if (cog_session_add_message(s, NULL, MSG_USER("only"), &root) != COG_OK) {
        return 1;
    }
    if (cog_session_discard_node(s, root) != COG_OK) {
        return 1;
    }
    if (!cog_session_is_empty(s)) {
        return 1;
    }
    if (cog_session_root(s) != NULL || cog_session_active(s) != NULL) {
        return 1;
    }
    /* The session recovers: a fresh add creates a new root. */
    if (cog_session_add_message(s, NULL, MSG_USER("again"), &root) != COG_OK) {
        return 1;
    }
    if (cog_session_root(s) != root || cog_node_data(root)->node_id != 2U) {
        return 1;
    }
    cog_session_free(s);
    return 0;
}

static int test_discard_then_history(void)
{
    cog_session_t *s = NULL;
    cog_session_node_t *root = NULL;
    cog_session_node_t *a = NULL;
    cog_session_node_t *b = NULL;
    const cog_message_t *msgs = NULL;
    size_t count = 0U;

    if (cog_session_new(&s, NULL) != COG_OK) {
        return 1;
    }
    if (cog_session_add_message(s, NULL, MSG_USER("r"), &root) != COG_OK) {
        return 1;
    }
    if (cog_session_add_message(s, root, MSG_USER("a"), &a) != COG_OK) {
        return 1;
    }
    if (cog_session_add_message(s, a, MSG_USER("b"), &b) != COG_OK) {
        return 1;
    }
    if (cog_session_discard_node(s, b) != COG_OK) {
        return 1;
    }
    if (cog_session_get_history(s, a, &msgs, &count) != COG_OK) {
        return 1;
    }
    if (count != 2U || strcmp(msgs[0].content, "r") != 0 ||
        strcmp(msgs[1].content, "a") != 0) {
        return 1;
    }
    cog_session_free(s);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Capacity (max_nodes pruning)                                        */
/* ------------------------------------------------------------------ */

static int test_max_nodes_prune(void)
{
    cog_session_t *s = NULL;
    cog_session_config_t cfg = {.id = NULL, .max_nodes = 3U};
    cog_session_node_t *root = NULL;
    cog_session_node_t *a = NULL;
    cog_session_node_t *b = NULL;
    cog_session_node_t *c = NULL;
    cog_session_node_t *d = NULL;
    const cog_message_t *msgs = NULL;
    size_t count = 0U;

    if (cog_session_new(&s, &cfg) != COG_OK) {
        return 1;
    }
    if (cog_session_add_message(s, NULL, MSG_USER("r"), &root) != COG_OK) {
        return 1;
    }
    if (cog_session_add_message(s, root, MSG_USER("a"), &a) != COG_OK) {
        return 1;
    }
    if (cog_session_add_message(s, root, MSG_USER("b"), &b) != COG_OK) {
        return 1;
    }
    if (cog_session_add_message(s, root, MSG_USER("c"), &c) != COG_OK) {
        return 1;
    }
    if (cog_session_add_message(s, root, MSG_USER("d"), &d) != COG_OK) {
        return 1;
    }
    /* 5 adds, cap 3: the two oldest leaves (a, b) were pruned. */
    if (cog_session_root(s) != root) {
        return 1;
    }
    if (cog_node_child_count(root) != 2U) {
        return 1;
    }
    if (cog_node_first_child(root) != c || cog_node_next_sibling(c) != d ||
        cog_node_next_sibling(d) != NULL) {
        return 1;
    }
    if (cog_session_active(s) != d) {
        return 1;
    }
    /* The surviving path still builds a correct history. */
    if (cog_session_get_history(s, d, &msgs, &count) != COG_OK) {
        return 1;
    }
    if (count != 2U || strcmp(msgs[0].content, "r") != 0 ||
        strcmp(msgs[1].content, "d") != 0) {
        return 1;
    }
    cog_session_free(s);
    return 0;
}

static int test_max_nodes_default(void)
{
    cog_session_t *s = NULL;
    cog_session_node_t *root = NULL;
    cog_session_node_t *n = NULL;
    size_t i;

    /* max_nodes = 0 selects the default (1024); 5 nodes fit untouched. */
    if (cog_session_new(&s, NULL) != COG_OK) {
        return 1;
    }
    if (cog_session_add_message(s, NULL, MSG_USER("r"), &root) != COG_OK) {
        return 1;
    }
    for (i = 0U; i < 4U; ++i) {
        if (cog_session_add_message(s, root, MSG_USER("x"), &n) != COG_OK) {
            return 1;
        }
    }
    if (cog_node_child_count(root) != 4U) {
        return 1;
    }
    cog_session_free(s);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Stable ids                                                          */
/* ------------------------------------------------------------------ */

static int test_node_ids_stable(void)
{
    cog_session_t *s = NULL;
    cog_session_node_t *root = NULL;
    cog_session_node_t *a = NULL;
    cog_session_node_t *b = NULL;
    cog_session_node_t *discarded = NULL;
    uint64_t id_root;
    uint64_t id_a;

    if (cog_session_new(&s, NULL) != COG_OK) {
        return 1;
    }
    if (cog_session_add_message(s, NULL, MSG_USER("r"), &root) != COG_OK) {
        return 1;
    }
    if (cog_session_add_message(s, root, MSG_USER("a"), &a) != COG_OK) {
        return 1;
    }
    if (cog_session_add_message(s, root, MSG_USER("b"), &b) != COG_OK) {
        return 1;
    }
    id_root = cog_node_data(root)->node_id;
    id_a = cog_node_data(a)->node_id;
    if (id_root != 1U || id_a != 2U || cog_node_data(b)->node_id != 3U) {
        return 1;
    }
    /* Ids are unique and stable across queries. */
    if (cog_node_data(root)->node_id != id_root ||
        cog_node_data(a)->node_id != id_a) {
        return 1;
    }
    /* Discarding does not reuse ids. */
    if (cog_session_discard_node(s, a) != COG_OK) {
        return 1;
    }
    if (cog_session_add_message(s, root, MSG_USER("c"), &discarded) !=
        COG_OK) {
        return 1;
    }
    if (cog_node_data(discarded)->node_id != 4U) {
        return 1;
    }
    cog_session_free(s);
    return 0;
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int main(void)
{
    unsigned failures = 0U;

#define CHECK(label, expr)                                                   \
    do {                                                                     \
        if ((expr) != 0) {                                                   \
            (void)fprintf(stderr, "FAIL: %s\n", label);                      \
            ++failures;                                                      \
        }                                                                    \
    } while (0)

    /* Lifecycle */
    CHECK("new defaults", test_new_defaults());
    CHECK("new with id", test_new_with_id());
    CHECK("new null out", test_new_null_out());
    CHECK("free null", test_free_null());
    CHECK("id null session", test_id_null_session());
    CHECK("reset", test_reset());
    CHECK("reset null", test_reset_null());

    /* Add message */
    CHECK("add message root", test_add_message_root());
    CHECK("add message child", test_add_message_child());
    CHECK("add message siblings", test_add_message_siblings());
    CHECK("add message nulls", test_add_message_nulls());
    CHECK("add message out null", test_add_message_out_null());
    CHECK("add message embedded nul", test_add_message_embedded_nul());
    CHECK("add message null content", test_add_message_null_content());
    CHECK("add message tool role", test_add_message_tool_role());
    CHECK("add message invalid role", test_add_message_invalid_role());
    CHECK("add message foreign parent", test_add_message_foreign_parent());

    /* Append */
    CHECK("append chain", test_append_chain());
    CHECK("append nulls", test_append_nulls());

    /* Tool call / result */
    CHECK("add tool call", test_add_tool_call());
    CHECK("add tool call nulls", test_add_tool_call_nulls());
    CHECK("add tool result", test_add_tool_result());
    CHECK("add tool result nulls", test_add_tool_result_nulls());

    /* Navigation */
    CHECK("navigation multi level", test_navigation_multi_level());
    CHECK("navigation null node", test_navigation_null_node());

    /* Node data */
    CHECK("node data fields", test_node_data_fields());

    /* Fork */
    CHECK("fork leaf", test_fork_leaf());
    CHECK("fork midtree", test_fork_midtree());
    CHECK("fork root", test_fork_root());
    CHECK("fork nulls", test_fork_nulls());
    CHECK("fork foreign", test_fork_foreign());
    CHECK("fork branches", test_fork_branches());
    CHECK("fork preserves timing/cost", test_fork_preserves_timing_cost());

    /* History */
    CHECK("history single", test_history_single());
    CHECK("history chain", test_history_chain());
    CHECK("history tool nodes", test_history_tool_nodes());
    CHECK("history nulls", test_history_nulls());
    CHECK("history foreign", test_history_foreign());
    CHECK("history rebuild overwrites", test_history_rebuild_overwrites());

    /* Timing and billing */
    CHECK("set timing/cost", test_set_timing_cost());
    CHECK("set timing/cost nulls", test_set_timing_cost_nulls());
    CHECK("set timing/cost foreign", test_set_timing_cost_foreign());
    CHECK("cumulative cost", test_cumulative_cost());
    CHECK("cumulative cost foreign", test_cumulative_cost_foreign());
    CHECK("cumulative timing", test_cumulative_timing());
    CHECK("cumulative timing nulls", test_cumulative_timing_nulls());

    /* Parent session */
    CHECK("parent id", test_parent_id());

    /* Discard */
    CHECK("discard leaf", test_discard_leaf());
    CHECK("discard active", test_discard_active());
    CHECK("discard nonleaf", test_discard_nonleaf());
    CHECK("discard foreign", test_discard_foreign());
    CHECK("discard nulls", test_discard_nulls());
    CHECK("discard root", test_discard_root());
    CHECK("discard then history", test_discard_then_history());

    /* Capacity */
    CHECK("max nodes prune", test_max_nodes_prune());
    CHECK("max nodes default", test_max_nodes_default());

    /* Stable ids */
    CHECK("node ids stable", test_node_ids_stable());

#undef CHECK

    if (failures == 0U) {
        (void)printf("test_session: ok\n");
    }
    return failures == 0U ? 0 : 1;
}

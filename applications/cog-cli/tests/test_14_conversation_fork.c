/**
 * @file test_14_conversation_fork.c
 * @brief Scenario 14 — conversation forking.
 *
 * The scenario doc (docs/scenarios/14-conversation-fork.md) covers
 * forking at a node, shared immutable ancestors, and independent
 * histories with independent timing/cost.  This test drives the real
 * @c cog_session_* APIs.
 *
 * Fork returns the existing immutable shared fork point. Divergent children
 * preserve ancestor node/effect identity and keep independent branch metrics.
 *
 * Tree built here:
 *   n1 (user "Explain X") -> n2 (assistant "answer A")
 *   fork at n2 -> n2B (same immutable node)
 *   branch A: child of n2  -> n3A (assistant "branch A")
 *   branch B: child of n2B -> n3B (assistant "branch B")
 *
 * What is verified:
 *  1. Forking preserves the immutable ancestor's exact node id.
 *  2. Shared ancestors: root and n1 are common to both branches.
 *  3. Independent histories: branch A and branch B diverge after n2.
 *  4. Independent timing/cost: cumulative cost/timing differ per branch.
 */
#define _POSIX_C_SOURCE 200809L

#include "cog/session.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            (void)fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__,         \
                          __LINE__, #cond);                             \
            ++failures;                                                 \
        }                                                               \
    } while (0)

#define CHECK_EQ_INT(a, b)                                              \
    do {                                                                \
        long long _a = (long long)(a);                                  \
        long long _b = (long long)(b);                                  \
        if (_a != _b) {                                                 \
            (void)fprintf(stderr, "FAIL %s:%d: %s == %s (%lld != %lld)\n", \
                          __FILE__, __LINE__, #a, #b, _a, _b);          \
            ++failures;                                                 \
        }                                                               \
    } while (0)

static cog_session_t *make_session(void)
{
    cog_session_t *s = NULL;

    if (cog_session_new(&s, NULL) != COG_OK) {
        return NULL;
    }
    return s;
}

static cog_session_node_t *add_msg(cog_session_t *s,
                                   cog_session_node_t *parent,
                                   cog_message_role_t role,
                                   const char *text)
{
    cog_message_t m = {0};
    cog_session_node_t *out = NULL;

    (void)memset(&m, 0, sizeof(m));
    m.role = role;
    m.content = text;
    m.content_len = strlen(text);
    if (cog_session_add_message(s, parent, &m, &out) != COG_OK) {
        return NULL;
    }
    return out;
}

static size_t read_history(cog_session_t *s, cog_session_node_t *leaf,
                           cog_message_t *out, size_t cap)
{
    const cog_message_t *msgs = NULL;
    size_t count = 0U;
    size_t i;

    if (cog_session_get_history(s, leaf, &msgs, &count) != COG_OK) {
        return 0U;
    }
    if (count > cap) {
        count = cap;
    }
    for (i = 0U; i < count; ++i) {
        out[i] = msgs[i];
    }
    return count;
}

static void scenario_fork(void)
{
    cog_session_t *s = make_session();
    cog_session_node_t *n1;
    cog_session_node_t *n2;
    cog_session_node_t *n2b;
    cog_session_node_t *n3a;
    cog_session_node_t *n3b;
    cog_message_t histA[8];
    cog_message_t histB[8];
    size_t nA;
    size_t nB;
    cog_cost_t cost;
    cog_timing_t timing;
    cog_timing_t cumA;
    cog_timing_t cumB;
    double costA;
    double costB;

    CHECK(s != NULL);

    /* root -> n1 (user) -> n2 (assistant). */
    n1 = add_msg(s, NULL, COG_MESSAGE_ROLE_USER, "Explain X");
    CHECK(n1 != NULL);
    n2 = add_msg(s, n1, COG_MESSAGE_ROLE_ASSISTANT, "answer A");
    CHECK(n2 != NULL);

    /* Set ancestor metrics before the node becomes shared and immutable. */
    (void)memset(&cost, 0, sizeof(cost));
    cost.input_tokens = 10U;
    cost.output_tokens = 5U;
    cost.total_cost = 15.0;
    CHECK(cog_session_set_cost(s, n2, &cost) == COG_OK);

    (void)memset(&timing, 0, sizeof(timing));
    timing.duration_ms = 100U;
    timing.ttft_ms = 20U;
    CHECK(cog_session_set_timing(s, n2, &timing) == COG_OK);

    /* Fork at n2 -> n2B (same immutable identity). */
    CHECK(cog_session_fork(s, n2, &n2b) == COG_OK);
    CHECK(n2b != NULL);
    CHECK(n2b == n2);
    CHECK(cog_node_data(n2b)->node_id == cog_node_data(n2)->node_id);
    CHECK(cog_session_active(s) == n2);
    CHECK(cog_session_set_cost(s, n2b, &cost) == COG_ERROR_INVALID_STATE);
    CHECK(cog_session_set_timing(s, n2b, &timing) == COG_ERROR_INVALID_STATE);

    /* Continue on both branches independently. */
    n3a = add_msg(s, n2, COG_MESSAGE_ROLE_ASSISTANT, "branch A");
    CHECK(n3a != NULL);
    n3b = add_msg(s, n2b, COG_MESSAGE_ROLE_ASSISTANT, "branch B");
    CHECK(n3b != NULL);
    CHECK(cog_node_parent(n3a) == cog_node_parent(n3b));
    CHECK(cog_node_data(n3a)->node_id != cog_node_data(n3b)->node_id);

    /* Independent cost on the two branch leaves. */
    (void)memset(&cost, 0, sizeof(cost));
    cost.input_tokens = 1U;
    cost.output_tokens = 1U;
    cost.total_cost = 2.0;
    CHECK(cog_session_set_cost(s, n3a, &cost) == COG_OK);

    (void)memset(&cost, 0, sizeof(cost));
    cost.input_tokens = 2U;
    cost.output_tokens = 2U;
    cost.total_cost = 4.0;
    CHECK(cog_session_set_cost(s, n3b, &cost) == COG_OK);

    /* Independent timing on the two branch leaves. */
    (void)memset(&timing, 0, sizeof(timing));
    timing.duration_ms = 50U;
    CHECK(cog_session_set_timing(s, n3a, &timing) == COG_OK);

    (void)memset(&timing, 0, sizeof(timing));
    timing.duration_ms = 200U;
    CHECK(cog_session_set_timing(s, n3b, &timing) == COG_OK);

    /* Independent histories. */
    nA = read_history(s, n3a, histA, 8U);
    nB = read_history(s, n3b, histB, 8U);
    CHECK_EQ_INT((long long)nA, 3LL);
    CHECK_EQ_INT((long long)nB, 3LL);

    /* Shared ancestors: both start with the same user message. */
    CHECK(histA[0].role == COG_MESSAGE_ROLE_USER);
    CHECK(histB[0].role == COG_MESSAGE_ROLE_USER);
    CHECK(memcmp(histA[0].content, "Explain X", 9U) == 0);
    CHECK(memcmp(histB[0].content, "Explain X", 9U) == 0);

    /* Divergence: branch A ends with "branch A", branch B with "branch B". */
    CHECK(histA[2].role == COG_MESSAGE_ROLE_ASSISTANT);
    CHECK(memcmp(histA[2].content, "branch A", 8U) == 0);
    CHECK(histB[2].role == COG_MESSAGE_ROLE_ASSISTANT);
    CHECK(memcmp(histB[2].content, "branch B", 8U) == 0);

    /* Independent cumulative cost: A = n2(15) + n3A(2) = 17;
     * B = shared n2(15) + n3B(4) = 19. */
    costA = cog_session_cumulative_cost(s, n3a);
    costB = cog_session_cumulative_cost(s, n3b);
    CHECK(costA > 16.9 && costA < 17.1);
    CHECK(costB > 18.9 && costB < 19.1);
    CHECK(costA != costB);

    /* Independent cumulative timing: A = 100 + 50 = 150; B = 100 + 200. */
    (void)memset(&cumA, 0, sizeof(cumA));
    (void)memset(&cumB, 0, sizeof(cumB));
    CHECK(cog_session_cumulative_timing(s, n3a, &cumA) == COG_OK);
    CHECK(cog_session_cumulative_timing(s, n3b, &cumB) == COG_OK);
    CHECK_EQ_INT((long long)cumA.duration_ms, 150LL);
    CHECK_EQ_INT((long long)cumB.duration_ms, 300LL);
    CHECK(cumA.duration_ms != cumB.duration_ms);

    cog_session_free(s);
}

int main(void)
{
    scenario_fork();

    if (failures != 0) {
        (void)fprintf(stderr, "test_14_conversation_fork: %d failure(s)\n",
                      failures);
        return 1;
    }
    (void)printf("test_14_conversation_fork: all checks passed\n");
    return 0;
}

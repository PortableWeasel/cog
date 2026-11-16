/** @file test_critic_mutations.c @brief Independent deterministic codec fault scan. */
#include "control.h"
#include "projection.h"
#include "check.h"

enum { BUFFER_BYTES = 1024, ITERATIONS = 120000 };
static uint32_t next_value(uint32_t *state)
{
    *state ^= *state << 13U; *state ^= *state >> 17U; *state ^= *state << 5U;
    return *state;
}
static void same(const void *actual, const void *before, size_t bytes)
{ CHECK(memcmp(actual, before, bytes) == 0); }
static void check_message(const unsigned char *bytes, size_t length, size_t cap)
{
    cog_cli_control_message_t view, before;
    unsigned char arena[BUFFER_BYTES], encoded[BUFFER_BYTES]; size_t required = SIZE_MAX;
    test_fill(&view, sizeof(view)); memcpy(&before, &view, sizeof(view)); test_fill(arena, sizeof(arena));
    cog_result_t rc = cog_cli_control_decode(bytes, length, &view, arena, cap, &required);
    if (rc != COG_OK) {
        same(&view, &before, sizeof(view)); test_unchanged(arena, sizeof(arena));
        if (rc != COG_ERROR_OVERFLOW) CHECK(required == SIZE_MAX);
        return;
    }
    CHECK(required <= cap); test_unchanged(arena + required, sizeof(arena) - required);
    size_t size = 0U; OK(cog_cli_control_encode(&view, encoded, sizeof(encoded), &size));
    CHECK(size == length); same(encoded, bytes, size);
}
static void check_command(const unsigned char *bytes, size_t length, size_t cap)
{
    cog_cli_command_t view, before;
    unsigned char arena[BUFFER_BYTES], encoded[BUFFER_BYTES]; size_t required = SIZE_MAX;
    test_fill(&view, sizeof(view)); memcpy(&before, &view, sizeof(view)); test_fill(arena, sizeof(arena));
    cog_result_t rc = cog_cli_command_decode(bytes, length, &view, arena, cap, &required);
    if (rc != COG_OK) {
        same(&view, &before, sizeof(view)); test_unchanged(arena, sizeof(arena));
        if (rc != COG_ERROR_OVERFLOW) CHECK(required == SIZE_MAX);
        return;
    }
    CHECK(required <= cap); test_unchanged(arena + required, sizeof(arena) - required);
    size_t size = 0U; OK(cog_cli_command_encode(&view, encoded, sizeof(encoded), &size));
    CHECK(size == length); same(encoded, bytes, size);
}
static void check_projection(const unsigned char *bytes, size_t length, size_t cap)
{
    cog_cli_projection_view_t view, before;
    cog_cli_display_item_t items[64]; cog_cli_agent_view_t agents[16]; cog_cli_policy_view_t policies[1];
    char arena[BUFFER_BYTES]; unsigned char encoded[BUFFER_BYTES];
    size_t ni = SIZE_MAX, na = SIZE_MAX, np = SIZE_MAX, nt = SIZE_MAX;
    test_fill(&view, sizeof(view)); memcpy(&before, &view, sizeof(view));
    test_fill(items, sizeof(items)); test_fill(agents, sizeof(agents));
    test_fill(policies, sizeof(policies)); test_fill(arena, sizeof(arena));
    cog_result_t rc = cog_cli_projection_decode(bytes, length, &view, items, 64U,
        agents, 16U, policies, 1U, arena, cap, &ni, &na, &np, &nt);
    if (rc != COG_OK) {
        same(&view, &before, sizeof(view)); test_unchanged(items, sizeof(items));
        test_unchanged(agents, sizeof(agents)); test_unchanged(policies, sizeof(policies));
        test_unchanged(arena, sizeof(arena));
        if (rc != COG_ERROR_OVERFLOW) CHECK(ni == SIZE_MAX && na == SIZE_MAX && np == SIZE_MAX && nt == SIZE_MAX);
        return;
    }
    CHECK(ni <= 64U && na <= 16U && np <= 1U && nt <= cap);
    test_unchanged(items + ni, (64U - ni) * sizeof(*items));
    test_unchanged(agents + na, (16U - na) * sizeof(*agents));
    test_unchanged(policies + np, (1U - np) * sizeof(*policies));
    test_unchanged(arena + nt, sizeof(arena) - nt);
    size_t size = 0U; OK(cog_cli_projection_encode(&view, encoded, sizeof(encoded), &size));
    CHECK(size == length); same(encoded, bytes, size);
}
static void check_receipt(const unsigned char *bytes, size_t length)
{
    cog_cli_control_receipt_t view, before; unsigned char encoded[55]; size_t size = 0U;
    test_fill(&view, sizeof(view)); memcpy(&before, &view, sizeof(view));
    cog_result_t rc = cog_cli_control_receipt_decode(bytes, length, &view);
    if (rc != COG_OK) { same(&view, &before, sizeof(view)); return; }
    OK(cog_cli_control_receipt_encode(&view, encoded, sizeof(encoded), &size));
    CHECK(size == length); same(encoded, bytes, size);
}
int main(int argc, char **argv)
{
    (void)test_negative_control(argc, argv);
    unsigned char seeds[4][BUFFER_BYTES] = {{0}}, bytes[BUFFER_BYTES]; size_t lengths[4];
    cog_cli_control_message_t message = {.kind = COG_CLI_CONTROL_CHILD_UPDATE,
        .lane = COG_CLI_LANE_UPDATE, .runtime_incarnation = 17U, .sender_incarnation = 23U,
        .wire_id = UINT64_MAX, .reply_to = 25U, .worker = {23U, 29U}, .owner_peer = 31U,
        .agent_id = 37U, .execution_epoch = 41U, .session_id = "child-\xe2\x98\x83",
        .payload = "\0opaque\xff", .payload_len = 8U};
    OK(cog_cli_control_encode(&message, seeds[0], BUFFER_BYTES, &lengths[0]));
    cog_cli_command_t command = {.kind = COG_CLI_COMMAND_POLICY, .observed_generation = 5U,
        .observed_sequence = 7U, .target_session_id = "root", .node_id = 11U,
        .nonce = UINT64_MAX, .call_ordinal = 15U, .decision = 1U};
    OK(cog_cli_command_encode(&command, seeds[1], BUFFER_BYTES, &lengths[1]));
    cog_cli_display_item_t items[] = {{3U, COG_MESSAGE_ROLE_SYSTEM, COG_MESSAGE_COMPLETE, "system", 6U},
        {5U, COG_MESSAGE_ROLE_USER, COG_MESSAGE_COMPLETE, "request", 7U},
        {0U, COG_MESSAGE_ROLE_ASSISTANT, COG_MESSAGE_INCOMPLETE, "partial\n\xe2\x98\x83", 11U}};
    cog_cli_agent_view_t agents[] = {{2U, 3U, 4U, 5U, "child-a", COG_AGENT_STATUS_RUNNING, COG_OK, "running", ""},
        {UINT64_MAX, 3U, 4U, 5U, "child-b", COG_AGENT_STATUS_FAILED, COG_ERROR_INDETERMINATE, "failed", "result"}};
    cog_cli_policy_view_t policy = {"root", 11U, 13U, 5U, UINT64_MAX, 15U, "command"};
    cog_cli_projection_view_t projection = {.display = {"root", 17U, 19U, items, 3U},
        .active_configuration = 5U, .published_configuration = 7U, .agents = agents,
        .agent_count = 2U, .policies = &policy, .policy_count = 1U};
    OK(cog_cli_projection_encode(&projection, seeds[2], BUFFER_BYTES, &lengths[2]));
    cog_cli_control_receipt_t receipt = {.disposition = COG_CLI_CONTROL_RECEIPT_RETRY,
        .parent_runtime_incarnation = 17U, .agent_id = 37U, .execution_epoch = 41U,
        .owner_peer = 31U, .wire_id = UINT64_MAX, .parent_accepted_sequence = UINT64_MAX,
        .accepted_terminal_status = COG_AGENT_STATUS_FAILED, .accepted_terminal_outcome = COG_ERROR_INDETERMINATE};
    OK(cog_cli_control_receipt_encode(&receipt, seeds[3], BUFFER_BYTES, &lengths[3]));
    uint32_t random = UINT32_C(0x75bada55);
    for (size_t iteration = 0U; iteration < ITERATIONS; ++iteration) {
        size_t which = iteration % 4U, length = lengths[which];
        memcpy(bytes, seeds[which], length);
        uint32_t mode = next_value(&random) % 5U;
        if (mode == 0U) length = next_value(&random) % (length + 1U);
        else if (mode == 1U) { bytes[length++] = (unsigned char)next_value(&random); }
        else if (mode != 2U) {
            size_t changes = 1U + next_value(&random) % 4U;
            for (size_t i = 0U; i < changes; ++i) bytes[next_value(&random) % length] ^= (unsigned char)(1U + next_value(&random) % 255U);
        }
        size_t cap = next_value(&random) % 3U == 0U ? 0U : BUFFER_BYTES;
        if (which == 0U) check_message(bytes, length, cap);
        else if (which == 1U) check_command(bytes, length, cap);
        else if (which == 2U) check_projection(bytes, length, cap);
        else check_receipt(bytes, length);
    }
    (void)puts("critic: 120000 deterministic mutations PASS");
    return 0;
}

#include "composition_internal.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void plan(void)
{
    cog_cli_options_t options = {.max_context_bytes = 1048576U,
        .max_tool_output_bytes = 65536U};
    cog_cli_host_runtime_config_t config = {0};
    cli_host_reservations_t bounds, untouched;
    memset(&bounds, 0xa5, sizeof(bounds)); untouched = bounds;
    assert(cli_host_plan(NULL, &options, &bounds) == COG_ERROR_NULL_PTR);
    assert(memcmp(&bounds, &untouched, sizeof(bounds)) == 0);
    assert(cli_host_plan(&config, &options, &bounds) == COG_OK);
    assert(bounds.executors == 6U && bounds.children == 4U);
    assert(bounds.display_bytes < bounds.projection_bytes);
    assert(bounds.lane_frame_bytes[COG_CLI_LANE_CANCEL - 1U] <
        bounds.lane_frame_bytes[COG_CLI_LANE_VIEW - 1U]);
    config.projection_bytes = 4096U; untouched = bounds;
    assert(cli_host_plan(&config, &options, &bounds) == COG_ERROR_LIMIT);
    assert(memcmp(&bounds, &untouched, sizeof(bounds)) == 0);
    config.projection_bytes = 0U; options.max_context_bytes = SIZE_MAX;
    assert(cli_host_plan(&config, &options, &bounds) == COG_ERROR_LIMIT);
    config.children = 17U;
    assert(cli_host_plan(&config, &options, &bounds) == COG_ERROR_INVALID_ARG);
}

static void mailbox(void)
{
    cli_executor_slot_t slots[4] = {0};
    cli_mailbox_entry_t entries[2] = {0};
    unsigned char text[64], receiver[256], sender[64], cache[256];
    cog_cli_host_runtime_t r = {.executors = slots, .bounds = {.executors = 4U}};
    slots[2].phase = CLI_SLOT_STARTED; slots[2].agent = 10U; slots[2].group = 4U;
    slots[3].phase = CLI_SLOT_STARTED; slots[3].agent = 11U; slots[3].group = 4U;
    strcpy(slots[2].parent_session, "root"); strcpy(slots[3].parent_session, "root");
    slots[3].mailbox = (cli_mailbox_t){.entries = entries, .entry_capacity = 2U,
        .text = text, .text_capacity = sizeof(text), .reply = {.data = cache, .capacity = sizeof(cache)}};
    cog_cli_delegation_context_t send_context = {.coordinator = &r, .agent = 10U, .group = 4U};
    cog_cli_delegation_context_t receive_context = {.coordinator = &r, .agent = 11U, .group = 4U};
    cli_delegation_handle_t send = {.context = &send_context, .operation = CLI_SEND_MESSAGE,
        .started = true, .destination = 11U, .message = "a\n\"", .message_length = 3U,
        .output = {.data = sender, .capacity = sizeof(sender)}};
    cli_delegation_handle_t receive = {.context = &receive_context, .operation = CLI_RECEIVE_MESSAGES,
        .started = true, .output = {.data = receiver, .capacity = sizeof(receiver)}};
    assert(cli_runtime_mailbox_step(&r, &send) == COG_OK);
    assert(slots[3].mailbox.count == 1U);
    assert(cli_runtime_mailbox_step(&r, &send) == COG_OK);
    assert(slots[3].mailbox.count == 1U); /* exact async retry is not a second send */
    send.output_cached = false; send.done = false;
    assert(cli_runtime_mailbox_step(&r, &send) == COG_OK);
    send.output_cached = false; send.done = false;
    assert(cli_runtime_mailbox_step(&r, &send) == COG_ERROR_AGAIN);
    assert(slots[3].mailbox.count == 2U);
    receive.output.capacity = 16U;
    assert(cli_runtime_mailbox_step(&r, &receive) == COG_ERROR_LIMIT);
    assert(slots[3].mailbox.count == 2U); /* no prefix copy/drop on capacity refusal */
    receive.output.capacity = sizeof(receiver);
    assert(cli_runtime_mailbox_step(&r, &receive) == COG_OK);
    assert(strcmp((const char *)receiver,
        "{\"messages\":[{\"agent_id\":\"10\",\"message\":\"a\\u000a\\\"\"},"
        "{\"agent_id\":\"10\",\"message\":\"a\\u000a\\\"\"}],\"more_pending\":false}") == 0);
    assert(slots[3].mailbox.count == 0U && slots[3].mailbox.text_used == 0U);
    assert(cli_runtime_mailbox_step(&r, &send) == COG_OK);
    assert(cli_runtime_mailbox_step(&r, &receive) == COG_OK);
    assert(slots[3].mailbox.count == 1U); /* cached receive cannot dequeue a new arrival */
    send.output_cached = false; slots[3].group = 5U;
    assert(cli_runtime_mailbox_step(&r, &send) == COG_ERROR_INVALID_STATE);
    slots[3].group = 4U; send.message_length = 4096U;
    assert(cli_runtime_mailbox_step(&r, &send) == COG_ERROR_LIMIT);
}

int main(void)
{
    plan(); mailbox(); puts("coordinator admission leaf: PASS"); return 0;
}

/** Actual coordinator receipt helpers under occupied client send storage. */
#include "../../src/host_runtime.c"
#include <assert.h>

int main(void)
{
    unsigned char send[512], original[512], reply[512], ack_cache[512], ack_reply[512];
    unsigned char hello_reply[512], wire[512], decode[512];
    cli_client_record_t client = {.used = true, .connected = true, .logical_lease = 7U,
        .send = {.data = send, .capacity = sizeof(send)}};
    cli_control_lane_t lanes[2U * CLI_HOST_LANES] = {0};
    cli_control_lane_t *lane = &lanes[CLI_HOST_LANES + COG_CLI_LANE_COMMAND - 1U];
    lane->final_original = (cli_host_bytes_t){.data = ack_cache, .capacity = sizeof(ack_cache)};
    lane->final_reply = (cli_host_bytes_t){.data = ack_reply, .capacity = sizeof(ack_reply)};
    cli_command_record_t record = {.phase = CLI_COMMAND_FINAL, .lease = 7U,
        .request_wire = 3U, .result_wire = 9U,
        .original = {.data = original, .capacity = sizeof(original)},
        .reply = {.data = reply, .capacity = sizeof(reply)}};
    original[5] = COG_CLI_LANE_COMMAND; record.original.length = 6U;
    cog_cli_host_runtime_t runtime = {.clients = &client, .lanes = lanes, .commands = &record,
        .bounds = {.command_records = 1U}, .runtime_incarnation = 1U, .next_nonce = 20U};
    strcpy(runtime.primary_session, "root");
    cog_cli_control_message_t ack = {.kind = COG_CLI_CONTROL_COMMAND_ACK, .lane = COG_CLI_LANE_COMMAND,
        .runtime_incarnation = 1U, .sender_incarnation = 7U, .wire_id = 4U, .reply_to = 9U,
        .session_id = "root"};
    size_t length;
    assert(cog_cli_control_encode(&ack, wire, sizeof(wire), &length) == COG_OK);
    cog_event_ipc_t event = {.msg_payload = wire, .msg_payload_len = length};
    memset(send, 0xab, 16U); client.send.length = 16U;
    assert(host_ui_ack(&runtime, &client, &event, &ack) == COG_ERROR_AGAIN);
    assert(record.phase == CLI_COMMAND_FREE && lane->pending_valid && lane->final_valid);
    uint64_t result_wire = lane->final_reply_wire;
    assert(host_ui_ack(&runtime, &client, &event, &ack) == COG_ERROR_AGAIN);
    assert(lane->pending_valid && lane->final_reply_wire == result_wire);
    cog_cli_control_message_t result;
    assert(cog_cli_control_decode(lane->final_reply.data, lane->final_reply.length,
        &result, decode, sizeof(decode), &length) == COG_OK);
    assert(result.reply_to == ack.wire_id && result.wire_id == result_wire);
    client.send.length = 0U;
    assert(host_client_queue(&client, &lane->final_reply) == COG_OK);
    assert(host_bytes_equal(&client.send, lane->final_reply.data, lane->final_reply.length));
    client.hello.final_valid = true; client.hello.result_wire = 30U;
    client.hello.ack_reply = (cli_host_bytes_t){.data = hello_reply, .capacity = sizeof(hello_reply)};
    ack.wire_id = 6U; ack.reply_to = 30U;
    assert(cog_cli_control_encode(&ack, wire, sizeof(wire), &length) == COG_OK);
    event.msg_payload_len = length;
    assert(host_ui_ack(&runtime, &client, &event, &ack) == COG_ERROR_AGAIN);
    assert(client.hello.pending && client.hello.acknowledged);
    assert(host_ui_ack(&runtime, &client, &event, &ack) == COG_ERROR_AGAIN);
    assert(client.hello.pending);
    puts("coordinator retained ACK pressure: PASS");
    return 0;
}

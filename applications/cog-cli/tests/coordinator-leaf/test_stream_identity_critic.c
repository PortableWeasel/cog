/** Real coordinator/projection/codec regression: receipts belong to complete stream keys. */
#include "../../src/host_runtime.c"
#include <assert.h>
int main(void)
{
    unsigned char buffers[12][1024] = {{0}};
    cli_control_lane_t lanes[2U * CLI_HOST_LANES] = {0};
    cli_command_record_t records[2] = {0};
    for (size_t i=0; i<2U; ++i) {
        records[i].original=(cli_host_bytes_t){buffers[i*3U],1024U,0U};
        records[i].reply=(cli_host_bytes_t){buffers[i*3U+1U],1024U,0U};
        records[i].decoded_storage=(cli_host_bytes_t){buffers[i*3U+2U],1024U,0U};
    }
    cli_client_record_t client={.used=true,.connected=true,.logical_lease=7U,
        .send={buffers[6],1024U,0U}, .hello={.acknowledged=true,
        .original={buffers[7],1024U,0U},.reply={buffers[8],1024U,0U},.ack_reply={buffers[9],1024U,0U}}};
    cog_cli_host_runtime_t r={.clients=&client,.commands=records,.lanes=lanes,
        .bounds={.clients=1U,.command_records=2U},.runtime_incarnation=7U,.next_nonce=20U,.next_client_lease=8U};
    strcpy(r.primary_session,"root");
    cog_cli_projection_config_t cfg={"root",7U,2U,4096U,2048U,512U,1U,16U,32U};
    assert(cog_cli_projection_new(&r.projection,&cfg)==COG_OK);
    cog_cli_display_item_t item={1U,COG_MESSAGE_ROLE_USER,COG_MESSAGE_COMPLETE,"hello",5U};
    cog_cli_projection_view_t view={{"root",7U,1U,&item,1U},1U,1U,NULL,0U,NULL,0U};
    uint64_t sequence; assert(cog_cli_projection_commit(r.projection,&view,&sequence)==COG_OK);
    unsigned char body[256],wire[512]; size_t n,wn;
    cog_cli_command_t command={.kind=COG_CLI_COMMAND_RESYNC,.observed_generation=7U,.observed_sequence=1U};
    assert(cog_cli_command_encode(&command,body,sizeof(body),&n)==COG_OK);
    cog_cli_control_message_t m={.kind=COG_CLI_CONTROL_UI_COMMAND,.lane=COG_CLI_LANE_COMMAND,
        .runtime_incarnation=7U,.sender_incarnation=7U,.wire_id=3U,.session_id="root",.payload=body,.payload_len=n};
    assert(cog_cli_control_encode(&m,wire,sizeof(wire),&wn)==COG_OK);
    cog_event_ipc_t e={.msg_payload=wire,.msg_payload_len=wn,.peer=12U};
    assert(host_ui_command(&r,&client,&e,&m)==COG_OK);
    command.kind=COG_CLI_COMMAND_CANCEL; command.observed_generation=7U; command.observed_sequence=1U;
    assert(cog_cli_command_encode(&command,body,sizeof(body),&n)==COG_OK);
    m.lane=COG_CLI_LANE_CANCEL; m.payload_len=n;
    assert(cog_cli_control_encode(&m,wire,sizeof(wire),&wn)==COG_OK); e.msg_payload_len=wn;
    cog_result_t lane_rc=host_ui_command(&r,&client,&e,&m);
    printf("independent COMMAND/CANCEL wire3: actual=%d expected=%d\n",(int)lane_rc,(int)COG_OK);
    /* Control: a distinct wire ID follows the valid refusal path. */
    m.wire_id=4U;
    assert(cog_cli_control_encode(&m,wire,sizeof(wire),&wn)==COG_OK); e.msg_payload_len=wn;
    assert(host_ui_command(&r,&client,&e,&m)==COG_OK);
    assert(records[1].phase==CLI_COMMAND_FINAL && records[1].result==COG_ERROR_DENIED);
    /* Expire old lease, then use a factual fresh HELLO on the same physical cell. */
    client.connected=false; client.send.length=0U; client.receipts_until_ms=1U; r.now_ms=2U;
    lanes[CLI_HOST_LANES+COG_CLI_LANE_COMMAND-1U].retired_through=100U;
    assert(host_client_slice(&r,&client)==COG_ERROR_AGAIN); assert(!client.used);
    unsigned char hello[18]={1U,2U};
    m=(cog_cli_control_message_t){.kind=COG_CLI_CONTROL_UI_HELLO,.lane=COG_CLI_LANE_COMMAND,
        .sender_incarnation=99U,.wire_id=1U,.session_id="",.payload=hello,.payload_len=18U};
    assert(cog_cli_control_encode(&m,wire,sizeof(wire),&wn)==COG_OK); e.msg_payload_len=wn;
    assert(host_ui_hello(&r,&e,&m)==COG_OK); assert(client.logical_lease==8U);
    command=(cog_cli_command_t){.kind=COG_CLI_COMMAND_RESYNC,.observed_generation=7U,.observed_sequence=1U};
    assert(cog_cli_command_encode(&command,body,sizeof(body),&n)==COG_OK);
    m=(cog_cli_control_message_t){.kind=COG_CLI_CONTROL_UI_COMMAND,.lane=COG_CLI_LANE_COMMAND,
        .runtime_incarnation=7U,.sender_incarnation=8U,.wire_id=3U,.session_id="root",.payload=body,.payload_len=n};
    assert(cog_cli_control_encode(&m,wire,sizeof(wire),&wn)==COG_OK); e.msg_payload_len=wn;
    /* Complete the actual HELLO ACK before requesting RESYNC. */
    cog_cli_control_message_t ack={.kind=COG_CLI_CONTROL_COMMAND_ACK,.lane=COG_CLI_LANE_COMMAND,
        .runtime_incarnation=7U,.sender_incarnation=8U,.wire_id=2U,.reply_to=client.hello.result_wire,.session_id="root"};
    unsigned char aw[512]; size_t an; assert(cog_cli_control_encode(&ack,aw,sizeof(aw),&an)==COG_OK);
    cog_event_ipc_t ae={.msg_payload=aw,.msg_payload_len=an}; client.send.length=0U;
    assert(host_ui_ack(&r,&client,&ae,&ack)==COG_OK); client.send.length=0U;
    cog_result_t lease_rc=host_ui_command(&r,&client,&e,&m);
    printf("new lease8 wire3 after lease7 watermark100: actual=%d expected=%d\n",(int)lease_rc,(int)COG_OK);
    cog_cli_projection_free(r.projection);
    return lane_rc==COG_OK && lease_rc==COG_OK ? 0 : 1;
}

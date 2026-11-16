/** Actual IPC-owned confirmation lifetime and terminal receipt correlation. */
#include "../src/worker_host.c"
#include <assert.h>
#include <stdio.h>

static void service(cog_ipc_endpoint_t *a,cog_ipc_endpoint_t *b)
{
    uint64_t deadline;
    for(unsigned i=0U;i<8U;++i) {
        cog_result_t rc=cog_ipc_endpoint_service(a,1U,&deadline);
        assert(rc==COG_OK || rc==COG_ERROR_AGAIN);
        rc=cog_ipc_endpoint_service(b,1U,&deadline);
        assert(rc==COG_OK || rc==COG_ERROR_AGAIN);
    }
}
static void bridge_payloads(void)
{
    unsigned char body[256],output[512];
    cog_cli_delegation_context_t context={0};context.mailbox_entries=2U;
    cli_delegation_handle_t handle={0};handle.context=&context;handle.request.count=1U;
    cli_wire_writer_t encoded={body,0U,true};
    cli_wire_put(&encoded,31U,8U);cli_wire_put(&encoded,0U,4U);cli_wire_put(&encoded,1U,2U);
    cli_wire_put(&encoded,1U,1U);cli_wire_put(&encoded,5U,8U);cli_wire_put(&encoded,1U,8U);
    cli_wire_put(&encoded,COG_AGENT_STATUS_COMPLETED,1U);cli_wire_put(&encoded,0U,4U);
    cli_wire_string(&encoded,"child",5U);cli_wire_string(&encoded,"ok\n",3U);cli_wire_put(&encoded,0U,1U);
    cog_cli_control_message_t message={0};message.kind=COG_CLI_CONTROL_DELEGATE_RESULT;
    message.payload=body;message.payload_len=encoded.used;
    cog_result_t outcome;bool effects;uint64_t group;
    cli_wire_writer_t count={NULL,0U,true};
    assert(bridge_output(&handle,&message,&count,&outcome,&effects,&group)==COG_OK);
    assert(outcome==COG_OK && !effects && group==31U && count.used<sizeof(output));
    cli_wire_writer_t write={output,0U,true};
    assert(bridge_output(&handle,&message,&write,&outcome,&effects,&group)==COG_OK && write.used==count.used);
    output[write.used]=0U;assert(strstr((const char *)output,"ok\\u000a")!=NULL);
    body[encoded.used-1U]=1U;count=(cli_wire_writer_t){NULL,0U,true};
    assert(bridge_output(&handle,&message,&count,&outcome,&effects,&group)==COG_ERROR_PROTOCOL);
    body[encoded.used-1U]=0U;--message.payload_len;count=(cli_wire_writer_t){NULL,0U,true};
    assert(bridge_output(&handle,&message,&count,&outcome,&effects,&group)==COG_ERROR_PROTOCOL);
    encoded=(cli_wire_writer_t){body,0U,true};
    cli_wire_put(&encoded,(uint32_t)COG_ERROR_AGAIN,4U);cli_wire_put(&encoded,1U,1U);
    message.kind=COG_CLI_CONTROL_MESSAGE_RESULT;message.payload_len=encoded.used;
    count=(cli_wire_writer_t){NULL,0U,true};
    assert(bridge_output(&handle,&message,&count,&outcome,&effects,&group)==COG_ERROR_AGAIN);
    assert(count.used==0U);
    encoded=(cli_wire_writer_t){body,0U,true};cli_wire_put(&encoded,0U,4U);
    cli_wire_put(&encoded,0U,1U);cli_wire_put(&encoded,1U,2U);cli_wire_put(&encoded,5U,8U);
    cli_wire_string(&encoded,"hello",5U);message.kind=COG_CLI_CONTROL_MAILBOX_RESULT;message.payload_len=encoded.used;
    count=(cli_wire_writer_t){NULL,0U,true};
    assert(bridge_output(&handle,&message,&count,&outcome,&effects,&group)==COG_OK);
    context.mailbox_entries=0U;count=(cli_wire_writer_t){NULL,0U,true};
    assert(bridge_output(&handle,&message,&count,&outcome,&effects,&group)==COG_ERROR_PROTOCOL);
}
int main(int argc,char **argv)
{
    assert(argc==2);bridge_payloads();
    cog_ipc_endpoint_config_t cfg={.max_frame_bytes=512U,.receive_frames=1U,.send_frames=1U,.service_quantum=1U};
    cog_ipc_endpoint_t *listener=NULL,*sender=NULL,*receiver=NULL;
    assert(cog_ipc_endpoint_new_ex(&listener,&cfg)==COG_OK);
    assert(cog_ipc_endpoint_new_ex(&sender,&cfg)==COG_OK);
    assert(cog_ipc_endpoint_listen(listener,argv[1])==COG_OK);
    cog_result_t rc=cog_ipc_endpoint_connect(sender,argv[1],NULL);
    assert(rc==COG_OK || rc==COG_ERROR_AGAIN);
    assert(cog_ipc_endpoint_accept(listener,&receiver,NULL)==COG_OK);
    service(sender,receiver);
    cog_cli_worker_host_t host={0};host.parent=sender;host.runtime_incarnation=7U;
    host.fixed.worker=(cog_worker_id_t){11U,3U};
    cli_control_lane_t lanes[CLI_HOST_LANES]={0};
    unsigned char pending[512],original[512];
    cli_control_lane_t *lane=&lanes[COG_CLI_LANE_UPDATE-1U];
    lane->pending=(cli_host_bytes_t){pending,sizeof(pending),0U};
    lane->final_original=(cli_host_bytes_t){original,sizeof(original),0U};
    lane->next_wire=UINT64_MAX;lane->final_valid=true;lane->final_reply_wire=29U;
    cli_executor_slot_t slot={0};slot.outbound=lanes;slot.agent=17U;slot.attempt=2U;
    slot.owner_peer=9U;strcpy(slot.session,"child");slot.local_done=true;slot.terminal_acked=true;
    slot.receipt.disposition=COG_CLI_CONTROL_RECEIPT_ACCEPTED;
    slot.receipt.accepted_terminal_status=COG_AGENT_STATUS_COMPLETED;
    /* Distinct receipt outer wire29 answers worker update wire19. Exact lost
       receipt replay is harmless; changed bytes under29 are a conflict. */
    unsigned char body[COG_CLI_CONTROL_RECEIPT_PAYLOAD_BYTES],raw[512];size_t n;
    cog_cli_control_receipt_t proof={.parent_runtime_incarnation=7U,.agent_id=17U,.execution_epoch=2U,
        .owner_peer=9U,.wire_id=19U,.parent_accepted_sequence=1U,
        .disposition=COG_CLI_CONTROL_RECEIPT_TERMINAL_FIXED,
        .accepted_terminal_status=COG_AGENT_STATUS_COMPLETED,.accepted_terminal_outcome=COG_OK};
    assert(cog_cli_control_receipt_encode(&proof,body,sizeof(body),&n)==COG_OK);
    cog_cli_control_message_t reply={COG_CLI_CONTROL_UPDATE_RECEIPT,COG_CLI_LANE_UPDATE,
        7U,7U,29U,19U,host.fixed.worker,9U,17U,2U,"child",body,n};
    assert(cog_cli_control_encode(&reply,raw,sizeof(raw),&n)==COG_OK);
    host.receive_secret=raw;slot.pending_update_wire=19U;slot.pending_update_sequence=2U;
    lane->final_valid=false;lane->pending_valid=true;lane->pending.length=1U;
    assert(update_receipt(&host,&slot,&reply,n)==COG_OK);
    assert(slot.terminal_acked && !lane->pending_valid && slot.pending_update_wire==0U);
    assert(lane->final_reply_wire==29U && lane->final_original.length==n);
    assert(update_receipt(&host,&slot,&reply,n)==COG_OK);
    raw[n-1U]^=1U;assert(update_receipt(&host,&slot,&reply,n)==COG_ERROR_PROTOCOL);raw[n-1U]^=1U;
    /* RETRY and cancellation suppression never manufacture consumption proof. */
    slot.receipt.disposition=COG_CLI_CONTROL_RECEIPT_RETRY;
    assert(confirm_terminal(&host,&slot)==COG_ERROR_INVALID_STATE);
    slot.receipt.disposition=COG_CLI_CONTROL_RECEIPT_CANCEL_CONSUMED;
    assert(confirm_terminal(&host,&slot)==COG_ERROR_INVALID_STATE);
    slot.receipt.disposition=COG_CLI_CONTROL_RECEIPT_ACCEPTED;
    slot.terminal_acked=false;
    assert(confirm_terminal(&host,&slot)==COG_ERROR_INVALID_STATE);slot.terminal_acked=true;
    /* Occupy the actual endpoint's one send descriptor. No fake send callback. */
    unsigned char filler=42U;cog_ipc_message_t frame={&filler,1U,1U};
    assert(cog_ipc_endpoint_send(sender,&frame)==COG_OK);
    assert(confirm_terminal(&host,&slot)==COG_ERROR_AGAIN);
    assert(lane->next_wire==0U && lane->pending_valid);
    unsigned char immutable[512];size_t retained=lane->pending.length;
    memcpy(immutable,pending,retained);
    assert(confirm_terminal(&host,&slot)==COG_ERROR_AGAIN);
    assert(lane->pending.length==retained && memcmp(immutable,pending,retained)==0);
    service(sender,receiver);
    unsigned char rx[512];cog_ipc_message_t received={rx,0U,sizeof(rx)};
    assert(cog_ipc_endpoint_recv(receiver,&received)==COG_OK && received.len==1U);
    assert(confirm_terminal(&host,&slot)==COG_OK);
    /* Accepted copied-send owns independent bytes even after local arena wipe. */
    memset(pending,0,sizeof(pending));service(sender,receiver);
    received.len=0U;assert(cog_ipc_endpoint_recv(receiver,&received)==COG_OK);
    cog_cli_control_message_t message;unsigned char decoded[512];size_t needed;
    assert(cog_cli_control_decode(rx,received.len,&message,decoded,sizeof(decoded),&needed)==COG_OK);
    assert(message.kind==COG_CLI_CONTROL_COMMAND_ACK && message.lane==COG_CLI_LANE_UPDATE);
    assert(message.reply_to==29U && message.wire_id==UINT64_MAX && message.payload_len==0U);
    assert(message.agent_id==17U && message.execution_epoch==2U && message.owner_peer==9U);
    assert(message.runtime_incarnation==7U && message.sender_incarnation==11U);
    assert(strcmp(message.session_id,"child")==0);
    assert(cog_ipc_endpoint_free(sender)==COG_OK);
    assert(cog_ipc_endpoint_free(receiver)==COG_OK);
    assert(cog_ipc_endpoint_free(listener)==COG_OK);
    puts("worker receipt real IPC ownership PASS");return 0;
}

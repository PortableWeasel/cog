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
    /* Continuous native and application contenders share actual one-frame TX.
       Alternate single native I/O units, as the production outer step does. */
    cli_control_lane_t incoming[CLI_HOST_LANES]={0};slot.inbound=incoming;
    slot.terminal_acked=false;host.now_ms=1U;
    unsigned char native_bytes[WORKER_NATIVE_BYTES];
    host.native_control[1]=(cli_host_bytes_t){native_bytes,sizeof(native_bytes),0U};
    assert(native_frame(&host,1U,COG_WORKER_HEARTBEAT,0U,0U,COG_OK)==COG_OK);
    assert(cog_cli_control_encode(&message,pending,sizeof(pending),&lane->pending.length)==COG_OK);
    lane->pending_valid=true;
    unsigned native_seen=0U,application_seen=0U;uint64_t next;
    for(size_t turn=0U;turn<72U;++turn) {
        bool progress=false;host.native_pending[1]=true;
        send_transport_turn(&host,&slot,COG_CLI_LANE_UPDATE-1U,turn%3U,&progress);
        rc=cog_ipc_internal_service_one(sender,1U,&next);assert(rc==COG_OK || rc==COG_ERROR_AGAIN);
        rc=cog_ipc_internal_service_one(receiver,1U,&next);assert(rc==COG_OK || rc==COG_ERROR_AGAIN);
        received.len=0U;rc=cog_ipc_endpoint_recv(receiver,&received);
        if(rc==COG_OK) {
            if(memcmp(rx,"CGW",3U)==0) ++native_seen;
            else { assert(memcmp(rx,"CGL",3U)==0);++application_seen; }
        } else assert(rc==COG_ERROR_AGAIN);
    }
    assert(native_seen>0U && application_seen>0U);
    /* A retransmitted publication can produce two identical coordinator
       replies. Retiring the first must make the second harmless stale input. */
    unsigned char incoming_wire[512], incoming_decode[512], response_bytes[512];
    host.receive_secret=incoming_wire;host.receive_capacity=sizeof(incoming_wire);
    host.decode_secret=incoming_decode;host.decode_capacity=sizeof(incoming_decode);
    host.executors=&slot;host.executor_capacity=1U;host.authenticated=true;host.startup_ready=true;
    slot.phase=CLI_SLOT_STARTED;slot.agent=0U;slot.attempt=0U;
    cog_cli_options_t options;char program[]="receipt",mock[]="--mock",no_store[]="--no-store";
    char *args[]={program,mock,no_store};int code=0;
    assert(cog_cli_options_parse(&options,3,args,&code)==COG_OK);options.config_path="/dev/null";
    cog_cli_configuration_t *configuration=NULL;
    assert(cog_cli_configuration_new(&configuration,&options,1U)==COG_OK);
    cog_cli_execution_admission_t admission={.kind=COG_CLI_EXECUTION_ROOT,.configuration=configuration,.configuration_generation=1U};
    assert(cog_cli_execution_new(&slot.app,&admission)==COG_OK);
    strcpy(slot.session,cog_session_id(slot.app->session));
    assert(cog_ipc_endpoint_get_peer_id(sender,&slot.owner_peer)==COG_OK);
    cli_control_lane_t *policy=&lanes[COG_CLI_LANE_POLICY-1U];
    policy->pending_valid=true;policy->pending_wire=42U;
    unsigned char accepted[21]={0};accepted[4]=2U;
    cog_cli_control_message_t answer={.kind=COG_CLI_CONTROL_COMMAND_RESULT,.lane=COG_CLI_LANE_POLICY,
        .runtime_incarnation=7U,.sender_incarnation=7U,.wire_id=51U,.reply_to=42U,
        .worker=host.fixed.worker,.owner_peer=slot.owner_peer,.session_id=slot.session,
        .payload=accepted,.payload_len=sizeof(accepted)};
    size_t answer_length;assert(cog_cli_control_encode(&answer,response_bytes,sizeof(response_bytes),&answer_length)==COG_OK);
    for(unsigned repeat=0U;repeat<2U;++repeat) {
        cog_ipc_message_t packet={response_bytes,answer_length,sizeof(response_bytes)};
        assert(cog_ipc_endpoint_send(receiver,&packet)==COG_OK);service(sender,receiver);
        bool made_progress=false;
        cog_result_t result=parent_step(&host,&made_progress,&next);
        (void)retain_error(&host,result);
        printf("policy receipt copy %u: rc=%d shutdown=%d\n",repeat,(int)result,(int)host.shutdown);
        if(repeat==0U) assert(!policy->pending_valid && policy->retired_through==42U && !host.shutdown);
    }
    assert(!host.shutdown);
    /* A new pending policy must not be consumed by an old valid receipt. */
    policy->pending_valid=true;policy->pending_wire=43U;
    assert(policy_result(&slot,&answer)==COG_ERROR_AGAIN && policy->pending_valid);
    answer.reply_to=44U;assert(policy_result(&slot,&answer)==COG_ERROR_INVALID_STATE);
    answer.reply_to=42U;answer.payload_len=20U;assert(policy_result(&slot,&answer)==COG_ERROR_PROTOCOL);
    answer.payload_len=sizeof(accepted);policy->pending_valid=false;
    assert(cog_cli_execution_close(&slot.app,COG_OK)==COG_OK);
    slot.phase=CLI_SLOT_RETAINED;
    /* A retired VIEW receipt remains harmless after factual app disposal. */
    unsigned char view_bytes[40];cli_wire_writer_t vw={view_bytes,0U,true};
    cli_wire_put(&vw,3U,8U);cli_wire_put(&vw,1U,8U);cli_wire_put(&vw,2U,8U);
    cli_wire_put(&vw,5U,8U);cli_wire_put(&vw,6U,8U);
    lanes[COG_CLI_LANE_VIEW-1U].retired_through=61U;
    answer.kind=COG_CLI_CONTROL_VIEW_COMMIT;answer.lane=COG_CLI_LANE_VIEW;
    answer.wire_id=62U;answer.reply_to=61U;answer.payload=view_bytes;answer.payload_len=sizeof(view_bytes);
    assert(cog_cli_control_encode(&answer,response_bytes,sizeof(response_bytes),&answer_length)==COG_OK);
    for(unsigned repeat=0U;repeat<2U;++repeat) {
        cog_ipc_message_t packet={response_bytes,answer_length,sizeof(response_bytes)};
        assert(cog_ipc_endpoint_send(receiver,&packet)==COG_OK);service(sender,receiver);
        bool progress=false;rc=parent_step(&host,&progress,&next);(void)retain_error(&host,rc);
        assert(!host.shutdown && lanes[COG_CLI_LANE_VIEW-1U].retired_through==61U);
    }
    answer.payload_len=39U;assert(view_commit(&slot,&answer)==COG_ERROR_PROTOCOL);
    answer.payload_len=40U;answer.reply_to=63U;assert(view_commit(&slot,&answer)==COG_ERROR_INVALID_STATE);
    /* COMMAND ACK consumes our response counter, never the peer request ID. */
    unsigned char cmd_original[512],cmd_final[512];
    cli_control_lane_t *command=&incoming[COG_CLI_LANE_COMMAND-1U];
    command->final_original=(cli_host_bytes_t){cmd_original,sizeof(cmd_original),0U};
    command->final_reply=(cli_host_bytes_t){cmd_final,sizeof(cmd_final),0U};
    command->final_valid=true;command->final_answered_wire=60U;command->final_reply_wire=70U;
    lanes[COG_CLI_LANE_COMMAND-1U].next_wire=71U;
    answer.kind=COG_CLI_CONTROL_COMMAND_ACK;answer.lane=COG_CLI_LANE_COMMAND;
    answer.wire_id=80U;answer.reply_to=70U;answer.payload=NULL;answer.payload_len=0U;
    for(unsigned repeat=0U;repeat<3U;++repeat) {
        if(repeat==2U) answer.wire_id=81U; /* fresh ACK of already consumed result */
        assert(cog_cli_control_encode(&answer,response_bytes,sizeof(response_bytes),&answer_length)==COG_OK);
        cog_ipc_message_t packet={response_bytes,answer_length,sizeof(response_bytes)};
        assert(cog_ipc_endpoint_send(receiver,&packet)==COG_OK);service(sender,receiver);
        bool progress=false;rc=parent_step(&host,&progress,&next);(void)retain_error(&host,rc);
        assert(!host.shutdown && lanes[COG_CLI_LANE_COMMAND-1U].retired_through==70U);
    }
    answer.reply_to=72U;assert(command_ack(&host,&slot,&answer,answer_length)==COG_ERROR_INVALID_STATE);
    answer.reply_to=70U;answer.payload=accepted;answer.payload_len=1U;
    assert(command_ack(&host,&slot,&answer,answer_length)==COG_ERROR_PROTOCOL);
    /* A late valid terminal disposition for a retired child is nonfatal even
       after physical slot reuse; a live conflicting identity is still refused. */
    host.opened_agent_high_watermark=17U;
    proof.owner_peer=slot.owner_peer;
    assert(cog_cli_control_receipt_encode(&proof,body,sizeof(body),&n)==COG_OK);
    answer.kind=COG_CLI_CONTROL_UPDATE_RECEIPT;answer.lane=COG_CLI_LANE_UPDATE;
    answer.wire_id=29U;answer.reply_to=19U;answer.agent_id=17U;answer.execution_epoch=2U;
    answer.session_id="old-child";answer.payload=body;answer.payload_len=n;
    assert(cog_cli_control_encode(&answer,response_bytes,sizeof(response_bytes),&answer_length)==COG_OK);
    cog_ipc_message_t retired_packet={response_bytes,answer_length,sizeof(response_bytes)};
    assert(cog_ipc_endpoint_send(receiver,&retired_packet)==COG_OK);service(sender,receiver);
    bool progress=false;rc=parent_step(&host,&progress,&next);(void)retain_error(&host,rc);
    assert(!host.shutdown);
    memcpy(incoming_wire,response_bytes,answer_length);slot.agent=17U;slot.attempt=3U;
    assert(application_receive(&host,answer_length)==COG_ERROR_INVALID_STATE);
    slot.agent=0U;slot.attempt=0U;
    answer.payload_len=54U;
    assert(cog_cli_control_encode(&answer,incoming_wire,sizeof(incoming_wire),&answer_length)==COG_OK);
    assert(application_receive(&host,answer_length)==COG_ERROR_PROTOCOL);
    bool stale_safe=!host.shutdown;
    assert(cog_ipc_endpoint_free(sender)==COG_OK);
    assert(cog_ipc_endpoint_free(receiver)==COG_OK);
    assert(cog_ipc_endpoint_free(listener)==COG_OK);
    puts("worker receipt real IPC ownership completed");return stale_safe?0:1;
}

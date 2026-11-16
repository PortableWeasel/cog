/** Real orchestrator send/Q endpoint regression; synthetic managed scalar record.
 * Native admission is separately exercised by the installed native fixtures. */
#include "../../src/host_runtime.c"
#include "../../../../libraries/cog-orchestrator/src/orchestrator_layout.h"
#include <assert.h>
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
static void copied_schedule(cog_ipc_endpoint_t *sender,cog_ipc_endpoint_t *receiver)
{
    orch_worker_t managed={.used=true,.ready=true,.id={1U,1U},.native_state=ORCH_NATIVE_OWNED,.link=0U};
    orch_link_t link={.endpoint=sender};
    cog_orchestrator_t orch={.workers=&managed,.links=&link,.bounds={.workers=1U,.frame_bytes=512U},
        .config={.health_interval_ms=1000U}};
    cog_cli_host_runtime_t worker={.orchestrator=&orch,.now_ms=10U};
    unsigned char data[4]={1U,2U,3U,4U},received[16];
    cli_host_bytes_t bytes={data,sizeof(data),sizeof(data)};
    uint64_t copied=0U,due=0U;
    assert(host_copied_send(&worker,managed.id,&bytes,1U,&copied,&due,false)==COG_OK);
    assert(copied==1U && due==1010U);
    for(unsigned i=0U;i<100U;++i) assert(host_copied_send(&worker,managed.id,&bytes,1U,&copied,&due,false)==COG_ERROR_AGAIN);
    /* A new wire is eligible but full TX accepts nothing. */
    assert(host_copied_send(&worker,managed.id,&bytes,2U,&copied,&due,false)==COG_ERROR_AGAIN);
    assert(copied==1U && due==1010U);
    service(sender,receiver);
    cog_ipc_message_t incoming={received,0U,sizeof(received)};
    assert(cog_ipc_endpoint_recv(receiver,&incoming)==COG_OK && incoming.len==sizeof(data));
    assert(memcmp(received,data,sizeof(data))==0);
    assert(cog_ipc_endpoint_recv(receiver,&incoming)==COG_ERROR_AGAIN);
    worker.now_ms=1009U;assert(host_copied_send(&worker,managed.id,&bytes,1U,&copied,&due,false)==COG_ERROR_AGAIN);
    worker.now_ms=1010U;assert(host_copied_send(&worker,managed.id,&bytes,1U,&copied,&due,false)==COG_OK);
    assert(due==2010U);service(sender,receiver);
    assert(cog_ipc_endpoint_recv(receiver,&incoming)==COG_OK);
    assert(cog_ipc_endpoint_recv(receiver,&incoming)==COG_ERROR_AGAIN);
    /* A retry while the exact managed worker is degraded is not a protocol error. */
    orch.config.unhealthy_threshold=3U;orch.now_ms=1000U;worker.now_ms=2010U;
    assert(host_copied_send(&worker,managed.id,&bytes,1U,&copied,&due,false)==COG_ERROR_AGAIN);
    assert(copied==1U && due==2010U);
    orch.now_ms=0U;
    /* One-way wire needs no future time, even at clock exhaustion. */
    worker.now_ms=UINT64_MAX;
    assert(host_copied_send(&worker,managed.id,&bytes,2U,&copied,&due,false)==COG_ERROR_LIMIT);
    assert(copied==1U && due==2010U);
    assert(host_copied_send(&worker,managed.id,&bytes,2U,&copied,&due,true)==COG_OK);
    service(sender,receiver);assert(cog_ipc_endpoint_recv(receiver,&incoming)==COG_OK);
    assert(host_copied_send(&worker,managed.id,&bytes,2U,&copied,&due,true)==COG_ERROR_AGAIN);
    assert(cog_ipc_endpoint_recv(receiver,&incoming)==COG_ERROR_AGAIN);
    cli_control_lane_t inbound[CLI_HOST_LANES]={0},outbound[CLI_HOST_LANES]={0};
    cli_executor_slot_t slot={.phase=CLI_SLOT_STARTED,.owner_peer=1U,.worker=managed.id,.inbound=inbound,.outbound=outbound};
    worker.executors=&slot;worker.bounds.executors=1U;worker.now_ms=2000U;
    cli_control_lane_t *reply=&inbound[0];reply->final_valid=true;reply->final_reply_wire=3U;
    reply->final_reply=bytes;reply->pending_valid=true;reply->pending_wire=99U;
    assert(host_reply_send(&worker,managed.id,reply,&reply->final_reply)==COG_OK);
    assert(reply->pending_reply_wire==0U && reply->pending_valid);
    assert(host_reply_send(&worker,managed.id,reply,&reply->final_reply)==COG_ERROR_AGAIN);
    assert(reply->pending_reply_wire==3U);
    service(sender,receiver);assert(cog_ipc_endpoint_recv(receiver,&incoming)==COG_OK);
    cog_cli_host_runtime_step_t step={.next_deadline_ms=UINT64_MAX};
    assert(host_retained_sends(&worker,&step)==COG_OK && step.next_deadline_ms==3000U);
    worker.now_ms=3000U;assert(host_retained_sends(&worker,&step)==COG_OK);
    assert(reply->pending_reply_wire==0U && reply->pending_valid && reply->pending_wire==99U);
    service(sender,receiver);assert(cog_ipc_endpoint_recv(receiver,&incoming)==COG_OK);
    assert(cog_ipc_endpoint_recv(receiver,&incoming)==COG_ERROR_AGAIN);
    /* A first reply under full TX remains armed without borrowing semantic ownership. */
    cog_ipc_message_t filler={data,sizeof(data),sizeof(data)};
    assert(cog_ipc_endpoint_send(sender,&filler)==COG_OK);
    reply->final_reply_wire=4U;
    assert(host_reply_send(&worker,managed.id,reply,&reply->final_reply)==COG_ERROR_AGAIN);
    assert(reply->pending_reply_wire==4U && reply->copied_wire==3U && reply->pending_valid);
    service(sender,receiver);assert(cog_ipc_endpoint_recv(receiver,&incoming)==COG_OK);
    assert(host_retained_sends(&worker,&step)==COG_OK && reply->pending_reply_wire==0U);
    service(sender,receiver);assert(cog_ipc_endpoint_recv(receiver,&incoming)==COG_OK);
    /* A new final supersedes a delayed provisional; old reply cannot reset cooldown. */
    reply->provisional_wire=5U;reply->provisional_reply=bytes;
    assert(host_reply_send(&worker,managed.id,reply,&reply->provisional_reply)==COG_OK);
    assert(host_reply_send(&worker,managed.id,reply,&reply->provisional_reply)==COG_ERROR_AGAIN);
    reply->final_reply_wire=6U;
    assert(host_reply_send(&worker,managed.id,reply,&reply->final_reply)==COG_ERROR_AGAIN);
    assert(reply->pending_reply_wire==6U);
    service(sender,receiver);assert(cog_ipc_endpoint_recv(receiver,&incoming)==COG_OK);
    assert(host_retained_sends(&worker,&step)==COG_OK && reply->copied_wire==6U);
    assert(host_reply_send(&worker,managed.id,reply,&reply->provisional_reply)==COG_ERROR_AGAIN);
    assert(reply->pending_reply_wire==0U && reply->copied_wire==6U);
    service(sender,receiver);assert(cog_ipc_endpoint_recv(receiver,&incoming)==COG_OK);
    worker.now_ms=1U;
    assert(host_reply_send(&worker,managed.id,reply,&reply->final_reply)==COG_ERROR_AGAIN);
    assert(reply->next_send_ms==4000U);
    step.next_deadline_ms=2500U;host_send_due(&worker,&step,3U,3U,4000U,false);
    assert(step.next_deadline_ms==2500U);
}
int main(int argc,char **argv)
{
    assert(argc==2);
    cog_ipc_endpoint_config_t cfg={.max_frame_bytes=512U,.receive_frames=1U,.send_frames=1U,.service_quantum=1U};
    cog_ipc_endpoint_t *listener=NULL,*sender=NULL,*receiver=NULL;
    assert(cog_ipc_endpoint_new_ex(&listener,&cfg)==COG_OK);
    assert(cog_ipc_endpoint_new_ex(&sender,&cfg)==COG_OK);
    assert(cog_ipc_endpoint_listen(listener,argv[1])==COG_OK);
    cog_result_t rc=cog_ipc_endpoint_connect(sender,argv[1],NULL);
    assert(rc==COG_OK || rc==COG_ERROR_AGAIN);
    assert(cog_ipc_endpoint_accept(listener,&receiver,NULL)==COG_OK);
    service(sender,receiver);copied_schedule(sender,receiver);
    assert(cog_ipc_endpoint_free(sender)==COG_OK);
    assert(cog_ipc_endpoint_free(receiver)==COG_OK);
    assert(cog_ipc_endpoint_free(listener)==COG_OK);
    puts("coordinator actual IPC scheduling: PASS");return 0;
}

/** @file test_17_session_migration.c
 * @brief Actual same-host fenced migration, target turns and cleanup receipts.
 * Old file-store transfer assertions remain in components/.
 */
#define _POSIX_C_SOURCE 200809L
#include "session_scenario.h"
#include <signal.h>
#include <stdlib.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"migration line %d: %s rc=%d\n",__LINE__,#x,(int)primary);if(primary==COG_OK)primary=COG_ERROR_PROTOCOL;goto finish;}}while(0)

/* Linker fault boundary drops one genuine received cleanup reply. It neither
   manufactures a reply nor changes a production record. Repeated receipt bytes
   are compared before the real coordinator can consume them. */
typedef struct migration_fault {
    bool enabled,dropped,retried;
    cog_cli_host_runtime_t *host;
    unsigned char reply[1024];size_t length;
} migration_fault_t;
static migration_fault_t *active_fault;
cog_result_t __real_cog_orchestrator_copy_event(const cog_orchestrator_t *orch,
    cog_orchestrator_event_class_t event_class,cog_orchestrator_event_t *event,
    void *storage,size_t capacity,size_t *required);
cog_result_t __wrap_cog_orchestrator_copy_event(const cog_orchestrator_t *orch,
    cog_orchestrator_event_class_t event_class,cog_orchestrator_event_t *event,
    void *storage,size_t capacity,size_t *required);
cog_result_t __wrap_cog_orchestrator_copy_event(const cog_orchestrator_t *orch,
    cog_orchestrator_event_class_t event_class,cog_orchestrator_event_t *event,
    void *storage,size_t capacity,size_t *required)
{
    cog_result_t rc=__real_cog_orchestrator_copy_event(orch,event_class,event,storage,capacity,required);
    if(rc!=COG_OK || active_fault==NULL || !active_fault->enabled || event->kind!=COG_ORCHESTRATOR_MESSAGE)return rc;
    unsigned char decoded[1024];size_t needed=0U;cog_cli_control_message_t message;
    if(cog_cli_control_decode(event->data,event->data_len,&message,decoded,sizeof(decoded),&needed)!=COG_OK ||
       message.kind!=COG_CLI_CONTROL_HANDOFF)return rc;
    cog_handoff_message_t body;char session[128];
    if(cog_orchestrator_handoff_decode(message.payload,message.payload_len,&body,session,sizeof(session),&needed)!=COG_OK || body.kind!=COG_HANDOFF_ABORT)return rc;
    migration_fault_t *f=active_fault;
    cog_cli_handoff_t *h=f->host->handoff;
    if(h->phase!=CLI_HANDOFF_CLEANUP || message.reply_to!=h->identity+12U ||
       message.runtime_incarnation!=f->host->runtime_incarnation ||
       message.owner_peer!=h->source_peer || event->peer_id!=h->source_peer ||
       event->worker_id.incarnation!=h->source_worker.incarnation ||
       event->worker_id.number!=h->source_worker.number ||
       strcmp(body.session_id,f->host->primary_session)!=0)return rc;
    if(!f->dropped){
        if(event->data_len>sizeof(f->reply))return COG_ERROR_LIMIT;
        memcpy(f->reply,event->data,event->data_len);f->length=event->data_len;f->dropped=true;
        rc=cog_orchestrator_ack_event((cog_orchestrator_t *)orch,event->event_id);
        return rc==COG_OK?COG_ERROR_AGAIN:rc;
    }
    if(!f->retried){
        if(event->data_len!=f->length || memcmp(event->data,f->reply,f->length)!=0)return COG_ERROR_PROTOCOL;
        f->retried=true;
    }
    return rc;
}
static uint64_t bytes_hash(uint64_t hash,const void *data,size_t length)
{
    const unsigned char *p=data;for(size_t i=0U;i<length;++i){hash^=p[i];hash*=UINT64_C(1099511628211);}return hash;
}
static cog_result_t history_hash(cog_session_t *session,uint64_t *hash,size_t *users,size_t *tools)
{
    const cog_message_t *messages=NULL;size_t count=0U;
    cog_result_t rc=cog_session_get_history(session,cog_session_active(session),&messages,&count);
    if(rc!=COG_OK)return rc;
    *hash=UINT64_C(1469598103934665603);*users=0U;*tools=0U;
    for(size_t i=0U;i<count;++i){const cog_message_t *m=&messages[i];
        *hash=bytes_hash(*hash,&m->role,sizeof(m->role));*hash=bytes_hash(*hash,&m->status,sizeof(m->status));
        *hash=bytes_hash(*hash,&m->content_len,sizeof(m->content_len));*hash=bytes_hash(*hash,m->content,m->content_len);
        *hash=bytes_hash(*hash,&m->tool_call_count,sizeof(m->tool_call_count));
        for(size_t j=0U;j<m->tool_call_count;++j){const cog_message_tool_call_t *t=&m->tool_calls[j];
            *hash=bytes_hash(*hash,t->id,strlen(t->id));*hash=bytes_hash(*hash,t->name,strlen(t->name));
            *hash=bytes_hash(*hash,t->arguments,t->arguments_len);}
        if(m->role==COG_MESSAGE_ROLE_USER)++*users;
        if(m->role==COG_MESSAGE_ROLE_TOOL)++*tools;
    }
    return COG_OK;
}
static cog_result_t checkpoint_hash(cog_cli_host_runtime_t *host,cog_store_checkpoint_t checkpoint,
    uint64_t *hash,size_t *users,size_t *tools)
{
    cog_session_t *loaded=NULL;cog_store_load_report_t report;
    cog_result_t rc=cog_store_checkpoint_load(host->catalog,host->primary_session,checkpoint,
        COG_STORE_LOAD_INSPECT,&loaded,&report);
    if(rc==COG_OK)rc=history_hash(loaded,hash,users,tools);
    cog_session_free(loaded);return rc;
}
static bool owner_equal(cog_store_owner_t a,cog_store_owner_t b)
{return a.epoch==b.epoch && a.instance_id==b.instance_id;}
static bool worker_equal(cog_worker_id_t a,cog_worker_id_t b)
{return a.incarnation==b.incarnation && a.number==b.number;}
int main(int argc,char **argv)
{
    headless_fixture_t f={0};session_client_t client={.input={-1,-1}};
    cog_result_t primary=COG_OK;bool cleanup=false,paused=false,expected_timeout=false;
    migration_fault_t fault={0};active_fault=&fault;
    cog_store_owner_guard_t *unexpected_guard=NULL;
    CHECK(argc==8);bool timeout=strcmp(argv[7],"migration-timeout")==0;
    CHECK(timeout || strcmp(argv[7],"migration")==0);
    const char *paths[6]={argv[1],argv[2],argv[3],argv[4],argv[5],argv[6]};
    CHECK(headless_fixture_init(&f,paths,1U,1U)==COG_OK);
    f.config.shutdown_ms=timeout?3000U:15000U;
    CHECK(headless_fixture_open(&f)==COG_OK);CHECK(headless_fixture_ready(&f,10000U)==COG_OK);
    char output[4096];CHECK(snprintf(output,sizeof(output),"%s/migration.ndjson",argv[4])>0);
    CHECK(session_client_open(&client,f.ui_path,output,false)==COG_OK);
    uint64_t deadline=headless_fixture_clock_ms()+3000U;
    while(!session_online(&f,&client) && headless_fixture_clock_ms()<deadline){primary=session_tick(&f,&client,NULL);CHECK(primary==COG_OK);}
    CHECK(session_online(&f,&client));
    char identity[128];CHECK(snprintf(identity,sizeof(identity),"%s",f.host->primary_session)>0);
    uint64_t generation=client.client->replay_generation;
    size_t physical[4]={0};
    unsigned char retained[1024];size_t retained_length=0U,retained_slot=0U;uint64_t retained_wire=0U;
    bool profile_preserved=false;
    for(unsigned int turn=0U;turn<=(timeout?0U:4U);++turn){
        if(turn!=0U){
            cog_cli_handoff_t *h=f.host->handoff;cog_store_owner_t before=f.host->primary_owner;
            cog_store_owner_t actual;cog_store_checkpoint_t latest;
            CHECK(cog_store_owner_read(f.host->catalog,identity,&actual,&latest)==COG_OK && owner_equal(actual,before));
            uint64_t before_hash=0U;size_t before_users=0U,before_tools=0U;
            CHECK(checkpoint_hash(f.host,latest,&before_hash,&before_users,&before_tools)==COG_OK);
            CHECK(before_users==turn && before_tools==turn);
            cog_worker_id_t target=turn%2U==1U?f.host->workers[0].id:(cog_worker_id_t){0};
            cog_result_t requested=COG_ERROR_AGAIN;deadline=headless_fixture_clock_ms()+10000U;
            while(requested==COG_ERROR_AGAIN && headless_fixture_clock_ms()<deadline){
                requested=cog_cli_handoff_request(h,target);
                if(requested==COG_ERROR_AGAIN){primary=session_tick(&f,&client,NULL);CHECK(primary==COG_OK);}
            }
            CHECK(requested==COG_OK);physical[turn-1U]=h->target_slot;
            if(turn==2U){fault.enabled=true;fault.host=f.host;} /* Remote old source cleanup. */
            bool cleanup_wait_seen=false;deadline=headless_fixture_clock_ms()+20000U;
            while(h->phase!=CLI_HANDOFF_IDLE && headless_fixture_clock_ms()<deadline){
                CHECK(!h->cas_committed || (h->checkpoint_saved && h->prepared && h->source_released));
                CHECK(!h->guard_acquired || h->cas_committed);
                CHECK(!h->route_published || h->guard_acquired);
                CHECK(!h->activation_acked || h->route_published);
                if(turn==3U && h->phase==CLI_HANDOFF_PREPARE && f.host->executors[h->target_slot].profile.pending){
                    cli_control_lane_t *lane=&f.host->executors[retained_slot].outbound[COG_CLI_LANE_HANDOFF-1U];
                    CHECK(h->target_slot==retained_slot && lane->final_valid && lane->final_answered_wire==retained_wire &&
                        lane->final_reply.length==retained_length && memcmp(lane->final_reply.data,retained,retained_length)==0);
                    profile_preserved=true;
                }
                if(fault.dropped && !fault.retried){
                    CHECK(h->phase==CLI_HANDOFF_CLEANUP && f.host->executors[h->source_slot].phase!=CLI_SLOT_FREE);
                    cleanup_wait_seen=true;
                }
                primary=session_tick(&f,&client,NULL);CHECK(primary==COG_OK);
            }
            CHECK(h->phase==CLI_HANDOFF_IDLE && h->primary==COG_OK && h->checkpoint_saved && h->prepared &&
                h->source_released && h->cas_committed && h->guard_acquired && h->route_published && h->activation_acked);
            CHECK(f.host->primary_owner.epoch==before.epoch+1U && f.host->primary_owner.instance_id!=before.instance_id && worker_equal(f.host->primary_worker,target));
            CHECK(cog_store_owner_read(f.host->catalog,identity,&actual,&latest)==COG_OK && owner_equal(actual,f.host->primary_owner) && latest.generation==h->checkpoint.generation);
            uint64_t after_hash=0U;size_t after_users=0U,after_tools=0U;
            CHECK(checkpoint_hash(f.host,h->checkpoint,&after_hash,&after_users,&after_tools)==COG_OK);
            CHECK(after_hash==before_hash && after_users==before_users && after_tools==before_tools);
            cog_peer_id_t peer=0U;cog_store_owner_t routed;cog_store_checkpoint_t routed_checkpoint;
            CHECK(cog_orchestrator_session_owner(f.host->orchestrator,identity,&peer,&routed,&routed_checkpoint)==COG_OK);
            CHECK(peer==f.host->primary_peer && owner_equal(routed,actual) && routed_checkpoint.generation==h->checkpoint.generation);
            CHECK(cog_store_owner_acquire(f.host->catalog,identity,actual,&unexpected_guard)==COG_ERROR_AGAIN && unexpected_guard==NULL);
            cli_executor_slot_t *source=&f.host->executors[h->source_slot];
            CHECK(source->phase==CLI_SLOT_FREE && source->local_done && source->app==NULL && source->net==NULL);
            if(h->source_peer!=0U){
                cli_control_lane_t *lane=&source->outbound[COG_CLI_LANE_HANDOFF-1U];
                CHECK(lane->final_valid && !lane->pending_valid && lane->final_answered_wire==h->identity+12U && lane->final_reply.length>0U);
                CHECK(lane->final_reply.length<=sizeof(retained));retained_length=lane->final_reply.length;
                memcpy(retained,lane->final_reply.data,retained_length);retained_wire=lane->final_answered_wire;retained_slot=h->source_slot;
            }
            if(turn==2U){CHECK(fault.dropped && fault.retried && cleanup_wait_seen);fault.enabled=false;}
            if(turn==3U)CHECK(profile_preserved);
            deadline=headless_fixture_clock_ms()+3000U;
            while(!session_online(&f,&client) && headless_fixture_clock_ms()<deadline){primary=session_tick(&f,&client,NULL);CHECK(primary==COG_OK);}
            CHECK(session_online(&f,&client) && strcmp(client.client->session,identity)==0 && client.client->replay_generation==generation);
        }
        char task[64],expected[64];CHECK(snprintf(task,sizeof(task),"turn %u\n",turn)>0);CHECK(snprintf(expected,sizeof(expected),"TURN_DONE_%u",turn)>0);
        CHECK(session_write(&client,task)==COG_OK);
        deadline=headless_fixture_clock_ms()+20000U;
        while((!session_contains(&client,expected) || client.client->requests[0].phase!=CLI_CLIENT_REQUEST_ACKED || !session_online(&f,&client)) && headless_fixture_clock_ms()<deadline){primary=session_tick(&f,&client,NULL);CHECK(primary==COG_OK);}
        CHECK(session_contains(&client,expected) && client.client->requests[0].phase==CLI_CLIENT_REQUEST_ACKED && session_online(&f,&client));
        CHECK(strcmp(identity,f.host->primary_session)==0);
    }
    if(timeout){
        cog_cli_handoff_t *h=f.host->handoff;cog_result_t requested=COG_ERROR_AGAIN;
        deadline=headless_fixture_clock_ms()+10000U;
        while(requested==COG_ERROR_AGAIN && headless_fixture_clock_ms()<deadline){requested=cog_cli_handoff_request(h,f.host->workers[0].id);if(requested==COG_ERROR_AGAIN){primary=session_tick(&f,&client,NULL);CHECK(primary==COG_OK);}}
        CHECK(requested==COG_OK);deadline=headless_fixture_clock_ms()+20000U;
        while(headless_fixture_clock_ms()<deadline){
            if(!paused && h->phase==CLI_HANDOFF_GUARD){CHECK(h->source_released && h->cas_committed);CHECK(headless_fixture_signal_worker(f.host,0U,SIGSTOP)==COG_OK);paused=true;}
            primary=session_tick(&f,&client,NULL);
            if(primary==COG_ERROR_TIMEOUT && paused){
                CHECK(f.host->admission_frozen && h->source_released && h->cas_committed && !h->guard_acquired && !h->route_published && !h->activation_acked);
                cli_executor_slot_t *source=&f.host->executors[h->source_slot];
                CHECK(source->app!=NULL && source->app->guard==NULL && source->app->headless->authority==CLI_AUTH_SOURCE_RELEASED);
                CHECK(cog_cli_app_queue(source->app,"forbidden replay",16U)==COG_ERROR_INVALID_STATE);
                cog_store_owner_t actual;cog_store_checkpoint_t checkpoint;
                CHECK(cog_store_owner_read(f.host->catalog,identity,&actual,&checkpoint)==COG_OK && owner_equal(actual,h->next) && checkpoint.generation==h->checkpoint.generation);
                expected_timeout=true;break;
            }
            CHECK(primary==COG_OK);
        }
        CHECK(expected_timeout);
    }else CHECK(physical[0]==physical[2] && physical[1]==physical[3] && physical[0]!=physical[1]);
finish:
    active_fault=NULL;
    if(unexpected_guard!=NULL)(void)cog_store_owner_release(unexpected_guard);
    if(paused){cog_result_t resumed=headless_fixture_signal_worker(f.host,0U,SIGCONT);if(resumed!=COG_OK)(void)cog_orchestrator_kill(f.host->orchestrator,f.host->workers[0].id);}
    primary=session_client_close(&client,primary);
    primary=headless_fixture_close(&f,primary,&cleanup);
    if(expected_timeout && primary==COG_ERROR_TIMEOUT && cleanup){puts("actual migration post-CAS timeout remains frozen and reaped: PASS");return 0;}
    if(primary!=COG_OK || !cleanup){fprintf(stderr,"migration outcome=%d cleanup=%d\n",(int)primary,(int)cleanup);return 1;}
    puts("actual four moves/target turns/exact checkpoint/cleanup receipt reuse: PASS");return 0;
}

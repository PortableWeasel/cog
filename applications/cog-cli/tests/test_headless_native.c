/** @file test_headless_native.c @brief Actual native child/provider/store join. */
#define _POSIX_C_SOURCE 200809L
#include "app_internal.h"
#include "composition_internal.h"
#include <curl/curl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"native line %d: %s\n",__LINE__,#x); if(primary==COG_OK)primary=COG_ERROR_PROTOCOL;goto finish; } } while(0)
static uint64_t clock_ms(void)
{
    struct timespec now;if(clock_gettime(CLOCK_MONOTONIC,&now)!=0)return 0U;
    return (uint64_t)now.tv_sec*1000U+(uint64_t)now.tv_nsec/1000000U;
}
static cog_result_t advance(cog_cli_host_runtime_t *host,bool *done)
{
    bool was_shutdown=host->shutdown;
    cog_cli_host_runtime_step_t step={0};cog_result_t rc=cog_cli_host_runtime_step(host,&step);
    *done=step.done;
    if(rc==COG_ERROR_AGAIN)rc=COG_OK;
    if(rc!=COG_OK && !was_shutdown) {
        cog_cli_app_t *app=host->executors[host->primary_slot].app;
        fprintf(stderr,"native drive rc=%d done=%d coordinator_failed=%d primary=%d control_ended=%d control_state=%u\n",
            (int)rc,(int)step.done,(int)host->coordinator_failed,(int)host->primary,
            (int)host->control_drive_ended,(unsigned)cog_core_state(host->control_core));
        if(app!=NULL)fprintf(stderr,"native root primary=%d drive=%d state=%u active=%d deferred=%d tool=%d\n",
            (int)app->headless->primary,(int)app->headless->drive,(unsigned)cog_core_state(app->core),
            (int)app->active,(int)app->deferred_error,(int)app->tool_execution_started);
        for(size_t i=0U;i<host->group_capacity;++i)fprintf(stderr,"native group%zu phase=%d admitted=%zu submitted=%zu\n",
            i,(int)host->groups[i].phase,host->groups[i].admitted,host->groups[i].submitted);
    }
    if(!step.done)(void)poll(NULL,0U,step.max_wait_ms>5U?5:(int)step.max_wait_ms);
    return rc;
}
int main(int argc,char **argv)
{
    cog_cli_host_runtime_t *host=NULL;cog_result_t primary=COG_OK;bool curl_ready=false;
    CHECK(argc==8); /* executable, CA, URL, workspace, store, socket prefix, mode */
    bool reject=strcmp(argv[7],"limit")==0,move=strcmp(argv[7],"handoff")==0;
    bool cancel=strcmp(argv[7],"cancel")==0,cancelled=false;
    CHECK(curl_global_init(CURL_GLOBAL_DEFAULT)==CURLE_OK);curl_ready=true;
    char ui[4096],workers[4096];
    CHECK(snprintf(ui,sizeof(ui),"%s.ui",argv[6])>0);
    CHECK(snprintf(workers,sizeof(workers),"%s.workers",argv[6])>0);
    cog_cli_options_t options={.backend=COG_CLI_BACKEND_REAL,.model_id="fixture/model",
        .credential_ref="env:COG_NATIVE_KEY",.base_url=argv[3],.workspace_root=argv[4],
        .store_dir=argv[5],.store_mode=COG_CLI_STORE_DURABLE,.tool_policy=COG_CLI_TOOL_POLICY_ALLOW,
        .max_context_bytes=65536U,.max_tool_output_bytes=reject?4096U:8192U,.max_output_tokens=512U,
        .max_tool_rounds=4U,.max_turn_ms=20000U,.max_tool_ms=15000U,.max_retries=0U,
        .explicit_options=COG_CLI_OPT_BACKEND|COG_CLI_OPT_MODEL|COG_CLI_OPT_CREDENTIAL|COG_CLI_OPT_BASE_URL|
            COG_CLI_OPT_WORKSPACE|COG_CLI_OPT_STORE_DIR|COG_CLI_OPT_STORE_MODE|COG_CLI_OPT_POLICY|
            COG_CLI_OPT_CONTEXT|COG_CLI_OPT_TOOL_OUTPUT|COG_CLI_OPT_OUTPUT_TOKENS|COG_CLI_OPT_TOOL_ROUNDS|
            COG_CLI_OPT_TURN_MS|COG_CLI_OPT_TOOL_MS|COG_CLI_OPT_RETRIES};
    cog_net_config_t transport={.host_runtime_initialized=true,.dependency_memory=COG_NET_DEPENDENCY_MEMORY_DYNAMIC,
        .max_request_bytes=1024U*1024U,.max_connections=2U,.ca_bundle=argv[2],.connect_timeout_ms=3000U,.stall_timeout_ms=5000U};
    cog_cli_host_runtime_config_t config={.root_options=&options,.net_config=&transport,
        .ui_socket=ui,.worker_socket=workers,.executable=argv[1],.workers=1U,.children=2U,.child_requests=32U,
        .shutdown_ms=move?15000U:3000U,
        .child_profile={.policy=COG_CLI_CHILD_POLICY_INHERIT,.tools=COG_CLI_CHILD_COMMAND,
            .max_tool_rounds=1U,.max_retries=0U},.signal_fd=-1};
    primary=cog_cli_host_runtime_new(&host,&config);CHECK(primary==COG_OK);
    primary=cog_cli_host_runtime_begin(host);CHECK(primary==COG_OK);
    uint64_t deadline=clock_ms()+10000U;bool done=false;
    while(!host->workers[0].profile_ready && clock_ms()<deadline){primary=advance(host,&done);CHECK(primary==COG_OK&&!done);}
    CHECK(host->workers[0].profile_ready && host->workers[0].native.peer_id!=0U);
    bool native_seen=false;
    for(unsigned int round=1U;round<=((reject || move || cancel || strcmp(argv[7],"single")==0)?1U:2U);++round){
        char task[64],expected[64];
        CHECK(snprintf(task,sizeof(task),"delegate round %u",round)>0);
        if(reject)CHECK(snprintf(expected,sizeof(expected),"PARENT_REFUSED")>0);
        else CHECK(snprintf(expected,sizeof(expected),"PARENT_DONE_%u",round)>0);
        cog_cli_app_t *app=host->executors[host->primary_slot].app;CHECK(app!=NULL);
        CHECK(cog_cli_app_queue(app,task,strlen(task))==COG_OK);
        deadline=clock_ms()+20000U;bool complete=false;
        while(clock_ms()<deadline){
            primary=advance(host,&done);CHECK(primary==COG_OK&&!done);
            for(size_t i=2U;i<host->bounds.executors;++i)
                if(host->executors[i].agent!=0U && host->executors[i].owner_peer!=0U)native_seen=true;
            if(cancel && !cancelled && native_seen) {
                char entered[4096];CHECK(snprintf(entered,sizeof(entered),"%s/native-entered",argv[4])>0);
                if(access(entered,F_OK)==0) for(size_t i=2U;i<host->bounds.executors;++i) {
                    if(host->executors[i].agent!=0U && host->executors[i].owner_peer!=0U) {
                        CHECK(cog_agent_cancel(host->control_core,host->executors[i].agent)==COG_OK);
                        CHECK(cog_agent_cancel(host->control_core,host->executors[i].agent)==COG_OK);
                        cancelled=true;break;
                    }
                }
            }
            cog_session_node_t *leaf=cog_session_active(app->session);
            if(leaf!=NULL){const cog_message_t *message=cog_node_data(leaf)->message;
                if(!app->active && message->role==COG_MESSAGE_ROLE_ASSISTANT && message->status==COG_MESSAGE_COMPLETE &&
                    message->content!=NULL && strcmp(message->content,expected)==0){complete=true;break;}}
        }
        if(!complete){
            for(size_t j=0U;j<host->group_capacity;++j)fprintf(stderr,"group %zu phase=%d admitted=%zu submitted=%zu gate=%d\n",j,(int)host->groups[j].phase,host->groups[j].admitted,host->groups[j].submitted,(int)host->groups[j].gate_open);
            for(size_t j=0U;j<host->bounds.executors;++j){cli_executor_slot_t *slot=&host->executors[j];
                fprintf(stderr,"slot %zu phase=%d agent=%llu peer=%llu started=%d done=%d accepted=%d outcome=%d\n",j,(int)slot->phase,(unsigned long long)slot->agent,(unsigned long long)slot->owner_peer,(int)slot->started,(int)slot->local_done,(int)slot->accepted_status,(int)slot->primary);
                if(slot->app!=NULL)fprintf(stderr," app active=%d state=%u queued=%d provider=%d tool=%d headlesskind=%d\n",(int)slot->app->active,(unsigned)cog_core_state(slot->app->core),(int)slot->app->event_queued,(int)slot->app->provider_active,(int)slot->app->tool_execution_started,(int)slot->app->headless->kind);
            }
        }
        CHECK(complete);
        deadline=clock_ms()+5000U;bool free_groups=false;
        while(clock_ms()<deadline){
            free_groups=true;for(size_t i=0U;i<host->group_capacity;++i)if(host->groups[i].phase!=CLI_GROUP_FREE)free_groups=false;
            if(free_groups)break;
            primary=advance(host,&done);CHECK(primary==COG_OK&&!done);
        }
        CHECK(free_groups);
    }
    CHECK(reject?!native_seen:native_seen);
    CHECK(!cancel || cancelled);
    if(move) for(unsigned int i=0U;i<4U;++i) {
        cog_store_owner_t before=host->primary_owner;
        cog_worker_id_t target=i%2U==0U?host->workers[0].id:(cog_worker_id_t){0};
        deadline=clock_ms()+10000U;cog_result_t requested=COG_ERROR_AGAIN;
        while(requested==COG_ERROR_AGAIN && clock_ms()<deadline) {
            requested=cog_cli_handoff_request(host->handoff,target);
            if(requested==COG_ERROR_AGAIN){primary=advance(host,&done);CHECK(primary==COG_OK&&!done);}
        }
        CHECK(requested==COG_OK);
        uint64_t began=clock_ms();deadline=began+20000U;
        while(host->handoff->phase!=CLI_HANDOFF_IDLE && clock_ms()<deadline) {
            cli_handoff_phase_t previous=host->handoff->phase;
            primary=advance(host,&done);
            if(previous!=host->handoff->phase || primary!=COG_OK)
                fprintf(stderr,"native handoff %u phase %d -> %d elapsed=%llu rc=%d\n",i+1U,
                    (int)previous,(int)host->handoff->phase,(unsigned long long)(clock_ms()-began),(int)primary);
            if(primary!=COG_OK) {
                cog_cli_handoff_t *h=host->handoff;
                fprintf(stderr,"native handoff state pending=%zu reply=%zu identity=%llu source=%zu/%llu target=%zu/%llu expected=%llu/%llu next=%llu/%llu token=%llu\n",
                    h->pending.length,h->reply.length,(unsigned long long)h->identity,
                    h->source_slot,(unsigned long long)h->source_peer,h->target_slot,(unsigned long long)h->target_peer,
                    (unsigned long long)h->expected.epoch,(unsigned long long)h->expected.instance_id,
                    (unsigned long long)h->next.epoch,(unsigned long long)h->next.instance_id,
                    (unsigned long long)h->checkpoint.generation);
                for(size_t slot=0U;slot<2U;++slot) {
                    cli_executor_slot_t *ex=&host->executors[slot];
                    fprintf(stderr,"native handoff slot%zu phase=%d worker=%llu/%llu peer=%llu owner=%llu/%llu\n",
                        slot,(int)ex->phase,(unsigned long long)ex->worker.incarnation,(unsigned long long)ex->worker.number,
                        (unsigned long long)ex->owner_peer,(unsigned long long)ex->owner.epoch,(unsigned long long)ex->owner.instance_id);
                }
            }
            CHECK(primary==COG_OK&&!done);
        }
        CHECK(host->handoff->phase==CLI_HANDOFF_IDLE && host->handoff->primary==COG_OK);
        CHECK(host->primary_owner.epoch==before.epoch+1U && host->primary_owner.instance_id!=before.instance_id);
        CHECK(host->primary_worker.incarnation==target.incarnation && host->primary_worker.number==target.number);
        cog_store_owner_t actual;cog_store_checkpoint_t checkpoint;
        CHECK(cog_store_owner_read(host->catalog,host->primary_session,&actual,&checkpoint)==COG_OK);
        CHECK(actual.epoch==host->primary_owner.epoch && actual.instance_id==host->primary_owner.instance_id);
        CHECK(i%2U==0U?host->executors[host->primary_slot].app==NULL:host->executors[host->primary_slot].app!=NULL);
        fprintf(stderr,"native handoff %u complete epoch=%llu\n",i+1U,(unsigned long long)actual.epoch);
    }
finish:
    if(host!=NULL){
        (void)cog_cli_host_runtime_request_shutdown(host);bool drained=host->done;uint64_t until=clock_ms()+10000U;
        while(!drained&&clock_ms()<until){cog_result_t rc=advance(host,&drained);if(primary==COG_OK)primary=rc;}
        if(!drained && primary==COG_OK)primary=COG_ERROR_TIMEOUT;
        cog_result_t closed=cog_cli_host_runtime_close(&host,primary);if(primary==COG_OK)primary=closed;
    }
    if(curl_ready)curl_global_cleanup();
    if(primary!=COG_OK){fprintf(stderr,"native outcome: %d\n",(int)primary);return 1;}
    puts("actual native delegation/reservation oracle: PASS");return 0;
}

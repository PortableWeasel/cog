/** @file test_09_subagent_messaging.c
 * @brief Actual sibling send/receive tools through a bounded coordinator mailbox.
 * Component endpoint routing lives in components/test_subagent_messaging.c.
 * Joined authorization uses same-group siblings and an observed retired ID;
 * single-primary sequential groups cannot expose a concurrently live foreign group.
 */
#define _POSIX_C_SOURCE 200809L
#include "fixtures/headless_fixture.h"
#include "execution_internal.h"
#include <stdio.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) {fprintf(stderr,"scenario09:%d: %s\n",__LINE__,#x);primary=COG_ERROR_PROTOCOL;goto finish;} } while(0)

static bool mark(const char *root,unsigned int round)
{
    char path[4096];int n=snprintf(path,sizeof(path),"%s/pressure-%u",root,round);
    if(n<=0 || (size_t)n>=sizeof(path))return false;
    FILE *stream=fopen(path,"w");if(stream==NULL)return false;
    bool ok=fputs("actual full mailbox and pending ninth send",stream)>=0;
    return fclose(stream)==0 && ok;
}
int main(int argc,char **argv)
{
    headless_fixture_t f={0};cog_result_t primary=COG_OK;bool cleanup=false,done=false;
    CHECK(argc==8 && strcmp(argv[7],"messaging")==0);
    const char *paths[6]={argv[1],argv[2],argv[3],argv[4],argv[5],argv[6]};
    CHECK(headless_fixture_init(&f,paths,1U,2U)==COG_OK);
    f.options.max_tool_rounds=8U;f.config.child_profile.max_tool_rounds=8U;
    f.config.child_profile.tools=0U;f.config.child_requests=64U;
    CHECK(headless_fixture_open(&f)==COG_OK);
    CHECK(headless_fixture_ready(&f,10000U)==COG_OK);
    cog_cli_host_runtime_t *h=f.host;cog_cli_app_t *parent=h->executors[h->primary_slot].app;
    CHECK(parent!=NULL);
    char metadata[4096];CHECK(snprintf(metadata,sizeof(metadata),"%s/registry.json",argv[4])>0);
    cog_agent_id_t previous[2]={0U,0U};
    for(unsigned int round=1U;round<=2U;++round) {
        char task[64],expected[64];CHECK(snprintf(task,sizeof(task),"delegate round %u",round)>0);
        CHECK(snprintf(expected,sizeof(expected),"PARENT_DONE_%u",round)>0);
        CHECK(cog_cli_app_queue(parent,task,strlen(task))==COG_OK);
        cog_agent_id_t ids[2]={0U,0U};bool running[2]={false,false},terminal[2]={false,false};
        cli_delegation_handle_t *retained=NULL;uint64_t pressure_since=0U,original_deadline=0U;
        bool pressure=false,complete=false;
        uint64_t end=headless_fixture_clock_ms()+20000U;
        while(headless_fixture_clock_ms()<end) {
            primary=headless_fixture_step(h,&done);CHECK(primary==COG_OK&&!done);
            cli_executor_slot_t *local=NULL,*native=NULL;
            for(size_t i=2U;i<h->bounds.executors;++i) {
                cli_executor_slot_t *s=&h->executors[i];if(s->agent==0U)continue;
                size_t k=s->owner_peer==0U?0U:1U;
                if(ids[k]==0U)ids[k]=s->agent;
                CHECK(ids[k]==s->agent && ids[k]!=previous[0] && ids[k]!=previous[1]);
                cog_agent_status_t status;CHECK(cog_agent_status(h->control_core,s->agent,&status)==COG_OK);
                if(status==COG_AGENT_STATUS_RUNNING)running[k]=true;
                if(s->terminal_acked) {CHECK(status==COG_AGENT_STATUS_COMPLETED);terminal[k]=true;}
                if(k==0U)local=s;else native=s;
            }
            if(local!=NULL && native!=NULL) {
                CHECK(ids[0]!=ids[1] && local->group!=0U && local->group==native->group);
                CHECK(strcmp(local->parent_session,native->parent_session)==0);
                CHECK(headless_fixture_registry(h,metadata)==COG_OK);
                if(!pressure && native->mailbox.count==native->mailbox.entry_capacity &&
                    local->app!=NULL && local->app->headless->delegation!=NULL) {
                    cli_delegation_handle_t *pending=local->app->headless->delegation->prepared;
                    if(pending!=NULL && pending->operation==CLI_SEND_MESSAGE && pending->started && !pending->done &&
                        strcmp(pending->call_id,"send-9")==0) {
                        CHECK(pending->destination==ids[1] && !pending->output_cached);
                        if(retained==NULL) {retained=pending;pressure_since=headless_fixture_clock_ms();original_deadline=pending->deadline_ms;}
                        CHECK(retained==pending && pending->deadline_ms==original_deadline && original_deadline>pressure_since);
                        if(headless_fixture_clock_ms()-pressure_since>=20U) {
                            CHECK(native->mailbox.count==8U && mark(argv[4],round));pressure=true;
                        }
                    }
                }
            }
            cog_session_node_t *leaf=cog_session_active(parent->session);
            if(leaf!=NULL) {const cog_message_t *m=cog_node_data(leaf)->message;
                if(!parent->active && m->role==COG_MESSAGE_ROLE_ASSISTANT && m->status==COG_MESSAGE_COMPLETE &&
                    m->content!=NULL && strcmp(m->content,expected)==0) {complete=true;break;}}
        }
        CHECK(complete && pressure && running[0]&&running[1]&&terminal[0]&&terminal[1]);
        end=headless_fixture_clock_ms()+5000U;bool retired=false;
        while(headless_fixture_clock_ms()<end) {
            retired=true;
            for(size_t i=0U;i<h->group_capacity;++i)if(h->groups[i].phase!=CLI_GROUP_FREE)retired=false;
            for(size_t i=2U;i<h->bounds.executors;++i)if(h->executors[i].phase!=CLI_SLOT_FREE)retired=false;
            if(retired)break;
            primary=headless_fixture_step(h,&done);CHECK(primary==COG_OK&&!done);
        }
        CHECK(retired);
        for(size_t k=0U;k<2U;++k) {
            cog_agent_status_t state;CHECK(cog_agent_status(h->control_core,ids[k],&state)==COG_ERROR_NOT_FOUND);
            previous[k]=ids[k];
        }
    }
finish:
    primary=headless_fixture_close(&f,primary,&cleanup);
    if(primary!=COG_OK||!cleanup) {fprintf(stderr,"scenario09 outcome=%d cleanup=%d\n",(int)primary,(int)cleanup);return 1;}
    puts("actual sibling tools, bounded retry and empty poll: PASS");return 0;
}

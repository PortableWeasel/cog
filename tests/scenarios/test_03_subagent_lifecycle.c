/** @file test_03_subagent_lifecycle.c
 * @brief Actual parent delegation, native/local execution and durable fan-in.
 * Historical registry transition coverage lives in components/test_agent_lifecycle.c.
 */
#define _POSIX_C_SOURCE 200809L
#include "fixtures/headless_fixture.h"
#include "agent_internal.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr,"scenario03:%d: %s\n",__LINE__,#x); primary=COG_ERROR_PROTOCOL;goto finish; } } while (0)

static bool saved_delegate(cog_session_t *session,unsigned int round,bool cancel)
{
    char call[32];
    if (snprintf(call,sizeof(call),"delegate-%u",round)<=0) return false;
    bool tool=false,ledger=false;
    for (cog_session_node_t *n=cog_session_active(session);n!=NULL;n=cog_node_parent(n)) {
        const cog_message_t *m=cog_node_data(n)->message;
        if (m==NULL) continue;
        if (m->role==COG_MESSAGE_ROLE_TOOL && m->tool_call_id!=NULL && strcmp(m->tool_call_id,call)==0) {
            tool=m->status==COG_MESSAGE_COMPLETE && m->content!=NULL &&
                strstr(m->content,"CHILD_DONE_local-")!=NULL &&
                (cancel?strstr(m->content,"\"status\":4")!=NULL:strstr(m->content,"CHILD_DONE_native-")!=NULL);
        }
        for (size_t i=0U;i<m->tool_call_count;++i) if (strcmp(m->tool_calls[i].id,call)==0) {
            cog_tool_execution_state_t state;
            ledger=cog_session_tool_state(session,n,call,&state)==COG_OK &&
                (cancel?state==COG_TOOL_EXECUTION_CANCELLED:state==COG_TOOL_EXECUTION_SUCCEEDED);
        }
    }
    return tool && ledger;
}

int main(int argc,char **argv)
{
    headless_fixture_t f={0};cog_result_t primary=COG_OK;bool cleanup=false,done=false;
    CHECK(argc==8);
    const char *paths[6]={argv[1],argv[2],argv[3],argv[4],argv[5],argv[6]};
    bool cancel=strcmp(argv[7],"cancel")==0;
    CHECK(cancel || strcmp(argv[7],"success")==0);
    CHECK(headless_fixture_init(&f,paths,1U,2U)==COG_OK);
    CHECK(headless_fixture_open(&f)==COG_OK);
    CHECK(headless_fixture_ready(&f,10000U)==COG_OK);
    cog_cli_host_runtime_t *h=f.host;
    cog_cli_app_t *parent=h->executors[h->primary_slot].app;
    CHECK(parent!=NULL);
    cog_agent_id_t previous[2]={0U,0U};
    for (unsigned int round=1U;round<=(cancel?1U:2U);++round) {
        char task[64],expected[64];
        CHECK(snprintf(task,sizeof(task),"delegate round %u",round)>0);
        CHECK(snprintf(expected,sizeof(expected),"PARENT_DONE_%u",round)>0);
        CHECK(cog_cli_app_queue(parent,task,strlen(task))==COG_OK);
        cog_agent_id_t ids[2]={0U,0U};bool running[2]={false,false};
        bool terminals[2]={false,false},linked[2]={false,false},cancelled=false,complete=false;
        uint64_t end=headless_fixture_clock_ms()+20000U;
        while (headless_fixture_clock_ms()<end) {
            primary=headless_fixture_step(h,&done);CHECK(primary==COG_OK&&!done);
            for (size_t i=2U;i<h->bounds.executors;++i) {
                cli_executor_slot_t *s=&h->executors[i];
                if (s->agent==0U) continue;
                size_t k=s->owner_peer==0U?0U:1U;
                if(ids[k]==0U) ids[k]=s->agent;
                CHECK(ids[k]==s->agent && ids[k]!=previous[0] && ids[k]!=previous[1]);
                const cog_agent_entry_t *a=cog_agent_internal_get(h->control_core,s->agent);
                CHECK(a!=NULL && a->execution_epoch==s->attempt && a->owner_peer==s->owner_peer);
                CHECK(a->parent_session_id!=NULL && strcmp(a->parent_session_id,h->primary_session)==0);
                CHECK(a->session_id!=NULL && strcmp(a->session_id,h->primary_session)!=0);
                CHECK(cog_session_parent_id(a->session)!=NULL);
                CHECK(strcmp(cog_session_parent_id(a->session),h->primary_session)==0);
                linked[k]=true;
                if(a->status==COG_AGENT_STATUS_RUNNING) running[k]=true;
                if(s->terminal_acked) {
                    CHECK(a->status==(cancel&&k==1U?COG_AGENT_STATUS_CANCELLED:COG_AGENT_STATUS_COMPLETED));
                    CHECK(!a->notification_pending);
                    terminals[k]=true;
                }
                if(cancel && k==1U && !cancelled) {
                    char entered[4096];CHECK(snprintf(entered,sizeof(entered),"%s/native-entered",argv[4])>0);
                    if(access(entered,F_OK)==0) {
                        CHECK(a->status==COG_AGENT_STATUS_RUNNING);
                        CHECK(cog_agent_cancel(h->control_core,s->agent)==COG_OK);
                        CHECK(cog_agent_cancel(h->control_core,s->agent)==COG_OK);
                        cancelled=true;
                    }
                }
            }
            cog_session_node_t *leaf=cog_session_active(parent->session);
            if(leaf!=NULL) {
                const cog_message_t *m=cog_node_data(leaf)->message;
                if(!parent->active && m->role==COG_MESSAGE_ROLE_ASSISTANT && m->status==COG_MESSAGE_COMPLETE &&
                    m->content!=NULL && strcmp(m->content,expected)==0) {complete=true;break;}
            }
        }
        CHECK(complete && ids[0]!=0U && ids[1]!=0U && ids[0]!=ids[1]);
        CHECK(running[0]&&running[1]&&linked[0]&&linked[1]&&terminals[0]&&terminals[1]);
        CHECK(!cancel || cancelled);
        CHECK(saved_delegate(parent->session,round,cancel));
        cog_session_t *saved=NULL;
        CHECK(cog_store_load(h->catalog,h->primary_session,&saved)==COG_OK);
        bool durable=saved_delegate(saved,round,cancel);cog_session_free(saved);CHECK(durable);
        end=headless_fixture_clock_ms()+5000U;bool retired=false;
        while(headless_fixture_clock_ms()<end) {
            retired=true;
            for(size_t i=0U;i<h->group_capacity;++i) if(h->groups[i].phase!=CLI_GROUP_FREE) retired=false;
            for(size_t i=2U;i<h->bounds.executors;++i) if(h->executors[i].phase!=CLI_SLOT_FREE) retired=false;
            if(retired) break;
            primary=headless_fixture_step(h,&done);CHECK(primary==COG_OK&&!done);
        }
        CHECK(retired);
        for(size_t k=0U;k<2U;++k) {
            cog_agent_status_t status;CHECK(cog_agent_status(h->control_core,ids[k],&status)==COG_ERROR_NOT_FOUND);
            previous[k]=ids[k];
        }
    }
finish:
    primary=headless_fixture_close(&f,primary,&cleanup);
    if(primary!=COG_OK || !cleanup) {fprintf(stderr,"scenario03 outcome=%d cleanup=%d\n",(int)primary,(int)cleanup);return 1;}
    puts("actual subagent lifecycle: PASS");return 0;
}

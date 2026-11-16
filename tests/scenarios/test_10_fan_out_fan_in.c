/** @file test_10_fan_out_fan_in.c
 * @brief Actual local plus two native workers, ordered fan-in and cancellation.
 * Historical placement/broadcast controls live in components/test_fan_out_fan_in.c.
 */
#define _POSIX_C_SOURCE 200809L
#include "fixtures/headless_fixture.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#define CHECK(x) do { if (!(x)) {fprintf(stderr,"scenario10:%d: %s\n",__LINE__,#x);primary=COG_ERROR_PROTOCOL;goto finish;} } while(0)

static bool marker(const char *root,const char *prefix,const char *role,unsigned int round,bool create)
{
    char path[4096];int n=snprintf(path,sizeof(path),"%s/%s-%s-%u",root,prefix,role,round);
    if(n<=0 || (size_t)n>=sizeof(path))return false;
    if(!create)return access(path,F_OK)==0;
    FILE *file=fopen(path,"w");if(file==NULL)return false;
    bool ok=fputs("observed",file)>=0;return fclose(file)==0 && ok;
}
int main(int argc,char **argv)
{
    headless_fixture_t f={0};cog_result_t primary=COG_OK;bool cleanup=false,done=false;
    CHECK(argc==8);bool cancel=strcmp(argv[7],"fanout-cancel")==0,limit=strcmp(argv[7],"fanout-limit")==0;
    CHECK(cancel||limit||strcmp(argv[7],"fanout")==0);
    const char *paths[6]={argv[1],argv[2],argv[3],argv[4],argv[5],argv[6]};
    CHECK(headless_fixture_init(&f,paths,2U,3U)==COG_OK);
    f.options.max_tool_output_bytes=limit?4096U:16384U;
    f.config.child_requests=64U;
    CHECK(headless_fixture_open(&f)==COG_OK);
    CHECK(headless_fixture_ready(&f,10000U)==COG_OK);
    cog_cli_host_runtime_t *h=f.host;
    CHECK(h->workers[0].native.peer_id!=0U && h->workers[1].native.peer_id!=0U &&
        h->workers[0].native.peer_id!=h->workers[1].native.peer_id);
    cog_cli_app_t *parent=h->executors[h->primary_slot].app;CHECK(parent!=NULL);
    cog_agent_id_t previous[3]={0U,0U,0U};
    static const char *const roles[3]={"local","native-a","native-b"};
    for(unsigned int round=1U;round<=((cancel||limit)?1U:2U);++round) {
        char prompt[64],expected[64];CHECK(snprintf(prompt,sizeof(prompt),"delegate round %u",round)>0);
        CHECK(snprintf(expected,sizeof(expected),limit?"PARENT_REFUSED":"PARENT_DONE_%u",round)>0);
        CHECK(cog_cli_app_queue(parent,prompt,strlen(prompt))==COG_OK);
        cog_agent_id_t ids[3]={0U,0U,0U};cog_peer_id_t peers[3]={0U,0U,0U};
        bool running[3]={false,false,false},terminal[3]={false,false,false};
        size_t completions=0U;bool cancelled=false,complete=false;
        uint64_t end=headless_fixture_clock_ms()+20000U;
        while(headless_fixture_clock_ms()<end) {
            primary=headless_fixture_step(h,&done);CHECK(primary==COG_OK&&!done);
            for(size_t i=2U;i<h->bounds.executors;++i) {
                cli_executor_slot_t *s=&h->executors[i];if(s->agent==0U)continue;
                CHECK(!limit);
                size_t k=3U;
                for(size_t j=0U;j<3U;++j) {char task[64];CHECK(snprintf(task,sizeof(task),"child %s %u",roles[j],round)>0);
                    if(s->task.length==strlen(task) && memcmp(s->task.data,task,s->task.length)==0)k=j;}
                CHECK(k<3U);
                if(ids[k]==0U) {ids[k]=s->agent;peers[k]=s->owner_peer;}
                CHECK(ids[k]==s->agent && peers[k]==s->owner_peer);
                for(size_t j=0U;j<3U;++j)CHECK(ids[k]!=previous[j]);
                CHECK(k==0U?s->owner_peer==0U:s->owner_peer!=0U);
                if(k!=0U)CHECK(s->owner_peer==h->workers[0].native.peer_id || s->owner_peer==h->workers[1].native.peer_id);
                cog_agent_status_t state;CHECK(cog_agent_status(h->control_core,s->agent,&state)==COG_OK);
                if(state==COG_AGENT_STATUS_RUNNING)running[k]=true;
                if(s->terminal_acked && !terminal[k]) {
                    CHECK(state==(cancel?COG_AGENT_STATUS_CANCELLED:COG_AGENT_STATUS_COMPLETED));
                    if(!cancel)CHECK(k==2U-completions);
                    terminal[k]=true;++completions;
                    CHECK(marker(argv[4],"terminal",roles[k],round,true));
                }
            }
            if(cancel && !cancelled && running[0]&&running[1]&&running[2] &&
                marker(argv[4],"entered",roles[0],round,false)&&marker(argv[4],"entered",roles[1],round,false)&&
                marker(argv[4],"entered",roles[2],round,false)) {
                CHECK(peers[1]!=peers[2] && ids[0]!=ids[1] && ids[1]!=ids[2] && ids[0]!=ids[2]);
                for(size_t k=0U;k<3U;++k)CHECK(cog_agent_cancel(h->control_core,ids[k])==COG_OK);
                cancelled=true;
            }
            cog_session_node_t *leaf=cog_session_active(parent->session);
            if(leaf!=NULL) {const cog_message_t *m=cog_node_data(leaf)->message;
                if(!parent->active && m->role==COG_MESSAGE_ROLE_ASSISTANT && m->status==COG_MESSAGE_COMPLETE &&
                    m->content!=NULL && strcmp(m->content,expected)==0) {complete=true;break;}}
        }
        CHECK(complete);
        if(limit)CHECK(ids[0]==0U && ids[1]==0U && ids[2]==0U);
        else {
            CHECK(completions==3U && running[0]&&running[1]&&running[2] && peers[1]!=peers[2]);
            CHECK(!cancel||cancelled);
        }
        end=headless_fixture_clock_ms()+5000U;bool retired=false;
        while(headless_fixture_clock_ms()<end) {
            retired=true;
            for(size_t i=0U;i<h->group_capacity;++i)if(h->groups[i].phase!=CLI_GROUP_FREE)retired=false;
            for(size_t i=2U;i<h->bounds.executors;++i)if(h->executors[i].phase!=CLI_SLOT_FREE)retired=false;
            if(retired)break;
            primary=headless_fixture_step(h,&done);CHECK(primary==COG_OK&&!done);
        }
        CHECK(retired);
        for(size_t k=0U;k<3U;++k) {
            if(ids[k]!=0U) {cog_agent_status_t status;CHECK(cog_agent_status(h->control_core,ids[k],&status)==COG_ERROR_NOT_FOUND);}
            previous[k]=ids[k];
        }
        CHECK(h->workers[0].reserved_slots==0U && h->workers[1].reserved_slots==0U);
    }
finish:
    primary=headless_fixture_close(&f,primary,&cleanup);
    if(primary!=COG_OK||!cleanup) {fprintf(stderr,"scenario10 outcome=%d cleanup=%d\n",(int)primary,(int)cleanup);return 1;}
    puts("actual distributed fan-out/fan-in: PASS");return 0;
}

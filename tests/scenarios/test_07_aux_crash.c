/** @file test_07_aux_crash.c
 * @brief Kill an actual owned native executor during a real child attempt.
 * Historical aux_core/registry controls live in components/test_aux_crash.c.
 */
#define _POSIX_C_SOURCE 200809L
#include "fixtures/headless_fixture.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr,"scenario07:%d: %s\n",__LINE__,#x); if(primary==COG_OK)primary=COG_ERROR_PROTOCOL;goto finish; } } while(0)

static bool exists(const char *root,const char *name)
{
    char path[4096];int n=snprintf(path,sizeof(path),"%s/%s",root,name);
    return n>0 && (size_t)n<sizeof(path) && access(path,F_OK)==0;
}
static bool once(const char *root)
{
    char path[4096],bytes[8]={0};int n=snprintf(path,sizeof(path),"%s/effect-native-1",root);
    if(n<=0 || (size_t)n>=sizeof(path)) return false;
    FILE *stream=fopen(path,"r");if(stream==NULL)return false;
    size_t size=fread(bytes,1U,sizeof(bytes),stream);bool ok=!ferror(stream)&&size==4U&&memcmp(bytes,"once",4U)==0;
    return fclose(stream)==0 && ok;
}
/* Pin /proc directories while these two actual command processes are alive.
   Later openat observes the same lifetimes even if numeric PIDs are reused.
   Neither is signalled or reaped by this harness: the workspace guardian owns
   shell/group cleanup and the outer process fixture owns orphan supervision. */
static bool pin_command(const char *root,int descriptors[2])
{
    char path[4096];int n=snprintf(path,sizeof(path),"%s/native-entered",root);
    if(n<=0 || (size_t)n>=sizeof(path))return false;
    FILE *stream=fopen(path,"r");if(stream==NULL)return false;
    long long pids[2];bool read_ok=fscanf(stream,"%lld %lld",&pids[0],&pids[1])==2;
    if(fclose(stream)!=0 || !read_ok)return false;
    for(size_t i=0U;i<2U;++i) {
        if(pids[i]<=0 || pids[i]>INT_MAX)return false;
        n=snprintf(path,sizeof(path),"/proc/%lld",pids[i]);
        if(n<=0 || (size_t)n>=sizeof(path))return false;
        descriptors[i]=open(path,O_RDONLY|O_DIRECTORY|O_CLOEXEC);
        if(descriptors[i]<0)return false;
    }
    return pids[0]!=pids[1];
}
static bool command_ended(int directory)
{
    int descriptor=openat(directory,"stat",O_RDONLY|O_CLOEXEC);
    if(descriptor<0)return errno==ENOENT || errno==ESRCH;
    char bytes[4096];ssize_t length=read(descriptor,bytes,sizeof(bytes)-1U);
    (void)close(descriptor);
    if(length<=0)return false;
    bytes[(size_t)length]='\0';char *end=strrchr(bytes,')');
    return end!=NULL && (size_t)(end-bytes)+2U<(size_t)length && end[1]==' ' && (end[2]=='Z' || end[2]=='X');
}
static bool durable_unknown(cog_session_t *session)
{
    bool ledger=false,result=false,result_seen=false;
    for(cog_session_node_t *n=cog_session_active(session);n!=NULL;n=cog_node_parent(n)) {
        const cog_message_t *m=cog_node_data(n)->message;if(m==NULL)continue;
        if(m->role==COG_MESSAGE_ROLE_TOOL && m->tool_call_id!=NULL && strcmp(m->tool_call_id,"delegate-1")==0) {
            result_seen=true;
            result=m->content!=NULL && strstr(m->content,"side_effects_possible=true")!=NULL;
        }
        for(size_t i=0U;i<m->tool_call_count;++i)if(strcmp(m->tool_calls[i].id,"delegate-1")==0) {
            cog_tool_execution_state_t state;
            cog_result_t rc=cog_session_tool_state(session,n,"delegate-1",&state);
            ledger=rc==COG_OK && state==COG_TOOL_EXECUTION_INDETERMINATE;
        }
    }
    /* A fatal host driver freezes admission before model continuation. Its
       recovery-only checkpoint may retain an INDETERMINATE ledger without a
       TOOL node; never demand a fabricated final child result after death. */
    return ledger && (!result_seen || result);
}
int main(int argc,char **argv)
{
    headless_fixture_t f={0};cog_result_t primary=COG_OK,first=COG_OK;int command[2]={-1,-1};
    bool done=false,cleanup=false,killed=false,uncertain=false,reaped=false,local_completed=false;
    CHECK(argc==8);bool after=strcmp(argv[7],"crash-after")==0;
    CHECK(after || strcmp(argv[7],"crash-before")==0);
    const char *paths[6]={argv[1],argv[2],argv[3],argv[4],argv[5],argv[6]};
    CHECK(headless_fixture_init(&f,paths,1U,2U)==COG_OK);
    primary=headless_fixture_open(&f);CHECK(primary==COG_OK);
    primary=headless_fixture_ready(&f,10000U);CHECK(primary==COG_OK);
    cog_cli_host_runtime_t *h=f.host;
    cog_cli_app_t *parent=h->executors[h->primary_slot].app;CHECK(parent!=NULL);
    CHECK(cog_cli_app_queue(parent,"delegate round 1",16U)==COG_OK);
    uint64_t end=headless_fixture_clock_ms()+20000U;
    cog_agent_id_t actual=0U;cog_worker_id_t owned=h->workers[0].id;
    pid_t pid=(pid_t)h->workers[0].native.pid;CHECK(pid>0);
    while(headless_fixture_clock_ms()<end) {
        cog_result_t rc=headless_fixture_step(h,&done);
        if(rc!=COG_OK && first==COG_OK)first=rc;
        for(size_t i=2U;i<h->bounds.executors;++i) {
            cli_executor_slot_t *s=&h->executors[i];
            if(s->agent==0U)continue;
            if(s->owner_peer==0U) {
                if(s->terminal_acked && s->accepted_status==COG_AGENT_STATUS_COMPLETED)local_completed=true;
                continue;
            }
            CHECK(s->worker.incarnation==owned.incarnation && s->worker.number==owned.number);
            if(actual==0U)actual=s->agent;
            CHECK(actual==s->agent);
            if(!killed && local_completed && exists(argv[4],"native-entered")) {
                CHECK(s->first_admission_sent && s->started && s->accepted_status==COG_AGENT_STATUS_RUNNING);
                CHECK(after?once(argv[4]):!exists(argv[4],"effect-native-1"));
                if(after)CHECK(pin_command(argv[4],command));
                CHECK(headless_fixture_signal_worker(h,0U,SIGKILL)==COG_OK);killed=true;
            }
            if(killed && s->effects_uncertain) {
                CHECK(s->first_admission_sent && s->primary==COG_ERROR_INDETERMINATE);
                uncertain=true;
            }
        }
        if(h->workers[0].native.reaped) {
            CHECK(!h->workers[0].native.ownership_lost && h->workers[0].native.pid==(int)pid);
            reaped=true;
        }
        if(first!=COG_OK) CHECK(h->primary==first && h->admission_frozen && h->shutdown);
        if(done)break;
    }
    CHECK(killed && actual!=0U && uncertain && reaped && done && first!=COG_OK);
    CHECK(h->workers[0].id.incarnation==owned.incarnation && h->workers[0].id.number==owned.number);
    cog_session_t *saved=NULL;cog_store_load_report_t report;
    CHECK(cog_store_load_ex(h->catalog,h->primary_session,COG_STORE_LOAD_INSPECT,&saved,&report)==COG_OK);
    bool durable=durable_unknown(saved);cog_session_free(saved);
    CHECK(durable && report.indeterminate_tools>=1U);
    char metadata[4096];CHECK(snprintf(metadata,sizeof(metadata),"%s/registry.json",argv[4])>0);
    CHECK(headless_fixture_registry(h,metadata)==COG_OK);
    /* Workspace guardian observes control EOF, kills its owned shell group
       and reaps descendants. Observe actual command lifetimes; worker death
       itself still cannot prove arbitrary external effects stopped. */
    if(after) {
        end=headless_fixture_clock_ms()+4000U;
        while((!command_ended(command[0]) || !command_ended(command[1])) && headless_fixture_clock_ms()<end)(void)poll(NULL,0U,5);
        CHECK(command_ended(command[0]) && command_ended(command[1]) && once(argv[4]));
    } else CHECK(!exists(argv[4],"effect-native-1"));
    for(size_t i=0U;i<2U;++i)if(command[i]>=0){(void)close(command[i]);command[i]=-1;}
    primary=headless_fixture_close(&f,first,&cleanup);
    CHECK(primary==first && cleanup && f.host==NULL);
    puts("actual owned-worker crash retained uncertainty and reaped: PASS");return 0;
finish:
    for(size_t i=0U;i<2U;++i)if(command[i]>=0)(void)close(command[i]);
    primary=headless_fixture_close(&f,primary,&cleanup);
    fprintf(stderr,"scenario07 outcome=%d first=%d cleanup=%d\n",(int)primary,(int)first,(int)cleanup);return 1;
}

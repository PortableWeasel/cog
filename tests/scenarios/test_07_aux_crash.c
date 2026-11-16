/** @file test_07_aux_crash.c
 * @brief Kill an actual owned native executor during a real child attempt.
 * Historical aux_core/registry controls live in components/test_aux_crash.c.
 */
#define _POSIX_C_SOURCE 200809L
#include "fixtures/headless_fixture.h"
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr,"scenario07:%d: %s\n",__LINE__,#x); primary=COG_ERROR_PROTOCOL;goto finish; } } while(0)

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
int main(int argc,char **argv)
{
    headless_fixture_t f={0};cog_result_t primary=COG_OK,first=COG_OK;
    bool done=false,cleanup=false,killed=false,uncertain=false,reaped=false,local_completed=false;
    CHECK(argc==8);bool after=strcmp(argv[7],"crash-after")==0;
    CHECK(after || strcmp(argv[7],"crash-before")==0);
    const char *paths[6]={argv[1],argv[2],argv[3],argv[4],argv[5],argv[6]};
    CHECK(headless_fixture_init(&f,paths,1U,2U)==COG_OK);
    CHECK(headless_fixture_open(&f)==COG_OK);
    CHECK(headless_fixture_ready(&f,10000U)==COG_OK);
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
    /* The after-effect shell is deliberately finite (two seconds). Worker
       death cannot prove descendants stopped: observe its final marker rather
       than claim rollback, kill a foreign PID or replay its accepted command. */
    if(after) {
        end=headless_fixture_clock_ms()+4000U;
        while(!exists(argv[4],"native-command-ended") && headless_fixture_clock_ms()<end)(void)poll(NULL,0U,5);
        CHECK(exists(argv[4],"native-command-ended") && once(argv[4]));
    } else CHECK(!exists(argv[4],"effect-native-1"));
    primary=headless_fixture_close(&f,first,&cleanup);
    CHECK(primary==first && cleanup && f.host==NULL);
    puts("actual owned-worker crash retained uncertainty and reaped: PASS");return 0;
finish:
    primary=headless_fixture_close(&f,primary,&cleanup);
    fprintf(stderr,"scenario07 outcome=%d first=%d cleanup=%d\n",(int)primary,(int)first,(int)cleanup);return 1;
}

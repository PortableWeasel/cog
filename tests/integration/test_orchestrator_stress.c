/** @file Fixture component migration: real owned processes, CGW, and retained diagnosis. */
#define _POSIX_C_SOURCE 200809L
#include <cog/orchestrator.h>
#include "orchestrator_internal.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#ifndef COG_AUX_CORE_BIN
#error COG_AUX_CORE_BIN required
#endif
static unsigned failures;
#define FX_CHECK(x) do { if(!(x)) { (void)fprintf(stderr,"FAIL %s:%d %s\n",__FILE__,__LINE__,#x);++failures; } } while(0)
static uint64_t fx_now(void)
{
    struct timespec ts;FX_CHECK(clock_gettime(CLOCK_MONOTONIC,&ts)==0);
    return (uint64_t)ts.tv_sec*1000U+(uint64_t)ts.tv_nsec/1000000U;
}
static void fx_pause(void) { (void)nanosleep(&(struct timespec){0,1000000L},NULL); }
static bool fx_same(cog_worker_id_t a,cog_worker_id_t b)
{ return a.incarnation==b.incarnation&&a.number==b.number; }
static cog_result_t fx_service(cog_orchestrator_t *o)
{
    bool done=false;uint64_t deadline;
    cog_result_t rc=cog_orchestrator_service(o,fx_now(),&done,&deadline);
    FX_CHECK(rc==COG_OK||rc==COG_ERROR_AGAIN||rc==COG_ERROR_TIMEOUT);
    return rc;
}
/* Failed prerequisites drain every owned lifetime before stopping this test.
 * Never continue a stress iteration with an all-zero identity or PID. */
static void fx_abort(cog_orchestrator_t *o)
{
    if(o!=NULL) {
        bool done=false;uint64_t deadline,end=fx_now()+4000;
        (void)cog_orchestrator_request_shutdown(o,fx_now(),1);
        while(!done&&fx_now()<end) { (void)cog_orchestrator_service(o,fx_now(),&done,&deadline);fx_pause(); }
        if(done) cog_orchestrator_free(o);
        else (void)fprintf(stderr,"fixture fatal: native cleanup did not complete\n");
    }
    exit(EXIT_FAILURE);
}
static cog_core_info_t fx_info(cog_orchestrator_t *o,cog_worker_id_t id)
{
    cog_core_info_t info[16],found={0};size_t count=0;
    FX_CHECK(cog_orchestrator_list(o,info,16,&count)==COG_OK);
    for(size_t i=0;i<count&&i<16;++i) if(fx_same(info[i].worker_id,id)) return info[i];
    return found;
}
static bool fx_signal_worker(cog_orchestrator_t *o,cog_worker_id_t id,int sig)
{
    if(id.incarnation==0||id.number==0) return false;
    cog_core_info_t info=fx_info(o,id);
    if(!fx_same(info.worker_id,id)||info.pid<=0||info.reaped||info.ownership_lost) return false;
    return kill((pid_t)info.pid,sig)==0;
}
static bool fx_wait(cog_orchestrator_t *o,cog_worker_id_t id,bool reaped)
{
    uint64_t end=fx_now()+4000;
    do {
        (void)fx_service(o);cog_core_info_t info=fx_info(o,id);
        if(reaped?info.reaped:info.health==COG_HEALTH_HEALTHY) return true;
        fx_pause();
    } while(fx_now()<end);
    return false;
}
static void fx_ack(cog_orchestrator_t *o)
{
    unsigned char data[256];cog_orchestrator_event_t event;size_t required;
    for(int cls=COG_ORCHESTRATOR_CONTROL_EVENTS;cls<=COG_ORCHESTRATOR_MESSAGE_EVENTS;++cls) {
        cog_result_t rc;
        while((rc=cog_orchestrator_copy_event(o,(cog_orchestrator_event_class_t)cls,&event,data,sizeof(data),&required))==COG_OK)
            FX_CHECK(cog_orchestrator_ack_event(o,event.event_id)==COG_OK);
        FX_CHECK(rc==COG_ERROR_AGAIN);
    }
}
static cog_result_t fx_release(cog_orchestrator_t *o,cog_worker_id_t id)
{
    cog_result_t rc;uint64_t end=fx_now()+2000;
    do {
        fx_ack(o);rc=cog_orchestrator_release_worker(o,id);
        if(rc!=COG_ERROR_INVALID_STATE) return rc;
        (void)fx_service(o);fx_pause();
    } while(fx_now()<end);
    return rc;
}
static void fx_command(cog_orchestrator_t *o,cog_worker_id_t id,unsigned char mode,unsigned char active)
{
    unsigned char bytes[6]={'F','X','M','1',mode,active};
    cog_ipc_message_t message={.data=bytes,.capacity=sizeof(bytes),.len=sizeof(bytes)};
    FX_CHECK(cog_orchestrator_send(o,id,&message)==COG_OK);
    uint64_t end=fx_now()+2000;bool received=false;
    while(fx_now()<end&&!received) {
        unsigned char data[256];cog_orchestrator_event_t event;size_t required;
        (void)fx_service(o);
        cog_result_t rc=cog_orchestrator_copy_event(o,COG_ORCHESTRATOR_MESSAGE_EVENTS,&event,data,sizeof(data),&required);
        if(rc==COG_OK) {
            FX_CHECK(fx_same(event.worker_id,id)&&required==6&&memcmp(data,bytes,6)==0);
            FX_CHECK(cog_orchestrator_ack_event(o,event.event_id)==COG_OK);received=true;
        } else FX_CHECK(rc==COG_ERROR_AGAIN);
        fx_pause();
    }
    FX_CHECK(received);
}
static cog_orchestrator_t *fx_new(char *path,size_t capacity,bool replace)
{
    static unsigned serial=0;
    int n=snprintf(path,capacity,"orch-%ld-%u.sock",(long)getpid(),++serial);
    FX_CHECK(n>0&&(size_t)n<capacity);(void)unlink(path);
    cog_orchestrator_config_t cfg={0};cfg.socket_path=path;cfg.max_aux_cores=8;
    cfg.health_interval_ms=200;cfg.unhealthy_threshold=3;cfg.default_core_capacity=8;
    cfg.shutdown_timeout_ms=200;cfg.auto_replace=replace;
    cfg.max_frame_bytes=256;cfg.message_bytes=4096;cfg.message_events=8;
    cog_orchestrator_t *o=NULL;
    cog_result_t rc=cog_orchestrator_new(&o,&cfg);FX_CHECK(rc==COG_OK);
    if(rc!=COG_OK||o==NULL) fx_abort(o);
    return o;
}
static cog_worker_id_t fx_spawn(cog_orchestrator_t *o)
{
    cog_worker_id_t id={0};
    cog_result_t rc=cog_orchestrator_spawn(o,COG_AUX_CORE_BIN,&id);FX_CHECK(rc==COG_OK);
    if(rc!=COG_OK||id.incarnation==0||id.number==0) fx_abort(o);
    bool ready=fx_wait(o,id,false);FX_CHECK(ready);if(!ready) fx_abort(o);
    cog_core_info_t info=fx_info(o,id);FX_CHECK(info.peer_id!=0&&info.pid>0&&info.capacity==8);
    return id;
}
static void fx_close_at(cog_orchestrator_t *o,char *path,cog_result_t expected,const char *test)
{
    FX_CHECK(cog_orchestrator_request_shutdown(o,fx_now(),300)==COG_OK);
    bool done=false;uint64_t deadline,end=fx_now()+4000;cog_result_t rc=COG_ERROR_AGAIN;
    while(!done&&fx_now()<end) { rc=cog_orchestrator_service(o,fx_now(),&done,&deadline);fx_pause(); }
    FX_CHECK(done);
    if(rc!=expected) (void)fprintf(stderr,"%s shutdown: actual %d expected %d done %d\n",test,(int)rc,(int)expected,(int)done);
    FX_CHECK(rc==expected);
    cog_core_info_t infos[16];size_t count=0;FX_CHECK(cog_orchestrator_list(o,infos,16,&count)==COG_OK);
    fx_ack(o);
    for(size_t i=0;i<count&&i<16;++i) {
        FX_CHECK(infos[i].reaped&&!infos[i].ownership_lost);
        /* Native done is not logical endpoint/event retirement. Continue the
           actual owner service and ACK copied events before bounded release. */
        cog_result_t released=fx_release(o,infos[i].worker_id);
        if(released!=COG_OK) (void)fprintf(stderr,"%s release worker %llu: actual %d reaped %d lost %d\n",test,(unsigned long long)infos[i].worker_id.number,(int)released,(int)infos[i].reaped,(int)infos[i].ownership_lost);
        FX_CHECK(released==COG_OK);
    }
    if(done) cog_orchestrator_free(o);
    (void)unlink(path);
}
#define fx_close(o,p,e) fx_close_at(o,p,e,__func__)
static cog_worker_id_t fx_replacement(cog_orchestrator_t *o,cog_worker_id_t old)
{
    uint64_t end=fx_now()+4000;
    do {
        fx_ack(o);(void)fx_service(o);cog_core_info_t infos[16];size_t count=0;
        FX_CHECK(cog_orchestrator_list(o,infos,16,&count)==COG_OK);
        if(count==1&&!fx_same(infos[0].worker_id,old)&&infos[0].health==COG_HEALTH_HEALTHY) return infos[0].worker_id;
        fx_pause();
    } while(fx_now()<end);
    FX_CHECK(false);fx_abort(o);return (cog_worker_id_t){0};
}

static void test_kill_self_connected(void)
{
    char path[96];cog_orchestrator_t *o=fx_new(path,sizeof(path),false);
    cog_ipc_endpoint_t *client=NULL;FX_CHECK(cog_ipc_endpoint_new(&client)==COG_OK);
    cog_result_t rc=cog_ipc_endpoint_connect(client,path,NULL);FX_CHECK(rc==COG_OK||rc==COG_ERROR_AGAIN);
    uint64_t end=fx_now()+50,deadline;
    while(fx_now()<end) { (void)fx_service(o);(void)cog_ipc_endpoint_service(client,fx_now(),&deadline);fx_pause(); }
    size_t count=99;FX_CHECK(cog_orchestrator_list(o,NULL,0,&count)==COG_OK&&count==0);
    /* Regression: the all-zero result of failed readiness/replacement cannot
       reach kill(0) or any native signal; an unknown nonzero tuple also refuses. */
    FX_CHECK(!fx_signal_worker(o,(cog_worker_id_t){0},SIGKILL));
    FX_CHECK(!fx_signal_worker(o,(cog_worker_id_t){1,1},SIGKILL));
    /* An unowned transport or guessed managed tuple cannot select this PID. */
    FX_CHECK(cog_orchestrator_kill(o,(cog_worker_id_t){1,1})==COG_ERROR_NOT_FOUND);
    FX_CHECK(kill(getpid(),0)==0);FX_CHECK(cog_ipc_endpoint_free(client)==COG_OK);
    fx_close(o,path,COG_OK);
}
static void test_repeated_spawn_kill(void)
{
    char path[96];cog_orchestrator_t *o=fx_new(path,sizeof(path),false);pid_t pids[5];
    for(size_t i=0;i<5;++i) {
        cog_worker_id_t id=fx_spawn(o);pids[i]=(pid_t)fx_info(o,id).pid;
        FX_CHECK(cog_orchestrator_kill(o,id)==COG_OK);FX_CHECK(fx_wait(o,id,true));
        FX_CHECK(pids[i]>0&&kill(pids[i],0)==-1);fx_ack(o);
        FX_CHECK(fx_release(o,id)==COG_OK);
    }
    fx_close(o,path,COG_OK);
    for(size_t i=0;i<5;++i) FX_CHECK(pids[i]>0&&kill(pids[i],0)==-1);
}
static void test_auto_replace_repeated(void)
{
    char path[96];cog_orchestrator_t *o=fx_new(path,sizeof(path),true);
    cog_worker_id_t id=fx_spawn(o);
    for(size_t i=0;i<3;++i) {
        cog_worker_id_t old=id;pid_t pid=(pid_t)fx_info(o,id).pid;
        bool signalled=fx_signal_worker(o,id,SIGKILL);FX_CHECK(signalled);if(!signalled) fx_abort(o);FX_CHECK(fx_wait(o,old,true));
        FX_CHECK(fx_info(o,old).reaped);fx_ack(o);id=fx_replacement(o,old);
        FX_CHECK(!fx_same(old,id)&&pid>0&&kill(pid,0)==-1);
    }
    fx_close(o,path,COG_OK);
}
static void test_stop_ackhang(void)
{
    char path[96];cog_orchestrator_t *o=fx_new(path,sizeof(path),false);
    cog_worker_id_t id=fx_spawn(o);fx_command(o,id,3,0);
    uint64_t start=fx_now();FX_CHECK(cog_orchestrator_stop(o,id)==COG_OK);
    FX_CHECK(fx_now()-start<100);FX_CHECK(fx_wait(o,id,true));
    /* Per-worker stop is bounded and reaped; all-child shutdown is still explicit. */
    fx_close(o,path,COG_OK);
}
static void test_shutdown_ackhang(void)
{
    char path[96];cog_orchestrator_t *o=fx_new(path,sizeof(path),false);
    cog_worker_id_t id=fx_spawn(o);fx_command(o,id,3,0);
    uint64_t start=fx_now();fx_close(o,path,COG_ERROR_TIMEOUT);FX_CHECK(fx_now()-start<4000);
}
static void test_auto_replace_ackhang(void)
{
    char path[96];cog_orchestrator_t *o=fx_new(path,sizeof(path),true);
    cog_worker_id_t id=fx_spawn(o);fx_command(o,id,3,0);pid_t pid=(pid_t)fx_info(o,id).pid;
    FX_CHECK(cog_orchestrator_stop(o,id)==COG_OK);
    uint64_t end=fx_now()+50;
    while(fx_now()<end) { (void)fx_service(o);fx_pause(); }
    FX_CHECK(fx_same(fx_info(o,id).worker_id,id)&&pid>0&&kill(pid,0)==0);
    FX_CHECK(fx_wait(o,id,true));fx_ack(o);
    for(size_t i=0;i<20;++i) { (void)fx_service(o);fx_pause(); }
    FX_CHECK(fx_same(fx_info(o,id).worker_id,id)); /* intentional stop is not replay/replacement */
    fx_close(o,path,COG_OK);FX_CHECK(pid>0&&kill(pid,0)==-1);
}
static void test_auto_replace_real_crash(void)
{
    char path[96];cog_orchestrator_t *o=fx_new(path,sizeof(path),true);
    cog_worker_id_t old=fx_spawn(o);fx_command(o,old,1,0);FX_CHECK(fx_wait(o,old,true));
    FX_CHECK(fx_info(o,old).reaped);fx_ack(o);
    cog_worker_id_t next=fx_replacement(o,old);FX_CHECK(!fx_same(next,old));fx_close(o,path,COG_OK);
}
int main(void)
{
    test_kill_self_connected();test_repeated_spawn_kill();test_auto_replace_repeated();test_stop_ackhang();
    test_shutdown_ackhang();test_auto_replace_ackhang();test_auto_replace_real_crash();return failures==0?0:1;
}

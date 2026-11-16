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
static void fx_close(cog_orchestrator_t *o,char *path,cog_result_t expected)
{
    FX_CHECK(cog_orchestrator_request_shutdown(o,fx_now(),300)==COG_OK);
    bool done=false;uint64_t deadline,end=fx_now()+4000;cog_result_t rc=COG_ERROR_AGAIN;
    while(!done&&fx_now()<end) { rc=cog_orchestrator_service(o,fx_now(),&done,&deadline);fx_pause(); }
    FX_CHECK(done);FX_CHECK(rc==expected);
    cog_core_info_t infos[16];size_t count=0;FX_CHECK(cog_orchestrator_list(o,infos,16,&count)==COG_OK);
    fx_ack(o);
    for(size_t i=0;i<count&&i<16;++i) {
        FX_CHECK(infos[i].reaped&&!infos[i].ownership_lost);
        FX_CHECK(cog_orchestrator_release_worker(o,infos[i].worker_id)==COG_OK);
    }
    if(done) cog_orchestrator_free(o);
    (void)unlink(path);
}
static cog_worker_id_t fx_replacement(cog_orchestrator_t *o,cog_worker_id_t old)
{
    uint64_t end=fx_now()+4000;
    do {
        (void)fx_service(o);cog_core_info_t infos[16];size_t count=0;
        FX_CHECK(cog_orchestrator_list(o,infos,16,&count)==COG_OK);
        if(count==1&&!fx_same(infos[0].worker_id,old)&&infos[0].health==COG_HEALTH_HEALTHY) return infos[0].worker_id;
        fx_pause();
    } while(fx_now()<end);
    FX_CHECK(false);fx_abort(o);return (cog_worker_id_t){0};
}

static void test_spawn_health_place(void)
{
    char path[96];cog_orchestrator_t *o=fx_new(path,sizeof(path),false);
    /* Host poison variables cannot become fixture configuration through exec. */
    (void)setenv("COG_AUX_MODE","crash",1);(void)setenv("COG_AUX_SESSION","forged",1);
    cog_worker_id_t id=fx_spawn(o);cog_core_info_t info=fx_info(o,id);
    FX_CHECK(cog_orchestrator_place(o,0)==info.peer_id);
    size_t count=0;FX_CHECK(cog_orchestrator_list(o,NULL,0,&count)==COG_OK&&count==1);
    fx_close(o,path,COG_OK);
    (void)unsetenv("COG_AUX_MODE");(void)unsetenv("COG_AUX_SESSION");
}
static void test_crash_recovery(void)
{
    char path[96];cog_orchestrator_t *o=fx_new(path,sizeof(path),false);
    cog_worker_id_t id=fx_spawn(o);cog_peer_id_t peer=fx_info(o,id).peer_id;
    FX_CHECK(cog_orchestrator_register_session(o,peer,"crash-sess")==COG_OK);
    fx_command(o,id,1,0);FX_CHECK(fx_wait(o,id,true));
    FX_CHECK(cog_orchestrator_health(o,id)==COG_HEALTH_DEAD);
    const char *sessions[8];size_t count=0;
    FX_CHECK(cog_orchestrator_sessions(o,peer,sessions,8,&count)==COG_OK);
    FX_CHECK(count==1&&strcmp(sessions[0],"crash-sess")==0);
    FX_CHECK(cog_orchestrator_release_worker(o,id)==COG_ERROR_INVALID_STATE);
    fx_ack(o);FX_CHECK(cog_orchestrator_release_worker(o,id)==COG_ERROR_INVALID_STATE);
    FX_CHECK(cog_orchestrator_unregister_session(o,peer,"crash-sess")==COG_OK);
    FX_CHECK(cog_orchestrator_release_worker(o,id)==COG_OK);
    FX_CHECK(cog_orchestrator_kill(o,id)==COG_ERROR_NOT_FOUND);
    fx_close(o,path,COG_OK);
}
static void test_kill(void)
{
    char path[96];cog_orchestrator_t *o=fx_new(path,sizeof(path),false);
    cog_worker_id_t id=fx_spawn(o);FX_CHECK(cog_orchestrator_kill(o,id)==COG_OK);
    FX_CHECK(fx_wait(o,id,true));FX_CHECK(cog_orchestrator_health(o,id)==COG_HEALTH_DEAD);
    fx_close(o,path,COG_OK);
}
static void test_auto_replace(void)
{
    char path[96];cog_orchestrator_t *o=fx_new(path,sizeof(path),true);
    cog_worker_id_t old=fx_spawn(o);cog_peer_id_t peer=fx_info(o,old).peer_id;
    /* Real crash, not stop intent: automatic idle replacement preserves the old
       record until the consumer ACKs its independently reserved transitions. */
    fx_command(o,old,1,0);FX_CHECK(fx_wait(o,old,true));
    FX_CHECK(fx_info(o,old).reaped);fx_ack(o);
    cog_worker_id_t replacement=fx_replacement(o,old);
    FX_CHECK(!fx_same(replacement,old)&&fx_info(o,replacement).peer_id!=peer);
    FX_CHECK(cog_orchestrator_kill(o,old)==COG_ERROR_NOT_FOUND);
    fx_close(o,path,COG_OK);
}
static void test_shutdown_timeout(void)
{
    char path[96];cog_orchestrator_t *o=fx_new(path,sizeof(path),false);
    cog_worker_id_t id=fx_spawn(o);fx_command(o,id,2,0);
    fx_close(o,path,COG_ERROR_TIMEOUT);
}
int main(void)
{
    test_spawn_health_place();test_crash_recovery();test_kill();test_auto_replace();test_shutdown_timeout();
    return failures==0?0:1;
}

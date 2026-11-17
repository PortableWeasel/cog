/** @file orchestrator.c @brief Public installed native lifecycle and codec proof. */
#define _POSIX_C_SOURCE 200809L
#include "installed_proof.h"
#include <cog/orchestrator.h>
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <unistd.h>

#define REQUIRE(x) do { if (!(x)) { fprintf(stderr,"installed orchestrator:%d: %s\n",__LINE__,#x); failed=1;goto cleanup; } } while(0)

static uint64_t now_ms(void)
{
    struct timespec ts;
    if(clock_gettime(CLOCK_MONOTONIC,&ts)!=0)return 0U;
    return (uint64_t)ts.tv_sec*1000U+(uint64_t)ts.tv_nsec/1000000U;
}
static bool same_worker(cog_worker_id_t a,cog_worker_id_t b)
{ return a.incarnation==b.incarnation && a.number==b.number; }
static bool consume_events(cog_orchestrator_t *orch,cog_worker_id_t id,
    bool *ready,bool *reaped,int *native_pid)
{
    unsigned char bytes[256];size_t required=0U;
    for(unsigned int cls=1U;cls<=2U;++cls) {
        for(;;) {
            cog_orchestrator_event_t event;
            cog_result_t rc=cog_orchestrator_copy_event(orch,(cog_orchestrator_event_class_t)cls,
                &event,bytes,sizeof(bytes),&required);
            if(rc==COG_ERROR_AGAIN)break;
            if(rc!=COG_OK || !same_worker(event.worker_id,id))return false;
            if(event.kind==COG_ORCHESTRATOR_READY) {
                if(event.peer_id==0U || event.peer_info.process_id==0U ||
                   event.peer_info.user_id!=(uint64_t)geteuid())return false;
                cog_core_info_t info;size_t count=0U;
                if(cog_orchestrator_list(orch,&info,1U,&count)!=COG_OK || count!=1U ||
                   info.pid<=0 || info.reaped || info.ownership_lost ||
                   info.peer_id!=event.peer_id || event.peer_info.process_id!=(uint64_t)info.pid)return false;
                *native_pid=info.pid;*ready=true;
            }
            if(event.kind==COG_ORCHESTRATOR_REAPED)*reaped=true;
            if(event.kind==COG_ORCHESTRATOR_OWNERSHIP_LOST || event.kind==COG_ORCHESTRATOR_MESSAGE)return false;
            if(cog_orchestrator_ack_event(orch,event.event_id)!=COG_OK ||
               cog_orchestrator_ack_event(orch,event.event_id)!=COG_OK)return false;
        }
    }
    return true;
}
static bool installed_binary(const char *executable,int pid)
{
    char path[128];struct stat expected,actual;
    int n=snprintf(path,sizeof(path),"/proc/%d/exe",pid);
    if(n<=0 || (size_t)n>=sizeof(path) || stat(executable,&expected)!=0 || stat(path,&actual)!=0 ||
       expected.st_dev!=actual.st_dev || expected.st_ino!=actual.st_ino)return false;
    n=snprintf(path,sizeof(path),"/proc/%d/environ",pid);
    if(n<=0 || (size_t)n>=sizeof(path))return false;
    FILE *stream=fopen(path,"rb");if(stream==NULL)return false;
    unsigned char bytes[4096];size_t count=fread(bytes,1U,sizeof(bytes),stream);
    bool ok=!ferror(stream) && count<sizeof(bytes);
    if(fclose(stream)!=0)return false;
    /* This is only our own worker's expected minimal environment. No bytes are
       printed, copied to receipts, or compared against external credentials. */
    static const char *const blocked[]={"synthetic-installed-proof","COG_INSTALLED_PROOF_KEY","LD_LIBRARY_PATH"};
    for(size_t k=0U;k<sizeof(blocked)/sizeof(blocked[0]);++k) {
        size_t length=strlen(blocked[k]);
        for(size_t i=0U;i+length<=count;++i)if(memcmp(bytes+i,blocked[k],length)==0)ok=false;
    }
    return ok;
}
static bool codecs(void)
{
    unsigned char wire[256];size_t required=0U;
    cog_worker_control_t control={.kind=COG_WORKER_HELLO,.worker={7U,11U}},decoded;
    if(cog_orchestrator_worker_encode(&control,wire,sizeof(wire),&required)!=COG_OK || required!=37U ||
       cog_orchestrator_worker_decode(wire,required,&decoded)!=COG_OK ||
       decoded.kind!=control.kind || !same_worker(decoded.worker,control.worker))return false;
    cog_handoff_message_t handoff={.kind=COG_HANDOFF_PREPARED,.session_id="installed-route",
        .source_peer=0U,.target_peer=5U,.expected={1U,21U},.next={2U,22U},.checkpoint={3U}},copy;
    char session[128];
    return cog_orchestrator_handoff_encode(&handoff,wire,sizeof(wire),&required)==COG_OK &&
        cog_orchestrator_handoff_decode(wire,required,&copy,session,sizeof(session),&required)==COG_OK &&
        copy.kind==handoff.kind && strcmp(copy.session_id,handoff.session_id)==0 &&
        copy.source_peer==0U && copy.target_peer==5U && copy.expected.epoch==1U &&
        copy.next.epoch==2U && copy.checkpoint.generation==3U;
}
int installed_orchestrator_proof(const char *executable,const char *directory)
{
    int failed=0,native_pid=0;bool spawned=false,ready=false,reaped=false,done=false;
    cog_orchestrator_t *orch=NULL;cog_store_t *store=NULL;cog_session_t *session=NULL;
    cog_store_owner_guard_t *guard=NULL;cog_worker_id_t worker={0};
    REQUIRE(codecs());
    REQUIRE(cog_store_new(&store,&(cog_store_config_t){.base_dir=directory,.require_owner=true,
        .max_record_bytes=1048576U})==COG_OK);
    REQUIRE(cog_session_new(&session,&(cog_session_config_t){.id="installed-route"})==COG_OK);
    cog_store_owner_t owner={1U,41U};cog_store_checkpoint_t checkpoint={0};
    REQUIRE(cog_store_owner_compare_exchange(store,"installed-route",(cog_store_owner_t){0},owner,checkpoint)==COG_OK);
    REQUIRE(cog_store_owner_acquire(store,"installed-route",owner,&guard)==COG_OK);
    REQUIRE(cog_store_checkpoint_save(store,session,guard,&checkpoint)==COG_OK && checkpoint.generation!=0U);
    cog_orchestrator_config_t config={.socket_path="installed-worker.sock",.max_aux_cores=1U,
        .default_core_capacity=1U,.max_frame_bytes=4096U,.message_bytes=4096U,.message_events=2U,
        .health_interval_ms=1000U,.shutdown_timeout_ms=1000U,.store=store};
    REQUIRE(cog_orchestrator_new(&orch,&config)==COG_OK);
    REQUIRE(cog_orchestrator_publish_owner(orch,"installed-route",0U,owner,checkpoint)==COG_OK);
    cog_peer_id_t peer=99U;cog_store_owner_t actual;cog_store_checkpoint_t copied;
    REQUIRE(cog_orchestrator_session_owner(orch,"installed-route",&peer,&actual,&copied)==COG_OK &&
        peer==0U && actual.epoch==owner.epoch && actual.instance_id==owner.instance_id && copied.generation==checkpoint.generation);
    REQUIRE(cog_orchestrator_forget_owner(orch,"installed-route",(cog_store_owner_t){2U,42U})==COG_ERROR_INVALID_STATE);
    REQUIRE(cog_orchestrator_forget_owner(orch,"installed-route",owner)==COG_OK);
    REQUIRE(cog_orchestrator_session_owner(orch,"installed-route",&peer,&actual,&copied)==COG_ERROR_NOT_FOUND);
    REQUIRE(cog_orchestrator_spawn(orch,executable,&worker)==COG_OK);spawned=true;
    REQUIRE(worker.incarnation!=0U && worker.number!=0U);
    uint64_t end=now_ms()+4000U;
    while(!ready && now_ms()<end) {
        uint64_t deadline;bool ended;
        cog_result_t rc=cog_orchestrator_service(orch,now_ms(),&ended,&deadline);
        REQUIRE(rc==COG_OK || rc==COG_ERROR_AGAIN);
        REQUIRE(!ended && consume_events(orch,worker,&ready,&reaped,&native_pid));
        (void)poll(NULL,0U,1);
    }
    REQUIRE(ready && !reaped && native_pid>0 && cog_orchestrator_health(orch,worker)==COG_HEALTH_HEALTHY);
    REQUIRE(installed_binary(executable,native_pid));
cleanup:
    if(orch!=NULL) {
        uint64_t cleanup_end=now_ms()+6000U;
        if(cog_orchestrator_request_shutdown(orch,now_ms(),1000U)!=COG_OK)failed=1;
        while(!done && now_ms()<cleanup_end) {
            uint64_t deadline;
            cog_result_t rc=cog_orchestrator_service(orch,now_ms(),&done,&deadline);
            if(rc!=COG_OK && rc!=COG_ERROR_AGAIN)failed=1;
            if(spawned && !consume_events(orch,worker,&ready,&reaped,&native_pid))failed=1;
            if(!done)(void)poll(NULL,0U,1);
        }
        if(spawned) {
            cog_core_info_t info;size_t count=0U;
            if(!done || !ready || !reaped || cog_orchestrator_list(orch,&info,1U,&count)!=COG_OK ||
               count!=1U || !info.reaped || info.ownership_lost || info.pid!=native_pid)failed=1;
            if(done) {
                /* Public retained reaped fact is independently corroborated by
                   exclusive native wait ownership: this child is already gone. */
                int status;errno=0;
                if(native_pid<=0 || waitpid((pid_t)native_pid,&status,WNOHANG)!=-1 || errno!=ECHILD)failed=1;
                cog_result_t retired=COG_ERROR_INVALID_STATE;
                uint64_t retire_end=now_ms()+1000U;
                while(retired==COG_ERROR_INVALID_STATE && now_ms()<retire_end) {
                    uint64_t deadline;
                    (void)cog_orchestrator_service(orch,now_ms(),&done,&deadline);
                    if(!consume_events(orch,worker,&ready,&reaped,&native_pid))failed=1;
                    retired=cog_orchestrator_release_worker(orch,worker);
                    if(retired==COG_ERROR_INVALID_STATE)(void)poll(NULL,0U,1);
                }
                if(retired!=COG_OK)failed=1;
                if(cog_orchestrator_health(orch,worker)!=COG_HEALTH_UNKNOWN)failed=1;
                if(cog_orchestrator_kill(orch,worker)!=COG_ERROR_NOT_FOUND)failed=1;
            }
        }
        if(done)cog_orchestrator_free(orch);
        else { /* Keep the owner for a final bounded exact-worker kill/reap. */
            if(spawned)(void)cog_orchestrator_kill(orch,worker);
            cleanup_end=now_ms()+5000U;
            while(!done && now_ms()<cleanup_end) {
                uint64_t deadline;(void)cog_orchestrator_service(orch,now_ms(),&done,&deadline);
                if(spawned)(void)consume_events(orch,worker,&ready,&reaped,&native_pid);
                (void)poll(NULL,0U,1);
            }
            if(done)cog_orchestrator_free(orch);
            else return 1; /* Do not destroy a still-owned native context. */
        }
    }
    if(guard!=NULL && cog_store_owner_release(guard)!=COG_OK)failed=1;
    cog_session_free(session);cog_store_free(store);
    if(!failed)puts("installed actual native worker READY/reap and public CGW1/CGH1/routes: PASS");
    return failed;
}

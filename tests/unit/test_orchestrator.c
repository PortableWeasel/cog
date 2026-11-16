/** @file test_orchestrator.c @brief Real owned worker and bounded lifecycle contracts. */
#define _GNU_SOURCE
#include <cog/orchestrator.h>
#include "orchestrator_layout.h"
#include "orchestrator_internal.h"
#include <assert.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

void *__real_malloc(size_t bytes); void *__wrap_malloc(size_t bytes);
void *__real_calloc(size_t count,size_t width); void *__wrap_calloc(size_t count,size_t width);
void *__real_realloc(void *data,size_t bytes); void *__wrap_realloc(void *data,size_t bytes);
static size_t allocation_calls,fail_allocation;
static bool watch_allocations,deny_allocations;
static bool allocation_fail(void)
{
    assert(!deny_allocations);
    return watch_allocations && ++allocation_calls==fail_allocation;
}
void *__wrap_malloc(size_t bytes) { return allocation_fail()?NULL:__real_malloc(bytes); }
void *__wrap_calloc(size_t count,size_t width) { return allocation_fail()?NULL:__real_calloc(count,width); }
void *__wrap_realloc(void *data,size_t bytes) { return allocation_fail()?NULL:__real_realloc(data,bytes); }
static uint64_t now_ms(void)
{
    struct timespec t;assert(clock_gettime(CLOCK_MONOTONIC,&t)==0);
    return (uint64_t)t.tv_sec*1000U+(uint64_t)t.tv_nsec/1000000U;
}
static void pause_unit(void) { const struct timespec t={0,1000000L};(void)nanosleep(&t,NULL); }
static int worker_main(int argc,char **argv)
{
    assert(argc==14 && strcmp(argv[1],"--worker")==0);
    assert(strcmp(argv[2],"--control-socket")==0 && strcmp(argv[4],"--worker-incarnation")==0 &&
        strcmp(argv[6],"--worker-number")==0 && strcmp(argv[8],"--parent-pid")==0 &&
        strcmp(argv[10],"--executor-slots")==0 && strcmp(argv[12],"--frame-bytes")==0);
    assert(getenv("COG_TEST_PARENT_SECRET")==NULL && getenv("LD_PRELOAD")==NULL);
    assert(strcmp(getenv("PATH"),"/usr/bin:/bin")==0 && strcmp(getenv("LC_ALL"),"C.UTF-8")==0);
    sigset_t mask;assert(sigprocmask(SIG_SETMASK,NULL,&mask)==0);
    assert(sigismember(&mask,SIGINT)==0 && sigismember(&mask,SIGTERM)==0);
    uint64_t physical=strtoull(argv[11],NULL,10);assert(physical>=3U);
    cog_worker_id_t id={strtoull(argv[5],NULL,10),strtoull(argv[7],NULL,10)};
    cog_ipc_endpoint_config_t config={0};config.max_frame_bytes=strtoull(argv[13],NULL,10);
    config.receive_frames=2U;config.send_frames=2U;config.wipe_buffers=true;
    cog_ipc_endpoint_t *endpoint;assert(cog_ipc_endpoint_new_ex(&endpoint,&config)==COG_OK);
    cog_result_t rc=cog_ipc_endpoint_connect(endpoint,argv[3],NULL);assert(rc==COG_ERROR_AGAIN || rc==COG_OK);
    unsigned char wire[256];size_t required;bool hello_sent=false,closing=false,stay=false;
    uint64_t limit=now_ms()+10000U,heartbeat=0U;unsigned flood=0U;
    while(now_ms()<limit) {
        uint64_t next;rc=cog_ipc_endpoint_service(endpoint,now_ms(),&next);
        if(rc!=COG_OK && rc!=COG_ERROR_AGAIN) break;
        cog_peer_id_t peer;
        if(!hello_sent && cog_ipc_endpoint_get_peer_id(endpoint,&peer)==COG_OK) {
            cog_ipc_peer_info_t proof;
            if(cog_ipc_endpoint_peer_info(endpoint,peer,&proof)!=COG_OK) { pause_unit();continue; }
            assert(proof.process_id==strtoull(argv[9],NULL,10));
            cog_worker_control_t message={COG_WORKER_HELLO,id,0U,0U,COG_OK};
            assert(cog_orchestrator_worker_encode(&message,wire,sizeof(wire),&required)==COG_OK);
            cog_ipc_message_t output={wire,required,sizeof(wire)};
            if(cog_ipc_endpoint_send(endpoint,&output)==COG_OK) hello_sent=true;
        }
        if(hello_sent && !closing && now_ms()>=heartbeat && flood==0U) {
            cog_worker_control_t message={COG_WORKER_HEARTBEAT,id,0U,0U,COG_OK};
            assert(cog_orchestrator_worker_encode(&message,wire,sizeof(wire),&required)==COG_OK);
            cog_ipc_message_t output={wire,required,sizeof(wire)};
            if(cog_ipc_endpoint_send(endpoint,&output)==COG_OK) heartbeat=now_ms()+20U;
        }
        if(flood!=0U) {
            unsigned char payload[48];memset(payload,(int)flood,sizeof(payload));
            cog_ipc_message_t output={payload,sizeof(payload),sizeof(payload)};
            if(cog_ipc_endpoint_send(endpoint,&output)==COG_OK) --flood;
        }
        cog_ipc_message_t input={wire,0U,sizeof(wire)};rc=cog_ipc_endpoint_recv(endpoint,&input);
        if(rc==COG_OK) {
            if(input.len==1U) {
                if(wire[0]=='X') break;
                if(wire[0]=='F') flood=6U;
                if(wire[0]=='S') stay=true;
            } else {
                cog_worker_control_t message;
                assert(cog_orchestrator_worker_decode(wire,input.len,&message)==COG_OK && message.kind==COG_WORKER_SHUTDOWN);
                message.kind=COG_WORKER_SHUTDOWN_ACK;
                assert(cog_orchestrator_worker_encode(&message,wire,sizeof(wire),&required)==COG_OK);
                cog_ipc_message_t output={wire,required,sizeof(wire)};
                do {
                    rc=cog_ipc_endpoint_send(endpoint,&output);
                    if(rc==COG_ERROR_AGAIN) { assert(cog_ipc_endpoint_service(endpoint,now_ms(),&next)==COG_OK);pause_unit(); }
                } while(rc==COG_ERROR_AGAIN);
                assert(rc==COG_OK);closing=true;
            }
        } else assert(rc==COG_ERROR_AGAIN || rc==COG_ERROR_INVALID_STATE);
        if(closing && !stay) {
            for(unsigned i=0U;i<8U;++i) { (void)cog_ipc_endpoint_service(endpoint,now_ms(),&next);pause_unit(); }
            break;
        }
        pause_unit();
    }
    assert(cog_ipc_endpoint_free(endpoint)==COG_OK);return 0;
}
static cog_orchestrator_t *make(char path[64],size_t workers)
{
    strcpy(path,"cog-orch-unit-XXXXXX");int fd=mkstemp(path);assert(fd>=0 && close(fd)==0 && unlink(path)==0);
    cog_orchestrator_config_t cfg={0};cfg.socket_path=path;cfg.max_aux_cores=workers;
    cfg.default_core_capacity=4U;cfg.max_frame_bytes=128U;cfg.message_bytes=128U;cfg.message_events=2U;
    cfg.health_interval_ms=1000U;cfg.shutdown_timeout_ms=100U;
    cog_orchestrator_t *o;assert(cog_orchestrator_new(&o,&cfg)==COG_OK);return o;
}
static void service(cog_orchestrator_t *o)
{
    bool done;uint64_t deadline;cog_result_t rc=cog_orchestrator_service(o,now_ms(),&done,&deadline);
    assert(rc==COG_OK || rc==COG_ERROR_AGAIN);pause_unit();
}
static cog_core_info_t info(cog_orchestrator_t *o,cog_worker_id_t id)
{
    cog_core_info_t list[4];size_t count;assert(cog_orchestrator_list(o,list,4U,&count)==COG_OK);
    for(size_t i=0U;i<count && i<4U;++i) if(list[i].worker_id.incarnation==id.incarnation && list[i].worker_id.number==id.number) return list[i];
    assert(false);return (cog_core_info_t){0};
}
static void ready(cog_orchestrator_t *o,cog_worker_id_t id)
{
    uint64_t until=now_ms()+3000U;
    while(now_ms()<until && cog_orchestrator_health(o,id)!=COG_HEALTH_HEALTHY) service(o);
    assert(cog_orchestrator_health(o,id)==COG_HEALTH_HEALTHY);
}
static void acknowledge(cog_orchestrator_t *o)
{
    unsigned char storage[256];cog_orchestrator_event_t event;size_t required;
    for(unsigned cls=1U;cls<=2U;++cls)
        while(cog_orchestrator_copy_event(o,(cog_orchestrator_event_class_t)cls,&event,storage,sizeof(storage),&required)==COG_OK) {
            assert(cog_orchestrator_ack_event(o,event.event_id)==COG_OK);
            assert(cog_orchestrator_ack_event(o,event.event_id)==COG_OK);
        }
}
static void command(cog_orchestrator_t *o,cog_worker_id_t id,unsigned char value)
{
    cog_ipc_message_t message={&value,1U,1U};cog_result_t rc;
    do { rc=cog_orchestrator_send(o,id,&message);if(rc==COG_ERROR_AGAIN) service(o); } while(rc==COG_ERROR_AGAIN);
    assert(rc==COG_OK);
}
static void lifecycle(const char *binary)
{
    char path[64];cog_orchestrator_t *o=make(path,2U);cog_worker_id_t first,second;
    assert(setenv("COG_TEST_PARENT_SECRET","must-not-inherit",1)==0);
    sigset_t blocked,old;assert(sigemptyset(&blocked)==0 && sigaddset(&blocked,SIGINT)==0 && sigaddset(&blocked,SIGTERM)==0);
    assert(sigprocmask(SIG_BLOCK,&blocked,&old)==0);
    assert(cog_orchestrator_spawn(o,binary,&first)==COG_OK);
    assert(cog_orchestrator_health(o,first)==COG_HEALTH_UNKNOWN && info(o,first).peer_id==0U);
    assert(sigprocmask(SIG_SETMASK,&old,NULL)==0);assert(unsetenv("COG_TEST_PARENT_SECRET")==0);
    assert(cog_orchestrator_spawn(o,binary,&second)==COG_OK);
    assert(first.number!=second.number && first.incarnation==second.incarnation);
    cog_worker_id_t untouched={77U,88U};assert(cog_orchestrator_spawn(o,binary,&untouched)==COG_ERROR_OVERFLOW);
    assert(untouched.number==88U);ready(o,first);ready(o,second);
    cog_core_info_t a=info(o,first),b=info(o,second);
    assert(a.pid!=b.pid && a.pid>0 && b.pid>0 && a.peer_id!=b.peer_id && a.peer_id!=0U);
    assert(cog_orchestrator_place(o,0U)==a.peer_id);
    assert(cog_orchestrator_heartbeat(o,a.peer_id,4U)==COG_OK && cog_orchestrator_place(o,0U)==b.peer_id);
    assert(cog_orchestrator_heartbeat(o,b.peer_id,4U)==COG_OK && cog_orchestrator_place(o,0U)==0U);
    assert(cog_orchestrator_heartbeat(o,a.peer_id,5U)==COG_ERROR_INVALID_ARG);
    assert(cog_orchestrator_register_session(o,a.peer_id,"diagnosis")==COG_OK);
    const char *sessions[2];size_t count;
    assert(cog_orchestrator_sessions(o,a.peer_id,sessions,2U,&count)==COG_OK && count==1U && strcmp(sessions[0],"diagnosis")==0);
    assert(cog_orchestrator_shutdown(o,1000U)==COG_OK);
    assert(info(o,first).reaped && info(o,second).reaped);
    assert(cog_orchestrator_kill(o,first)==COG_OK);
    assert(cog_orchestrator_release_worker(o,first)==COG_ERROR_INVALID_STATE);
    acknowledge(o);
    assert(cog_orchestrator_unregister_session(o,a.peer_id,"diagnosis")==COG_OK);
    /* EOF drain can follow native reap; its retained proof remains exact. */
    for(unsigned i=0U;i<16U;++i) { service(o);acknowledge(o); }
    assert(cog_orchestrator_release_worker(o,first)==COG_OK);
    assert(cog_orchestrator_kill(o,first)==COG_ERROR_NOT_FOUND);
    assert(cog_orchestrator_release_worker(o,second)==COG_OK);cog_orchestrator_free(o);
    assert(access(path,F_OK)!=0);
}
static void loss_and_pressure(const char *binary)
{
    char path[64];cog_orchestrator_t *o=make(path,1U);cog_worker_id_t id;
    assert(cog_orchestrator_spawn(o,binary,&id)==COG_OK);ready(o,id);acknowledge(o);
    command(o,id,'F');
    uint64_t until=now_ms()+1000U;
    while(now_ms()<until && o->message_count<2U) service(o);
    assert(o->message_count==2U && o->bytes_used==96U);
    cog_orchestrator_event_t event,before;memset(&event,0xa5,sizeof(event));before=event;
    unsigned char storage[128];memset(storage,0xcc,sizeof(storage));size_t needed=0U;
    assert(cog_orchestrator_copy_event(o,COG_ORCHESTRATOR_MESSAGE_EVENTS,&event,storage,1U,&needed)==COG_ERROR_OVERFLOW);
    assert(needed==48U && memcmp(&event,&before,sizeof(event))==0 && storage[0]==0xcc);
    assert(cog_orchestrator_stop(o,id)==COG_OK);
    bool done=false;uint64_t deadline;
    assert(cog_orchestrator_request_shutdown(o,now_ms(),50U)==COG_OK);
    cog_result_t rc=COG_OK;until=now_ms()+2000U;
    while(!done && now_ms()<until) { rc=cog_orchestrator_service(o,now_ms(),&done,&deadline);pause_unit(); }
    assert(done && (rc==COG_OK || rc==COG_ERROR_TIMEOUT));assert(info(o,id).reaped);
    assert(o->control_count>=1U && o->message_count==2U);
    assert(cog_orchestrator_copy_event(o,COG_ORCHESTRATOR_MESSAGE_EVENTS,&event,storage,sizeof(storage),&needed)==COG_OK);
    assert(event.worker_id.number==id.number && event.peer_info.process_id==(uint64_t)info(o,id).pid);
    for(size_t i=0U;i<needed;++i) assert(storage[i]==6U);
    assert(cog_orchestrator_ack_event(o,event.event_id)==COG_OK);
    cog_orchestrator_free(o);

    o=make(path,1U);assert(cog_orchestrator_spawn(o,binary,&id)==COG_OK);ready(o,id);
    cog_core_info_t child=info(o,id);assert(kill(child.pid,SIGKILL)==0);
    int status;assert(waitpid(child.pid,&status,0)==child.pid); /* Deliberate host-profile violation. */
    assert(cog_orchestrator_kill(o,id)==COG_ERROR_IO);
    assert(info(o,id).ownership_lost && !info(o,id).reaped);
    assert(cog_orchestrator_request_shutdown(o,now_ms(),0U)==COG_OK);
    assert(cog_orchestrator_service(o,now_ms(),&done,&deadline)==COG_ERROR_IO && done);
    bool lost=false;
    while(cog_orchestrator_copy_event(o,COG_ORCHESTRATOR_CONTROL_EVENTS,&event,NULL,0U,&needed)==COG_OK) {
        if(event.kind==COG_ORCHESTRATOR_OWNERSHIP_LOST) lost=true;
        assert(cog_orchestrator_ack_event(o,event.event_id)==COG_OK);
    }
    assert(lost);cog_orchestrator_free(o);
}
static void boundaries(const char *binary)
{
    cog_orchestrator_t *o=NULL;cog_orchestrator_config_t cfg={0};
    assert(cog_orchestrator_new(NULL,&cfg)==COG_ERROR_NULL_PTR);
    assert(cog_orchestrator_new(&o,NULL)==COG_ERROR_NULL_PTR);
    assert(cog_orchestrator_new(&o,&cfg)==COG_ERROR_INVALID_ARG);
    char path[64];o=make(path,1U);cog_worker_id_t unknown={1U,1U},id;
    assert(cog_orchestrator_spawn(NULL,binary,&id)==COG_ERROR_NULL_PTR);
    assert(cog_orchestrator_spawn(o,NULL,&id)==COG_ERROR_NULL_PTR);
    assert(cog_orchestrator_spawn(o,binary,NULL)==COG_ERROR_NULL_PTR);
    assert(cog_orchestrator_spawn(o,"relative",&id)==COG_ERROR_INVALID_ARG);
    assert(cog_orchestrator_stop(NULL,unknown)==COG_ERROR_NULL_PTR);
    assert(cog_orchestrator_kill(NULL,unknown)==COG_ERROR_NULL_PTR);
    assert(cog_orchestrator_stop(o,unknown)==COG_ERROR_NOT_FOUND);
    assert(cog_orchestrator_kill(o,unknown)==COG_ERROR_NOT_FOUND);
    assert(cog_orchestrator_health(NULL,unknown)==COG_HEALTH_UNKNOWN);
    assert(cog_orchestrator_place(NULL,0U)==0U && cog_orchestrator_shutdown(NULL,0U)==COG_ERROR_NULL_PTR);
    size_t count;assert(cog_orchestrator_list(o,NULL,0U,&count)==COG_OK && count==0U);
    assert(cog_orchestrator_list(o,NULL,1U,&count)==COG_ERROR_NULL_PTR);
    bool done=true;uint64_t deadline=123U;
    assert(cog_orchestrator_service(o,o->now_ms-1U,&done,&deadline)==COG_ERROR_INVALID_ARG && done && deadline==123U);
    o->last_worker_number=UINT64_MAX;
    assert(cog_orchestrator_spawn(o,binary,&id)==COG_ERROR_LIMIT);o->last_worker_number=0U;
    o->last_event_id=UINT64_MAX-2U;
    assert(cog_orchestrator_spawn(o,binary,&id)==COG_ERROR_LIMIT);o->last_event_id=0U;
    /* Exec failure is postpublication ownership, never synchronous spawn IO. */
    assert(cog_orchestrator_spawn(o,"/nonexistent/cog-worker",&id)==COG_OK);
    uint64_t until=now_ms()+1000U;
    while(!info(o,id).reaped && now_ms()<until) service(o);
    assert(info(o,id).reaped && info(o,id).peer_id==0U && cog_orchestrator_health(o,id)==COG_HEALTH_DEAD);
    acknowledge(o);assert(cog_orchestrator_release_worker(o,id)==COG_OK);
    cog_worker_id_t old=id;
    assert(cog_orchestrator_spawn(o,binary,&id)==COG_OK && id.number>old.number);
    assert(cog_orchestrator_kill(o,old)==COG_ERROR_NOT_FOUND);
    assert(cog_orchestrator_stop(o,id)==COG_OK); /* Before connection. */
    cog_result_t rc=cog_orchestrator_shutdown(o,100U);assert(rc==COG_OK || rc==COG_ERROR_TIMEOUT);
    cog_orchestrator_free(o);cog_orchestrator_free(NULL);
    struct sigaction previous,ignored={0};ignored.sa_handler=SIG_IGN;assert(sigemptyset(&ignored.sa_mask)==0);
    assert(sigaction(SIGCHLD,&ignored,&previous)==0);cfg.socket_path="ignored-orch.sock";
    assert(cog_orchestrator_new(&o,&cfg)==COG_ERROR_UNSUPPORTED);
    assert(sigaction(SIGCHLD,&previous,NULL)==0);
    ignored.sa_handler=SIG_DFL;ignored.sa_flags=SA_NOCLDWAIT;
    assert(sigaction(SIGCHLD,&ignored,&previous)==0);
    assert(cog_orchestrator_new(&o,&cfg)==COG_ERROR_UNSUPPORTED);
    assert(sigaction(SIGCHLD,&previous,NULL)==0);
    /* The orchestrator must never reap a different host child. */
    o=make(path,1U);pid_t unrelated=fork();assert(unrelated>=0);
    if(unrelated==0) _exit(23);
    for(unsigned i=0U;i<8U;++i) service(o);
    int status;assert(waitpid(unrelated,&status,0)==unrelated && WEXITSTATUS(status)==23);cog_orchestrator_free(o);
    cfg.default_core_capacity=UINT32_MAX;orch_reservations_t plan;
    assert(orch_reservation_plan(&cfg,&plan)==COG_OK && plan.worker_executor_slots==UINT64_C(4294967297));
    cfg.max_aux_cores=SIZE_MAX;assert(orch_reservation_plan(&cfg,&plan)==COG_ERROR_INVALID_ARG);
}
static void allocation_bounds(const char *binary)
{
    char path[]="cog-orch-allocation-XXXXXX";int fd=mkstemp(path);assert(fd>=0 && close(fd)==0 && unlink(path)==0);
    cog_orchestrator_config_t cfg={0};cfg.socket_path=path;cfg.max_aux_cores=2U;
    cfg.max_frame_bytes=128U;cfg.message_bytes=256U;
    cog_orchestrator_t *o;allocation_calls=0U;fail_allocation=0U;watch_allocations=true;
    assert(cog_orchestrator_new(&o,&cfg)==COG_OK);watch_allocations=false;
    size_t count=allocation_calls;assert(count>10U);cog_orchestrator_free(o);
    for(size_t i=1U;i<=count;++i) {
        allocation_calls=0U;fail_allocation=i;watch_allocations=true;
        assert(cog_orchestrator_new(&o,&cfg)==COG_ERROR_NOMEM && o==NULL);
        watch_allocations=false;assert(access(path,F_OK)!=0);
    }
    char other[64];o=make(other,1U);cog_worker_id_t id;
    assert(cog_orchestrator_spawn(o,binary,&id)==COG_OK);ready(o,id);acknowledge(o);
    deny_allocations=true;
    for(unsigned i=0U;i<20U;++i) service(o);
    command(o,id,'F');
    for(unsigned i=0U;i<50U;++i) { service(o);acknowledge(o); }
    assert(cog_orchestrator_request_shutdown(o,now_ms(),500U)==COG_OK);
    bool done=false;uint64_t deadline;uint64_t until=now_ms()+1500U;
    while(!done && now_ms()<until) {
        cog_result_t rc=cog_orchestrator_service(o,now_ms(),&done,&deadline);
        assert(rc==COG_OK || rc==COG_ERROR_AGAIN);acknowledge(o);pause_unit();
    }
    assert(done);deny_allocations=false;cog_orchestrator_free(o);
}
static void impostor_and_ids(const char *binary)
{
    char path[64];cog_orchestrator_t *o=make(path,2U);cog_worker_id_t id;
    assert(cog_orchestrator_spawn(o,binary,&id)==COG_OK);
    cog_ipc_endpoint_t *fake;assert(cog_ipc_endpoint_new(&fake)==COG_OK);
    cog_result_t rc=cog_ipc_endpoint_connect(fake,path,NULL);assert(rc==COG_OK || rc==COG_ERROR_AGAIN);
    unsigned char wire[37];size_t length;
    cog_worker_control_t claim={COG_WORKER_HELLO,id,0U,0U,COG_OK};
    assert(cog_orchestrator_worker_encode(&claim,wire,sizeof(wire),&length)==COG_OK);
    bool sent=false;uint64_t until=now_ms()+1000U;
    while(now_ms()<until) {
        service(o);uint64_t next;rc=cog_ipc_endpoint_service(fake,now_ms(),&next);
        if(rc!=COG_OK && rc!=COG_ERROR_AGAIN) break;
        cog_ipc_message_t message={wire,length,sizeof(wire)};
        if(!sent && cog_ipc_endpoint_send(fake,&message)==COG_OK) sent=true;
    }
    assert(sent);ready(o,id);cog_core_info_t actual=info(o,id);
    assert(actual.pid!=(int)getpid() && actual.peer_id!=0U && o->worker_count==1U);
    assert(cog_ipc_endpoint_free(fake)==COG_OK);acknowledge(o);
    /* Ordinary event capture must leave both final control IDs available. */
    assert(o->mandatory_event_ids==2U);o->last_event_id=UINT64_MAX-2U;
    command(o,id,'F');bool done=false;uint64_t next;until=now_ms()+1000U;
    do { rc=cog_orchestrator_service(o,now_ms(),&done,&next);pause_unit(); } while(rc!=COG_ERROR_LIMIT && now_ms()<until);
    assert(rc==COG_ERROR_LIMIT && o->message_count==0U && o->last_event_id+o->mandatory_event_ids==UINT64_MAX);
    assert(cog_orchestrator_request_shutdown(o,now_ms(),10U)==COG_OK);until=now_ms()+1000U;
    while(!done && now_ms()<until) { assert(cog_orchestrator_service(o,now_ms(),&done,&next)==COG_ERROR_LIMIT);pause_unit(); }
    assert(done && info(o,id).reaped && o->mandatory_event_ids==0U && o->last_event_id==UINT64_MAX);
    cog_orchestrator_free(o);
}
static void shortened_shutdown(const char *binary)
{
    char path[64];cog_orchestrator_t *o=make(path,1U);cog_worker_id_t id;
    assert(cog_orchestrator_spawn(o,binary,&id)==COG_OK);ready(o,id);command(o,id,'S');
    for(unsigned i=0U;i<10U;++i) service(o);
    assert(cog_orchestrator_request_shutdown(o,now_ms(),1000U)==COG_OK);
    while(!o->workers[0].shutdown_queued) service(o);
    uint64_t wire_deadline=o->workers[0].stop_deadline_ms;
    assert(cog_orchestrator_request_shutdown(o,now_ms(),100U)==COG_OK);
    assert(o->workers[0].stop_deadline_ms==wire_deadline && o->workers[0].terminate_deadline_ms<wire_deadline);
    bool done=false;uint64_t next,until=now_ms()+1500U;cog_result_t rc=COG_OK;
    while(!done && now_ms()<until) { rc=cog_orchestrator_service(o,now_ms(),&done,&next);pause_unit(); }
    assert(done && rc==COG_ERROR_TIMEOUT && o->workers[0].shutdown_acked && info(o,id).reaped);
    cog_orchestrator_free(o);
}
static void silent_handshake(void)
{
    char path[64];cog_orchestrator_t *o=make(path,1U);o->config.service_quantum=1U;
    cog_orchestrator_set_spawn_timeout(o,100U);
    cog_ipc_endpoint_t *peer;assert(cog_ipc_endpoint_new(&peer)==COG_OK);
    cog_result_t rc=cog_ipc_endpoint_connect(peer,path,NULL);assert(rc==COG_OK || rc==COG_ERROR_AGAIN);
    bool done;uint64_t next;uint64_t now=now_ms();
    assert(cog_orchestrator_service(o,now,&done,&next)==COG_OK);
    assert(o->link_count==1U && next==now+100U);
    assert(cog_orchestrator_service(o,now,&done,&next)==COG_ERROR_AGAIN && next==now+100U);
    for(unsigned i=0U;i<9U;++i) {
        rc=cog_orchestrator_service(o,now,&done,&next);assert(rc==COG_OK || rc==COG_ERROR_AGAIN);
        uint64_t endpoint_deadline;rc=cog_ipc_endpoint_service(peer,now,&endpoint_deadline);
        assert(rc==COG_OK || rc==COG_ERROR_AGAIN);
    }
    assert(o->links[0].proof_valid && o->links[0].worker==SIZE_MAX);
    for(unsigned i=0U;i<3U;++i) {
        rc=cog_orchestrator_service(o,now+100U,&done,&next);assert(rc==COG_OK || rc==COG_ERROR_AGAIN);
    }
    assert(o->link_count==0U);assert(cog_ipc_endpoint_free(peer)==COG_OK);cog_orchestrator_free(o);
}
static void remove_tree(const char *path)
{
    DIR *dir=opendir(path);assert(dir!=NULL);struct dirent *entry;
    while((entry=readdir(dir))!=NULL) {
        if(strcmp(entry->d_name,".")==0 || strcmp(entry->d_name,"..")==0) continue;
        char child[PATH_MAX];int n=snprintf(child,sizeof(child),"%s/%s",path,entry->d_name);
        assert(n>0 && (size_t)n<sizeof(child));struct stat status;assert(lstat(child,&status)==0);
        if(S_ISDIR(status.st_mode)) remove_tree(child);else assert(unlink(child)==0);
    }
    assert(closedir(dir)==0 && rmdir(path)==0);
}
static void owner_routes(void)
{
    char base[]="cog-orch-store-XXXXXX";assert(mkdtemp(base)!=NULL);
    cog_store_config_t store_cfg={0};store_cfg.base_dir=base;store_cfg.require_owner=true;
    store_cfg.max_record_bytes=1048576U;
    cog_store_t *store;assert(cog_store_new(&store,&store_cfg)==COG_OK);
    cog_session_config_t session_cfg={0};session_cfg.id="guarded-route";
    cog_session_t *session;assert(cog_session_new(&session,&session_cfg)==COG_OK);
    cog_store_owner_t old={1U,41U},next={2U,42U};cog_store_checkpoint_t checkpoint={0};
    assert(cog_store_owner_compare_exchange(store,session_cfg.id,(cog_store_owner_t){0},old,checkpoint)==COG_OK);
    cog_store_owner_guard_t *guard;assert(cog_store_owner_acquire(store,session_cfg.id,old,&guard)==COG_OK);
    assert(cog_store_checkpoint_save(store,session,guard,&checkpoint)==COG_OK && checkpoint.generation!=0U);
    char path[64];cog_orchestrator_t *o=make(path,1U);
    assert(cog_orchestrator_publish_owner(o,session_cfg.id,0U,old,checkpoint)==COG_ERROR_UNSUPPORTED);
    cog_orchestrator_config_t config=o->config;config.socket_path=path;config.store=store;
    cog_orchestrator_free(o);assert(cog_orchestrator_new(&o,&config)==COG_OK);
    deny_allocations=true;
    assert(cog_orchestrator_publish_owner(o,session_cfg.id,0U,old,checkpoint)==COG_OK);
    assert(cog_orchestrator_publish_owner(o,session_cfg.id,0U,old,checkpoint)==COG_OK);
    assert(o->route_count==1U);
    assert(cog_orchestrator_publish_owner(o,session_cfg.id,99U,old,checkpoint)==COG_ERROR_NOT_FOUND);
    assert(cog_orchestrator_publish_owner(o,session_cfg.id,0U,next,checkpoint)==COG_ERROR_INVALID_STATE);
    cog_peer_id_t peer=7U;cog_store_owner_t owner={0};cog_store_checkpoint_t copied={0};
    assert(cog_orchestrator_session_owner(o,session_cfg.id,&peer,&owner,&copied)==COG_OK);
    assert(peer==0U && owner.epoch==old.epoch && owner.instance_id==old.instance_id && copied.generation==checkpoint.generation);
    assert(cog_orchestrator_forget_owner(o,session_cfg.id,next)==COG_ERROR_INVALID_STATE);
    assert(cog_store_owner_release(guard)==COG_OK);
    assert(cog_store_owner_compare_exchange(store,session_cfg.id,old,next,checkpoint)==COG_OK);
    deny_allocations=false;assert(cog_store_owner_acquire(store,session_cfg.id,next,&guard)==COG_OK);deny_allocations=true;
    assert(cog_orchestrator_publish_owner(o,session_cfg.id,0U,next,checkpoint)==COG_OK);
    assert(cog_orchestrator_publish_owner(o,session_cfg.id,0U,old,checkpoint)==COG_ERROR_INVALID_STATE);
    assert(cog_orchestrator_forget_owner(o,session_cfg.id,old)==COG_ERROR_INVALID_STATE);
    assert(cog_orchestrator_forget_owner(o,session_cfg.id,next)==COG_OK && o->route_count==0U);
    assert(cog_store_owner_release(guard)==COG_OK);deny_allocations=false;
    cog_orchestrator_free(o);cog_session_free(session);cog_store_free(store);remove_tree(base);
}
int main(int argc,char **argv)
{
    if(argc>1) return worker_main(argc,argv);
    char executable[PATH_MAX];ssize_t size=readlink("/proc/self/exe",executable,sizeof(executable)-1U);
    assert(size>0 && (size_t)size<sizeof(executable));executable[size]='\0';
    boundaries(executable);lifecycle(executable);loss_and_pressure(executable);
    allocation_bounds(executable);impostor_and_ids(executable);shortened_shutdown(executable);silent_handshake();owner_routes();
    puts("orchestrator owned native lifecycle PASS");return 0;
}

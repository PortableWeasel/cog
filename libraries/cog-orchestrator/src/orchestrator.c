/** @file orchestrator.c @brief Bounded authenticated native-worker ownership. */
#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "orchestrator_layout.h"
#include "orchestrator_internal.h"
#include "../../cog-ipc/private/core_ipc.h"
#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static size_t normalized(size_t value,size_t fallback) { return value?value:fallback; }
static void wipe(void *data,size_t bytes)
{
    volatile unsigned char *p=data;
    while(bytes--!=0U) *p++=0U;
}
static bool worker_equal(cog_worker_id_t a,cog_worker_id_t b)
{ return a.incarnation==b.incarnation && a.number==b.number; }
static bool owner_equal(cog_store_owner_t a,cog_store_owner_t b)
{ return a.epoch==b.epoch && a.instance_id==b.instance_id; }
static uint64_t deadline_add(uint64_t now,uint64_t delay)
{ return delay>UINT64_MAX-now?UINT64_MAX:now+delay; }
static cog_result_t current_time(uint64_t *out)
{
    struct timespec t;
    if(clock_gettime(CLOCK_MONOTONIC,&t)!=0 || t.tv_sec<0 || t.tv_nsec<0 ||
       t.tv_nsec>=1000000000L || (uint64_t)t.tv_sec>(UINT64_MAX-(uint64_t)t.tv_nsec/1000000U)/1000U)
        return COG_ERROR_IO;
    *out=(uint64_t)t.tv_sec*1000U+(uint64_t)t.tv_nsec/1000000U;return COG_OK;
}
static cog_result_t remember(cog_orchestrator_t *o,cog_result_t rc)
{
    if(rc!=COG_OK && rc!=COG_ERROR_AGAIN) {
        if(o->primary==COG_OK) o->primary=rc;
        o->admission_frozen=true;
    }
    return rc;
}
static cog_result_t reap_profile(void)
{
    struct sigaction action;
    if(sigaction(SIGCHLD,NULL,&action)!=0) return COG_ERROR_IO;
    return action.sa_handler==SIG_IGN || (action.sa_flags&SA_NOCLDWAIT)!=0?
        COG_ERROR_UNSUPPORTED:COG_OK;
}
static bool add_size(size_t *total,size_t count,size_t width)
{
    if(width!=0U && count>(SIZE_MAX-*total)/width) return false;
    *total+=count*width;return true;
}
cog_result_t orch_reservation_plan(const cog_orchestrator_config_t *cfg,orch_reservations_t *out)
{
    if(cfg==NULL || out==NULL) return COG_ERROR_NULL_PTR;
    if(cfg->socket_path==NULL || cfg->socket_path[0]=='\0' ||
       strlen(cfg->socket_path)>=sizeof(((struct sockaddr_un *)0)->sun_path)) return COG_ERROR_INVALID_ARG;
    orch_reservations_t p={0};size_t capacity=normalized(cfg->default_core_capacity,4U);
    p.workers=normalized(cfg->max_aux_cores,4U);p.links=p.workers;
    p.routes=normalized(cfg->max_sessions,64U);p.message_entries=normalized(cfg->message_events,16U);
    p.message_bytes=normalized(cfg->message_bytes,4U*1024U*1024U);
    p.frame_bytes=normalized(cfg->max_frame_bytes,1024U*1024U);p.binary_bytes=PATH_MAX;
    if(capacity>UINT32_MAX || p.frame_bytes>UINT32_MAX || p.frame_bytes>p.message_bytes ||
       p.workers>SIZE_MAX/ORCH_CONTROL_TRANSITIONS || p.workers>UINT64_MAX/ORCH_CONTROL_TRANSITIONS)
        return COG_ERROR_INVALID_ARG;
    p.control_entries=p.workers*ORCH_CONTROL_TRANSITIONS;
    p.worker_executor_slots=(uint64_t)capacity+ORCH_RESERVED_EXECUTOR_SLOTS;
    size_t total=sizeof(cog_orchestrator_t);
    if(!add_size(&total,strlen(cfg->socket_path)+1U,1U) ||
       !add_size(&total,p.workers,sizeof(orch_worker_t)) ||
       !add_size(&total,p.links,sizeof(orch_link_t)) ||
       !add_size(&total,p.routes,sizeof(orch_route_t)) ||
       !add_size(&total,p.control_entries,sizeof(orch_control_ref_t)) ||
       !add_size(&total,p.message_entries,sizeof(orch_message_slot_t)) ||
       !add_size(&total,p.message_bytes,1U) || !add_size(&total,p.workers,p.binary_bytes) ||
       !add_size(&total,p.links,p.frame_bytes)) return COG_ERROR_INVALID_ARG;
    p.payload_bytes=p.links*p.frame_bytes;p.total_bytes=total;*out=p;return COG_OK;
}
static orch_worker_t *find_worker(const cog_orchestrator_t *o,cog_worker_id_t id)
{
    for(size_t i=0U;i<o->bounds.workers;++i)
        if(o->workers[i].used && worker_equal(o->workers[i].id,id)) return &o->workers[i];
    return NULL;
}
static orch_worker_t *find_peer(const cog_orchestrator_t *o,cog_peer_id_t peer)
{
    if(peer==0U) return NULL;
    for(size_t i=0U;i<o->bounds.workers;++i)
        if(o->workers[i].used && o->workers[i].peer==peer) return &o->workers[i];
    return NULL;
}
static cog_health_t health_at(const cog_orchestrator_t *o,const orch_worker_t *w)
{
    if(w->native_state!=ORCH_NATIVE_OWNED || w->disconnected) return COG_HEALTH_DEAD;
    if(!w->ready) return COG_HEALTH_UNKNOWN;
    uint64_t elapsed=o->now_ms>=w->last_heartbeat_ms?o->now_ms-w->last_heartbeat_ms:0U;
    if(elapsed<o->config.health_interval_ms) return COG_HEALTH_HEALTHY;
    return elapsed<(uint64_t)o->config.health_interval_ms*o->config.unhealthy_threshold?
        COG_HEALTH_DEGRADED:COG_HEALTH_DEAD;
}
static void freeze_routes(cog_orchestrator_t *o,const orch_worker_t *w)
{
    for(size_t i=0U;i<o->bounds.routes;++i)
        if(o->routes[i].used && worker_equal(o->routes[i].worker,w->id)) o->routes[i].frozen=true;
}
cog_result_t orch_control_capture(cog_orchestrator_t *o,orch_worker_t *w,
    orch_control_slot_kind_t transition,cog_result_t outcome)
{
    if(o==NULL || w==NULL) return COG_ERROR_NULL_PTR;
    if((unsigned)transition>=ORCH_CONTROL_TRANSITIONS || !w->used) return COG_ERROR_INVALID_STATE;
    orch_control_slot_t *slot=&w->control[transition];
    if(slot->pending || slot->acknowledged) return COG_OK;
    if(!slot->reserved || o->mandatory_event_ids==0U || o->last_event_id==UINT64_MAX ||
       o->control_count>=o->bounds.control_entries) return COG_ERROR_INVALID_STATE;
    cog_orchestrator_event_t event={0};event.event_id=++o->last_event_id;
    event.kind=transition==ORCH_CONTROL_READY?COG_ORCHESTRATOR_READY:
        transition==ORCH_CONTROL_DISCONNECTED?COG_ORCHESTRATOR_DISCONNECTED:
        w->native_state==ORCH_NATIVE_LOST?COG_ORCHESTRATOR_OWNERSHIP_LOST:COG_ORCHESTRATOR_REAPED;
    event.worker_id=w->id;event.peer_id=w->peer;event.peer_info=w->proof;event.outcome=outcome;
    slot->event=event;slot->reserved=false;slot->pending=true;--o->mandatory_event_ids;
    size_t tail=(o->control_head+o->control_count)%o->bounds.control_entries;
    o->control_fifo[tail]=(orch_control_ref_t){(size_t)(w-o->workers),transition};++o->control_count;
    return COG_OK;
}
static void release_hold(cog_orchestrator_t *o,orch_worker_t *w,orch_control_slot_kind_t kind)
{
    if(w->control[kind].reserved) { w->control[kind].reserved=false;assert(o->mandatory_event_ids!=0U);--o->mandatory_event_ids; }
}
static void native_terminal(cog_orchestrator_t *o,orch_worker_t *w,bool lost)
{
    w->native_state=lost?ORCH_NATIVE_LOST:ORCH_NATIVE_REAPED;w->health=COG_HEALTH_DEAD;
    freeze_routes(o,w);
    if(!w->ready) release_hold(o,w,ORCH_CONTROL_READY);
    if(!w->bound_once) release_hold(o,w,ORCH_CONTROL_DISCONNECTED);
    (void)remember(o,orch_control_capture(o,w,ORCH_CONTROL_FINAL,lost?COG_ERROR_IO:COG_OK));
    w->replacement_pending=o->config.auto_replace && !o->shutting_down &&
        (w->replacement_pending || !w->stop_requested);
}
cog_result_t orch_native_check(cog_orchestrator_t *o,orch_worker_t *w)
{
    if(o==NULL || w==NULL) return COG_ERROR_NULL_PTR;
    if(w->native_state==ORCH_NATIVE_LOST) return COG_ERROR_IO;
    if(w->native_state!=ORCH_NATIVE_OWNED) return COG_OK;
    int status=0;pid_t result=waitpid(w->native_pid,&status,WNOHANG);
    if(result==0) return COG_OK;
    if(result==w->native_pid) { w->wait_status=status;w->wait_status_known=true;native_terminal(o,w,false);return COG_OK; }
    if(result<0 && errno==EINTR) return COG_ERROR_AGAIN;
    if(result<0 && errno==ECHILD) { native_terminal(o,w,true);return remember(o,COG_ERROR_IO); }
    return remember(o,COG_ERROR_IO);
}
cog_result_t orch_native_signal(cog_orchestrator_t *o,orch_worker_t *w,int number)
{
    if(number!=SIGTERM && number!=SIGKILL) return COG_ERROR_INVALID_ARG;
    cog_result_t rc=orch_native_check(o,w);
    if(rc!=COG_OK || w->native_state!=ORCH_NATIVE_OWNED) return rc;
    if(kill(w->native_pid,number)==0) return COG_OK;
    if(errno==ESRCH) return orch_native_check(o,w);
    return remember(o,COG_ERROR_IO);
}
static void link_close(cog_orchestrator_t *o,orch_link_t *link,cog_result_t outcome)
{
    if(link->state==ORCH_LINK_FREE) return;
    if(link->worker!=SIZE_MAX) {
        orch_worker_t *w=&o->workers[link->worker];w->disconnected=true;w->health=COG_HEALTH_DEAD;
        w->link=SIZE_MAX;freeze_routes(o,w);
        (void)remember(o,orch_control_capture(o,w,ORCH_CONTROL_DISCONNECTED,outcome));
    }
    (void)cog_ipc_endpoint_free(link->endpoint);
    orch_bytes_t frame=link->frame;wipe(frame.data,frame.capacity);frame.length=0U;
    memset(link,0,sizeof(*link));link->worker=SIZE_MAX;link->frame=frame;--o->link_count;
}
static void storage_free(cog_orchestrator_t *o)
{
    if(o==NULL) return;
    if(o->links!=NULL) for(size_t i=0U;i<o->bounds.links;++i) {
        if(o->links[i].endpoint!=NULL) (void)cog_ipc_endpoint_free(o->links[i].endpoint);
        if(o->links[i].frame.data!=NULL) wipe(o->links[i].frame.data,o->links[i].frame.capacity);
        free(o->links[i].frame.data);
    }
    if(o->workers!=NULL) for(size_t i=0U;i<o->bounds.workers;++i) free(o->workers[i].binary_path);
    if(o->listener!=NULL) (void)cog_ipc_endpoint_free(o->listener);
    if(o->message_data!=NULL) wipe(o->message_data,o->bounds.message_bytes);
    free(o->workers);free(o->links);free(o->routes);free(o->control_fifo);
    free(o->messages);free(o->message_data);free(o->socket_path);free(o);
}
cog_result_t cog_orchestrator_new(cog_orchestrator_t **out,const cog_orchestrator_config_t *cfg)
{
    if(out==NULL || cfg==NULL) return COG_ERROR_NULL_PTR;
    *out=NULL;orch_reservations_t bounds;
    cog_result_t rc=orch_reservation_plan(cfg,&bounds);if(rc!=COG_OK) return rc;
    rc=reap_profile();if(rc!=COG_OK) return rc;
#ifndef SO_PEERCRED
    return COG_ERROR_UNSUPPORTED;
#endif
    cog_orchestrator_t *o=calloc(1U,sizeof(*o));if(o==NULL) return COG_ERROR_NOMEM;
    o->bounds=bounds;o->config=*cfg;
    o->config.max_aux_cores=bounds.workers;o->config.default_core_capacity=normalized(cfg->default_core_capacity,4U);
    o->config.service_quantum=normalized(cfg->service_quantum,16U);
    o->config.health_interval_ms=cfg->health_interval_ms?cfg->health_interval_ms:1000U;
    o->config.unhealthy_threshold=cfg->unhealthy_threshold?cfg->unhealthy_threshold:3U;
    o->config.shutdown_timeout_ms=cfg->shutdown_timeout_ms?cfg->shutdown_timeout_ms:1000U;
    o->config.max_frame_bytes=bounds.frame_bytes;o->config.message_bytes=bounds.message_bytes;
    o->spawn_timeout_ms=5000U;o->ack_timeout_ms=o->config.shutdown_timeout_ms;
    o->socket_path=malloc(strlen(cfg->socket_path)+1U);
    o->workers=calloc(bounds.workers,sizeof(*o->workers));o->links=calloc(bounds.links,sizeof(*o->links));
    o->routes=calloc(bounds.routes,sizeof(*o->routes));o->control_fifo=calloc(bounds.control_entries,sizeof(*o->control_fifo));
    o->messages=calloc(bounds.message_entries,sizeof(*o->messages));o->message_data=calloc(bounds.message_bytes,1U);
    if(o->socket_path==NULL || o->workers==NULL || o->links==NULL || o->routes==NULL ||
       o->control_fifo==NULL || o->messages==NULL || o->message_data==NULL) { storage_free(o);return COG_ERROR_NOMEM; }
    strcpy(o->socket_path,cfg->socket_path);o->config.socket_path=o->socket_path;
    for(size_t i=0U;i<bounds.workers;++i) {
        o->workers[i].binary_path=calloc(bounds.binary_bytes,1U);o->workers[i].binary_capacity=bounds.binary_bytes;
        o->workers[i].link=SIZE_MAX;
        if(o->workers[i].binary_path==NULL) { storage_free(o);return COG_ERROR_NOMEM; }
    }
    for(size_t i=0U;i<bounds.links;++i) {
        o->links[i].frame.data=calloc(bounds.frame_bytes,1U);o->links[i].frame.capacity=bounds.frame_bytes;
        o->links[i].worker=SIZE_MAX;
        if(o->links[i].frame.data==NULL) { storage_free(o);return COG_ERROR_NOMEM; }
    }
    if(getrandom(&o->incarnation,sizeof(o->incarnation),0)!=(ssize_t)sizeof(o->incarnation) || o->incarnation==0U) {
        storage_free(o);return COG_ERROR_IO;
    }
    rc=current_time(&o->now_ms);if(rc!=COG_OK) { storage_free(o);return rc; }
    o->clock_set=true;
    cog_ipc_endpoint_config_t ipc={0};ipc.max_frame_bytes=bounds.frame_bytes;
    ipc.receive_frames=2U;ipc.send_frames=2U;ipc.wipe_buffers=true;
    rc=cog_ipc_endpoint_new_ex(&o->listener,&ipc);
    if(rc==COG_OK) rc=cog_ipc_endpoint_listen(o->listener,o->socket_path);
    if(rc!=COG_OK) { storage_free(o);return rc; }
    *out=o;return COG_OK;
}
void cog_orchestrator_free(cog_orchestrator_t *o)
{
    if(o==NULL) return;
    assert(!o->in_service);
    for(size_t i=0U;i<o->bounds.workers;++i) assert(o->workers[i].native_state!=ORCH_NATIVE_OWNED);
    storage_free(o);
}
static bool decimal(char *out,uint64_t value)
{ int n=snprintf(out,ORCH_NUMBER_BYTES,"%" PRIu64,value);return n>0 && n<(int)ORCH_NUMBER_BYTES; }
cog_result_t cog_orchestrator_spawn(cog_orchestrator_t *o,const char *binary,cog_worker_id_t *out)
{
    if(o==NULL || binary==NULL || out==NULL) return COG_ERROR_NULL_PTR;
    if(binary[0]!='/' || strlen(binary)>=o->bounds.binary_bytes) return COG_ERROR_INVALID_ARG;
    if(o->shutting_down || o->admission_frozen) return COG_ERROR_INVALID_STATE;
    if(o->worker_count==o->bounds.workers) return COG_ERROR_OVERFLOW;
    if(o->last_worker_number==UINT64_MAX || o->mandatory_event_ids>UINT64_MAX-o->last_event_id ||
       UINT64_MAX-o->last_event_id-o->mandatory_event_ids<ORCH_CONTROL_TRANSITIONS) return COG_ERROR_LIMIT;
    cog_result_t rc=reap_profile();if(rc!=COG_OK) return rc;
    orch_worker_t *w=NULL;
    for(size_t i=0U;i<o->bounds.workers;++i) if(!o->workers[i].used) { w=&o->workers[i];break; }
    assert(w!=NULL);
    cog_worker_id_t id={o->incarnation,o->last_worker_number+1U};
    strcpy(w->binary_path,binary);
    if(!decimal(w->incarnation_arg,id.incarnation) || !decimal(w->number_arg,id.number) ||
       !decimal(w->parent_arg,(uint64_t)getpid()) || !decimal(w->executors_arg,o->bounds.worker_executor_slots) ||
       !decimal(w->frame_arg,o->bounds.frame_bytes)) return COG_ERROR_INVALID_ARG;
    char *argv[]={w->binary_path,(char *)"--worker",(char *)"--control-socket",o->socket_path,
        (char *)"--worker-incarnation",w->incarnation_arg,(char *)"--worker-number",w->number_arg,
        (char *)"--parent-pid",w->parent_arg,(char *)"--executor-slots",w->executors_arg,(char *)"--frame-bytes",w->frame_arg,NULL};
    memcpy(w->argv,argv,sizeof(argv));
    pid_t pid=fork();
    if(pid<0) return COG_ERROR_IO;
    if(pid==0) {
        sigset_t mask;
        if(sigprocmask(SIG_SETMASK,NULL,&mask)!=0 || sigdelset(&mask,SIGINT)!=0 ||
           sigdelset(&mask,SIGTERM)!=0 || sigprocmask(SIG_SETMASK,&mask,NULL)!=0) _exit(127);
        char *const environment[]={(char *)"PATH=/usr/bin:/bin",(char *)"LC_ALL=C.UTF-8",NULL};
        execve(w->binary_path,w->argv,environment);_exit(127);
    }
    w->used=true;w->id=id;w->native_pid=pid;w->native_state=ORCH_NATIVE_OWNED;
    w->expected_uid=(uint64_t)getuid();w->capacity=o->config.default_core_capacity;w->link=SIZE_MAX;
    w->startup_deadline_ms=deadline_add(o->now_ms,o->spawn_timeout_ms);
    for(size_t i=0U;i<ORCH_CONTROL_TRANSITIONS;++i) w->control[i].reserved=true;
    o->mandatory_event_ids+=ORCH_CONTROL_TRANSITIONS;o->last_worker_number=id.number;
    ++o->worker_count;*out=id;return COG_OK;
}
static void request_stop(cog_orchestrator_t *o,orch_worker_t *w,uint64_t deadline)
{
    if(!w->stop_requested) w->terminate_deadline_ms=deadline;
    else if(deadline<w->terminate_deadline_ms) w->terminate_deadline_ms=deadline;
    if(!w->shutdown_queued && (!w->stop_requested || deadline<w->stop_deadline_ms)) {
        w->stop_deadline_ms=deadline;w->shutdown_length=0U;
    }
    w->stop_requested=true;w->replacement_pending=false;(void)o;
}
cog_result_t cog_orchestrator_stop(cog_orchestrator_t *o,cog_worker_id_t id)
{
    if(o==NULL) return COG_ERROR_NULL_PTR;
    orch_worker_t *w=find_worker(o,id);if(w==NULL) return COG_ERROR_NOT_FOUND;
    request_stop(o,w,deadline_add(o->now_ms,o->ack_timeout_ms));return COG_OK;
}
cog_result_t cog_orchestrator_kill(cog_orchestrator_t *o,cog_worker_id_t id)
{
    if(o==NULL) return COG_ERROR_NULL_PTR;
    orch_worker_t *w=find_worker(o,id);if(w==NULL) return COG_ERROR_NOT_FOUND;
    cog_result_t rc=orch_native_signal(o,w,SIGKILL);
    if(rc==COG_OK && w->native_state==ORCH_NATIVE_OWNED) w->kill_sent=true;
    return rc;
}
cog_health_t cog_orchestrator_health(const cog_orchestrator_t *o,cog_worker_id_t id)
{
    if(o==NULL) return COG_HEALTH_UNKNOWN;
    const orch_worker_t *w=find_worker(o,id);return w==NULL?COG_HEALTH_UNKNOWN:health_at(o,w);
}
cog_result_t cog_orchestrator_release_worker(cog_orchestrator_t *o,cog_worker_id_t id)
{
    if(o==NULL) return COG_ERROR_NULL_PTR;
    orch_worker_t *w=find_worker(o,id);if(w==NULL) return COG_ERROR_NOT_FOUND;
    if(w->native_state==ORCH_NATIVE_OWNED || w->link!=SIZE_MAX) return COG_ERROR_INVALID_STATE;
    for(size_t i=0U;i<ORCH_CONTROL_TRANSITIONS;++i) if(w->control[i].pending) return COG_ERROR_INVALID_STATE;
    size_t index=(size_t)(w-o->workers);
    for(size_t i=0U;i<o->message_count;++i)
        if(o->messages[(o->message_head+i)%o->bounds.message_entries].worker==index) return COG_ERROR_INVALID_STATE;
    for(size_t i=0U;i<o->bounds.routes;++i)
        if(o->routes[i].used && worker_equal(o->routes[i].worker,id)) return COG_ERROR_INVALID_STATE;
    for(size_t i=0U;i<ORCH_CONTROL_TRANSITIONS;++i) release_hold(o,w,(orch_control_slot_kind_t)i);
    char *binary=w->binary_path;size_t capacity=w->binary_capacity;wipe(binary,capacity);
    memset(w,0,sizeof(*w));w->binary_path=binary;w->binary_capacity=capacity;w->link=SIZE_MAX;
    --o->worker_count;return COG_OK;
}
cog_result_t cog_orchestrator_send(cog_orchestrator_t *o,cog_worker_id_t id,const cog_ipc_message_t *message)
{
    if(o==NULL || message==NULL || (message->len!=0U && message->data==NULL)) return COG_ERROR_NULL_PTR;
    orch_worker_t *w=find_worker(o,id);if(w==NULL) return COG_ERROR_NOT_FOUND;
    if(o->admission_frozen || o->shutting_down || w->stop_requested || !w->ready ||
       health_at(o,w)!=COG_HEALTH_HEALTHY || w->link==SIZE_MAX) return COG_ERROR_INVALID_STATE;
    if(message->len>o->bounds.frame_bytes) return COG_ERROR_LIMIT;
    return cog_ipc_endpoint_send(o->links[w->link].endpoint,message);
}
static cog_result_t message_capture(cog_orchestrator_t *o,orch_link_t *link)
{
    size_t length=link->frame.length;
    if(o->message_count==o->bounds.message_entries || length>o->bounds.message_bytes-o->bytes_used)
        return COG_ERROR_AGAIN;
    if(o->mandatory_event_ids>=UINT64_MAX-o->last_event_id) return COG_ERROR_LIMIT;
    size_t index=(o->message_head+o->message_count)%o->bounds.message_entries;
    orch_message_slot_t *slot=&o->messages[index];orch_worker_t *w=&o->workers[link->worker];
    slot->offset=(o->byte_head+o->bytes_used)%o->bounds.message_bytes;
    slot->length=length;slot->worker=link->worker;
    size_t first=length<o->bounds.message_bytes-slot->offset?length:o->bounds.message_bytes-slot->offset;
    if(first!=0U) memcpy(o->message_data+slot->offset,link->frame.data,first);
    if(length>first) memcpy(o->message_data,link->frame.data+first,length-first);
    slot->event=(cog_orchestrator_event_t){0};slot->event.event_id=++o->last_event_id;
    slot->event.kind=COG_ORCHESTRATOR_MESSAGE;slot->event.worker_id=w->id;
    slot->event.peer_id=w->peer;slot->event.peer_info=w->proof;slot->event.data_len=length;
    ++o->message_count;o->bytes_used+=length;return COG_OK;
}
cog_result_t cog_orchestrator_copy_event(const cog_orchestrator_t *o,cog_orchestrator_event_class_t cls,
    cog_orchestrator_event_t *event,void *storage,size_t capacity,size_t *required)
{
    if(o==NULL || event==NULL || required==NULL || (storage==NULL && capacity!=0U)) return COG_ERROR_NULL_PTR;
    if(cls!=COG_ORCHESTRATOR_CONTROL_EVENTS && cls!=COG_ORCHESTRATOR_MESSAGE_EVENTS) return COG_ERROR_INVALID_ARG;
    cog_orchestrator_event_t copy;
    if(cls==COG_ORCHESTRATOR_CONTROL_EVENTS) {
        if(o->control_count==0U) return COG_ERROR_AGAIN;
        orch_control_ref_t ref=o->control_fifo[o->control_head];copy=o->workers[ref.worker].control[ref.transition].event;
        *required=0U;*event=copy;return COG_OK;
    }
    if(o->message_count==0U) return COG_ERROR_AGAIN;
    const orch_message_slot_t *slot=&o->messages[o->message_head];
    if(capacity<slot->length) { *required=slot->length;return COG_ERROR_OVERFLOW; }
    size_t first=slot->length<o->bounds.message_bytes-slot->offset?slot->length:o->bounds.message_bytes-slot->offset;
    if(first!=0U) memcpy(storage,o->message_data+slot->offset,first);
    if(slot->length>first) memcpy((unsigned char *)storage+first,o->message_data,slot->length-first);
    copy=slot->event;copy.data=slot->length?storage:NULL;*required=slot->length;*event=copy;return COG_OK;
}
cog_result_t cog_orchestrator_ack_event(cog_orchestrator_t *o,uint64_t id)
{
    if(o==NULL) return COG_ERROR_NULL_PTR;
    if(id==0U) return COG_ERROR_NOT_FOUND;
    if(id==o->last_control_ack || id==o->last_message_ack) return COG_OK;
    for(size_t i=0U;i<o->control_count;++i) {
        orch_control_ref_t ref=o->control_fifo[(o->control_head+i)%o->bounds.control_entries];
        orch_control_slot_t *slot=&o->workers[ref.worker].control[ref.transition];
        if(slot->event.event_id!=id) continue;
        if(i!=0U) return COG_ERROR_INVALID_STATE;
        slot->pending=false;slot->acknowledged=true;
        o->control_head=(o->control_head+1U)%o->bounds.control_entries;--o->control_count;
        o->last_control_ack=id;return COG_OK;
    }
    for(size_t i=0U;i<o->message_count;++i) {
        orch_message_slot_t *slot=&o->messages[(o->message_head+i)%o->bounds.message_entries];
        if(slot->event.event_id!=id) continue;
        if(i!=0U) return COG_ERROR_INVALID_STATE;
        size_t first=slot->length<o->bounds.message_bytes-slot->offset?slot->length:o->bounds.message_bytes-slot->offset;
        wipe(o->message_data+slot->offset,first);wipe(o->message_data,slot->length-first);
        o->byte_head=(slot->offset+slot->length)%o->bounds.message_bytes;o->bytes_used-=slot->length;
        memset(slot,0,sizeof(*slot));o->message_head=(o->message_head+1U)%o->bounds.message_entries;
        --o->message_count;o->last_message_ack=id;return COG_OK;
    }
    return COG_ERROR_NOT_FOUND;
}
cog_result_t cog_orchestrator_list(const cog_orchestrator_t *o,cog_core_info_t *infos,size_t capacity,size_t *count)
{
    if(o==NULL || count==NULL || (infos==NULL && capacity!=0U)) return COG_ERROR_NULL_PTR;
    size_t n=0U;
    for(size_t i=0U;i<o->bounds.workers;++i) if(o->workers[i].used) {
        const orch_worker_t *w=&o->workers[i];
        if(n<capacity) infos[n]=(cog_core_info_t){w->peer,(int)w->native_pid,health_at(o,w),
            w->active_subagents,w->capacity,w->last_heartbeat_ms,w->id,
            w->native_state==ORCH_NATIVE_REAPED,w->native_state==ORCH_NATIVE_LOST};
        ++n;
    }
    *count=n;return COG_OK;
}
cog_result_t cog_orchestrator_heartbeat(cog_orchestrator_t *o,cog_peer_id_t peer,size_t active)
{
    if(o==NULL) return COG_ERROR_NULL_PTR;
    orch_worker_t *w=find_peer(o,peer);if(w==NULL) return COG_ERROR_NOT_FOUND;
    if(!w->ready || w->disconnected || w->native_state!=ORCH_NATIVE_OWNED) return COG_ERROR_INVALID_STATE;
    if(active>w->capacity) return COG_ERROR_INVALID_ARG;
    w->active_subagents=active;w->last_heartbeat_ms=o->now_ms;w->has_heartbeat=true;w->health=COG_HEALTH_HEALTHY;
    return COG_OK;
}
cog_peer_id_t cog_orchestrator_place(const cog_orchestrator_t *o,size_t load)
{
    if(o==NULL || o->shutting_down || o->admission_frozen) return 0U;
    const orch_worker_t *best=NULL;size_t needed=load?load:1U;
    for(size_t i=0U;i<o->bounds.workers;++i) {
        const orch_worker_t *w=&o->workers[i];
        if(!w->used || w->stop_requested || !w->ready || health_at(o,w)!=COG_HEALTH_HEALTHY ||
           w->active_subagents>w->capacity || needed>w->capacity-w->active_subagents) continue;
        if(best==NULL || w->active_subagents<best->active_subagents) best=w;
    }
    return best?best->peer:0U;
}
static cog_result_t hello(cog_orchestrator_t *o,orch_link_t *link,const cog_worker_control_t *message)
{
    if(link->worker!=SIZE_MAX) return COG_ERROR_PROTOCOL;
    orch_worker_t *w=find_worker(o,message->worker);
    if(w==NULL || w->bound_once || w->native_state!=ORCH_NATIVE_OWNED ||
       link->proof.process_id!=(uint64_t)w->native_pid || link->proof.user_id!=w->expected_uid)
        return COG_ERROR_DENIED;
    cog_result_t rc=orch_native_check(o,w);
    if(rc!=COG_OK) return rc;
    if(w->native_state!=ORCH_NATIVE_OWNED || w->stop_requested || o->admission_frozen || o->shutting_down)
        return COG_ERROR_DENIED;
    w->bound_once=true;w->ready=true;w->link=(size_t)(link-o->links);w->peer=link->peer;
    w->proof=link->proof;w->last_heartbeat_ms=o->now_ms;w->ready_at_ms=o->now_ms;
    w->has_heartbeat=true;w->health=COG_HEALTH_HEALTHY;
    link->worker=(size_t)(w-o->workers);link->state=ORCH_LINK_READY;
    return orch_control_capture(o,w,ORCH_CONTROL_READY,COG_OK);
}
static cog_result_t heartbeat_frame(cog_orchestrator_t *o,orch_link_t *link,const cog_worker_control_t *message)
{
    if(link->worker==SIZE_MAX) return COG_ERROR_DENIED;
    orch_worker_t *w=&o->workers[link->worker];
    if(!worker_equal(w->id,message->worker) || message->active_subagents>w->capacity) return COG_ERROR_PROTOCOL;
    /* A copied final heartbeat cannot resurrect an already reaped reservation. */
    if(w->native_state!=ORCH_NATIVE_OWNED) return COG_OK;
    return cog_orchestrator_heartbeat(o,w->peer,message->active_subagents);
}
static cog_result_t shutdown_ack(cog_orchestrator_t *o,orch_link_t *link,const cog_worker_control_t *message)
{
    if(link->worker==SIZE_MAX) return COG_ERROR_DENIED;
    orch_worker_t *w=&o->workers[link->worker];
    if(!worker_equal(w->id,message->worker) || !w->stop_requested || !w->shutdown_queued ||
       message->deadline_ms!=w->stop_deadline_ms) return COG_ERROR_PROTOCOL;
    w->shutdown_acked=true;return message->outcome;
}
typedef cog_result_t (*control_handler_t)(cog_orchestrator_t *,orch_link_t *,const cog_worker_control_t *);
static const control_handler_t native_handlers[]={NULL,hello,heartbeat_frame,NULL,shutdown_ack};
static cog_result_t process_frame(cog_orchestrator_t *o,orch_link_t *link)
{
    cog_result_t rc;
    if(link->frame.length>=3U && memcmp(link->frame.data,"CGW",3U)==0) {
        cog_worker_control_t message;
        rc=cog_orchestrator_worker_decode(link->frame.data,link->frame.length,&message);
        if(rc==COG_OK) {
            if((unsigned)message.kind>=sizeof(native_handlers)/sizeof(native_handlers[0]) || native_handlers[message.kind]==NULL)
                rc=COG_ERROR_PROTOCOL;
            else rc=native_handlers[message.kind](o,link,&message);
        }
    } else {
        if(link->worker==SIZE_MAX) rc=COG_ERROR_DENIED;
        else if(o->admission_frozen) rc=COG_ERROR_AGAIN;
        else rc=message_capture(o,link);
    }
    if(rc!=COG_ERROR_AGAIN) {
        wipe(link->frame.data,link->frame.length);link->frame.length=0U;link->staged_frame_valid=false;
    }
    return rc;
}
cog_result_t orch_link_step(cog_orchestrator_t *o,orch_link_t *link,uint64_t now,uint64_t *deadline)
{
    if(o==NULL || link==NULL || deadline==NULL) return COG_ERROR_NULL_PTR;
    *deadline=UINT64_MAX;
    if(link->state==ORCH_LINK_FREE) return COG_ERROR_AGAIN;
    if(link->worker==SIZE_MAX && now>=link->handshake_deadline_ms) { link_close(o,link,COG_ERROR_TIMEOUT);return COG_OK; }
    if(link->worker==SIZE_MAX) *deadline=link->handshake_deadline_ms;
    cog_result_t rc;
    if(link->staged_frame_valid) {
        rc=process_frame(o,link);
        if(rc==COG_ERROR_AGAIN) return rc;
        if(rc!=COG_OK) {
            bool authenticated=link->worker!=SIZE_MAX;link_close(o,link,rc);
            return authenticated?rc:COG_OK;
        }
        return COG_OK;
    }
    /* Existing accepted endpoints publish identity in exactly one service unit.
     * Capture immediately before any subsequent unit or frame consumption. */
    uint64_t next=UINT64_MAX;
    cog_result_t serviced=cog_ipc_internal_service_one(link->endpoint,now,&next);
    if(next<*deadline) *deadline=next;
    if(!link->proof_valid) {
        cog_peer_id_t peer;
        rc=cog_ipc_endpoint_get_peer_id(link->endpoint,&peer);
        if(rc==COG_OK) rc=cog_ipc_endpoint_peer_info(link->endpoint,peer,&link->proof);
        if(rc==COG_OK) { link->peer=peer;link->proof_valid=true;link->state=ORCH_LINK_PROVED; }
        else if(rc!=COG_ERROR_AGAIN || (serviced!=COG_OK && serviced!=COG_ERROR_AGAIN)) {
            link_close(o,link,rc);return COG_OK;
        } else return COG_ERROR_AGAIN;
    }
    if(serviced!=COG_OK && serviced!=COG_ERROR_AGAIN && serviced!=COG_ERROR_IO) {
        bool authenticated=link->worker!=SIZE_MAX;link_close(o,link,serviced);
        return authenticated?serviced:COG_OK;
    }
    cog_ipc_message_t frame={link->frame.data,0U,link->frame.capacity};
    rc=cog_ipc_endpoint_recv(link->endpoint,&frame);
    if(rc==COG_ERROR_AGAIN) return serviced==COG_ERROR_IO?COG_ERROR_AGAIN:serviced;
    if(rc!=COG_OK) {
        bool authenticated=link->worker!=SIZE_MAX;link_close(o,link,rc);
        return rc==COG_ERROR_IO || rc==COG_ERROR_INVALID_STATE || !authenticated?COG_OK:rc;
    }
    link->frame.length=frame.len;link->staged_frame_valid=true;
    rc=process_frame(o,link);
    if(rc!=COG_OK && rc!=COG_ERROR_AGAIN) {
        bool authenticated=link->worker!=SIZE_MAX;link_close(o,link,rc);
        return authenticated?rc:COG_OK;
    }
    return rc;
}
cog_result_t cog_orchestrator_accept(cog_orchestrator_t *o,cog_peer_id_t *out)
{
    if(o==NULL) return COG_ERROR_NULL_PTR;
    if(o->link_count==o->bounds.links || o->admission_frozen || o->shutting_down)
        return cog_ipc_internal_refuse(o->listener);
    orch_link_t *link=NULL;
    for(size_t i=0U;i<o->bounds.links;++i) if(o->links[i].state==ORCH_LINK_FREE) { link=&o->links[i];break; }
    assert(link!=NULL);
    cog_ipc_endpoint_t *endpoint; cog_peer_id_t peer;
    cog_result_t rc=cog_ipc_endpoint_accept(o->listener,&endpoint,&peer);
    if(rc!=COG_OK) return rc;
    link->endpoint=endpoint;link->peer=peer;link->state=ORCH_LINK_HANDSHAKE;link->worker=SIZE_MAX;
    link->handshake_deadline_ms=deadline_add(o->now_ms,o->spawn_timeout_ms);++o->link_count;
    if(out!=NULL) *out=peer;
    return COG_OK;
}
static cog_result_t worker_step(cog_orchestrator_t *o,orch_worker_t *w)
{
    if(!w->used) return COG_ERROR_AGAIN;
    cog_result_t rc=orch_native_check(o,w);
    if(rc!=COG_OK) return rc;
    if(w->native_state!=ORCH_NATIVE_OWNED) {
        /* Automatic replacement needs a genuinely free lifetime slot. Old
         * event/route records cannot be discarded merely to create capacity. */
        if(w->replacement_pending && !o->admission_frozen && !o->shutting_down) {
            char binary[PATH_MAX];memcpy(binary,w->binary_path,strlen(w->binary_path)+1U);
            cog_worker_id_t old=w->id,next;
            rc=cog_orchestrator_release_worker(o,old);
            if(rc==COG_OK) return cog_orchestrator_spawn(o,binary,&next);
            if(rc!=COG_ERROR_INVALID_STATE) return rc;
        }
        return COG_OK;
    }
    if(!w->ready && o->now_ms>=w->startup_deadline_ms && !w->stop_requested)
        request_stop(o,w,o->now_ms);
    w->health=health_at(o,w);
    if(w->health==COG_HEALTH_DEAD && !w->stop_requested) {
        request_stop(o,w,deadline_add(o->now_ms,o->ack_timeout_ms));
        w->replacement_pending=o->config.auto_replace && !o->shutting_down;
    }
    if(!w->stop_requested) return COG_OK;
    if(o->now_ms>=w->terminate_deadline_ms) {
        if(!w->kill_sent) {
            rc=orch_native_signal(o,w,SIGKILL);
            if(rc==COG_OK && w->native_state==ORCH_NATIVE_OWNED) w->kill_sent=true;
            return rc;
        }
        return COG_OK;
    }
    if(w->ready && w->link!=SIZE_MAX && !w->shutdown_queued) {
        if(w->shutdown_length==0U) {
            cog_worker_control_t message={COG_WORKER_SHUTDOWN,w->id,0U,w->stop_deadline_ms,COG_OK};
            rc=cog_orchestrator_worker_encode(&message,w->shutdown_frame,sizeof(w->shutdown_frame),&w->shutdown_length);
            if(rc!=COG_OK) return rc;
        }
        cog_ipc_message_t frame={w->shutdown_frame,w->shutdown_length,w->shutdown_length};
        rc=cog_ipc_endpoint_send(o->links[w->link].endpoint,&frame);
        if(rc==COG_OK) w->shutdown_queued=true;
        else if(rc!=COG_ERROR_AGAIN && rc!=COG_ERROR_IO) return rc;
    }
    /* TERM is the bounded escape when FIFO pressure prevents wire shutdown. */
    if(!w->shutdown_queued && !w->term_sent) {
        rc=orch_native_signal(o,w,SIGTERM);
        if(rc==COG_OK && w->native_state==ORCH_NATIVE_OWNED) w->term_sent=true;
        return rc;
    }
    return COG_OK;
}
cog_result_t cog_orchestrator_request_shutdown(cog_orchestrator_t *o,uint64_t now,uint32_t timeout)
{
    if(o==NULL) return COG_ERROR_NULL_PTR;
    if(o->clock_set && now<o->now_ms) return COG_ERROR_INVALID_ARG;
    uint64_t deadline=deadline_add(now,timeout);
    o->now_ms=now;o->clock_set=true;
    if(!o->shutting_down || deadline<o->shutdown_deadline_ms) o->shutdown_deadline_ms=deadline;
    bool first=!o->shutting_down;
    o->shutting_down=true;
    for(size_t i=0U;i<o->bounds.workers;++i) if(o->workers[i].used) {
        orch_worker_t *w=&o->workers[i];
        if(w->native_state==ORCH_NATIVE_OWNED) request_stop(o,w,o->shutdown_deadline_ms);
        else if(first) w->terminate_deadline_ms=0U; /* No timer in this shutdown. */
    }
    return COG_OK;
}
cog_result_t cog_orchestrator_service(cog_orchestrator_t *o,uint64_t now,bool *done,uint64_t *deadline)
{
    if(o==NULL || done==NULL || deadline==NULL) return COG_ERROR_NULL_PTR;
    if(o->in_service) return COG_ERROR_INVALID_STATE;
    if(o->clock_set && now<o->now_ms) return COG_ERROR_INVALID_ARG;
    o->now_ms=now;o->clock_set=true;o->in_service=true;
    bool progress=false;uint64_t next=UINT64_MAX;
    size_t span=1U+o->bounds.workers+o->bounds.links;
    for(size_t unit=0U;unit<o->config.service_quantum;++unit) {
        size_t turn=o->round_robin_worker++;
        if(o->round_robin_worker==span) o->round_robin_worker=0U;
        cog_result_t rc;
        if(turn==0U) rc=cog_orchestrator_accept(o,NULL);
        else if(turn<=o->bounds.workers) rc=worker_step(o,&o->workers[turn-1U]);
        else {
            uint64_t link_deadline=UINT64_MAX;
            rc=orch_link_step(o,&o->links[turn-1U-o->bounds.workers],now,&link_deadline);
            if(link_deadline<next) next=link_deadline;
        }
        if(rc==COG_OK) progress=true;
        (void)remember(o,rc);
    }
    bool disposed=true,timed_out=false;
    for(size_t i=0U;i<o->bounds.workers;++i) {
        orch_worker_t *w=&o->workers[i];if(!w->used) continue;
        if(w->native_state==ORCH_NATIVE_OWNED) {
            disposed=false;
            uint64_t due=w->stop_requested?w->terminate_deadline_ms:!w->ready?w->startup_deadline_ms:
                deadline_add(w->last_heartbeat_ms,o->config.health_interval_ms);
            if(due<next) next=due;
        }
        if(w->stop_requested && w->kill_sent && w->terminate_deadline_ms!=0U) timed_out=true;
    }
    /* Timers belong to every retained link, even outside this service quantum.
     * Native proof alone is not HELLO readiness and cannot hold a slot forever. */
    for(size_t i=0U;i<o->bounds.links;++i) {
        const orch_link_t *link=&o->links[i];
        if(link->state!=ORCH_LINK_FREE && link->worker==SIZE_MAX && link->handshake_deadline_ms<next)
            next=link->handshake_deadline_ms;
    }
    o->shutdown_done=o->shutting_down && disposed;
    if(o->shutdown_done && timed_out && o->primary==COG_OK) (void)remember(o,COG_ERROR_TIMEOUT);
    *done=o->shutdown_done;*deadline=next;o->in_service=false;
    return o->primary!=COG_OK?o->primary:(progress || o->shutdown_done)?COG_OK:COG_ERROR_AGAIN;
}
cog_result_t cog_orchestrator_shutdown(cog_orchestrator_t *o,uint32_t timeout)
{
    if(o==NULL) return COG_ERROR_NULL_PTR;
    uint64_t now;
    cog_result_t rc=current_time(&now);if(rc!=COG_OK) return rc;
    if(o->clock_overridden) now=o->now_ms;
    rc=cog_orchestrator_request_shutdown(o,now,timeout);if(rc!=COG_OK) return rc;
    bool done=false;uint64_t deadline;
    do {
        if(!o->clock_overridden) { rc=current_time(&now);if(rc!=COG_OK) return remember(o,rc); }
        else now=deadline_add(now,1U);
        rc=cog_orchestrator_service(o,now,&done,&deadline);
        if(!done) { const struct timespec pause={0,1000000L};(void)nanosleep(&pause,NULL); }
    } while(!done);
    return rc==COG_ERROR_AGAIN?COG_OK:rc;
}
static bool session_valid(const char *text)
{
    size_t length=strnlen(text,ORCH_SESSION_BYTES),i=0U;
    if(length==0U || length==ORCH_SESSION_BYTES) return false;
    while(i<length) {
        unsigned char first=(unsigned char)text[i++];
        if(first<0x80U) continue;
        size_t rest;uint32_t value,minimum;
        if(first>=0xc2U && first<=0xdfU) { rest=1U;value=first&0x1fU;minimum=0x80U; }
        else if(first>=0xe0U && first<=0xefU) { rest=2U;value=first&0x0fU;minimum=0x800U; }
        else if(first>=0xf0U && first<=0xf4U) { rest=3U;value=first&7U;minimum=0x10000U; }
        else return false;
        if(rest>length-i) return false;
        while(rest--!=0U) {
            unsigned char c=(unsigned char)text[i++];if((c&0xc0U)!=0x80U) return false;
            value=(value<<6U)|(c&0x3fU);
        }
        if(value<minimum || value>0x10ffffU || (value>=0xd800U && value<=0xdfffU)) return false;
    }
    return true;
}
static orch_route_t *find_route(const cog_orchestrator_t *o,const char *session)
{
    for(size_t i=0U;i<o->bounds.routes;++i)
        if(o->routes[i].used && strcmp(o->routes[i].session,session)==0) return &o->routes[i];
    return NULL;
}
static orch_route_t *free_route(cog_orchestrator_t *o)
{
    for(size_t i=0U;i<o->bounds.routes;++i) if(!o->routes[i].used) return &o->routes[i];
    return NULL;
}
cog_result_t cog_orchestrator_publish_owner(cog_orchestrator_t *o,const char *session,cog_peer_id_t peer,
    cog_store_owner_t owner,cog_store_checkpoint_t checkpoint)
{
    if(o==NULL || session==NULL) return COG_ERROR_NULL_PTR;
    if(!session_valid(session) || owner.epoch==0U || owner.instance_id==0U || checkpoint.generation==0U)
        return COG_ERROR_INVALID_STATE;
    if(o->config.store==NULL || !cog_store_requires_owner(o->config.store)) return COG_ERROR_UNSUPPORTED;
    orch_worker_t *w=peer?find_peer(o,peer):NULL;
    if(peer!=0U && w==NULL) return COG_ERROR_NOT_FOUND;
    if(o->admission_frozen || o->shutting_down || (w!=NULL &&
       (!w->ready || w->stop_requested || health_at(o,w)!=COG_HEALTH_HEALTHY))) return COG_ERROR_INVALID_STATE;
    orch_route_t *route=find_route(o,session);
    if(route!=NULL && (owner.epoch<route->owner.epoch ||
       (owner.epoch==route->owner.epoch && (!owner_equal(owner,route->owner) ||
        peer!=route->peer || checkpoint.generation!=route->checkpoint.generation)))) return COG_ERROR_INVALID_STATE;
    cog_store_owner_t actual; cog_store_checkpoint_t saved;
    cog_result_t rc=cog_store_owner_read(o->config.store,session,&actual,&saved);
    if(rc!=COG_OK) return rc;
    if(!owner_equal(owner,actual) || checkpoint.generation!=saved.generation) return COG_ERROR_INVALID_STATE;
    if(route==NULL) {
        route=free_route(o);if(route==NULL) return COG_ERROR_LIMIT;
        ++o->route_count;
    }
    *route=(orch_route_t){0};route->used=true;strcpy(route->session,session);
    route->peer=peer;route->owner=owner;route->checkpoint=checkpoint;
    if(w!=NULL) route->worker=w->id;
    return COG_OK;
}
cog_result_t cog_orchestrator_session_owner(const cog_orchestrator_t *o,const char *session,
    cog_peer_id_t *peer,cog_store_owner_t *owner,cog_store_checkpoint_t *checkpoint)
{
    if(o==NULL || session==NULL || peer==NULL || owner==NULL || checkpoint==NULL) return COG_ERROR_NULL_PTR;
    const orch_route_t *route=find_route(o,session);if(route==NULL) return COG_ERROR_NOT_FOUND;
    *peer=route->peer;*owner=route->owner;*checkpoint=route->checkpoint;return COG_OK;
}
cog_result_t cog_orchestrator_forget_owner(cog_orchestrator_t *o,const char *session,cog_store_owner_t expected)
{
    if(o==NULL || session==NULL) return COG_ERROR_NULL_PTR;
    orch_route_t *route=find_route(o,session);if(route==NULL) return COG_ERROR_NOT_FOUND;
    if(!owner_equal(route->owner,expected)) return COG_ERROR_INVALID_STATE;
    memset(route,0,sizeof(*route));--o->route_count;return COG_OK;
}
cog_result_t cog_orchestrator_sessions(const cog_orchestrator_t *o,cog_peer_id_t peer,
    const char **sessions,size_t capacity,size_t *count)
{
    if(o==NULL || sessions==NULL || count==NULL) return COG_ERROR_NULL_PTR;
    if(find_peer(o,peer)==NULL) return COG_ERROR_NOT_FOUND;
    size_t n=0U;
    for(size_t i=0U;i<o->bounds.routes;++i) if(o->routes[i].used && o->routes[i].peer==peer) {
        if(n<capacity) sessions[n]=o->routes[i].session;
        ++n;
    }
    *count=n;return COG_OK;
}
/* Legacy test-only diagnosis hooks have no durable owner/guard authority. */
cog_result_t cog_orchestrator_register_session(cog_orchestrator_t *o,cog_peer_id_t peer,const char *session)
{
    if(o==NULL || session==NULL) return COG_ERROR_NULL_PTR;
    orch_worker_t *w=find_peer(o,peer);if(w==NULL) return COG_ERROR_NOT_FOUND;
    if(!session_valid(session)) return COG_ERROR_INVALID_ARG;
    orch_route_t *route=find_route(o,session);
    if(route!=NULL) return route->peer==peer?COG_OK:COG_ERROR_INVALID_STATE;
    route=free_route(o);if(route==NULL) return COG_ERROR_OVERFLOW;
    route->used=true;route->peer=peer;route->worker=w->id;strcpy(route->session,session);++o->route_count;
    return COG_OK;
}
cog_result_t cog_orchestrator_unregister_session(cog_orchestrator_t *o,cog_peer_id_t peer,const char *session)
{
    if(o==NULL || session==NULL) return COG_ERROR_NULL_PTR;
    if(find_peer(o,peer)==NULL) return COG_ERROR_NOT_FOUND;
    orch_route_t *route=find_route(o,session);
    if(route==NULL || route->peer!=peer) return COG_ERROR_NOT_FOUND;
    if(route->owner.epoch!=0U) return COG_ERROR_INVALID_STATE;
    memset(route,0,sizeof(*route));--o->route_count;return COG_OK;
}
cog_result_t cog_orchestrator_poll(cog_orchestrator_t *o,int timeout)
{
    if(o==NULL) return COG_ERROR_NULL_PTR;
    if(timeout<0) return COG_ERROR_INVALID_ARG;
    uint64_t now=o->now_ms,deadline;bool done;
    if(!o->clock_overridden) { cog_result_t rc=current_time(&now);if(rc!=COG_OK) return rc; }
    cog_result_t rc=cog_orchestrator_service(o,now,&done,&deadline);
    return rc==COG_ERROR_AGAIN?COG_OK:rc;
}
void cog_orchestrator_set_now(cog_orchestrator_t *o,uint64_t now)
{ if(o!=NULL) { o->clock_overridden=true;o->clock_set=true;o->now_ms=now; } }
void cog_orchestrator_set_spawn_timeout(cog_orchestrator_t *o,uint32_t timeout)
{ if(o!=NULL) o->spawn_timeout_ms=timeout; }
void cog_orchestrator_set_ack_timeout(cog_orchestrator_t *o,uint32_t timeout)
{ if(o!=NULL) o->ack_timeout_ms=timeout; }
uint64_t cog_orchestrator_now(const cog_orchestrator_t *o) { return o?o->now_ms:0U; }

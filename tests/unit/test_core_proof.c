/** @file test_core_proof.c @brief Real first-proof barrier under event/arena pressure. */
#define _GNU_SOURCE
#include <cog/core.h>
#include "../../libraries/cog-core/src/core_internal.h"
#include "../../libraries/cog-ipc/src/ipc_internal.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

cog_result_t __real_cog_ipc_endpoint_peer_info(const cog_ipc_endpoint_t *ep,
    cog_peer_id_t id,cog_ipc_peer_info_t *info);
cog_result_t __wrap_cog_ipc_endpoint_peer_info(const cog_ipc_endpoint_t *ep,
    cog_peer_id_t id,cog_ipc_peer_info_t *info);
int __real_getsockopt(int fd,int level,int option,void *data,socklen_t *length);
int __wrap_getsockopt(int fd,int level,int option,void *data,socklen_t *length);
typedef struct fixture {
    cog_core_t *core; cog_session_t *session;
    bool fill, protected_ingress, failed_native;
    unsigned captures, full_captures, arena_captures, connected, messages, disconnected, generic;
    cog_peer_id_t last;
} fixture_t;
static fixture_t *observed;
cog_result_t __wrap_cog_ipc_endpoint_peer_info(const cog_ipc_endpoint_t *ep,
    cog_peer_id_t id,cog_ipc_peer_info_t *info)
{
    cog_result_t rc=__real_cog_ipc_endpoint_peer_info(ep,id,info);
    if (rc==COG_OK && observed!=NULL) {
        ++observed->captures;
        if (cog_event_queue_is_full(observed->core->ipc_events)) ++observed->full_captures;
        if (observed->core->ipc_arena_used==3U) ++observed->arena_captures;
    }
    return rc;
}
int __wrap_getsockopt(int fd,int level,int option,void *data,socklen_t *length)
{
    if (observed!=NULL && observed->failed_native && level==SOL_SOCKET && option==SO_PEERCRED) {
        errno=EIO;return -1;
    }
    return __real_getsockopt(fd,level,option,data,length);
}
static cog_handler_result_t handle(cog_core_t *core,const cog_event_t *event)
{
    fixture_t *f=cog_core_user(core);
    if (event->kind==COG_EVENT_USER_INPUT) return (cog_handler_result_t){COG_TRANSITION_SELF,COG_OK};
    const cog_event_ipc_t *ipc=event->payload; assert(ipc!=NULL);
    if (ipc->peer==0U) { assert(!ipc->peer_info_valid);++f->generic; }
    else {
        assert(ipc->peer_info_valid==f->protected_ingress);
        if (f->protected_ingress) {
            assert(ipc->peer_info.peer_id==ipc->peer);
            assert(ipc->peer_info.process_id==(uint64_t)getpid());
            assert(ipc->peer_info.user_id==(uint64_t)getuid());
        }
        if(event->kind==COG_EVENT_IPC_CONNECTED) ++f->connected;
        else if(event->kind==COG_EVENT_IPC_MESSAGE) {
            assert(ipc->msg_payload_len==3U);
            assert(memcmp(ipc->msg_payload,"one",3U)==0 || memcmp(ipc->msg_payload,"two",3U)==0);
            ++f->messages;
        } else { assert(event->kind==COG_EVENT_IPC_DISCONNECTED);++f->disconnected; }
        f->last=ipc->peer;
    }
    return (cog_handler_result_t){COG_TRANSITION_SELF,COG_OK};
}
static const cog_state_t transitions[]={COG_TRANSITION_SELF,COG_TRANSITION_END};
static const cog_dispatch_entry_t table[]={
    COG_DISPATCH_ENTRY(0,COG_EVENT_USER_INPUT,handle,transitions),
    COG_DISPATCH_ENTRY(0,COG_EVENT_IPC_CONNECTED,handle,transitions),
    COG_DISPATCH_ENTRY(0,COG_EVENT_IPC_MESSAGE,handle,transitions),
    COG_DISPATCH_ENTRY(0,COG_EVENT_IPC_DISCONNECTED,handle,transitions)
};
static cog_result_t service(void *arg,uint64_t now,uint64_t *deadline)
{
    fixture_t *f=arg;(void)now;*deadline=UINT64_MAX;
    if(f->fill) {
        cog_event_t dummy={COG_EVENT_USER_INPUT,COG_EVENT_USER_INPUT,NULL,0U,0U};
        while(!cog_event_queue_is_full(f->core->ipc_events))
            assert(cog_event_queue_push(f->core->ipc_events,&dummy)==COG_OK);
        f->fill=false;
    }
    return COG_OK;
}
static cog_ipc_endpoint_t *listen_new(char path[108])
{
    (void)snprintf(path,108U,"/tmp/cog-core-proof-XXXXXX");
    int tmp=mkstemp(path);assert(tmp>=0 && close(tmp)==0 && unlink(path)==0);
    cog_ipc_endpoint_config_t cfg={0};cfg.max_frame_bytes=16U;cfg.receive_frames=2U;cfg.send_frames=2U;
    cog_ipc_endpoint_t *ep;assert(cog_ipc_endpoint_new_ex(&ep,&cfg)==COG_OK);
    assert(cog_ipc_endpoint_listen(ep,path)==COG_OK);return ep;
}
static void create(fixture_t *f,cog_ipc_endpoint_t *ep)
{
    cog_core_config_t cfg={0};assert(cog_session_new(&f->session,NULL)==COG_OK);
    cfg.session=f->session;cfg.dispatch_table=table;cfg.dispatch_table_size=4U;cfg.user=f;
    cfg.ipc_endpoint=ep;cfg.require_peer_credentials=f->protected_ingress;
    cfg.ipc_max_frame_bytes=3U;cfg.ipc_payload_bytes=3U;cfg.ipc_service_quantum=32U;
    cfg.ipc_peer_capacity=2U;cfg.step_event_limit=1U;cfg.service_interval=1U;
    cfg.service_fn=service;cfg.service_arg=f;cfg.max_idle_wait_ms=7U;
    assert(cog_core_new(&f->core,&cfg)==COG_OK);observed=f;
}
static int raw(const char *path,const char *text)
{
    int fd=socket(AF_UNIX,SOCK_STREAM,0);assert(fd>=0);
    struct sockaddr_un address={0};address.sun_family=AF_UNIX;
    assert(strlen(path)<sizeof(address.sun_path));memcpy(address.sun_path,path,strlen(path)+1U);
    assert(connect(fd,(const struct sockaddr *)&address,sizeof(address))==0);
    unsigned char wire[7]={0U,0U,0U,3U,0U,0U,0U};memcpy(wire+4U,text,3U);
    assert(write(fd,wire,sizeof(wire))==(ssize_t)sizeof(wire));
    assert(shutdown(fd,SHUT_WR)==0);return fd;
}
static void step(fixture_t *f)
{
    cog_core_step_info_t info;assert(cog_core_step(f->core,&info)==COG_OK && !info.done);
    assert(info.dispatched<=1U && info.max_wait_ms<=7U);
}
static void stop(fixture_t *f)
{
    cog_core_step_info_t info;cog_core_stop(f->core);
    assert(cog_core_step(f->core,&info)==COG_OK && info.done);
    assert(cog_core_drive_end(f->core)==COG_OK);
}
static void drop(fixture_t *f)
{
    observed=NULL;cog_core_free(f->core);cog_session_free(f->session);
}
static void saturated(void)
{
    char path[108];fixture_t f={0};f.fill=true;f.protected_ingress=true;
    create(&f,listen_new(path));int first=raw(path,"one");
    assert(cog_core_drive_begin(f.core)==COG_OK);step(&f);
    assert(f.captures==1U && f.full_captures==1U);
    assert(f.core->ipc_client_count==1U && f.core->ipc_clients[0].proof_valid);
    cog_ipc_peer_info_t untouched={0};
    assert(cog_ipc_endpoint_peer_info(f.core->ipc_clients[0].endpoint,
           f.core->ipc_clients[0].id,&untouched)==COG_ERROR_NOT_FOUND);
    stop(&f);assert(cog_core_drive_begin(f.core)==COG_OK);
    for(unsigned i=0U;i<8U && f.core->ipc_arena_used==0U;++i) step(&f);
    assert(f.core->ipc_arena_used==3U && f.messages==0U && f.captures==1U);
    int second=raw(path,"two");
    for(unsigned i=0U;i<8U && f.captures<2U;++i) step(&f);
    assert(f.captures==2U && f.arena_captures>=1U && f.messages==0U);
    /* Public descriptors cannot forge proof or steal an owned arena lease. */
    cog_event_ipc_t ipc={0};ipc.peer_info_valid=true;
    cog_event_t external={COG_EVENT_IPC_MESSAGE,COG_EVENT_IPC_MESSAGE,&ipc,sizeof(ipc),0U};
    size_t queued=cog_event_queue_count(f.core->event_queue);
    assert(cog_core_enqueue(f.core,&external)==COG_ERROR_INVALID_ARG);
    assert(cog_event_queue_count(f.core->event_queue)==queued);
    ipc.peer_info_valid=false;ipc.msg_payload=f.core->ipc_arena;
    ipc.msg_payload_len=3U;
    assert(cog_core_enqueue(f.core,&external)==COG_OK);
    for(unsigned i=0U;i<4U && f.generic==0U;++i) step(&f);
    assert(f.generic==1U && f.core->ipc_arena_used==3U);
    for(unsigned i=0U;i<96U && f.disconnected<2U;++i) step(&f);
    assert(f.connected==2U && f.messages==2U && f.disconnected==2U && f.captures==2U);
    assert(f.core->ipc_client_count==0U && f.core->ipc_arena_used==0U);
    cog_peer_id_t last=f.last;int third=raw(path,"one");
    for(unsigned i=0U;i<32U && f.disconnected<3U;++i) step(&f);
    assert(f.connected==3U && f.messages==3U && f.disconnected==3U && f.last>last);
    stop(&f);assert(close(first)==0 && close(second)==0 && close(third)==0);drop(&f);
}
static void rejected_or_generic(bool protected_ingress)
{
    fixture_t f={0};char path[108];f.protected_ingress=protected_ingress;
    create(&f,listen_new(path));f.failed_native=true;
    int fd=raw(path,"one");assert(cog_core_drive_begin(f.core)==COG_OK);
    for(unsigned i=0U;i<16U;++i) step(&f);
    assert(f.captures==0U && f.core->ipc_client_count==0U);
    assert(f.connected==(protected_ingress?0U:1U));
    assert(f.messages==(protected_ingress?0U:1U));
    assert(f.disconnected==(protected_ingress?0U:1U));
    stop(&f);assert(close(fd)==0);drop(&f);
}
static void attached(void)
{
    fixture_t f={0};char path[108];f.protected_ingress=true;
    cog_ipc_endpoint_t *listener=listen_new(path),*client,*server;
    assert(cog_ipc_endpoint_new(&client)==COG_OK);
    assert(cog_ipc_endpoint_connect(client,path,NULL)==COG_ERROR_AGAIN);
    assert(cog_ipc_endpoint_accept(listener,&server,NULL)==COG_OK);
    uint64_t deadline;
    for(unsigned i=0U;i<4U;++i) {
        cog_result_t a=cog_ipc_endpoint_service(server,1U,&deadline);
        cog_result_t b=cog_ipc_endpoint_service(client,1U,&deadline);
        assert((a==COG_OK || a==COG_ERROR_AGAIN) && (b==COG_OK || b==COG_ERROR_AGAIN));
    }
    create(&f,client);assert(cog_core_drive_begin(f.core)==COG_OK);
    cog_core_step_info_t info;assert(cog_core_step(f.core,&info)==COG_OK && !info.done);
    assert(f.captures==1U && info.max_wait_ms>0U && info.max_wait_ms<=7U);
    const unsigned char wire[]={0U,0U,0U,3U,'o','n','e'};
    assert(write(server->fd,wire,sizeof(wire))==(ssize_t)sizeof(wire));
    assert(shutdown(server->fd,SHUT_WR)==0);
    for(unsigned i=0U;i<16U && f.disconnected==0U;++i) step(&f);
    assert(f.connected==0U && f.messages==1U && f.disconnected==1U && f.captures==1U);
    stop(&f);drop(&f);
    assert(cog_ipc_endpoint_free(server)==COG_OK && cog_ipc_endpoint_free(listener)==COG_OK);
}
int main(void)
{
    saturated();rejected_or_generic(true);rejected_or_generic(false);attached();
    puts("protected IPC proof under event/arena pressure PASS");return 0;
}

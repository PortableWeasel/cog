/** @file test_ipc_o1_review.c @brief Independent O1 prequeued-proof and wrapped-retirement joins. */
#define _GNU_SOURCE
#include <cog/ipc.h>
#include "../../libraries/cog-ipc/src/ipc_internal.h"
#include "../../libraries/cog-ipc/private/core_ipc.h"
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#define CHECK(x) do { if (!(x)) { (void)fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x); exit(1); } } while (0)
#define OK(x) CHECK((x)==COG_OK)
int __real_getsockopt(int fd,int level,int option,void *value,socklen_t *size);
int __wrap_getsockopt(int fd,int level,int option,void *value,socklen_t *size);
ssize_t __real_recv(int fd,void *data,size_t length,int flags);
ssize_t __wrap_recv(int fd,void *data,size_t length,int flags);
void *__real_malloc(size_t size); void *__wrap_malloc(size_t size);
void *__real_calloc(size_t count,size_t size); void *__wrap_calloc(size_t count,size_t size);
void *__real_realloc(void *p,size_t size); void *__wrap_realloc(void *p,size_t size);
void __real_free(void *p); void __wrap_free(void *p);
static unsigned queries,reads,allocations;
static bool count_allocations;
int __wrap_getsockopt(int fd,int level,int option,void *value,socklen_t *size)
{ if(level==SOL_SOCKET && option==SO_PEERCRED) ++queries; return __real_getsockopt(fd,level,option,value,size); }
ssize_t __wrap_recv(int fd,void *data,size_t length,int flags)
{ ++reads; return __real_recv(fd,data,length,flags); }
void *__wrap_malloc(size_t size) { if(count_allocations) ++allocations; return __real_malloc(size); }
void *__wrap_calloc(size_t n,size_t size) { if(count_allocations) ++allocations; return __real_calloc(n,size); }
void *__wrap_realloc(void *p,size_t size) { if(count_allocations) ++allocations; return __real_realloc(p,size); }
void __wrap_free(void *p) { __real_free(p); }
static bool zero(const void *data,size_t size)
{ const unsigned char *p=data; for(size_t i=0;i<size;++i) if(p[i]!=0U) return false; return true; }
static cog_ipc_endpoint_config_t config(void)
{ return (cog_ipc_endpoint_config_t){.max_frame_bytes=8,.receive_frames=2,.send_frames=2,.service_quantum=32,.wipe_buffers=true}; }
static cog_result_t one(cog_ipc_endpoint_t *ep)
{ uint64_t deadline; return cog_ipc_internal_service_one(ep,1,&deadline); }
static void listen_new(cog_ipc_endpoint_t **out,char path[108])
{
    (void)snprintf(path,108,"/tmp/cog-o1-review-XXXXXX"); int fd=mkstemp(path); CHECK(fd>=0); CHECK(close(fd)==0); CHECK(unlink(path)==0);
    cog_ipc_endpoint_config_t c=config(); OK(cog_ipc_endpoint_new_ex(out,&c)); OK(cog_ipc_endpoint_listen(*out,path));
}
static int raw_connect(const char *path)
{
    int fd=socket(AF_UNIX,SOCK_STREAM,0); CHECK(fd>=0);
    struct sockaddr_un address={0}; address.sun_family=AF_UNIX; CHECK(strlen(path)<sizeof(address.sun_path));
    memcpy(address.sun_path,path,strlen(path)+1U);
    CHECK(connect(fd,(const struct sockaddr *)&address,sizeof(address))==0); return fd;
}
static void prequeued_barrier_and_wrapped_partial(void)
{
    cog_ipc_endpoint_t *listener,*peer; char path[108]; listen_new(&listener,path);
    int fd=raw_connect(path); cog_peer_id_t id;
    queries=reads=0; OK(cog_ipc_endpoint_accept(listener,&peer,&id)); CHECK(queries==1U);
    /* Both full frames and partial final frame+FIN precede the FIRST service. */
    static const unsigned char wire[]={0,0,0,1,'a',0,0,0,3,'k','e','y',0,0,0,5,'p','w'};
    CHECK(write(fd,wire,sizeof(wire))==(ssize_t)sizeof(wire)); CHECK(shutdown(fd,SHUT_WR)==0);
    allocations=0; count_allocations=true;
    OK(one(peer)); CHECK(peer->identity_used==8U && peer->rx_count==0U && reads==0U && peer->failure==COG_OK);
    cog_ipc_peer_info_t saved;
    OK(cog_ipc_endpoint_peer_info(listener,id,&saved)); CHECK(queries==1U && saved.process_id==(uint64_t)getpid());
    for(unsigned i=0;i<32U && peer->rx_count<2U;++i) CHECK(one(peer)==COG_OK || peer->failure==COG_OK);
    CHECK(peer->rx_count==2U && peer->failure==COG_OK);
    unsigned char output[8]; cog_ipc_message_t msg={output,0,sizeof(output)};
    OK(cog_ipc_endpoint_recv(peer,&msg)); CHECK(msg.len==1U && output[0]=='a'); CHECK(peer->rx_head==1U && zero(peer->rx_data,12U));
    for(unsigned i=0;i<32U && peer->failure==COG_OK;++i) (void)one(peer);
    CHECK(peer->failure==COG_ERROR_PROTOCOL && peer->rx_head==1U && peer->rx_count==1U);
    CHECK(zero(peer->rx_data,12U) && memcmp(peer->rx_data+12U,"key",3U)==0);
    cog_ipc_peer_info_t current,before; memset(&current,0xa5,sizeof(current)); memcpy(&before,&current,sizeof(current));
    CHECK(cog_ipc_endpoint_peer_info(listener,id,&current)==COG_ERROR_NOT_FOUND); CHECK(memcmp(&current,&before,sizeof(current))==0);
    CHECK(saved.process_id==(uint64_t)getpid() && queries==1U && reads>0U);
    memset(output,0xa5,sizeof(output)); msg.capacity=2U;
    CHECK(cog_ipc_endpoint_recv(peer,&msg)==COG_ERROR_OVERFLOW && msg.len==3U && output[0]==0xa5U);
    CHECK(memcmp(peer->rx_data+12U,"key",3U)==0); msg.capacity=sizeof(output);
    OK(cog_ipc_endpoint_recv(peer,&msg)); CHECK(msg.len==3U && memcmp(output,"key",3U)==0 && zero(peer->rx_data,24U));
    CHECK(cog_ipc_endpoint_recv(peer,&msg)==COG_ERROR_PROTOCOL); CHECK(allocations==0U);
    count_allocations=false; CHECK(close(fd)==0); OK(cog_ipc_endpoint_free(peer)); OK(cog_ipc_endpoint_free(listener));
}
static void pressure_is_per_peer(void)
{
    cog_ipc_endpoint_t *listener,*a,*b; char path[108]; listen_new(&listener,path);
    int fa=raw_connect(path),fb=raw_connect(path); cog_peer_id_t ia,ib;
    OK(cog_ipc_endpoint_accept(listener,&a,&ia)); OK(cog_ipc_endpoint_accept(listener,&b,&ib)); OK(one(a)); OK(one(b)); CHECK(ia!=ib);
    unsigned char data[]="only-a",other[]="only-b";
    cog_ipc_message_t ma={data,sizeof(data),sizeof(data)},mb={other,sizeof(other),sizeof(other)};
    allocations=0; count_allocations=true;
    OK(cog_ipc_send_to_peer(listener,ia,&ma)); OK(cog_ipc_send_to_peer(listener,ia,&ma));
    CHECK(cog_ipc_send_to_peer(listener,ia,&ma)==COG_ERROR_AGAIN);
    CHECK(a->tx_count==2U && b->tx_count==0U); OK(cog_ipc_send_to_peer(listener,ib,&mb));
    CHECK(a->tx_count==2U && b->tx_count==1U);
    /* Disconnect of full A cannot discard B's already-copied output. */
    OK(cog_ipc_endpoint_disconnect(a)); CHECK(zero(a->tx_data,24U) && b->tx_count==1U);
    CHECK(cog_ipc_send_to_peer(listener,ia,&ma)==COG_ERROR_NOT_FOUND);
    CHECK(memcmp(b->tx_data+4U,other,sizeof(other))==0);
    for(unsigned i=0;i<8U && b->tx_count!=0U;++i) { cog_result_t rc=one(b); CHECK(rc==COG_OK || rc==COG_ERROR_AGAIN); }
    CHECK(b->tx_count==0U && zero(b->tx_data,12U)); CHECK(allocations==0U); count_allocations=false;
    /* Real native bytes: identity prefix followed by precisely B's one frame. */
    unsigned char actual[19]; size_t used=0U;
    while(used<sizeof(actual)) {
        struct pollfd ready={fb,POLLIN,0}; CHECK(poll(&ready,1,500)==1);
        ssize_t n=read(fb,actual+used,sizeof(actual)-used); CHECK(n>0); used+=(size_t)n;
    }
    CHECK(actual[11]==7U && memcmp(actual+12U,other,7U)==0); /* prefix8+length4+payload7 */
    CHECK(close(fa)==0 && close(fb)==0); OK(cog_ipc_endpoint_free(a)); OK(cog_ipc_endpoint_free(b)); OK(cog_ipc_endpoint_free(listener));
}
static void rejected_bounds_allocate_nothing(void)
{
    cog_ipc_endpoint_config_t c=config(); cog_ipc_endpoint_t *ep=NULL;
    allocations=0; count_allocations=true;
    OK(cog_ipc_endpoint_new_ex(&ep,&c)); CHECK(allocations==6U);
    count_allocations=false; OK(cog_ipc_endpoint_free(ep)); ep=NULL;
    for(unsigned i=0;i<4U;++i) {
        c=config();
        if(i==0U) c.max_frame_bytes=SIZE_MAX;
        if(i==1U) c.receive_frames=SIZE_MAX;
        if(i==2U) c.send_frames=SIZE_MAX;
        if(i==3U) c.max_routes=SIZE_MAX;
        allocations=0; count_allocations=true;
        CHECK(cog_ipc_endpoint_new_ex(&ep,&c)==COG_ERROR_INVALID_ARG && ep==NULL && allocations==0U);
        count_allocations=false;
    }
}
int main(int argc,char **argv)
{
    if(argc>1 && strcmp(argv[1],"--negative-control")==0) CHECK(false);
    prequeued_barrier_and_wrapped_partial(); pressure_is_per_peer(); rejected_bounds_allocate_nothing();
    return 0;
}

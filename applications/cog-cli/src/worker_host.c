/** @file worker_host.c @brief Bounded owned-worker transport and executor host. */
#define _POSIX_C_SOURCE 200809L
#include "composition_internal.h"
#include "app_internal.h"
#include "codec_internal.h"
#include "core_ipc.h"
#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <sys/un.h>
#include <unistd.h>

#define WORKER_PROFILE_BYTES (96U * 1024U)
#define WORKER_ARGUMENT_BYTES (65536U + 4096U)
#define WORKER_RESULT_BYTES (1024U * 1024U)
#define WORKER_NATIVE_BYTES 37U

static void worker_wipe(void *bytes,size_t capacity)
{
    volatile unsigned char *p=bytes;
    while(capacity--!=0U) *p++=0U;
}
static size_t lesser(size_t a,size_t b) { return a<b?a:b; }
static cog_result_t bytes_new(cli_host_bytes_t *bytes,size_t capacity)
{
    bytes->capacity=capacity;
    if(capacity==0U) return COG_OK;
    bytes->data=calloc(capacity,1U);
    return bytes->data!=NULL?COG_OK:COG_ERROR_NOMEM;
}
static void bytes_free(cli_host_bytes_t *bytes)
{
    if(bytes->data!=NULL) worker_wipe(bytes->data,bytes->capacity);
    free(bytes->data);memset(bytes,0,sizeof(*bytes));
}
static void secret_free(cli_secret_exchange_t *secret)
{
    if(secret->body!=NULL) worker_wipe(secret->body,secret->body_capacity);
    if(secret->frame!=NULL) worker_wipe(secret->frame,secret->frame_capacity);
    if(secret->scratch!=NULL) worker_wipe(secret->scratch,secret->scratch_capacity);
    free(secret->body);free(secret->frame);free(secret->scratch);
    memset(secret,0,sizeof(*secret));
}
static cog_result_t secret_new(cli_secret_exchange_t *secret,size_t frame)
{
    secret->body_capacity=lesser(frame,WORKER_PROFILE_BYTES);
    secret->frame_capacity=lesser(frame,WORKER_PROFILE_BYTES+256U);
    secret->scratch_capacity=secret->frame_capacity;
    secret->body=calloc(secret->body_capacity,1U);
    secret->frame=calloc(secret->frame_capacity,1U);
    secret->scratch=calloc(secret->scratch_capacity,1U);
    return secret->body!=NULL && secret->frame!=NULL && secret->scratch!=NULL?
        COG_OK:COG_ERROR_NOMEM;
}
static size_t lane_capacity(size_t index,size_t physical,size_t frame)
{
    /* Child conversations never reserve a root projection lane. PROFILE bytes
       never enter these ordinary exact-original/reply arenas. */
    static const size_t maxima[CLI_HOST_LANES]={
        WORKER_ARGUMENT_BYTES,WORKER_RESULT_BYTES+512U,0U,1024U,
        WORKER_ARGUMENT_BYTES,WORKER_ARGUMENT_BYTES,0U,512U,0U,512U};
    if(index==COG_CLI_LANE_VIEW-1U) return physical<2U?frame:0U;
    return lesser(frame,maxima[index]);
}
static cog_result_t lane_new(cli_control_lane_t *lane,size_t capacity)
{
    lane->next_wire=1U;
    cog_result_t rc=bytes_new(&lane->pending,capacity);
    if(rc==COG_OK) rc=bytes_new(&lane->provisional_reply,capacity);
    if(rc==COG_OK) rc=bytes_new(&lane->final_original,capacity);
    if(rc==COG_OK) rc=bytes_new(&lane->final_reply,capacity);
    return rc;
}
static void lane_free(cli_control_lane_t *lane)
{
    bytes_free(&lane->pending);bytes_free(&lane->provisional_reply);
    bytes_free(&lane->final_original);bytes_free(&lane->final_reply);
}
static void worker_storage_free(cog_cli_worker_host_t *worker)
{
    if(worker==NULL) return;
    if(worker->parent!=NULL) (void)cog_ipc_endpoint_free(worker->parent);
    cog_cli_configuration_free(worker->startup_configuration);
    secret_free(&worker->startup_profile);
    if(worker->executors!=NULL) for(size_t i=0U;i<worker->executor_capacity;++i) {
        cli_executor_slot_t *slot=&worker->executors[i];
        /* This helper consumes only never-admitted or already-disposed slots. */
        cog_cli_configuration_free(slot->pending_profile);
        if(slot->net!=NULL) cog_net_free(slot->net);
        cog_cli_observer_free(slot->observer);
        secret_free(&slot->profile);
        bytes_free(&slot->task);bytes_free(&slot->status);
        bytes_free(&slot->result);bytes_free(&slot->update);
    }
    if(worker->lanes!=NULL) for(size_t i=0U;i<worker->executor_capacity*2U*CLI_HOST_LANES;++i)
        lane_free(&worker->lanes[i]);
    for(size_t i=0U;i<3U;++i) bytes_free(&worker->native_control[i]);
    if(worker->receive_secret!=NULL) worker_wipe(worker->receive_secret,worker->receive_capacity);
    if(worker->decode_secret!=NULL) worker_wipe(worker->decode_secret,worker->decode_capacity);
    free(worker->receive_secret);free(worker->decode_secret);
    free(worker->lanes);free(worker->executors);free(worker->socket_path);free(worker->ca_bundle);
    free(worker);
}
static bool reserve_bytes(size_t *total,size_t count,size_t width)
{
    if(width!=0U && count>(SIZE_MAX-*total)/width) return false;
    *total+=count*width;return true;
}
static cog_result_t worker_reservations(const cog_cli_worker_startup_t *startup)
{
    size_t total=sizeof(cog_cli_worker_host_t);
    size_t frame=startup->frame_bytes,slots=startup->executor_slots;
    if(frame>SIZE_MAX-128U || !reserve_bytes(&total,2U,frame+128U) ||
       !reserve_bytes(&total,slots,sizeof(cli_executor_slot_t)) ||
       !reserve_bytes(&total,slots*2U*CLI_HOST_LANES,sizeof(cli_control_lane_t)) ||
       !reserve_bytes(&total,slots+1U,3U*(WORKER_PROFILE_BYTES+256U)) ||
       !reserve_bytes(&total,slots,WORKER_ARGUMENT_BYTES+2U*WORKER_RESULT_BYTES+1024U))
        return COG_ERROR_LIMIT;
    for(size_t i=0U;i<slots;++i) for(size_t lane=0U;lane<CLI_HOST_LANES;++lane)
        if(!reserve_bytes(&total,8U,lane_capacity(lane,i,frame))) return COG_ERROR_LIMIT;
    return COG_OK;
}
cog_result_t cog_cli_worker_host_new(cog_cli_worker_host_t **out,
    const cog_cli_worker_startup_t *startup)
{
    if(out==NULL) return COG_ERROR_NULL_PTR;
    *out=NULL;
    if(startup==NULL) return COG_ERROR_NULL_PTR;
    if(startup->socket_path==NULL || startup->socket_path[0]=='\0' ||
       strlen(startup->socket_path)>=sizeof(((struct sockaddr_un *)0)->sun_path) ||
       startup->worker.incarnation==0U || startup->worker.number==0U ||
       startup->parent_pid==0U || startup->parent_pid!=(uint64_t)getppid() ||
       startup->signal_fd< -1 || startup->executor_slots<3U ||
       startup->executor_slots>CLI_HOST_MAX_EXECUTORS || startup->frame_bytes<WORKER_NATIVE_BYTES ||
       startup->frame_bytes>UINT32_MAX) return COG_ERROR_INVALID_ARG;
    if(startup->net_config==NULL || !startup->net_config->host_runtime_initialized ||
       startup->net_config->dependency_memory!=COG_NET_DEPENDENCY_MEMORY_DYNAMIC ||
       startup->net_config->dependency_memory_limit!=0U ||
       startup->net_config->observer!=NULL || startup->net_config->observer_user!=NULL)
        return COG_ERROR_UNSUPPORTED;
    cog_result_t rc=worker_reservations(startup);if(rc!=COG_OK) return rc;
    cog_cli_worker_host_t *worker=calloc(1U,sizeof(*worker));if(worker==NULL) return COG_ERROR_NOMEM;
    worker->fixed=*startup;worker->net_template=*startup->net_config;
    worker->executor_capacity=startup->executor_slots;
    worker->socket_path=strdup(startup->socket_path);
    if(startup->net_config->ca_bundle!=NULL) worker->ca_bundle=strdup(startup->net_config->ca_bundle);
    worker->executors=calloc(worker->executor_capacity,sizeof(*worker->executors));
    worker->lanes=calloc(worker->executor_capacity*2U*CLI_HOST_LANES,sizeof(*worker->lanes));
    worker->receive_capacity=startup->frame_bytes;worker->decode_capacity=startup->frame_bytes+128U;
    worker->receive_secret=calloc(worker->receive_capacity,1U);worker->decode_secret=calloc(worker->decode_capacity,1U);
    if(worker->socket_path==NULL || worker->executors==NULL || worker->lanes==NULL ||
       worker->receive_secret==NULL || worker->decode_secret==NULL ||
       (startup->net_config->ca_bundle!=NULL && worker->ca_bundle==NULL)) rc=COG_ERROR_NOMEM;
    worker->fixed.socket_path=worker->socket_path;worker->net_template.ca_bundle=worker->ca_bundle;
    worker->fixed.net_config=&worker->net_template;
    if(rc==COG_OK) rc=secret_new(&worker->startup_profile,startup->frame_bytes);
    for(size_t i=0U;rc==COG_OK && i<worker->executor_capacity;++i) {
        cli_executor_slot_t *slot=&worker->executors[i];
        slot->inbound=worker->lanes+i*2U*CLI_HOST_LANES;slot->outbound=slot->inbound+CLI_HOST_LANES;
        for(size_t lane=0U;rc==COG_OK && lane<CLI_HOST_LANES;++lane) {
            size_t capacity=lane_capacity(lane,i,startup->frame_bytes);
            rc=lane_new(&slot->inbound[lane],capacity);
            if(rc==COG_OK) rc=lane_new(&slot->outbound[lane],capacity);
        }
        if(rc==COG_OK) rc=secret_new(&slot->profile,startup->frame_bytes);
        if(rc==COG_OK) rc=bytes_new(&slot->task,lesser(startup->frame_bytes,WORKER_ARGUMENT_BYTES));
        if(rc==COG_OK) rc=bytes_new(&slot->status,1024U);
        if(rc==COG_OK) rc=bytes_new(&slot->result,lesser(startup->frame_bytes,WORKER_RESULT_BYTES));
        if(rc==COG_OK) rc=bytes_new(&slot->update,lesser(startup->frame_bytes,WORKER_RESULT_BYTES+512U));
    }
    for(size_t i=0U;rc==COG_OK && i<3U;++i) rc=bytes_new(&worker->native_control[i],WORKER_NATIVE_BYTES);
    cog_ipc_endpoint_config_t ipc={0};ipc.max_frame_bytes=startup->frame_bytes;
    ipc.receive_frames=2U;ipc.send_frames=2U;ipc.wipe_buffers=true;
    if(rc==COG_OK) rc=cog_ipc_endpoint_new_ex(&worker->parent,&ipc);
    if(rc==COG_OK) {
        rc=cog_ipc_endpoint_connect(worker->parent,worker->socket_path,NULL);
        if(rc==COG_ERROR_AGAIN) rc=COG_OK;
    }
    if(rc!=COG_OK) { worker_storage_free(worker);return rc; }
    *out=worker;return COG_OK;
}

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
    worker->ca_bundle=calloc(4096U,1U);
    if(worker->ca_bundle!=NULL && startup->net_config->ca_bundle!=NULL) {
        size_t ca_length=strlen(startup->net_config->ca_bundle);
        if(ca_length>4095U) { worker_storage_free(worker);return COG_ERROR_LIMIT; }
        memcpy(worker->ca_bundle,startup->net_config->ca_bundle,ca_length+1U);
    }
    worker->executors=calloc(worker->executor_capacity,sizeof(*worker->executors));
    worker->lanes=calloc(worker->executor_capacity*2U*CLI_HOST_LANES,sizeof(*worker->lanes));
    worker->receive_capacity=startup->frame_bytes;worker->decode_capacity=startup->frame_bytes+128U;
    worker->receive_secret=calloc(worker->receive_capacity,1U);worker->decode_secret=calloc(worker->decode_capacity,1U);
    if(worker->socket_path==NULL || worker->executors==NULL || worker->lanes==NULL ||
       worker->receive_secret==NULL || worker->decode_secret==NULL ||
       worker->ca_bundle==NULL) rc=COG_ERROR_NOMEM;
    worker->fixed.socket_path=worker->socket_path;worker->net_template.ca_bundle=worker->ca_bundle!=NULL && worker->ca_bundle[0]!='\0'?worker->ca_bundle:NULL;
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

static bool same_worker(cog_worker_id_t a,cog_worker_id_t b)
{ return a.incarnation==b.incarnation && a.number==b.number; }
static bool same_owner(cog_store_owner_t a,cog_store_owner_t b)
{ return a.epoch==b.epoch && a.instance_id==b.instance_id; }
static cog_result_t retain_error(cog_cli_worker_host_t *worker,cog_result_t rc)
{
    if(rc!=COG_OK && rc!=COG_ERROR_AGAIN) {
        if(worker->primary==COG_OK) worker->primary=rc;
        (void)cog_cli_worker_host_request_shutdown(worker);
    }
    return rc;
}
static cog_result_t profile_probe(const cog_cli_configuration_t *configuration,
    const cog_cli_bootstrap_probe_t *probe)
{
    const cog_cli_options_t *options=cog_cli_configuration_options(configuration);
    if(options==NULL || options->max_context_bytes>SIZE_MAX/16U) return COG_ERROR_LIMIT;
    cog_store_config_t config={0};config.base_dir=options->store_dir;
    config.require_owner=true;config.max_record_bytes=options->max_context_bytes*16U;
    cog_store_t *store=NULL;cog_session_t *session=NULL;cog_store_load_report_t report;
    cog_result_t rc=cog_store_new(&store,&config);
    if(rc==COG_OK) rc=cog_store_checkpoint_load(store,probe->session_id,probe->checkpoint,
        COG_STORE_LOAD_INSPECT,&session,&report);
    cog_store_owner_t actual={0};cog_store_checkpoint_t latest={0};
    if(rc==COG_OK) rc=cog_store_owner_read(store,probe->session_id,&actual,&latest);
    if(rc==COG_OK && !same_owner(actual,probe->owner)) rc=COG_ERROR_INVALID_STATE;
    /* The immutable probe need not be the latest normal turn checkpoint. */
    cog_session_free(session);cog_store_free(store);return rc;
}
static bool same_transport(const cog_net_config_t *a,const cog_net_config_t *b)
{
    return a->max_connections==b->max_connections && a->connect_timeout_ms==b->connect_timeout_ms &&
        a->stall_timeout_ms==b->stall_timeout_ms && a->max_request_bytes==b->max_request_bytes &&
        a->max_header_bytes==b->max_header_bytes && a->chunk_bytes==b->chunk_bytes &&
        a->chunks_per_request==b->chunks_per_request && a->service_quantum==b->service_quantum &&
        ((a->ca_bundle==NULL && b->ca_bundle==NULL) || (a->ca_bundle!=NULL && b->ca_bundle!=NULL &&
         strcmp(a->ca_bundle,b->ca_bundle)==0));
}
static cog_result_t profile_construct(cog_cli_worker_host_t *worker,cli_executor_slot_t *slot,
    const unsigned char *body,size_t length,cog_cli_configuration_t **out,cog_cli_bootstrap_probe_t *probe)
{
    char ca[4096];size_t required;cog_net_config_t net={0};
    cog_result_t rc=cog_cli_bootstrap_transport_decode(body,length,&net,ca,sizeof(ca),&required);
    if(rc!=COG_OK) return rc;
    if(worker->startup_ready && !same_transport(&net,&worker->net_template)) return COG_ERROR_PROTOCOL;
    if(slot->net!=NULL && !same_transport(&net,&worker->net_template)) return COG_ERROR_PROTOCOL;
    bool made_net=slot->net==NULL;
    net.host_runtime_initialized=worker->fixed.net_config->host_runtime_initialized;
    net.dependency_memory=worker->fixed.net_config->dependency_memory;
    if(made_net) {
        rc=cog_cli_observer_new(&slot->observer,net.max_connections);
        if(rc==COG_OK) {
            net.observer=cog_cli_observer_capture;net.observer_user=slot->observer;
            rc=cog_net_new(&slot->net,&net);
        }
    }
    if(rc==COG_OK) rc=cog_cli_configuration_bootstrap_new(out,body,length,slot->net,probe);
    if(rc==COG_OK) rc=profile_probe(*out,probe);
    if(rc==COG_OK && worker->startup_ready) {
        const cog_cli_options_t *base=cog_cli_configuration_options(worker->startup_configuration);
        const cog_cli_options_t *candidate=cog_cli_configuration_options(*out);
        if(strcmp(base->store_dir,candidate->store_dir)!=0 ||
           ((base->workspace_root==NULL)!=(candidate->workspace_root==NULL)) ||
           (base->workspace_root!=NULL && strcmp(base->workspace_root,candidate->workspace_root)!=0))
            rc=COG_ERROR_PROTOCOL;
    }
    if(rc!=COG_OK) {
        cog_cli_configuration_free(*out);*out=NULL;
        if(made_net) {
            cog_net_free(slot->net);slot->net=NULL;
            cog_cli_observer_free(slot->observer);slot->observer=NULL;
        }
    } else if(!worker->startup_ready) {
        if(net.ca_bundle!=NULL) memcpy(worker->ca_bundle,net.ca_bundle,strlen(net.ca_bundle)+1U);
        else worker->ca_bundle[0]='\0';
        net.ca_bundle=worker->ca_bundle[0]!='\0'?worker->ca_bundle:NULL;
        net.observer=NULL;net.observer_user=NULL;
        worker->net_template=net;
    }
    return rc;
}
static cog_result_t envelope_send(cog_cli_worker_host_t *worker,const cli_host_bytes_t *bytes)
{
    cog_ipc_message_t frame={bytes->data,bytes->length,bytes->capacity};
    return cog_ipc_endpoint_send(worker->parent,&frame);
}
static cog_cli_control_message_t reply_envelope(cog_cli_worker_host_t *worker,
    const cog_cli_control_message_t *request,cog_cli_control_kind_t kind,uint64_t wire,
    const void *payload,size_t length)
{
    cog_cli_control_message_t response=*request;
    response.kind=kind;response.sender_incarnation=worker->fixed.worker.incarnation;
    response.wire_id=wire;response.reply_to=request->wire_id;
    response.payload=payload;response.payload_len=length;return response;
}
static cog_result_t fresh_wire(cli_control_lane_t *lane,uint64_t *wire)
{
    /* Leave final/cancel/receipt identities for an admitted exchange. */
    if(lane->next_wire==0U || lane->next_wire>UINT64_MAX-8U) return COG_ERROR_LIMIT;
    *wire=lane->next_wire++;return COG_OK;
}
static cog_result_t lane_reply(cog_cli_worker_host_t *worker,cli_executor_slot_t *slot,
    const cog_cli_control_message_t *request,cog_cli_control_kind_t kind,
    const void *payload,size_t length,size_t original_length)
{
    size_t index=(size_t)request->lane-1U;
    cli_control_lane_t *in=&slot->inbound[index],*out=&slot->outbound[index];
    if(original_length>in->final_original.capacity) return COG_ERROR_LIMIT;
    uint64_t wire;cog_result_t rc=fresh_wire(out,&wire);if(rc!=COG_OK) return rc;
    cog_cli_control_message_t response=reply_envelope(worker,request,kind,wire,payload,length);
    size_t needed;
    rc=cog_cli_control_encode(&response,in->final_reply.data,in->final_reply.capacity,&needed);
    if(rc!=COG_OK) return rc;
    memcpy(in->final_original.data,worker->receive_secret,original_length);
    in->final_original.length=original_length;in->final_reply.length=needed;
    if(in->final_valid) in->retired_through=in->final_answered_wire;
    in->final_answered_wire=request->wire_id;in->final_reply_wire=wire;
    in->final_valid=true;in->pending_valid=true;return COG_OK;
}
static cog_result_t lane_duplicate(cli_control_lane_t *lane,uint64_t wire,
    const unsigned char *original,size_t length,bool *handled)
{
    *handled=false;
    if(lane->final_valid && wire==lane->final_answered_wire) {
        if(length!=lane->final_original.length || memcmp(original,lane->final_original.data,length)!=0)
            return COG_ERROR_PROTOCOL;
        lane->pending_valid=true;*handled=true;return COG_OK;
    }
    if(wire<=lane->retired_through || (lane->final_valid && wire<lane->final_answered_wire))
        return COG_ERROR_AGAIN; /* Covered identity, no effect or binding error. */
    return COG_OK;
}
static cli_executor_slot_t *find_attempt(cog_cli_worker_host_t *worker,
    const cog_cli_control_message_t *message)
{
    for(size_t i=0U;i<worker->executor_capacity;++i) {
        cli_executor_slot_t *slot=&worker->executors[i];
        if(slot->phase!=CLI_SLOT_FREE && slot->agent==message->agent_id &&
           slot->attempt==message->execution_epoch && slot->owner_peer==message->owner_peer &&
           strcmp(slot->session,message->session_id)==0) return slot;
    }
    return NULL;
}
static cog_result_t open_attempt(cog_cli_worker_host_t *worker,
    const cog_cli_control_message_t *message,cli_executor_slot_t **out)
{
    cli_executor_slot_t *slot=find_attempt(worker,message);
    if(slot!=NULL) { *out=slot;return COG_OK; }
    if(message->agent_id==0U || message->agent_id<=worker->opened_agent_high_watermark ||
       message->execution_epoch==0U || !worker->startup_ready || worker->shutdown)
        return COG_ERROR_INVALID_STATE;
    for(size_t i=2U;i<worker->executor_capacity;++i) if(worker->executors[i].phase==CLI_SLOT_FREE) {
        slot=&worker->executors[i];
        for(size_t lane=0U;lane<CLI_HOST_LANES;++lane)
            if(slot->outbound[lane].next_wire==0U || slot->outbound[lane].next_wire>UINT64_MAX-8U)
                return COG_ERROR_LIMIT;
        slot->phase=CLI_SLOT_RESERVED;
        slot->kind=COG_CLI_EXECUTION_REMOTE_CHILD;slot->agent=message->agent_id;
        slot->attempt=message->execution_epoch;slot->owner_peer=message->owner_peer;
        slot->worker=worker->fixed.worker;slot->proof=worker->parent_proof;
        slot->runtime=worker->runtime_incarnation;strcpy(slot->session,message->session_id);
        worker->opened_agent_high_watermark=message->agent_id;*out=slot;return COG_OK;
    }
    return COG_ERROR_LIMIT;
}
static void secret_bind(cli_secret_exchange_t *exchange,const cog_cli_control_message_t *message)
{
    exchange->key.fabric=CLI_FABRIC_WORKER;
    exchange->key.runtime=message->runtime_incarnation;exchange->key.sender=message->sender_incarnation;
    exchange->key.worker=message->worker;exchange->key.owner_peer=message->owner_peer;
    exchange->key.agent=message->agent_id;exchange->key.attempt=message->execution_epoch;
    exchange->key.lane=message->lane;strcpy(exchange->key.session,message->session_id);
    exchange->wire=message->wire_id;
}
static bool secret_matches(const cli_secret_exchange_t *exchange,const cog_cli_control_message_t *message)
{
    return exchange->key.runtime==message->runtime_incarnation && exchange->key.sender==message->sender_incarnation &&
        same_worker(exchange->key.worker,message->worker) && exchange->key.owner_peer==message->owner_peer &&
        exchange->key.agent==message->agent_id && exchange->key.attempt==message->execution_epoch &&
        strcmp(exchange->key.session,message->session_id)==0 && exchange->wire==message->wire_id;
}
static bool secret_stream_matches(const cli_secret_exchange_t *exchange,const cog_cli_control_message_t *message)
{
    return exchange->key.lane==message->lane && exchange->key.runtime==message->runtime_incarnation &&
        exchange->key.sender==message->sender_incarnation && same_worker(exchange->key.worker,message->worker) &&
        exchange->key.owner_peer==message->owner_peer && exchange->key.agent==message->agent_id &&
        exchange->key.attempt==message->execution_epoch && strcmp(exchange->key.session,message->session_id)==0;
}
static bool retired_profile(cog_cli_worker_host_t *worker,const cog_cli_control_message_t *message)
{
    for(size_t i=0U;i<=worker->executor_capacity;++i) {
        cli_secret_exchange_t *exchange=i==worker->executor_capacity?&worker->startup_profile:&worker->executors[i].profile;
        if(message->wire_id>exchange->retired_through || !secret_stream_matches(exchange,message)) continue;
        /* No original secret bytes remain. This is uniform stale bookkeeping,
           not an exact-byte duplicate or fresh acceptance of changed payload. */
        if(message->wire_id==exchange->wire && exchange->purpose==4U && exchange->source==NULL &&
           exchange->body_length!=0U) exchange->pending=true;
        return true;
    }
    return false;
}
static cog_result_t reload_profile(cli_executor_slot_t *slot,cli_secret_exchange_t *exchange,
    cog_cli_configuration_t **configuration)
{
    const cog_cli_configuration_t *admitted=*configuration;
    exchange->source=NULL; /* The consuming call may free this generation. */
    uint64_t published=0U;
    cog_result_t rc=cog_cli_execution_reload_resolved(slot->app,configuration,exchange->reload_attempts_used,&published);
    if(rc==COG_OK) exchange->source=admitted; /* Exact executor-owned generation. */
    else {
        exchange->retired_through=exchange->wire;
        memset(&exchange->probe,0,sizeof(exchange->probe));
        worker_wipe(exchange->frame,exchange->frame_capacity);
        worker_wipe(exchange->scratch,exchange->scratch_capacity);
        exchange->frame_length=0U;
    }
    return rc;
}
static cog_result_t bootstrap_duplicate(cog_cli_worker_host_t *worker,cli_secret_exchange_t *exchange,
    const cog_cli_control_message_t *message,size_t original_length)
{
    if(!secret_matches(exchange,message) || exchange->source==NULL) return COG_ERROR_INVALID_STATE;
    size_t profile_length=0U,frame_length=0U;
    cog_result_t rc=cog_cli_configuration_bootstrap_encode(exchange->source,&exchange->probe,
        &worker->net_template,exchange->scratch+5U,exchange->scratch_capacity-5U,&profile_length);
    if(rc==COG_OK) {
        cli_wire_writer_t prefix={exchange->scratch,0U,true};
        cli_wire_put(&prefix,exchange->purpose,1U);cli_wire_put(&prefix,exchange->reload_attempts_used,4U);
        cog_cli_control_message_t original=*message;original.payload=exchange->scratch;
        original.payload_len=profile_length+5U;
        rc=cog_cli_control_encode(&original,exchange->frame,exchange->frame_capacity,&frame_length);
    }
    if(rc==COG_OK && (frame_length!=original_length ||
       memcmp(exchange->frame,worker->receive_secret,original_length)!=0)) rc=COG_ERROR_PROTOCOL;
    worker_wipe(exchange->scratch,exchange->scratch_capacity);
    worker_wipe(exchange->frame,exchange->frame_capacity);
    if(rc==COG_OK) exchange->pending=true;
    return rc;
}
static cog_result_t bootstrap_reply(cog_cli_worker_host_t *worker,cli_secret_exchange_t *exchange,
    const cog_cli_control_message_t *request,cog_result_t outcome,uint8_t state)
{
    unsigned char payload[13];cli_wire_writer_t writer={payload,0U,true};
    cli_wire_put(&writer,(uint32_t)outcome,4U);cli_wire_put(&writer,exchange->generation,8U);
    cli_wire_put(&writer,state,1U);
    if(exchange->replied_wire==UINT64_MAX) return COG_ERROR_LIMIT;
    ++exchange->replied_wire;
    cog_cli_control_message_t reply=reply_envelope(worker,request,COG_CLI_CONTROL_BOOTSTRAP_ACK,
        exchange->replied_wire,payload,sizeof(payload));
    cog_result_t rc=cog_cli_control_encode(&reply,exchange->body,exchange->body_capacity,&exchange->body_length);
    if(rc==COG_OK) exchange->pending=true;
    return rc;
}
static cog_result_t bootstrap(cog_cli_worker_host_t *worker,const cog_cli_control_message_t *message,
    size_t original_length)
{
    if(message->lane!=COG_CLI_LANE_PROFILE || message->reply_to!=0U) return COG_ERROR_PROTOCOL;
    if(retired_profile(worker,message)) return COG_OK;
    if(message->payload_len<17U) return COG_ERROR_PROTOCOL;
    cli_wire_reader_t reader={message->payload,message->payload_len,0U,true};
    uint8_t purpose=(uint8_t)cli_wire_get(&reader,1U);
    uint32_t attempts=(uint32_t)cli_wire_get(&reader,4U);
    if(purpose<1U || purpose>4U || attempts>64U ||
       ((purpose==1U || purpose==2U) && attempts!=0U) || (purpose==4U && attempts==0U) ||
       ((purpose==2U)!=(message->agent_id!=0U))) return COG_ERROR_PROTOCOL;
    cli_executor_slot_t *slot=NULL;cli_secret_exchange_t *exchange;
    if(purpose==1U) {
        if(worker->startup_ready && !secret_matches(&worker->startup_profile,message)) return COG_ERROR_INVALID_STATE;
        slot=&worker->executors[0];exchange=&worker->startup_profile;
    } else {
        if(!worker->startup_ready) return COG_ERROR_INVALID_STATE;
        if(purpose==2U) {
            cog_result_t rc=open_attempt(worker,message,&slot);if(rc!=COG_OK) return rc;
        } else if(purpose==3U) {
            slot=&worker->executors[1];
        }
        else {
            slot=find_attempt(worker,message);
            if(slot==NULL || slot->app==NULL || slot->agent!=0U) return COG_ERROR_INVALID_STATE;
        }
        exchange=&slot->profile;
    }
    if(message->wire_id==exchange->wire) return bootstrap_duplicate(worker,exchange,message,original_length);
    if(purpose==3U && (slot->app!=NULL || slot->pending_profile!=NULL || slot->primary!=COG_OK ||
       (slot->session[0]!='\0' && strcmp(slot->session,message->session_id)!=0))) return COG_ERROR_INVALID_STATE;
    if(message->wire_id<=exchange->retired_through || exchange->pending ||
       (exchange->source!=NULL && !(purpose==4U && exchange->acknowledged && message->wire_id>exchange->wire)))
        return COG_ERROR_INVALID_STATE;
    if(exchange->replied_wire==UINT64_MAX) return COG_ERROR_LIMIT;
    const unsigned char *profile=(const unsigned char *)message->payload+5U;
    size_t profile_length=message->payload_len-5U;
    cog_cli_configuration_t *configuration=NULL;cog_cli_bootstrap_probe_t probe={0};
    cog_result_t outcome=profile_construct(worker,slot,profile,profile_length,&configuration,&probe);
    if(outcome!=COG_OK) return outcome;
    if(slot->cancel_latched && purpose!=3U) {
        cog_cli_configuration_free(configuration);configuration=NULL;outcome=COG_ERROR_ABORTED;
    }
    cli_wire_reader_t generation={profile,profile_length,4U,true};
    uint64_t number=cli_wire_get(&generation,8U);
    if(!generation.valid || number==0U || (purpose==4U && number!=(uint64_t)attempts+1U)) {
        cog_cli_configuration_free(configuration);return COG_ERROR_PROTOCOL;
    }
    if(exchange->wire!=0U) exchange->retired_through=exchange->wire;
    secret_bind(exchange,message);exchange->purpose=purpose;exchange->reload_attempts_used=attempts;
    exchange->generation=number;exchange->probe=probe;exchange->source=configuration;
    if(configuration!=NULL) {
        if(purpose==1U) {
            worker->startup_configuration=configuration;worker->probe=probe;worker->startup_ready=true;
        } else if(purpose==4U) {
            outcome=reload_profile(slot,exchange,&configuration);
        } else {
            slot->pending_profile=configuration;slot->configuration_generation=number;
            slot->phase=CLI_SLOT_PROFILE;
            if(purpose==3U) {
                slot->cancel_latched=false;slot->local_done=false;slot->primary=COG_OK;
                slot->kind=COG_CLI_EXECUTION_STAGED;slot->owner_peer=message->owner_peer;
                slot->runtime=worker->runtime_incarnation;slot->worker=worker->fixed.worker;
                strcpy(slot->session,message->session_id);
            }
        }
    }
    return bootstrap_reply(worker,exchange,message,outcome,purpose==4U && outcome==COG_OK?2U:1U);
}
static cog_result_t child_start(cog_cli_worker_host_t *worker,cli_executor_slot_t *slot,
    const cog_cli_control_message_t *message,size_t original_length)
{
    if(message->lane!=COG_CLI_LANE_DELEGATE || message->reply_to!=0U || slot->agent==0U)
        return COG_ERROR_PROTOCOL;
    cli_wire_reader_t r={message->payload,message->payload_len,0U,true};
    size_t parent_length,call_length,task_length;
    const char *parent=cli_wire_read_string(&r,&parent_length);
    uint64_t node=cli_wire_get(&r,8U);uint16_t ordinal=(uint16_t)cli_wire_get(&r,2U);
    const char *call=cli_wire_read_string(&r,&call_length);
    const char *task=cli_wire_read_string(&r,&task_length);
    uint64_t generation=cli_wire_get(&r,8U),deadline=cli_wire_get(&r,8U),group=cli_wire_get(&r,8U);
    size_t status_bytes=(size_t)cli_wire_get(&r,4U),result_bytes=(size_t)cli_wire_get(&r,4U);
    size_t count=(size_t)cli_wire_get(&r,2U);cog_agent_id_t members[CLI_HOST_MAX_CHILDREN]={0};
    if(count==0U || count>CLI_HOST_MAX_CHILDREN) return COG_ERROR_PROTOCOL;
    bool self=false;
    for(size_t i=0U;i<count;++i) {
        members[i]=cli_wire_get(&r,8U);
        if(members[i]==0U || (i!=0U && members[i]<=members[i-1U])) return COG_ERROR_PROTOCOL;
        if(members[i]==slot->agent) self=true;
    }
    if(!r.valid || r.used!=r.length || !self || !cli_wire_session(parent,parent_length) ||
       node==0U || ordinal>=16U || call_length==0U || call_length>255U || task_length==0U ||
       generation==0U || deadline==0U || group==0U || status_bytes==0U || result_bytes==0U)
        return COG_ERROR_PROTOCOL;
    if(task_length>=slot->task.capacity || status_bytes>slot->status.capacity ||
       result_bytes>slot->result.capacity) return COG_ERROR_LIMIT;
    cli_control_lane_t *incoming=&slot->inbound[COG_CLI_LANE_DELEGATE-1U];
    cli_control_lane_t *outgoing=&slot->outbound[COG_CLI_LANE_DELEGATE-1U];
    if(original_length>incoming->final_original.capacity || incoming->final_reply.capacity<256U ||
       outgoing->next_wire==0U || outgoing->next_wire>UINT64_MAX-8U) return COG_ERROR_LIMIT;
    cog_result_t outcome=COG_OK;
    if(!slot->profile.acknowledged) return COG_ERROR_INVALID_STATE;
    if(slot->cancel_latched) outcome=COG_ERROR_ABORTED;
    else if(slot->started || slot->pending_profile==NULL || generation!=slot->configuration_generation)
        outcome=COG_ERROR_INVALID_STATE;
    else if(deadline<=worker->now_ms) outcome=COG_ERROR_TIMEOUT;
    if(outcome==COG_OK) {
        memcpy(slot->parent_session,parent,parent_length);slot->parent_session[parent_length]='\0';
        memcpy(slot->parent_call,call,call_length);slot->parent_call[call_length]='\0';
        memcpy(slot->task.data,task,task_length);slot->task.data[task_length]='\0';slot->task.length=task_length;
        slot->parent_node=node;slot->parent_ordinal=ordinal;slot->group=group;slot->deadline_ms=deadline;
        slot->member_count=count;memcpy(slot->members,members,count*sizeof(*members));
        /* Actual negotiated whole result bound, not the larger physical arena. */
        slot->result.length=result_bytes;slot->status.length=status_bytes;
        char provenance[4096];size_t needed;
        cog_cli_child_context_t context={slot->parent_session,slot->parent_call,node,worker->runtime_incarnation,
            group,ordinal,slot->agent,slot->attempt,generation,members,count};
        outcome=cog_cli_child_context_encode(&context,provenance,sizeof(provenance),&needed);
        if(outcome==COG_OK) {
            cog_cli_execution_admission_t admission={0};admission.kind=COG_CLI_EXECUTION_REMOTE_CHILD;
            admission.configuration=slot->pending_profile;slot->pending_profile=NULL;
            admission.session_id=slot->session;admission.parent_session_id=slot->parent_session;
            admission.task=(const char *)slot->task.data;admission.provenance=provenance;
            admission.provenance_len=needed-1U;admission.configuration_generation=generation;
            admission.absolute_deadline_ms=deadline;admission.observer=slot->observer;
            outcome=cog_cli_execution_new(&slot->app,&admission);
            if(outcome!=COG_OK) slot->profile.source=NULL;
        }
        if(outcome==COG_OK) {
            cog_cli_delegation_context_t *bridge;
            outcome=cog_cli_delegation_context_new(&bridge,slot->app,NULL,worker,slot->agent,group,0U,0U);
        }
        if(outcome==COG_OK) {
            cog_store_checkpoint_t checkpoint;
            outcome=cog_store_owner_read(slot->app->store,slot->session,&slot->owner,&checkpoint);
        }
        if(outcome==COG_OK) outcome=cog_cli_execution_begin(slot->app);
        if(outcome==COG_OK) {
            slot->started=true;slot->phase=CLI_SLOT_STARTED;slot->factual_status=COG_AGENT_STATUS_RUNNING;
            slot->factual_outcome=COG_OK;
        }
    }
    if(outcome!=COG_OK && !slot->started) {
        slot->primary=outcome;slot->local_done=true;slot->phase=CLI_SLOT_RETAINED;
        slot->factual_status=outcome==COG_ERROR_ABORTED?COG_AGENT_STATUS_CANCELLED:
            outcome==COG_ERROR_TIMEOUT?COG_AGENT_STATUS_TIMEOUT:COG_AGENT_STATUS_FAILED;
        slot->factual_outcome=outcome;
        static const char failed[]="child admission failed";
        size_t n=sizeof(failed)-1U;
        if(n>=slot->result.capacity) n=0U;
        memcpy(slot->result.data,failed,n);slot->result.data[n]='\0';slot->result.length=n;
    }
    unsigned char result[4];cli_wire_writer_t writer={result,0U,true};cli_wire_put(&writer,(uint32_t)outcome,4U);
    return lane_reply(worker,slot,message,COG_CLI_CONTROL_CHILD_START_ACK,result,sizeof(result),original_length);
}
static cog_result_t child_cancel(cog_cli_worker_host_t *worker,cli_executor_slot_t *slot,
    const cog_cli_control_message_t *message)
{
    if(message->lane!=COG_CLI_LANE_CANCEL || message->payload_len!=8U || message->reply_to!=0U)
        return COG_ERROR_PROTOCOL;
    cli_wire_reader_t r={message->payload,message->payload_len,0U,true};uint64_t deadline=cli_wire_get(&r,8U);
    if(deadline==0U) return COG_ERROR_PROTOCOL;
    slot->cancel_latched=true;
    if(slot->drain_deadline_ms==0U || deadline<slot->drain_deadline_ms) slot->drain_deadline_ms=deadline;
    if(slot->app!=NULL) (void)cog_cli_app_shutdown(slot->app);
    else {
        slot->local_done=true;slot->phase=CLI_SLOT_RETAINED;
        slot->factual_status=COG_AGENT_STATUS_CANCELLED;slot->factual_outcome=COG_ERROR_ABORTED;
    }
    (void)worker;return COG_OK;
}
static bool terminal_status(cog_agent_status_t status)
{ return status>=COG_AGENT_STATUS_COMPLETED && status<=COG_AGENT_STATUS_TIMEOUT; }
static cog_result_t update_receipt(cog_cli_worker_host_t *worker,cli_executor_slot_t *slot,
    const cog_cli_control_message_t *message,size_t original_length)
{
    if(message->lane!=COG_CLI_LANE_UPDATE) return COG_ERROR_PROTOCOL;
    cli_control_lane_t *lane=&slot->outbound[COG_CLI_LANE_UPDATE-1U];
    if(lane->final_valid && message->wire_id==lane->final_reply_wire) {
        return original_length==lane->final_original.length &&
            memcmp(worker->receive_secret,lane->final_original.data,original_length)==0?COG_OK:COG_ERROR_PROTOCOL;
    }
    cog_cli_control_receipt_t receipt;
    cog_result_t rc=cog_cli_control_receipt_decode(message->payload,message->payload_len,&receipt);
    if(rc!=COG_OK) return rc;
    if(receipt.parent_runtime_incarnation!=worker->runtime_incarnation || receipt.agent_id!=slot->agent ||
       receipt.execution_epoch!=slot->attempt || receipt.owner_peer!=slot->owner_peer ||
       receipt.wire_id!=message->reply_to || message->reply_to==0U)
        return COG_ERROR_PROTOCOL;
    if(message->reply_to<=lane->retired_through) return COG_ERROR_AGAIN;
    if(!lane->pending_valid || message->reply_to!=slot->pending_update_wire) return COG_ERROR_INVALID_STATE;
    if(receipt.parent_accepted_sequence<slot->accepted_sequence) return COG_ERROR_PROTOCOL;
    uint64_t sequence=slot->pending_update_sequence;
    if(receipt.disposition==COG_CLI_CONTROL_RECEIPT_RETRY) {
        if(receipt.parent_accepted_sequence!=sequence && receipt.parent_accepted_sequence!=sequence-1U)
            return COG_ERROR_PROTOCOL;
    } else if(receipt.disposition==COG_CLI_CONTROL_RECEIPT_ACCEPTED) {
        if(receipt.parent_accepted_sequence!=sequence) return COG_ERROR_PROTOCOL;
        cog_agent_id_t id;cog_agent_update_t update;char data[1];size_t required;
        /* Scalar status is in our retained CGA body, never inferred from a newer
           factual result which may already have advanced while this wire waited. */
        rc=cog_agent_update_decode(slot->update.data,slot->update.length,&id,&update,data,sizeof(data),&required);
        if(rc!=COG_OK && rc!=COG_ERROR_OVERFLOW) return rc;
        cli_wire_reader_t body={slot->update.data,slot->update.length,36U,true};
        cog_agent_status_t status=(cog_agent_status_t)cli_wire_get(&body,1U);
        cog_result_t outcome=cli_wire_signed(cli_wire_get(&body,4U));
        if(terminal_status(status)?(receipt.accepted_terminal_status!=status || receipt.accepted_terminal_outcome!=outcome):
           receipt.accepted_terminal_status!=COG_AGENT_STATUS_PENDING) return COG_ERROR_PROTOCOL;
        slot->accepted_status=status;slot->accepted_outcome=outcome;
    } else if(receipt.disposition==COG_CLI_CONTROL_RECEIPT_CANCEL_CONSUMED) {
        if(!slot->cancel_latched || receipt.parent_accepted_sequence==UINT64_MAX ||
           receipt.parent_accepted_sequence+1U!=sequence) return COG_ERROR_PROTOCOL;
        cli_wire_reader_t body={slot->update.data,slot->update.length,36U,true};
        cog_agent_status_t status=(cog_agent_status_t)cli_wire_get(&body,1U);
        if(status!=COG_AGENT_STATUS_RUNNING && status!=COG_AGENT_STATUS_COMPLETED) return COG_ERROR_PROTOCOL;
    } else if(receipt.disposition!=COG_CLI_CONTROL_RECEIPT_TERMINAL_FIXED) return COG_ERROR_PROTOCOL;
    if(original_length>lane->final_original.capacity) return COG_ERROR_LIMIT;
    memcpy(lane->final_original.data,worker->receive_secret,original_length);
    lane->final_original.length=original_length;lane->final_reply_wire=message->wire_id;lane->final_valid=true;
    slot->receipt=receipt;
    if(receipt.disposition!=COG_CLI_CONTROL_RECEIPT_RETRY) {
        slot->accepted_sequence=receipt.parent_accepted_sequence;
        if(terminal_status(receipt.accepted_terminal_status)) {
            slot->accepted_status=receipt.accepted_terminal_status;
            slot->accepted_outcome=receipt.accepted_terminal_outcome;slot->terminal_acked=true;
        }
        lane->retired_through=slot->pending_update_wire;
        lane->pending_valid=false;worker_wipe(lane->pending.data,lane->pending.capacity);lane->pending.length=0U;
        slot->pending_update_wire=0U;slot->pending_update_sequence=0U;
    }
    return COG_OK;
}
static cog_result_t queue_update(cog_cli_worker_host_t *worker,cli_executor_slot_t *slot)
{
    if(slot->agent==0U || slot->terminal_acked || slot->factual_status==COG_AGENT_STATUS_PENDING)
        return COG_ERROR_AGAIN;
    cli_control_lane_t *lane=&slot->outbound[COG_CLI_LANE_UPDATE-1U];
    if(lane->pending_valid) return COG_ERROR_AGAIN;
    if(slot->factual_status==COG_AGENT_STATUS_RUNNING &&
       (slot->accepted_status==COG_AGENT_STATUS_RUNNING || slot->cancel_latched)) return COG_ERROR_AGAIN;
    if(slot->accepted_sequence==UINT64_MAX) return COG_ERROR_LIMIT;
    cog_agent_status_t status=slot->factual_status;cog_result_t outcome=slot->factual_outcome;
    if(slot->local_done && slot->cancel_latched &&
       slot->receipt.disposition==COG_CLI_CONTROL_RECEIPT_CANCEL_CONSUMED) {
        status=COG_AGENT_STATUS_CANCELLED;outcome=COG_ERROR_ABORTED;
    }
    const char *text=slot->local_done?(const char *)slot->result.data:"running";
    size_t length=slot->local_done?slot->result.length:7U;
    cog_agent_update_t update={slot->owner_peer,slot->attempt,slot->accepted_sequence+1U,status,outcome,text,length};
    cog_result_t rc=cog_agent_update_encode(slot->agent,&update,slot->update.data,slot->update.capacity,&slot->update.length);
    if(rc!=COG_OK) return rc;
    uint64_t wire;rc=fresh_wire(lane,&wire);if(rc!=COG_OK) return rc;
    cog_cli_control_message_t message={COG_CLI_CONTROL_CHILD_UPDATE,COG_CLI_LANE_UPDATE,
        worker->runtime_incarnation,worker->fixed.worker.incarnation,wire,0U,worker->fixed.worker,
        slot->owner_peer,slot->agent,slot->attempt,slot->session,slot->update.data,slot->update.length};
    rc=cog_cli_control_encode(&message,lane->pending.data,lane->pending.capacity,&lane->pending.length);
    if(rc==COG_OK) { lane->pending_valid=true;slot->pending_update_wire=wire;slot->pending_update_sequence=update.sequence; }
    return rc;
}
static cog_result_t native_frame(cog_cli_worker_host_t *worker,size_t index,cog_worker_control_kind_t kind,
    uint32_t active,uint64_t deadline,cog_result_t outcome)
{
    if(worker->native_pending[index]) return COG_ERROR_AGAIN;
    cog_worker_control_t control={kind,worker->fixed.worker,active,deadline,outcome};
    cli_host_bytes_t *bytes=&worker->native_control[index];
    cog_result_t rc=cog_orchestrator_worker_encode(&control,bytes->data,bytes->capacity,&bytes->length);
    if(rc==COG_OK) worker->native_pending[index]=true;
    return rc;
}
cog_result_t cog_cli_worker_host_request_shutdown(cog_cli_worker_host_t *worker)
{
    if(worker==NULL) return COG_ERROR_NULL_PTR;
    if(!worker->shutdown) {
        worker->shutdown=true;
        if(worker->shutdown_deadline_ms==0U)
            worker->shutdown_deadline_ms=worker->now_ms>UINT64_MAX-3000U?UINT64_MAX:worker->now_ms+3000U;
        for(size_t i=0U;i<worker->executor_capacity;++i) {
            cli_executor_slot_t *slot=&worker->executors[i];
            if(slot->phase==CLI_SLOT_FREE) continue;
            slot->cancel_latched=true;
            if(slot->app!=NULL) (void)cog_cli_app_shutdown(slot->app);
            else { slot->local_done=true;slot->factual_status=COG_AGENT_STATUS_CANCELLED;slot->factual_outcome=COG_ERROR_ABORTED; }
        }
    }
    return COG_OK;
}
static cog_result_t native_receive(cog_cli_worker_host_t *worker,const void *bytes,size_t length)
{
    cog_worker_control_t control;cog_result_t rc=cog_orchestrator_worker_decode(bytes,length,&control);
    if(rc!=COG_OK) return rc;
    if(control.kind!=COG_WORKER_SHUTDOWN || !same_worker(control.worker,worker->fixed.worker)) return COG_ERROR_PROTOCOL;
    if(worker->native_control[2].length!=0U) {
        cog_worker_control_t previous;
        rc=cog_orchestrator_worker_decode(worker->native_control[2].data,worker->native_control[2].length,&previous);
        if(rc!=COG_OK || previous.deadline_ms!=control.deadline_ms) return COG_ERROR_PROTOCOL;
    } else {
        /* Retain immutable requested deadline in the ACK reservation; queue it
           only after actual local provider/tool drain has ended. */
        control.kind=COG_WORKER_SHUTDOWN_ACK;
        rc=cog_orchestrator_worker_encode(&control,worker->native_control[2].data,
            worker->native_control[2].capacity,&worker->native_control[2].length);
        if(rc!=COG_OK) return rc;
    }
    if(worker->shutdown_deadline_ms==0U || control.deadline_ms<worker->shutdown_deadline_ms)
        worker->shutdown_deadline_ms=control.deadline_ms;
    return cog_cli_worker_host_request_shutdown(worker);
}
static cog_result_t dispose_execution(cli_executor_slot_t *slot)
{
    cog_result_t rc=cog_cli_execution_close(&slot->app,COG_OK);
    if(slot->app!=NULL) return rc;
    slot->profile.source=NULL;
    cog_cli_configuration_free(slot->pending_profile);slot->pending_profile=NULL;
    if(slot->net!=NULL) {
        if(slot->observer!=NULL) {
            cog_result_t cleanup=cog_cli_observer_clear(slot->observer,slot->net);
            if(rc==COG_OK) rc=cleanup;
        }
        cog_net_free(slot->net);slot->net=NULL;
    }
    cog_cli_observer_free(slot->observer);slot->observer=NULL;
    return rc;
}
static void capture_result(cli_executor_slot_t *slot)
{
    const cog_node_data_t *data=NULL;
    if(slot->app!=NULL) {
        for(cog_session_node_t *node=cog_session_active(slot->app->session);node!=NULL;node=cog_node_parent(node)) {
            const cog_node_data_t *candidate=cog_node_data(node);
            if(candidate->message!=NULL && candidate->message->role==COG_MESSAGE_ROLE_ASSISTANT) { data=candidate;break; }
        }
    }
    size_t limit=slot->result.length!=0U?slot->result.length:slot->result.capacity;
    size_t length=data!=NULL?data->content_len:0U;const char *text=data!=NULL?data->content:"";
    bool completed=data!=NULL && data->message!=NULL && data->message->status==COG_MESSAGE_COMPLETE &&
        data->message->tool_call_count==0U && slot->app!=NULL && !slot->app->continuation_required && !slot->app->recovery;
    if(slot->primary==COG_OK && !completed) {
        slot->primary=slot->app!=NULL && slot->app->deferred_error!=COG_OK?slot->app->deferred_error:
            slot->cancel_latched?COG_ERROR_ABORTED:COG_ERROR_IO;
    }
    slot->factual_outcome=slot->primary;
    slot->factual_status=slot->primary==COG_OK?COG_AGENT_STATUS_COMPLETED:
        slot->primary==COG_ERROR_ABORTED?COG_AGENT_STATUS_CANCELLED:
        slot->primary==COG_ERROR_TIMEOUT?COG_AGENT_STATUS_TIMEOUT:COG_AGENT_STATUS_FAILED;
    if(length>=limit || length>=slot->result.capacity) {
        text="child result exceeds negotiated delivery bound";length=strlen(text);
        slot->factual_status=COG_AGENT_STATUS_FAILED;slot->factual_outcome=COG_ERROR_LIMIT;
        if(length>=limit || length>=slot->result.capacity) { text="";length=0U; }
    }
    if(length!=0U) memcpy(slot->result.data,text,length);
    slot->result.data[length]='\0';slot->result.length=length;slot->terminal_copied=true;
}
static cog_result_t executor_step(cog_cli_worker_host_t *worker,cli_executor_slot_t *slot,
    cog_cli_host_runtime_step_t *info)
{
    if(slot->app==NULL || slot->local_done) return COG_ERROR_AGAIN;
    cog_core_step_info_t step={0};cog_result_t rc;
    if(slot->app->headless->drive==CLI_DRIVE_DRAINING)
        rc=cog_cli_execution_drain_step(slot->app,&step);
    else if(slot->app->headless->drive==CLI_DRIVE_UNBEGUN) {
        if(!worker->shutdown) return COG_ERROR_AGAIN;
        slot->local_done=true;return COG_OK;
    } else rc=cog_cli_execution_step(slot->app,&step);
    if(rc!=COG_OK && rc!=COG_ERROR_AGAIN && slot->primary==COG_OK) slot->primary=rc;
    if(step.dispatched>SIZE_MAX-info->dispatched) return COG_ERROR_LIMIT;
    info->dispatched+=step.dispatched;
    if(step.next_deadline_ms<info->next_deadline_ms) info->next_deadline_ms=step.next_deadline_ms;
    if(step.max_wait_ms<info->max_wait_ms) info->max_wait_ms=step.max_wait_ms;
    if(step.done) {
        slot->local_done=true;slot->phase=CLI_SLOT_RETAINED;
        cog_result_t ended=cog_cli_execution_end(slot->app);
        if(slot->primary==COG_OK && ended!=COG_OK) slot->primary=ended;
        if(slot->agent!=0U) capture_result(slot);
    }
    return rc;
}
cog_result_t cog_cli_worker_host_close(cog_cli_worker_host_t **pointer,cog_result_t primary)
{
    if(pointer==NULL) return COG_ERROR_NULL_PTR;
    cog_cli_worker_host_t *worker=*pointer;if(worker==NULL) return primary;
    if(worker->in_step || (worker->stepped && !worker->done)) return COG_ERROR_INVALID_STATE;
    if(primary==COG_OK) primary=worker->primary;
    cog_cli_configuration_free(worker->startup_configuration);worker->startup_configuration=NULL;
    worker->startup_profile.source=NULL;
    for(size_t i=0U;i<worker->executor_capacity;++i) {
        cog_result_t rc=dispose_execution(&worker->executors[i]);
        if(primary==COG_OK) primary=rc;
        if(worker->executors[i].app!=NULL) return COG_ERROR_INVALID_STATE;
    }
    worker_storage_free(worker);*pointer=NULL;return primary;
}
static bool handoff_tuple(const cog_handoff_message_t *a,const cog_handoff_message_t *b)
{
    return strcmp(a->session_id,b->session_id)==0 && a->source_peer==b->source_peer &&
        a->target_peer==b->target_peer && same_owner(a->expected,b->expected) &&
        same_owner(a->next,b->next) && a->checkpoint.generation==b->checkpoint.generation;
}
static cog_result_t prior_handoff(cli_executor_slot_t *slot,const cog_handoff_message_t *request)
{
    cli_control_lane_t *lane=&slot->inbound[COG_CLI_LANE_HANDOFF-1U];
    if(!lane->final_valid) return COG_ERROR_INVALID_STATE;
    unsigned char storage[512];cog_cli_control_message_t envelope;size_t required;
    cog_result_t rc=cog_cli_control_decode(lane->final_reply.data,lane->final_reply.length,
        &envelope,storage,sizeof(storage),&required);
    if(rc!=COG_OK) return rc;
    char session[128];cog_handoff_message_t previous;
    rc=cog_orchestrator_handoff_decode(envelope.payload,envelope.payload_len,&previous,session,sizeof(session),&required);
    return rc!=COG_OK?rc:handoff_tuple(&previous,request)?COG_OK:COG_ERROR_INVALID_STATE;
}
static cog_result_t handoff_receive(cog_cli_worker_host_t *worker,cli_executor_slot_t *slot,
    const cog_cli_control_message_t *message,size_t original_length)
{
    if(message->lane!=COG_CLI_LANE_HANDOFF || message->agent_id!=0U || message->reply_to!=0U)
        return COG_ERROR_PROTOCOL;
    char session[128];cog_handoff_message_t request;size_t required;
    cog_result_t rc=cog_orchestrator_handoff_decode(message->payload,message->payload_len,&request,
        session,sizeof(session),&required);
    if(rc!=COG_OK) return rc;
    if(strcmp(session,message->session_id)!=0) return COG_ERROR_PROTOCOL;
    cli_control_lane_t *incoming=&slot->inbound[COG_CLI_LANE_HANDOFF-1U];
    cli_control_lane_t *outgoing=&slot->outbound[COG_CLI_LANE_HANDOFF-1U];
    if(original_length>incoming->final_original.capacity || incoming->final_reply.capacity<384U ||
       outgoing->next_wire==0U || outgoing->next_wire>UINT64_MAX-8U) return COG_ERROR_LIMIT;
    cog_handoff_message_t response=request;
    switch(request.kind) {
    case COG_HANDOFF_QUIESCE:
        if(slot->app==NULL || slot->owner_peer!=request.source_peer || !same_owner(slot->owner,request.expected))
            return COG_ERROR_INVALID_STATE;
        rc=cog_cli_execution_quiesce(slot->app,request.expected,&response.checkpoint);
        if(rc==COG_OK) response.kind=COG_HANDOFF_CHECKPOINTED;
        break;
    case COG_HANDOFF_CHECKPOINTED:
        if(slot!=&worker->executors[1] || slot->owner_peer!=request.target_peer || slot->app!=NULL ||
           slot->pending_profile==NULL || slot->profile.purpose!=3U ||
           slot->outbound[COG_CLI_LANE_VIEW-1U].pending_valid ||
           slot->outbound[COG_CLI_LANE_POLICY-1U].pending_valid ||
           strcmp(slot->profile.probe.session_id,session)!=0 || !same_owner(slot->profile.probe.owner,request.expected) ||
           slot->profile.probe.checkpoint.generation!=request.checkpoint.generation)
            return COG_ERROR_INVALID_STATE;
        {
            cog_cli_execution_admission_t admission={0};admission.kind=COG_CLI_EXECUTION_STAGED;
            admission.configuration=slot->pending_profile;slot->pending_profile=NULL;
            admission.session_id=slot->session;admission.expected=request.expected;admission.next=request.next;
            admission.checkpoint=request.checkpoint;admission.configuration_generation=slot->configuration_generation;
            admission.reload_attempts_used=slot->profile.reload_attempts_used;admission.observer=slot->observer;
            rc=cog_cli_execution_new(&slot->app,&admission);
            if(rc!=COG_OK) slot->profile.source=NULL;
            if(rc==COG_OK) {
                cog_cli_delegation_context_t *bridge;
                rc=cog_cli_delegation_context_new(&bridge,slot->app,NULL,worker,0U,0U,0U,0U);
            }
            if(rc==COG_OK) {
                /* These serials belong to the newly constructed executor, not
                   the retained native stream. Wire receipts/counters survive. */
                slot->outbound[COG_CLI_LANE_VIEW-1U].provisional_prefix=0U;
                slot->outbound[COG_CLI_LANE_POLICY-1U].provisional_prefix=0U;
                slot->kind=COG_CLI_EXECUTION_STAGED;slot->phase=CLI_SLOT_PREPARED;response.kind=COG_HANDOFF_PREPARED;
            }
        }
        break;
    case COG_HANDOFF_PREPARED:
        if(slot->app==NULL || slot->owner_peer!=request.source_peer || !same_owner(slot->owner,request.expected))
            return COG_ERROR_INVALID_STATE;
        rc=prior_handoff(slot,&request);
        if(rc==COG_OK) rc=cog_cli_execution_release_source(slot->app,request.checkpoint);
        /* Retain this source-release response independently of app disposal. */
        break;
    case COG_HANDOFF_COMMIT:
        if(slot->app==NULL || slot->owner_peer!=request.target_peer || slot->phase!=CLI_SLOT_PREPARED)
            return COG_ERROR_INVALID_STATE;
        rc=prior_handoff(slot,&request);
        if(rc==COG_OK) rc=cog_cli_execution_stage_guard(slot->app,request.next,request.checkpoint);
        if(rc==COG_OK) { slot->owner=request.next;response.kind=COG_HANDOFF_READY; }
        break;
    case COG_HANDOFF_ACTIVATE:
        if(slot->app==NULL || slot->owner_peer!=request.target_peer || !same_owner(slot->owner,request.next))
            return COG_ERROR_INVALID_STATE;
        rc=prior_handoff(slot,&request);
        if(rc==COG_OK) rc=cog_cli_execution_stage_activate(slot->app,request.next,request.checkpoint);
        if(rc==COG_OK) rc=cog_cli_execution_begin(slot->app);
        if(rc==COG_OK) { slot->started=true;slot->phase=CLI_SLOT_STARTED; }
        break;
    case COG_HANDOFF_ABORT:
        /* A bootstrap-only target has no prior HANDOFF response. Its immutable
           profile probe is the cleanup binding; after reuse it also excludes
           fresh late requests for the old disposed transaction. */
        if(slot->phase==CLI_SLOT_PROFILE && slot->profile.purpose==3U && slot->app==NULL) {
            if(slot->owner_peer!=request.target_peer || strcmp(slot->profile.probe.session_id,session)!=0 ||
               !same_owner(slot->profile.probe.owner,request.expected) ||
               slot->profile.probe.checkpoint.generation!=request.checkpoint.generation)
                return COG_ERROR_INVALID_STATE;
        } else {
            rc=prior_handoff(slot,&request);
            if(rc!=COG_OK) return rc;
        }
        /* Consuming failure is retained independently of the surviving pointer.
           A later fresh request must not turn dispose(NULL) into reuse proof. */
        if(slot->primary!=COG_OK) { rc=slot->primary;break; }
        if(incoming->pending.length!=0U &&
           (incoming->pending.length!=original_length ||
            memcmp(incoming->pending.data,worker->receive_secret,original_length)!=0)) return COG_ERROR_PROTOCOL;
        if(incoming->pending.length==0U) {
            if(original_length>incoming->pending.capacity) return COG_ERROR_LIMIT;
            memcpy(incoming->pending.data,worker->receive_secret,original_length);
            incoming->pending.length=original_length;
            slot->cancel_latched=true;
            if(slot->app!=NULL) {
                rc=cog_cli_app_shutdown(slot->app);
                if(rc!=COG_OK) break;
                if(slot->app->headless->drive==CLI_DRIVE_UNBEGUN) slot->local_done=true;
            }
        }
        if(slot->app!=NULL && !slot->local_done) return COG_ERROR_AGAIN;
        /* Ordered final disposal must follow consumption of old executor state.
           Otherwise an unsent old POLICY (which has no owner epoch field) could
           survive resource reuse and impersonate a new executor prompt. */
        {
            cli_control_lane_t *view=&slot->outbound[COG_CLI_LANE_VIEW-1U];
            cli_control_lane_t *policy=&slot->outbound[COG_CLI_LANE_POLICY-1U];
            if(view->pending_valid || policy->pending_valid ||
               (policy->provisional_reply.length!=0U && policy->provisional_reply.data[0]==1U))
                return COG_ERROR_AGAIN;
        }
        rc=dispose_execution(slot);
        if(rc==COG_OK) {
            slot->phase=CLI_SLOT_RETAINED;slot->started=false;slot->local_done=true;
            response.kind=COG_HANDOFF_ABORT;
        }
        break;
    default:return COG_ERROR_PROTOCOL;
    }
    if(rc==COG_ERROR_AGAIN) return rc;
    if(rc!=COG_OK) {
        if(slot->primary==COG_OK) slot->primary=rc;
        response.kind=COG_HANDOFF_FAILED;
    }
    unsigned char body[192];size_t length;
    cog_result_t encoded=cog_orchestrator_handoff_encode(&response,body,sizeof(body),&length);
    if(encoded!=COG_OK) return encoded;
    rc=lane_reply(worker,slot,message,COG_CLI_CONTROL_HANDOFF,body,length,original_length);
    if(rc==COG_OK) incoming->pending.length=0U;
    return rc;
}
static cog_result_t queue_control(cog_cli_worker_host_t *worker,cli_executor_slot_t *slot,
    cog_cli_control_kind_t kind,cog_cli_control_lane_t kind_lane,const void *body,size_t length)
{
    cli_control_lane_t *lane=&slot->outbound[(size_t)kind_lane-1U];
    if(lane->pending_valid) return COG_ERROR_AGAIN;
    uint64_t wire;cog_result_t rc=fresh_wire(lane,&wire);if(rc!=COG_OK) return rc;
    cog_cli_control_message_t message={kind,kind_lane,worker->runtime_incarnation,
        worker->fixed.worker.incarnation,wire,0U,worker->fixed.worker,slot->owner_peer,
        slot->agent,slot->attempt,slot->session,body,length};
    rc=cog_cli_control_encode(&message,lane->pending.data,lane->pending.capacity,&lane->pending.length);
    if(rc==COG_OK) { lane->pending_valid=true;lane->pending_wire=wire; }
    return rc;
}
static cog_result_t publish_view(cog_cli_worker_host_t *worker,cli_executor_slot_t *slot)
{
    if(slot->agent!=0U || slot->app==NULL || !slot->started) return COG_ERROR_AGAIN;
    cli_control_lane_t *lane=&slot->outbound[COG_CLI_LANE_VIEW-1U];
    if(lane->pending_valid) return COG_ERROR_AGAIN;
    cog_cli_display_snapshot_t view;uint64_t serial;
    cog_result_t rc=cog_cli_execution_copy_view(slot->app,&serial,&view);if(rc!=COG_OK) return rc;
    uint64_t active,published;uint32_t attempts;
    rc=cog_cli_execution_copy_configuration(slot->app,&active,&published,&attempts);if(rc!=COG_OK) return rc;
    cli_host_bytes_t *body=&lane->provisional_reply;
    if(body->capacity<44U) return COG_ERROR_LIMIT;
    size_t required;
    rc=cog_cli_display_snapshot_encode(&view,body->data+44U,body->capacity-44U,&required);
    if(rc!=COG_OK) return rc;
    cli_wire_writer_t w={body->data,0U,true};cli_wire_put(&w,serial,8U);
    cli_wire_put(&w,slot->owner.epoch,8U);cli_wire_put(&w,slot->owner.instance_id,8U);
    cli_wire_put(&w,active,8U);cli_wire_put(&w,published,8U);cli_wire_put(&w,attempts,4U);
    lane->provisional_prefix=serial;
    return queue_control(worker,slot,COG_CLI_CONTROL_EXECUTOR_VIEW,COG_CLI_LANE_VIEW,body->data,required+44U);
}
static cog_result_t view_commit(cli_executor_slot_t *slot,const cog_cli_control_message_t *message)
{
    if(message->lane!=COG_CLI_LANE_VIEW || message->payload_len!=40U || message->reply_to==0U || slot->agent!=0U)
        return COG_ERROR_PROTOCOL;
    cli_control_lane_t *lane=&slot->outbound[COG_CLI_LANE_VIEW-1U];
    cli_wire_reader_t r={message->payload,message->payload_len,0U,true};uint64_t serial=cli_wire_get(&r,8U);
    cog_store_owner_t owner={cli_wire_get(&r,8U),cli_wire_get(&r,8U)};
    uint64_t generation=cli_wire_get(&r,8U),sequence=cli_wire_get(&r,8U);
    if(serial==0U || owner.epoch==0U || owner.instance_id==0U || generation==0U || sequence==0U)
        return COG_ERROR_PROTOCOL;
    if(message->reply_to<=lane->retired_through) return COG_ERROR_AGAIN;
    if(slot->app==NULL || !lane->pending_valid || message->reply_to!=lane->pending_wire) return COG_ERROR_INVALID_STATE;
    if(!same_owner(owner,slot->owner) || serial!=lane->provisional_prefix) return COG_ERROR_INVALID_STATE;
    cog_result_t rc=cog_cli_execution_commit_view(slot->app,serial,generation,sequence);
    if(rc==COG_OK) { lane->retired_through=lane->pending_wire;lane->pending_valid=false; }
    return rc;
}
static cog_result_t executor_command(cog_cli_worker_host_t *worker,cli_executor_slot_t *slot,
    const cog_cli_control_message_t *message,size_t original_length)
{
    if((message->lane!=COG_CLI_LANE_COMMAND && message->lane!=COG_CLI_LANE_CANCEL) ||
       message->payload_len<40U || message->reply_to!=0U || slot->app==NULL) return COG_ERROR_PROTOCOL;
    cli_control_lane_t *in=&slot->inbound[(size_t)message->lane-1U];
    cli_control_lane_t *out=&slot->outbound[(size_t)message->lane-1U];
    if(original_length>in->final_original.capacity || in->final_reply.capacity<256U ||
       out->next_wire==0U || out->next_wire>UINT64_MAX-8U) return COG_ERROR_LIMIT;
    cli_wire_reader_t r={message->payload,message->payload_len,0U,true};
    uint64_t lease=cli_wire_get(&r,8U),wire=cli_wire_get(&r,8U);
    cog_store_owner_t owner={cli_wire_get(&r,8U),cli_wire_get(&r,8U)};
    uint64_t serial=cli_wire_get(&r,8U);
    if(lease==0U || wire==0U) return COG_ERROR_PROTOCOL;
    cog_cli_command_t command;size_t required;
    /* Input already lives in decode_secret; disjoint constructor receive arena
       becomes temporary command string storage only after original was copied. */
    if(original_length>in->pending.capacity) return COG_ERROR_LIMIT;
    memcpy(in->pending.data,worker->receive_secret,original_length);in->pending.length=original_length;
    cog_result_t rc=cog_cli_command_decode(r.bytes+r.used,r.length-r.used,&command,
        worker->receive_secret,worker->receive_capacity,&required);
    if(rc!=COG_OK) return rc;
    if((command.kind==COG_CLI_COMMAND_CANCEL)!=(message->lane==COG_CLI_LANE_CANCEL) ||
       (slot->agent!=0U && command.kind!=COG_CLI_COMMAND_CANCEL)) return COG_ERROR_PROTOCOL;
    cog_result_t outcome=cog_cli_execution_apply_command(slot->app,&command,owner,serial);
    if(outcome==COG_ERROR_AGAIN || outcome==COG_ERROR_OVERFLOW) {
        memcpy(worker->receive_secret,in->pending.data,original_length);return COG_ERROR_AGAIN;
    }
    if(command.kind==COG_CLI_COMMAND_CANCEL && outcome==COG_OK) slot->cancel_latched=true;
    unsigned char body[5];cli_wire_writer_t writer={body,0U,true};
    cli_wire_put(&writer,(uint32_t)outcome,4U);cli_wire_put(&writer,2U,1U);
    memcpy(worker->receive_secret,in->pending.data,original_length);
    return lane_reply(worker,slot,message,COG_CLI_CONTROL_EXECUTOR_COMMAND_RESULT,body,sizeof(body),original_length);
}
static cog_result_t publish_policy(cog_cli_worker_host_t *worker,cli_executor_slot_t *slot)
{
    if(slot->agent!=0U || slot->app==NULL || !slot->started) return COG_ERROR_AGAIN;
    cli_control_lane_t *lane=&slot->outbound[COG_CLI_LANE_POLICY-1U];
    if(lane->pending_valid) return COG_ERROR_AGAIN;
    cog_cli_policy_view_t view;uint64_t serial;
    cog_result_t rc=cog_cli_execution_copy_policy(slot->app,&serial,&view);
    cli_host_bytes_t *body=&lane->provisional_reply;
    if(rc==COG_ERROR_AGAIN) {
        if(body->length==0U || body->data[0]!=1U) return COG_ERROR_AGAIN;
        body->data[0]=2U;
    } else {
        if(rc!=COG_OK) return rc;
        if(serial==lane->provisional_prefix) return COG_ERROR_AGAIN;
        size_t length=strlen(view.tool_name);
        if(length>255U || body->capacity<39U+length) return COG_ERROR_LIMIT;
        cli_wire_writer_t w={body->data,0U,true};cli_wire_put(&w,1U,1U);cli_wire_put(&w,serial,8U);
        cli_wire_put(&w,view.assistant_node,8U);cli_wire_put(&w,view.call_ordinal,2U);
        cli_wire_put(&w,view.configuration_generation,8U);cli_wire_put(&w,view.deadline_ms,8U);
        cli_wire_string(&w,view.tool_name,length);body->length=w.used;lane->provisional_prefix=serial;
    }
    return queue_control(worker,slot,COG_CLI_CONTROL_POLICY_STATE,COG_CLI_LANE_POLICY,body->data,body->length);
}
static cog_result_t policy_result(cli_executor_slot_t *slot,const cog_cli_control_message_t *message)
{
    if(message->lane!=COG_CLI_LANE_POLICY || message->payload_len!=21U || message->reply_to==0U)
        return COG_ERROR_PROTOCOL;
    cli_control_lane_t *lane=&slot->outbound[COG_CLI_LANE_POLICY-1U];
    cli_wire_reader_t r={message->payload,message->payload_len,0U,true};
    cog_result_t result=cli_wire_signed(cli_wire_get(&r,4U));uint8_t state=(uint8_t)cli_wire_get(&r,1U);
    if(!cli_wire_result(result) || state!=2U || cli_wire_get(&r,8U)!=0U || cli_wire_get(&r,8U)!=0U)
        return COG_ERROR_PROTOCOL;
    if(message->reply_to<=lane->retired_through) return COG_ERROR_AGAIN;
    if(!lane->pending_valid || message->reply_to!=lane->pending_wire) return COG_ERROR_INVALID_STATE;
    if(result!=COG_OK) return result;
    lane->retired_through=lane->pending_wire;lane->pending_valid=false;return COG_OK;
}
static cog_result_t delegation_reply(cog_cli_worker_host_t *worker,cli_executor_slot_t *slot,
    const cog_cli_control_message_t *message,size_t length)
{
    if(slot->app==NULL || slot->app->headless->delegation==NULL) return COG_ERROR_INVALID_STATE;
    cli_delegation_handle_t *handle=slot->app->headless->delegation->prepared;
    if(handle==NULL || handle->request_wire!=message->reply_to || handle->pending.length==0U)
        return COG_ERROR_INVALID_STATE;
    if((handle->operation==CLI_DELEGATE && message->lane!=COG_CLI_LANE_DELEGATE) ||
       (handle->operation!=CLI_DELEGATE && message->lane!=COG_CLI_LANE_MAILBOX)) return COG_ERROR_PROTOCOL;
    if(length>handle->reply.capacity) return COG_ERROR_LIMIT;
    if(handle->reply.length!=0U) return length==handle->reply.length &&
        memcmp(worker->receive_secret,handle->reply.data,length)==0?COG_OK:COG_ERROR_AGAIN;
    memcpy(handle->reply.data,worker->receive_secret,length);handle->reply.length=length;return COG_OK;
}
static cog_result_t command_ack(cog_cli_worker_host_t *worker,cli_executor_slot_t *slot,
    const cog_cli_control_message_t *message,size_t length)
{
    if(message->payload_len!=0U || message->reply_to==0U ||
       (message->lane!=COG_CLI_LANE_COMMAND && message->lane!=COG_CLI_LANE_CANCEL)) return COG_ERROR_PROTOCOL;
    cli_control_lane_t *lane=&slot->inbound[(size_t)message->lane-1U];
    cli_control_lane_t *out=&slot->outbound[(size_t)message->lane-1U];
    if(message->reply_to<=out->retired_through) return COG_ERROR_AGAIN;
    if(!lane->final_valid || message->reply_to!=lane->final_reply_wire) return COG_ERROR_INVALID_STATE;
    unsigned char body[21]={0};body[4]=2U;uint64_t consumed=lane->final_reply_wire;
    cog_result_t rc=lane_reply(worker,slot,message,COG_CLI_CONTROL_COMMAND_RESULT,body,sizeof(body),length);
    if(rc==COG_OK) out->retired_through=consumed;
    return rc;
}
static cog_result_t dispatch_cancel(cog_cli_worker_host_t *worker,cli_executor_slot_t *slot,
    const cog_cli_control_message_t *message,size_t length)
{
    cli_control_lane_t *lane=&slot->inbound[COG_CLI_LANE_CANCEL-1U];
    if(length>lane->final_original.capacity) return COG_ERROR_LIMIT;
    cog_result_t rc=child_cancel(worker,slot,message);
    if(rc==COG_OK) {
        if(lane->final_valid) lane->retired_through=lane->final_answered_wire;
        memcpy(lane->final_original.data,worker->receive_secret,length);
        lane->final_original.length=length;lane->final_answered_wire=message->wire_id;lane->final_valid=true;
    }
    return rc;
}
static cog_result_t dispatch_view(cog_cli_worker_host_t *worker,cli_executor_slot_t *slot,
    const cog_cli_control_message_t *message,size_t length)
{ (void)worker;(void)length;return view_commit(slot,message); }
static cog_result_t dispatch_result(cog_cli_worker_host_t *worker,cli_executor_slot_t *slot,
    const cog_cli_control_message_t *message,size_t length)
{
    if(message->lane==COG_CLI_LANE_POLICY) return policy_result(slot,message);
    return delegation_reply(worker,slot,message,length);
}
typedef cog_result_t (*worker_handler_t)(cog_cli_worker_host_t *,cli_executor_slot_t *,
    const cog_cli_control_message_t *,size_t);
static const struct { cog_cli_control_kind_t kind;worker_handler_t handler; } worker_dispatch[]={
    {COG_CLI_CONTROL_CHILD_START,child_start},{COG_CLI_CONTROL_CHILD_CANCEL,dispatch_cancel},
    {COG_CLI_CONTROL_UPDATE_RECEIPT,update_receipt},{COG_CLI_CONTROL_VIEW_COMMIT,dispatch_view},
    {COG_CLI_CONTROL_EXECUTOR_COMMAND,executor_command},{COG_CLI_CONTROL_HANDOFF,handoff_receive},
    {COG_CLI_CONTROL_COMMAND_ACK,command_ack},{COG_CLI_CONTROL_COMMAND_RESULT,dispatch_result},
    {COG_CLI_CONTROL_DELEGATE_RESERVATION,delegation_reply},{COG_CLI_CONTROL_DELEGATE_RESULT,delegation_reply},
    {COG_CLI_CONTROL_MESSAGE_RESULT,delegation_reply},{COG_CLI_CONTROL_MAILBOX_RESULT,delegation_reply}
};
static cog_result_t application_receive(cog_cli_worker_host_t *worker,size_t length)
{
    cog_cli_control_message_t message;size_t required;
    cog_result_t rc=cog_cli_control_decode(worker->receive_secret,length,&message,
        worker->decode_secret,worker->decode_capacity,&required);
    if(rc!=COG_OK) return rc;
    cog_peer_id_t peer=0U;rc=cog_ipc_endpoint_get_peer_id(worker->parent,&peer);if(rc!=COG_OK) return rc;
    if(!same_worker(message.worker,worker->fixed.worker) || message.owner_peer!=peer ||
       message.sender_incarnation!=message.runtime_incarnation) return COG_ERROR_DENIED;
    if(worker->runtime_incarnation==0U) {
        if(message.kind!=COG_CLI_CONTROL_BOOTSTRAP || message.agent_id!=0U ||
           message.payload_len==0U || ((const unsigned char *)message.payload)[0]!=1U) return COG_ERROR_DENIED;
        worker->runtime_incarnation=message.runtime_incarnation;
    }
    if(message.runtime_incarnation!=worker->runtime_incarnation) return COG_ERROR_INVALID_STATE;
    if(message.kind==COG_CLI_CONTROL_BOOTSTRAP) return bootstrap(worker,&message,length);
    if(!worker->startup_ready) return COG_ERROR_INVALID_STATE;
    cli_executor_slot_t *slot=find_attempt(worker,&message);
    if(slot==NULL && message.kind==COG_CLI_CONTROL_UPDATE_RECEIPT &&
       message.agent_id!=0U && message.agent_id<=worker->opened_agent_high_watermark) {
        /* Physical child reuse retired the whole attempt. A delayed valid
           disposition cannot apply to another occupant or poison its binding. */
        for(size_t i=0U;i<worker->executor_capacity;++i)
            if(worker->executors[i].phase!=CLI_SLOT_FREE && worker->executors[i].agent==message.agent_id)
                return COG_ERROR_INVALID_STATE; /* Live conflicting identity. */
        cog_cli_control_receipt_t receipt;
        rc=cog_cli_control_receipt_decode(message.payload,message.payload_len,&receipt);
        if(rc!=COG_OK) return rc;
        if(message.lane!=COG_CLI_LANE_UPDATE || message.reply_to==0U ||
           receipt.parent_runtime_incarnation!=message.runtime_incarnation || receipt.agent_id!=message.agent_id ||
           receipt.execution_epoch!=message.execution_epoch || receipt.owner_peer!=message.owner_peer ||
           receipt.wire_id!=message.reply_to) return COG_ERROR_PROTOCOL;
        return COG_ERROR_AGAIN;
    }
    if(slot==NULL && message.kind==COG_CLI_CONTROL_CHILD_CANCEL) {
        rc=open_attempt(worker,&message,&slot);if(rc!=COG_OK) return rc;
    }
    if(slot==NULL) return COG_ERROR_INVALID_STATE;
    bool duplicate=false;
    if(message.reply_to==0U || message.kind==COG_CLI_CONTROL_COMMAND_ACK) {
        rc=lane_duplicate(&slot->inbound[(size_t)message.lane-1U],message.wire_id,
            worker->receive_secret,length,&duplicate);
        if(rc!=COG_OK || duplicate) return rc;
    }
    for(size_t i=0U;i<sizeof(worker_dispatch)/sizeof(worker_dispatch[0]);++i)
        if(worker_dispatch[i].kind==message.kind) return worker_dispatch[i].handler(worker,slot,&message,length);
    return COG_ERROR_PROTOCOL;
}
/* Reuse the consumed UPDATE arena only after factual child cleanup. The
   retained receipt remains separate until endpoint TX owns this exact proof. */
static cog_result_t confirm_terminal(cog_cli_worker_host_t *worker,cli_executor_slot_t *slot)
{
    cli_control_lane_t *lane=&slot->outbound[COG_CLI_LANE_UPDATE-1U];
    if(!slot->terminal_acked || !slot->local_done || slot->app!=NULL || slot->net!=NULL ||
       slot->observer!=NULL || slot->pending_profile!=NULL || !lane->final_valid ||
       lane->final_reply_wire==0U || !terminal_status(slot->receipt.accepted_terminal_status) ||
       (slot->receipt.disposition!=COG_CLI_CONTROL_RECEIPT_ACCEPTED &&
        slot->receipt.disposition!=COG_CLI_CONTROL_RECEIPT_TERMINAL_FIXED)) return COG_ERROR_INVALID_STATE;
    if(!lane->pending_valid) {
        if(lane->next_wire==0U) return COG_ERROR_LIMIT;
        uint64_t wire=lane->next_wire;
        cog_cli_control_message_t message={COG_CLI_CONTROL_COMMAND_ACK,COG_CLI_LANE_UPDATE,
            worker->runtime_incarnation,worker->fixed.worker.incarnation,wire,lane->final_reply_wire,
            worker->fixed.worker,slot->owner_peer,slot->agent,slot->attempt,slot->session,NULL,0U};
        cog_result_t rc=cog_cli_control_encode(&message,lane->pending.data,lane->pending.capacity,&lane->pending.length);
        if(rc!=COG_OK) return rc;
        lane->next_wire=wire==UINT64_MAX?0U:wire+1U;
        lane->pending_wire=wire;lane->pending_valid=true;
    }
    return envelope_send(worker,&lane->pending);
}
static void retire_attempt(cli_executor_slot_t *slot)
{
    cli_host_bytes_t task=slot->task,status=slot->status,result=slot->result,update=slot->update;
    cli_control_lane_t *in=slot->inbound,*out=slot->outbound;
    cli_secret_exchange_t secret=slot->profile;
    for(size_t i=0U;i<CLI_HOST_LANES;++i) for(size_t direction=0U;direction<2U;++direction) {
        cli_control_lane_t *lane=direction==0U?&in[i]:&out[i];
        cli_host_bytes_t a=lane->pending,b=lane->provisional_reply,c=lane->final_original,d=lane->final_reply;
        worker_wipe(a.data,a.capacity);worker_wipe(b.data,b.capacity);worker_wipe(c.data,c.capacity);worker_wipe(d.data,d.capacity);
        uint64_t next_wire=lane->next_wire;
        a.length=0U;b.length=0U;c.length=0U;d.length=0U;memset(lane,0,sizeof(*lane));
        lane->pending=a;lane->provisional_reply=b;lane->final_original=c;lane->final_reply=d;lane->next_wire=next_wire;
    }
    worker_wipe(task.data,task.capacity);worker_wipe(status.data,status.capacity);
    worker_wipe(result.data,result.capacity);worker_wipe(update.data,update.capacity);
    worker_wipe(secret.body,secret.body_capacity);worker_wipe(secret.frame,secret.frame_capacity);
    worker_wipe(secret.scratch,secret.scratch_capacity);
    task.length=0U;status.length=0U;result.length=0U;update.length=0U;
    unsigned char *body=secret.body,*frame=secret.frame,*scratch=secret.scratch;
    size_t bc=secret.body_capacity,fc=secret.frame_capacity,sc=secret.scratch_capacity;
    memset(slot,0,sizeof(*slot));slot->task=task;slot->status=status;slot->result=result;slot->update=update;
    slot->inbound=in;slot->outbound=out;slot->profile.body=body;slot->profile.frame=frame;slot->profile.scratch=scratch;
    slot->profile.body_capacity=bc;slot->profile.frame_capacity=fc;slot->profile.scratch_capacity=sc;
}
static cog_result_t send_profile_ack(cog_cli_worker_host_t *worker,cli_secret_exchange_t *exchange)
{
    if(!exchange->pending) return COG_ERROR_AGAIN;
    cog_ipc_message_t frame={exchange->body,exchange->body_length,exchange->body_capacity};
    cog_result_t rc=cog_ipc_endpoint_send(worker->parent,&frame);
    if(rc==COG_OK) { exchange->pending=false;exchange->acknowledged=true; }
    return rc;
}
static void send_native_turn(cog_cli_worker_host_t *worker,cli_executor_slot_t *slot,
    size_t lane_index,bool *progress)
{
    (void)slot;(void)lane_index;cog_result_t rc;
        for(size_t i=0U;i<2U;++i) if(worker->native_pending[i]) {
            rc=envelope_send(worker,&worker->native_control[i]);
            if(rc==COG_OK) {
                worker->native_pending[i]=false;*progress=true;
                if(i==1U) worker->next_heartbeat_ms=worker->now_ms>UINT64_MAX-100U?UINT64_MAX:worker->now_ms+100U;
            } else (void)retain_error(worker,rc);
        }
}
static void send_application_turn(cog_cli_worker_host_t *worker,cli_executor_slot_t *slot,
    size_t lane_index,bool *progress)
{
    cog_result_t rc;
        rc=send_profile_ack(worker,&worker->startup_profile);if(rc==COG_OK) *progress=true;
        else (void)retain_error(worker,rc);
        rc=send_profile_ack(worker,&slot->profile);if(rc==COG_OK) *progress=true;
        else (void)retain_error(worker,rc);
        cli_control_lane_t *in=&slot->inbound[lane_index],*out=&slot->outbound[lane_index];
        if(in->pending_valid && in->final_reply.length!=0U) {
            rc=envelope_send(worker,&in->final_reply);
            if(rc==COG_OK) { in->pending_valid=false;*progress=true; } else (void)retain_error(worker,rc);
        }
        if(out->pending_valid && out->pending.length!=0U &&
           !(lane_index==COG_CLI_LANE_UPDATE-1U && slot->terminal_acked)) {
            rc=envelope_send(worker,&out->pending);
            if(rc==COG_OK) {
                *progress=true;
                if(lane_index==COG_CLI_LANE_CANCEL-1U) out->pending_valid=false;
            } else (void)retain_error(worker,rc);
        }
}
static void send_transport_turn(cog_cli_worker_host_t *worker,cli_executor_slot_t *slot,
    size_t lane_index,size_t phase,bool *progress)
{
    typedef void (*send_fn)(cog_cli_worker_host_t *,cli_executor_slot_t *,size_t,bool *);
    /* Three phases prevent priority from locking to IPC's alternating RX/TX
       units: every selected lane sees native-first and two application-first
       opportunities. Both classes progress even with saturated copied TX. */
    static const send_fn order[3][2]={
        {send_native_turn,send_application_turn},
        {send_application_turn,send_native_turn},
        {send_application_turn,send_native_turn}};
    for(size_t i=0U;i<2U;++i) order[phase][i](worker,slot,lane_index,progress);
}
static cog_result_t parent_step(cog_cli_worker_host_t *worker,bool *progress,uint64_t *deadline)
{
    cog_result_t serviced=cog_ipc_internal_service_one(worker->parent,worker->now_ms,deadline);
    if(serviced==COG_OK) *progress=true;
    if(!worker->authenticated) {
        cog_peer_id_t peer;cog_result_t rc=cog_ipc_endpoint_get_peer_id(worker->parent,&peer);
        if(rc==COG_OK) rc=cog_ipc_endpoint_peer_info(worker->parent,peer,&worker->parent_proof);
        if(rc==COG_OK) {
            if(worker->parent_proof.process_id!=worker->fixed.parent_pid ||
               worker->parent_proof.user_id!=(uint64_t)geteuid()) return COG_ERROR_DENIED;
            worker->authenticated=true;
            rc=native_frame(worker,0U,COG_WORKER_HELLO,0U,0U,COG_OK);
            if(rc!=COG_OK) return rc;
        } else if(rc!=COG_ERROR_AGAIN && rc!=COG_ERROR_INVALID_STATE) return rc;
        if(!worker->authenticated) return serviced;
    }
    /* No second transport unit before the immutable native proof above. */
    if(serviced!=COG_OK && serviced!=COG_ERROR_AGAIN && serviced!=COG_ERROR_IO) return serviced;
    if(serviced==COG_ERROR_IO) (void)retain_error(worker,serviced);
    cog_ipc_message_t input={worker->receive_secret,0U,worker->receive_capacity};
    cog_result_t rc=cog_ipc_endpoint_recv(worker->parent,&input);
    if(rc==COG_OK) {
        *progress=true;
        if(input.len>=3U && memcmp(input.data,"CGW",3U)==0) rc=native_receive(worker,input.data,input.len);
        else if(input.len>=3U && memcmp(input.data,"CGL",3U)==0) rc=application_receive(worker,input.len);
        else rc=COG_ERROR_PROTOCOL;
        worker_wipe(worker->receive_secret,worker->receive_capacity);
        worker_wipe(worker->decode_secret,worker->decode_capacity);
        if(rc!=COG_OK && rc!=COG_ERROR_AGAIN) return rc;
    } else if(rc!=COG_ERROR_AGAIN && rc!=COG_ERROR_INVALID_STATE) return rc;
    return serviced;
}
cog_result_t cog_cli_worker_host_step(cog_cli_worker_host_t *worker,cog_cli_host_runtime_step_t *info)
{
    if(worker==NULL || info==NULL) return COG_ERROR_NULL_PTR;
    if(worker->in_step || worker->done) return COG_ERROR_INVALID_STATE;
    uint64_t now;bool first=!worker->stepped;worker->stepped=true;worker->in_step=true;
    cog_result_t rc=cog_cli_clock(&now);
    if(rc==COG_OK && now<worker->now_ms) rc=COG_ERROR_IO;
    if(rc==COG_OK && first && now>UINT64_MAX-5000U) rc=COG_ERROR_LIMIT;
    if(rc==COG_OK) { worker->now_ms=now;if(first) worker->startup_deadline_ms=now+5000U; }
    else (void)retain_error(worker,rc);
    cog_cli_host_runtime_step_t step={false,0U,UINT64_MAX,10U};bool progress=false;
    if(worker->fixed.signal_fd>=0) {
        struct pollfd signal={worker->fixed.signal_fd,POLLIN,0};int polled=poll(&signal,1U,0);
        if(polled<0 && errno!=EINTR) (void)retain_error(worker,COG_ERROR_IO);
        else if(polled>0) {
            if((signal.revents&(POLLERR|POLLHUP|POLLNVAL))!=0) (void)retain_error(worker,COG_ERROR_IO);
            else if((signal.revents&POLLIN)!=0) (void)cog_cli_worker_host_request_shutdown(worker);
        }
    }
    if(!worker->startup_ready && !worker->shutdown && worker->now_ms>=worker->startup_deadline_ms)
        (void)retain_error(worker,COG_ERROR_TIMEOUT);
    uint64_t parent_deadline=UINT64_MAX;
    rc=parent_step(worker,&progress,&parent_deadline);(void)retain_error(worker,rc);
    if(parent_deadline<step.next_deadline_ms) step.next_deadline_ms=parent_deadline;
    size_t turn=worker->round_robin++;
    if(worker->round_robin==worker->executor_capacity*CLI_HOST_LANES*3U) worker->round_robin=0U;
    size_t logical=turn/3U;
    cli_executor_slot_t *slot=&worker->executors[logical%worker->executor_capacity];
    size_t lane_index=(logical/worker->executor_capacity)%CLI_HOST_LANES;
    if(slot->phase!=CLI_SLOT_FREE) {
        rc=executor_step(worker,slot,&step);if(rc==COG_OK) progress=true;
        if(rc!=COG_OK && rc!=COG_ERROR_AGAIN) (void)retain_error(worker,rc);
        if(!worker->shutdown) {
            rc=queue_update(worker,slot);if(rc!=COG_OK && rc!=COG_ERROR_AGAIN) (void)retain_error(worker,rc);
            rc=publish_view(worker,slot);if(rc!=COG_OK && rc!=COG_ERROR_AGAIN) (void)retain_error(worker,rc);
            rc=publish_policy(worker,slot);if(rc!=COG_OK && rc!=COG_ERROR_AGAIN) (void)retain_error(worker,rc);
        }
        if(slot->local_done && slot->agent!=0U && slot->terminal_acked && slot->phase!=CLI_SLOT_DISPOSING) {
            slot->phase=CLI_SLOT_DISPOSING;
            rc=dispose_execution(slot);
            if(rc==COG_OK) {
                slot->phase=CLI_SLOT_RETAINED;
                rc=confirm_terminal(worker,slot);
            }
            if(rc==COG_OK) { retire_attempt(slot);progress=true; }
            else if(rc!=COG_ERROR_AGAIN) (void)retain_error(worker,rc);
        }
    }
    if(worker->authenticated) {
        uint32_t active=0U;
        for(size_t i=2U;i<worker->executor_capacity;++i)
            if(worker->executors[i].started && !worker->executors[i].local_done) ++active;
        if(!worker->shutdown && worker->now_ms>=worker->next_heartbeat_ms && !worker->native_pending[1])
            (void)retain_error(worker,native_frame(worker,1U,COG_WORKER_HEARTBEAT,active,0U,COG_OK));
        send_transport_turn(worker,slot,lane_index,turn%3U,&progress);
    }
    bool drained=true;
    for(size_t i=0U;i<worker->executor_capacity;++i)
        if(worker->executors[i].app!=NULL && !worker->executors[i].local_done) drained=false;
    if(worker->shutdown && drained) {
        /* Done is actual local drain, not a claim the parent received this ACK.
           The parent still proves native termination through its owned PID. */
        if(worker->authenticated && worker->native_control[2].length!=0U) {
            rc=envelope_send(worker,&worker->native_control[2]);
            if(rc==COG_OK) {
                uint64_t ignored;rc=cog_ipc_internal_service_one(worker->parent,worker->now_ms,&ignored);
            }
            if(rc!=COG_OK && rc!=COG_ERROR_AGAIN) (void)retain_error(worker,rc);
            if(rc==COG_ERROR_AGAIN && worker->now_ms<worker->shutdown_deadline_ms) drained=false;
        }
        if(drained) worker->done=true;
    }
    if(worker->shutdown && worker->shutdown_deadline_ms<step.next_deadline_ms) step.next_deadline_ms=worker->shutdown_deadline_ms;
    else if(!worker->startup_ready && worker->startup_deadline_ms<step.next_deadline_ms) step.next_deadline_ms=worker->startup_deadline_ms;
    if(!worker->shutdown && worker->next_heartbeat_ms<step.next_deadline_ms) step.next_deadline_ms=worker->next_heartbeat_ms;
    if(progress || step.next_deadline_ms<=worker->now_ms) step.max_wait_ms=0U;
    step.done=worker->done;*info=step;worker->in_step=false;
    return worker->primary!=COG_OK?worker->primary:progress?COG_OK:COG_ERROR_AGAIN;
}

/* Count and emit JSON into the constructor-owned output arena. */
static void bridge_literal(cli_wire_writer_t *w,const char *s) { cli_wire_bytes(w,s,strlen(s)); }
static void bridge_number(cli_wire_writer_t *w,uint64_t value)
{
    char b[20];size_t n=0U;do { b[n++]=(char)('0'+value%10U);value/=10U; } while(value!=0U);
    while(n!=0U) cli_wire_bytes(w,&b[--n],1U);
}
static void bridge_signed(cli_wire_writer_t *w,cog_result_t value)
{
    if(value<0) { bridge_literal(w,"-");bridge_number(w,(uint64_t)(-(int64_t)value)); }
    else bridge_number(w,(uint64_t)value);
}
static void bridge_string(cli_wire_writer_t *w,const char *s,size_t n)
{
    static const char hex[]="0123456789abcdef";
    bridge_literal(w,"\"");
    for(size_t i=0U;i<n;++i) {
        unsigned char c=(unsigned char)s[i];
        if(c=='"' || c=='\\') { bridge_literal(w,"\\");cli_wire_bytes(w,&c,1U); }
        else if(c<32U) { char b[6]={'\\','u','0','0',hex[c>>4U],hex[c&15U]};cli_wire_bytes(w,b,6U); }
        else cli_wire_bytes(w,&c,1U);
    }
    bridge_literal(w,"\"");
}
static cog_result_t bridge_output(cli_delegation_handle_t *h,const cog_cli_control_message_t *m,
    cli_wire_writer_t *w,cog_result_t *outcome,bool *effects,uint64_t *group)
{
    cli_wire_reader_t r={m->payload,m->payload_len,0U,true};
    *effects=false;*group=0U;
    if(m->kind==COG_CLI_CONTROL_DELEGATE_RESULT) {
        *group=cli_wire_get(&r,8U);*outcome=cli_wire_signed(cli_wire_get(&r,4U));
        size_t count=(size_t)cli_wire_get(&r,2U);
        if(*group==0U || !cli_wire_result(*outcome) || count!=h->request.count) return COG_ERROR_PROTOCOL;
        bridge_literal(w,"{\"group\":\"");bridge_number(w,*group);bridge_literal(w,"\",\"submitted\":");
        bridge_number(w,count);bridge_literal(w,",\"members\":[");
        uint64_t previous=0U;
        for(size_t i=0U;i<count;++i) {
            if(i!=0U) bridge_literal(w,",");
            uint64_t present=cli_wire_get(&r,1U);
            if(present==0U) {
                cog_result_t failure=cli_wire_signed(cli_wire_get(&r,4U));
                if(!cli_wire_result(failure) || failure==COG_OK || *outcome==COG_OK) return COG_ERROR_PROTOCOL;
                bridge_literal(w,"{\"outcome\":");bridge_signed(w,failure);bridge_literal(w,"}");
            } else if(present==1U) {
                uint64_t agent=cli_wire_get(&r,8U),attempt=cli_wire_get(&r,8U);
                cog_agent_status_t status=(cog_agent_status_t)cli_wire_get(&r,1U);
                cog_result_t result=cli_wire_signed(cli_wire_get(&r,4U));size_t sn,rn;
                const char *session=cli_wire_read_string(&r,&sn),*text=cli_wire_read_string(&r,&rn);
                uint64_t uncertain=cli_wire_get(&r,1U);
                if(!r.valid || agent<=previous || attempt==0U || !terminal_status(status) ||
                   !cli_wire_result(result) || !cli_wire_session(session,sn) || uncertain>1U ||
                   (*outcome==COG_OK && (status!=COG_AGENT_STATUS_COMPLETED || result!=COG_OK || uncertain!=0U)))
                    return COG_ERROR_PROTOCOL;
                previous=agent;*effects=*effects || uncertain!=0U;
                bridge_literal(w,"{\"agent_id\":\"");bridge_number(w,agent);bridge_literal(w,"\",\"attempt\":\"");
                bridge_number(w,attempt);bridge_literal(w,"\",\"status\":");bridge_number(w,(uint64_t)status);
                bridge_literal(w,",\"outcome\":");bridge_signed(w,result);bridge_literal(w,",\"session\":");
                bridge_string(w,session,sn);bridge_literal(w,",\"result\":");bridge_string(w,text,rn);
                bridge_literal(w,",\"side_effects_possible\":");bridge_literal(w,uncertain!=0U?"true}":"false}");
            } else return COG_ERROR_PROTOCOL;
        }
        bridge_literal(w,"]}");
        if(*effects && *outcome!=COG_ERROR_INDETERMINATE) return COG_ERROR_PROTOCOL;
    } else if(m->kind==COG_CLI_CONTROL_MESSAGE_RESULT) {
        *outcome=cli_wire_signed(cli_wire_get(&r,4U));uint64_t state=cli_wire_get(&r,1U);
        if(!cli_wire_result(*outcome) || (state!=1U && state!=2U)) return COG_ERROR_PROTOCOL;
        if(!r.valid || r.used!=r.length) return COG_ERROR_PROTOCOL;
        if(state==1U) return *outcome==COG_ERROR_AGAIN?COG_ERROR_AGAIN:COG_ERROR_PROTOCOL;
        bridge_literal(w,*outcome==COG_OK?"{\"sent\":true}":"{\"sent\":false}");
    } else if(m->kind==COG_CLI_CONTROL_MAILBOX_RESULT) {
        *outcome=cli_wire_signed(cli_wire_get(&r,4U));uint64_t more=cli_wire_get(&r,1U);
        size_t count=(size_t)cli_wire_get(&r,2U);
        if(!cli_wire_result(*outcome) || more>1U || count>h->context->mailbox_entries ||
           (*outcome!=COG_OK && (count!=0U || more!=0U))) return COG_ERROR_PROTOCOL;
        bridge_literal(w,"{\"messages\":[");
        for(size_t i=0U;i<count;++i) {
            uint64_t source=cli_wire_get(&r,8U);size_t n;const char *s=cli_wire_read_string(&r,&n);
            if(!r.valid || source==0U) return COG_ERROR_PROTOCOL;
            if(i!=0U) bridge_literal(w,",");
            bridge_literal(w,"{\"agent_id\":\"");bridge_number(w,source);bridge_literal(w,"\",\"message\":");
            bridge_string(w,s,n);bridge_literal(w,"}");
        }
        bridge_literal(w,"],\"more_pending\":");bridge_literal(w,more!=0U?"true}":"false}");
    } else return COG_ERROR_PROTOCOL;
    return r.valid && r.used==r.length && w->valid?COG_OK:COG_ERROR_PROTOCOL;
}
static void bridge_request(cli_delegation_handle_t *h,cog_cli_control_kind_t kind,
    cog_store_checkpoint_t checkpoint,cli_wire_writer_t *w)
{
    if(kind==COG_CLI_CONTROL_CHILD_MESSAGE) {
        cli_wire_put(w,h->destination,8U);cli_wire_string(w,h->message,h->message_length);return;
    }
    if(kind==COG_CLI_CONTROL_MAILBOX_POLL) return;
    cli_wire_put(w,h->request.owner.epoch,8U);cli_wire_put(w,h->request.owner.instance_id,8U);
    if(kind==COG_CLI_CONTROL_DELEGATE_START) cli_wire_put(w,h->reservation,8U);
    cli_wire_put(w,h->request.pinned_configuration,8U);
    if(kind==COG_CLI_CONTROL_DELEGATE_START) cli_wire_put(w,checkpoint.generation,8U);
    cli_wire_put(w,h->request.parent_node,8U);cli_wire_put(w,h->request.ordinal,2U);
    cli_wire_string(w,h->call_id,strlen(h->call_id));cli_wire_put(w,h->deadline_ms,8U);
    cli_wire_put(w,h->request.count,2U);
    for(size_t i=0U;i<h->request.count;++i) cli_wire_string(w,h->tasks[i],h->task_lengths[i]);
}
/** One bounded send or retained response transition; never reads the socket. */
cog_result_t cli_worker_delegation_step(cog_cli_worker_host_t *worker,cli_delegation_handle_t *h)
{
    if(worker==NULL || h==NULL) return COG_ERROR_NULL_PTR;
    if(h->context==NULL || h->context->worker!=worker || !worker->authenticated || !worker->startup_ready)
        return COG_ERROR_INVALID_STATE;
    cli_executor_slot_t *slot=NULL;
    for(size_t i=0U;i<worker->executor_capacity;++i)
        if(worker->executors[i].app==h->context->app) { slot=&worker->executors[i];break; }
    if(slot==NULL || slot->app==NULL || (h->operation==CLI_DELEGATE && slot->agent!=0U) ||
       (h->operation!=CLI_DELEGATE && slot->agent==0U)) return COG_ERROR_INVALID_STATE;
    if(h->cancelled && h->operation==CLI_DELEGATE && h->reservation!=0U) {
        cli_control_lane_t *cancel=&slot->outbound[COG_CLI_LANE_CANCEL-1U];
        if(cancel->provisional_prefix!=h->reservation) {
            if(cancel->pending_valid) return COG_ERROR_AGAIN;
            unsigned char body[8];cli_wire_writer_t w={body,0U,true};cli_wire_put(&w,h->reservation,8U);
            cog_result_t rc=queue_control(worker,slot,COG_CLI_CONTROL_DELEGATE_RESERVATION_RELEASE,
                COG_CLI_LANE_CANCEL,body,sizeof(body));
            if(rc!=COG_OK) return rc;
            cancel->provisional_prefix=h->reservation;
        }
        /* The copied cleanup request outlives destruction of this handle;
           started groups still await their factual terminal result. */
        if(!h->started) return COG_OK;
    }
    if(worker->shutdown) return COG_ERROR_INDETERMINATE;
    bool save=h->done && h->output_cached && !h->result_saved && h->codec.length==16U;
    if(h->done && !save) return COG_OK;
    cog_cli_control_lane_t lane=h->operation==CLI_DELEGATE?COG_CLI_LANE_DELEGATE:COG_CLI_LANE_MAILBOX;
    cog_cli_control_kind_t kind=save?COG_CLI_CONTROL_DELEGATE_RESULT_ACK:
        h->operation==CLI_SEND_MESSAGE?COG_CLI_CONTROL_CHILD_MESSAGE:
        h->operation==CLI_RECEIVE_MESSAGES?COG_CLI_CONTROL_MAILBOX_POLL:
        h->started?COG_CLI_CONTROL_DELEGATE_START:COG_CLI_CONTROL_DELEGATE_RESERVE;
    if(h->reply.length!=0U && !save) {
        cog_cli_control_message_t m;size_t needed;
        cog_result_t rc=cog_cli_control_decode(h->reply.data,h->reply.length,&m,h->codec.data,h->codec.capacity,&needed);
        if(rc!=COG_OK) return rc;
        if(m.reply_to!=h->request_wire) return COG_ERROR_PROTOCOL;
        if(kind==COG_CLI_CONTROL_DELEGATE_RESERVE) {
            if(m.kind!=COG_CLI_CONTROL_DELEGATE_RESERVATION || m.payload_len!=20U) return COG_ERROR_PROTOCOL;
            cli_wire_reader_t r={m.payload,m.payload_len,0U,true};
            cog_result_t outcome=cli_wire_signed(cli_wire_get(&r,4U));
            uint64_t reservation=cli_wire_get(&r,8U),deadline=cli_wire_get(&r,8U);
            if(!cli_wire_result(outcome) || (outcome==COG_OK && (reservation==0U || deadline>h->deadline_ms || deadline==0U)) ||
               (outcome!=COG_OK && reservation!=0U)) return COG_ERROR_PROTOCOL;
            if(outcome!=COG_OK) return outcome;
            h->reservation=reservation;h->deadline_ms=deadline;h->prepared=true;
            h->pending.length=0U;h->reply.length=0U;h->codec.length=0U;
            if(h->cancelled) return cli_worker_delegation_step(worker,h);
            return COG_OK;
        }
        if((kind==COG_CLI_CONTROL_DELEGATE_START && m.kind!=COG_CLI_CONTROL_DELEGATE_RESULT) ||
           (kind==COG_CLI_CONTROL_CHILD_MESSAGE && m.kind!=COG_CLI_CONTROL_MESSAGE_RESULT) ||
           (kind==COG_CLI_CONTROL_MAILBOX_POLL && m.kind!=COG_CLI_CONTROL_MAILBOX_RESULT)) return COG_ERROR_PROTOCOL;
        cli_wire_writer_t count={NULL,0U,true};cog_result_t outcome;bool effects;uint64_t group;
        rc=bridge_output(h,&m,&count,&outcome,&effects,&group);
        if(rc==COG_ERROR_AGAIN) { h->reply.length=0U;return rc; }
        if(rc!=COG_OK) return rc;
        if(count.used>=h->output.capacity || count.used>h->fixed.max_output_bytes) return COG_ERROR_LIMIT;
        cli_wire_writer_t write={h->output.data,0U,true};rc=bridge_output(h,&m,&write,&outcome,&effects,&group);
        if(rc!=COG_OK) return rc;
        h->output.data[write.used]=0U;h->output.length=write.used;h->group=group;h->done=true;h->output_cached=true;h->codec.length=0U;
        h->result=(cog_tool_output_t){outcome,(const char *)h->output.data,write.used,false,effects,0,false};
        h->pending.length=0U;return COG_OK;
    }
    if(h->prepared && !h->started && !save) return COG_OK;
    if(h->pending.length==0U) {
        cog_store_checkpoint_t checkpoint={0};
        if(kind==COG_CLI_CONTROL_DELEGATE_START) {
            cog_store_owner_t owner;cog_result_t rc=cog_store_owner_read(slot->app->store,h->parent_session,&owner,&checkpoint);
            if(rc!=COG_OK) return rc;
            if(!same_owner(owner,h->request.owner) || checkpoint.generation==0U) return COG_ERROR_INVALID_STATE;
        }
        size_t body_length=16U;
        if(!save) {
            cli_wire_writer_t count={NULL,0U,true};bridge_request(h,kind,checkpoint,&count);
            if(!count.valid || count.used>h->codec.capacity) return COG_ERROR_LIMIT;
            cli_wire_writer_t write={h->codec.data,0U,true};bridge_request(h,kind,checkpoint,&write);body_length=write.used;
        }
        uint64_t wire;cog_result_t rc=fresh_wire(&slot->outbound[(size_t)lane-1U],&wire);if(rc!=COG_OK) return rc;
        cog_cli_control_message_t message={kind,lane,worker->runtime_incarnation,worker->fixed.worker.incarnation,
            wire,0U,worker->fixed.worker,slot->owner_peer,slot->agent,slot->attempt,slot->session,h->codec.data,body_length};
        rc=cog_cli_control_encode(&message,h->pending.data,h->pending.capacity,&h->pending.length);
        if(rc!=COG_OK) return rc;
        h->request_wire=wire;
    }
    cog_result_t rc=envelope_send(worker,&h->pending);
    if(rc==COG_OK && save) { h->result_saved=true;h->pending.length=0U;h->reply.length=0U; }
    return rc;
}

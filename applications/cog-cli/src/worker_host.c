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
        return COG_ERROR_INVALID_STATE;
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
        slot=&worker->executors[i];slot->phase=CLI_SLOT_RESERVED;
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
    if(message->lane!=COG_CLI_LANE_PROFILE || message->reply_to!=0U || message->payload_len<17U)
        return COG_ERROR_PROTOCOL;
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
        } else if(purpose==3U) slot=&worker->executors[1];
        else {
            slot=find_attempt(worker,message);
            if(slot==NULL || slot->app==NULL || slot->agent!=0U) return COG_ERROR_INVALID_STATE;
        }
        exchange=&slot->profile;
    }
    if(message->wire_id==exchange->wire) return bootstrap_duplicate(worker,exchange,message,original_length);
    if(message->wire_id<=exchange->retired_through || exchange->pending || exchange->source!=NULL)
        return COG_ERROR_INVALID_STATE;
    const unsigned char *profile=(const unsigned char *)message->payload+5U;
    size_t profile_length=message->payload_len-5U;
    cog_cli_configuration_t *configuration=NULL;cog_cli_bootstrap_probe_t probe={0};
    cog_result_t outcome=slot->cancel_latched?COG_ERROR_ABORTED:
        profile_construct(worker,slot,profile,profile_length,&configuration,&probe);
    if(outcome!=COG_OK && outcome!=COG_ERROR_ABORTED) return outcome;
    cli_wire_reader_t generation={profile,profile_length,4U,true};
    uint64_t number=cli_wire_get(&generation,8U);
    if(!generation.valid || number==0U || (purpose==4U && number!=(uint64_t)attempts+1U)) {
        cog_cli_configuration_free(configuration);return COG_ERROR_PROTOCOL;
    }
    secret_bind(exchange,message);exchange->purpose=purpose;exchange->reload_attempts_used=attempts;
    exchange->generation=number;exchange->probe=probe;exchange->source=configuration;
    if(configuration!=NULL) {
        if(purpose==1U) {
            worker->startup_configuration=configuration;worker->probe=probe;worker->startup_ready=true;
        } else if(purpose==4U) {
            uint64_t published=0U;
            outcome=cog_cli_execution_reload_resolved(slot->app,&configuration,attempts,&published);
            if(outcome!=COG_OK) exchange->source=NULL;
        } else {
            slot->pending_profile=configuration;slot->configuration_generation=number;
            slot->phase=CLI_SLOT_PROFILE;
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
    if(!lane->pending_valid || message->reply_to!=slot->pending_update_wire) return COG_ERROR_INVALID_STATE;
    cog_cli_control_receipt_t receipt;
    cog_result_t rc=cog_cli_control_receipt_decode(message->payload,message->payload_len,&receipt);
    if(rc!=COG_OK) return rc;
    if(receipt.parent_runtime_incarnation!=worker->runtime_incarnation || receipt.agent_id!=slot->agent ||
       receipt.execution_epoch!=slot->attempt || receipt.owner_peer!=slot->owner_peer ||
       receipt.wire_id!=slot->pending_update_wire || receipt.parent_accepted_sequence<slot->accepted_sequence)
        return COG_ERROR_PROTOCOL;
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

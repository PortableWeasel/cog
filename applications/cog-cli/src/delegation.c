/** @file delegation.c @brief Static one-shot delegation and mailbox tools. */
#include "composition_internal.h"
#include "app_internal.h"
#include "codec_internal.h"
#include <stdlib.h>
#include <string.h>

typedef struct delegation_json {
    const unsigned char *input;size_t length,at;
    char *output;size_t capacity,used;bool valid;
} delegation_json_t;
static void json_space(delegation_json_t *p)
{ while(p->at<p->length && (p->input[p->at]==' ' || p->input[p->at]=='\n' || p->input[p->at]=='\r' || p->input[p->at]=='\t')) ++p->at; }
static bool json_token(delegation_json_t *p,unsigned char c)
{
    json_space(p);
    if(p->at==p->length || p->input[p->at]!=c) { p->valid=false;return false; }
    ++p->at;return true;
}
static bool json_emit(delegation_json_t *p,unsigned char c)
{
    if(p->used>=p->capacity) { p->valid=false;return false; }
    p->output[p->used++]=(char)c;return true;
}
static unsigned hex_digit(unsigned char c)
{
    if(c>='0' && c<='9') return (unsigned)(c-'0');
    if(c>='a' && c<='f') return (unsigned)(c-'a')+10U;
    if(c>='A' && c<='F') return (unsigned)(c-'A')+10U;
    return 16U;
}
static uint32_t json_hex(delegation_json_t *p)
{
    uint32_t value=0U;
    for(size_t i=0U;i<4U;++i) {
        if(p->at==p->length) { p->valid=false;return 0U; }
        unsigned digit=hex_digit(p->input[p->at++]);if(digit==16U) { p->valid=false;return 0U; }
        value=value*16U+digit;
    }
    return value;
}
static const char *json_string(delegation_json_t *p,size_t *length)
{
    if(!json_token(p,'"')) return NULL;
    size_t start=p->used;bool closed=false;
    while(p->valid && p->at<p->length) {
        unsigned char c=p->input[p->at++];if(c=='"') { closed=true;break; }
        if(c<32U) { p->valid=false;break; }
        if(c!='\\') { (void)json_emit(p,c);continue; }
        if(p->at==p->length) { p->valid=false;break; }
        c=p->input[p->at++];
        if(c=='"' || c=='\\' || c=='/') (void)json_emit(p,c);
        else if(c=='b') (void)json_emit(p,8U);
        else if(c=='f') (void)json_emit(p,12U);
        else if(c=='n') (void)json_emit(p,10U);
        else if(c=='r') (void)json_emit(p,13U);
        else if(c=='t') (void)json_emit(p,9U);
        else if(c=='u') {
            uint32_t code=json_hex(p);
            if(code>=0xd800U && code<=0xdbffU) {
                if(p->at+2U>p->length || p->input[p->at]!='\\' || p->input[p->at+1U]!='u') { p->valid=false;break; }
                p->at+=2U;uint32_t low=json_hex(p);
                if(low<0xdc00U || low>0xdfffU) { p->valid=false;break; }
                code=0x10000U+(code-0xd800U)*1024U+low-0xdc00U;
            }
            if(code==0U || (code>=0xdc00U && code<=0xdfffU)) { p->valid=false;break; }
            if(code<0x80U) (void)json_emit(p,(unsigned char)code);
            else {
                unsigned width=code<0x800U?2U:code<0x10000U?3U:4U;
                (void)json_emit(p,(unsigned char)((width==2U?0xc0U:width==3U?0xe0U:0xf0U)|(code>>(6U*(width-1U)))));
                for(unsigned i=width-1U;i>0U;--i) (void)json_emit(p,(unsigned char)(0x80U|((code>>(6U*(i-1U)))&0x3fU)));
            }
        } else p->valid=false;
    }
    *length=p->used-start;
    if(!closed || !p->valid || !cli_wire_utf8(p->output+start,*length) || !json_emit(p,0U)) { p->valid=false;return NULL; }
    return p->output+start;
}
static bool key_is(const char *key,size_t length,const char *wanted)
{ return key!=NULL && strlen(wanted)==length && memcmp(key,wanted,length)==0; }
static cog_result_t arguments_parse(cli_delegation_handle_t *h,const char *request,size_t length)
{
    delegation_json_t p={(const unsigned char *)request,length,0U,(char *)h->arguments.data,h->arguments.capacity,0U,true};
    if(!json_token(&p,'{')) return COG_ERROR_INVALID_ARG;
    if(h->operation==CLI_RECEIVE_MESSAGES) {
        if(!json_token(&p,'}')) return COG_ERROR_INVALID_ARG;
    } else if(h->operation==CLI_DELEGATE) {
        size_t n=0U;const char *key=json_string(&p,&n);
        if(!key_is(key,n,"tasks") || !json_token(&p,':') || !json_token(&p,'[')) return COG_ERROR_INVALID_ARG;
        for(;;) {
            if(h->request.count==CLI_HOST_MAX_CHILDREN || !json_token(&p,'{')) return COG_ERROR_INVALID_ARG;
            key=json_string(&p,&n);
            if(!key_is(key,n,"task") || !json_token(&p,':')) return COG_ERROR_INVALID_ARG;
            const char *task=json_string(&p,&n);
            if(task==NULL || n==0U || !json_token(&p,'}')) return COG_ERROR_INVALID_ARG;
            size_t i=h->request.count++;h->tasks[i]=task;h->task_lengths[i]=n;
            json_space(&p);if(p.at<p.length && p.input[p.at]==']') { ++p.at;break; }
            if(!json_token(&p,',')) return COG_ERROR_INVALID_ARG;
        }
        if(!json_token(&p,'}')) return COG_ERROR_INVALID_ARG;
    } else {
        bool have_agent=false,have_message=false;
        for(size_t i=0U;i<2U;++i) {
            size_t n=0U;const char *key=json_string(&p,&n);
            if(key==NULL || !json_token(&p,':')) return COG_ERROR_INVALID_ARG;
            bool agent=key_is(key,n,"agent_id"),message=key_is(key,n,"message");
            if((!agent && !message) || (agent && have_agent) || (message && have_message)) return COG_ERROR_INVALID_ARG;
            const char *value=json_string(&p,&n);if(value==NULL) return COG_ERROR_INVALID_ARG;
            if(agent) {
                if(n==0U || n>20U || value[0]=='0') return COG_ERROR_INVALID_ARG;
                uint64_t id=0U;
                for(size_t j=0U;j<n;++j) {
                    if(value[j]<'0' || value[j]>'9' || id>(UINT64_MAX-(uint64_t)(value[j]-'0'))/10U) return COG_ERROR_INVALID_ARG;
                    id=id*10U+(uint64_t)(value[j]-'0');
                }
                h->destination=id;have_agent=true;
            } else { if(n>4095U) return COG_ERROR_LIMIT;h->message=value;h->message_length=n;have_message=true; }
            if(i==0U && !json_token(&p,',')) return COG_ERROR_INVALID_ARG;
        }
        if(!have_agent || !have_message || !json_token(&p,'}')) return COG_ERROR_INVALID_ARG;
    }
    json_space(&p);h->arguments.length=p.used;
    return p.valid && p.at==p.length?COG_OK:COG_ERROR_INVALID_ARG;
}
static void handle_storage_free(cli_delegation_handle_t *h)
{
    if(h==NULL) return;
    free(h->arguments.data);free(h->output.data);free(h->codec.data);
    free(h->pending.data);free(h->reply.data);free(h);
}
static bool buffer_new(cli_host_bytes_t *b,size_t capacity)
{ b->capacity=capacity;b->data=calloc(capacity,1U);return b->data!=NULL; }
cog_result_t cog_cli_delegation_context_new(cog_cli_delegation_context_t **out,cog_cli_app_t *app,
    cog_cli_host_runtime_t *coordinator,cog_cli_worker_host_t *worker,cog_agent_id_t agent,uint64_t group,
    size_t entries,size_t bytes)
{
    if(out==NULL) return COG_ERROR_NULL_PTR;
    *out=NULL;if(app==NULL) return COG_ERROR_NULL_PTR;
    if((coordinator==NULL)==(worker==NULL) || ((agent==0U)!=(group==0U))) return COG_ERROR_INVALID_ARG;
    if(app->headless==NULL || app->headless->drive!=CLI_DRIVE_UNBEGUN || app->headless->delegation!=NULL)
        return COG_ERROR_INVALID_STATE;
    if(entries==0U) entries=8U;
    if(bytes==0U) bytes=65536U;
    if(entries>64U || bytes>1048576U) return COG_ERROR_LIMIT;
    bool child=app->headless->kind==COG_CLI_EXECUTION_LOCAL_CHILD || app->headless->kind==COG_CLI_EXECUTION_REMOTE_CHILD;
    if(child!=(agent!=0U)) return COG_ERROR_INVALID_ARG;
    cog_cli_delegation_context_t *context=calloc(1U,sizeof(*context));if(context==NULL) return COG_ERROR_NOMEM;
    context->app=app;context->coordinator=coordinator;context->worker=worker;context->agent=agent;context->group=group;
    context->mailbox_entries=entries;context->mailbox_bytes=bytes;
    app->headless->delegation=context;*out=context;return COG_OK;
}
cog_result_t cog_cli_delegation_context_free(cog_cli_delegation_context_t *context)
{
    if(context==NULL) return COG_OK;
    if(context->live_handles!=0U || context->prepared!=NULL) return COG_ERROR_INVALID_STATE;
    if(context->app!=NULL && context->app->headless!=NULL && context->app->headless->delegation==context)
        context->app->headless->delegation=NULL;
    free(context);return COG_OK;
}
static cog_result_t tool_new(cog_core_t *core,const char *request,size_t length,
    const cog_tool_async_config_t *config,void **out,cli_delegation_operation_t operation)
{
    if(out==NULL) return COG_ERROR_NULL_PTR;
    *out=NULL;if(core==NULL || request==NULL || config==NULL || config->user==NULL) return COG_ERROR_NULL_PTR;
    cog_cli_delegation_context_t *context=config->user;cog_cli_app_t *app=context->app;
    if(app==NULL || app->core!=core || app->headless==NULL || config->owner_guard==NULL ||
       config->owner_guard!=app->guard || config->max_output_bytes==0U || config->timeout_ms==0U)
        return COG_ERROR_INVALID_ARG;
    if(context->prepared!=NULL || app->assistant==NULL || app->tool_index>=app->call_count) return COG_ERROR_INVALID_STATE;
    if((operation==CLI_DELEGATE)!=(context->agent==0U)) return COG_ERROR_INVALID_ARG;
    if(operation==CLI_DELEGATE && (app->opts.backend!=COG_CLI_BACKEND_REAL || app->opts.workspace_root==NULL))
        return COG_ERROR_UNSUPPORTED;
    if((operation==CLI_DELEGATE && config->max_output_bytes<app->opts.max_tool_output_bytes) ||
       (operation==CLI_SEND_MESSAGE && config->max_output_bytes<14U) ||
       (operation==CLI_RECEIVE_MESSAGES && config->max_output_bytes<36U) ||
       (config->max_json_depth!=0U && config->max_json_depth<(operation==CLI_DELEGATE?3U:1U)))
        return COG_ERROR_LIMIT;
    size_t arguments=config->max_argument_bytes?config->max_argument_bytes:65536U;
    if(length>=arguments || arguments>SIZE_MAX-2048U || config->max_output_bytes>SIZE_MAX-2048U)
        return COG_ERROR_LIMIT;
    size_t frame=arguments>config->max_output_bytes?arguments:config->max_output_bytes;
    frame+=2048U;
    cli_delegation_handle_t *h=calloc(1U,sizeof(*h));if(h==NULL) return COG_ERROR_NOMEM;
    h->context=context;h->operation=operation;h->fixed=*config;
    if(!buffer_new(&h->arguments,arguments) || !buffer_new(&h->output,config->max_output_bytes+1U) ||
       !buffer_new(&h->codec,frame) || !buffer_new(&h->pending,frame) || !buffer_new(&h->reply,frame)) {
        handle_storage_free(h);return COG_ERROR_NOMEM;
    }
    cog_result_t rc=arguments_parse(h,request,length);
    if(rc!=COG_OK) { handle_storage_free(h);return rc; }
    const char *session=cog_session_id(app->session),*call=app->call_views[app->tool_index].id;
    if(session==NULL || call==NULL || strlen(session)>127U || strlen(call)>255U ||
       app->now>UINT64_MAX-config->timeout_ms || app->started_at>UINT64_MAX-app->opts.max_turn_ms) {
        handle_storage_free(h);return COG_ERROR_LIMIT;
    }
    strcpy(h->parent_session,session);strcpy(h->call_id,call);
    uint64_t deadline=app->now+config->timeout_ms,parent=app->started_at+app->opts.max_turn_ms;
    if(parent<deadline) deadline=parent;
    h->deadline_ms=deadline;h->request.deadline_ms=deadline;
    h->request.owner=app->headless->expected_owner;h->request.parent_session=h->parent_session;
    h->request.call_id=h->call_id;h->request.parent_node=cog_node_data(app->assistant)->node_id;
    h->request.ordinal=(uint16_t)app->tool_index;h->request.pinned_configuration=app->configuration_generation;
    h->request.tasks=h->tasks;h->request.task_lengths=h->task_lengths;
    context->prepared=h;++context->live_handles;*out=h;return COG_OK;
}
static cog_result_t delegate_new(cog_core_t *c,const char *r,size_t n,const cog_tool_async_config_t *f,void **o)
{ return tool_new(c,r,n,f,o,CLI_DELEGATE); }
static cog_result_t send_new(cog_core_t *c,const char *r,size_t n,const cog_tool_async_config_t *f,void **o)
{ return tool_new(c,r,n,f,o,CLI_SEND_MESSAGE); }
static cog_result_t receive_new(cog_core_t *c,const char *r,size_t n,const cog_tool_async_config_t *f,void **o)
{ return tool_new(c,r,n,f,o,CLI_RECEIVE_MESSAGES); }
static cli_group_record_t *local_group(cli_delegation_handle_t *h)
{
    cog_cli_host_runtime_t *r=h->context->coordinator;if(r==NULL) return NULL;
    for(size_t i=0U;i<r->group_capacity;++i) {
        cli_group_record_t *g=&r->groups[i];
        if(g->phase!=CLI_GROUP_FREE && g->reservation==h->reservation &&
           g->parent_node==h->request.parent_node && g->ordinal==h->request.ordinal &&
           strcmp(g->parent_session,h->parent_session)==0 && strcmp(g->call_id,h->call_id)==0) return g;
    }
    return NULL;
}
static void failed_result(cli_delegation_handle_t *h,cog_result_t rc)
{
    if(h->output_cached) return;
    h->output.data[0]='\0';h->output.length=0U;
    h->result=(cog_tool_output_t){rc,(const char *)h->output.data,0U,false,h->started,false,false};
    h->output_cached=true;h->done=true;
}
cog_result_t cog_cli_delegation_prepare(cog_cli_delegation_context_t *context,bool *ready)
{
    if(context==NULL || ready==NULL) return COG_ERROR_NULL_PTR;
    cli_delegation_handle_t *h=context->prepared;
    if(h==NULL || h->started || h->cancelled || h->done) return COG_ERROR_INVALID_STATE;
    if(context->app->now>=h->deadline_ms) return COG_ERROR_TIMEOUT;
    if(h->prepared) { *ready=true;return COG_OK; }
    cog_result_t rc=COG_OK;
    if(h->operation==CLI_DELEGATE) {
        if(context->coordinator!=NULL) rc=cli_runtime_reserve(context->coordinator,&h->request,&h->reservation);
        else rc=cli_worker_delegation_step(context->worker,h);
    }
    if(rc==COG_OK && (h->operation!=CLI_DELEGATE || h->reservation!=0U)) h->prepared=true;
    *ready=h->prepared;return h->prepared?COG_OK:rc==COG_OK?COG_ERROR_AGAIN:rc;
}
static cog_result_t tool_start(void *opaque,uint64_t now)
{
    if(opaque==NULL) return COG_ERROR_NULL_PTR;
    cli_delegation_handle_t *h=opaque;
    if(h->started || h->cancelled || h->done || !h->prepared) return COG_ERROR_INVALID_STATE;
    if(now<h->last_now_ms) return COG_ERROR_INVALID_ARG;
    h->last_now_ms=now;
    if(now>=h->deadline_ms) return COG_ERROR_INVALID_STATE;
    cog_cli_app_t *app=h->context->app;cog_tool_execution_state_t state;
    cog_result_t rc=cog_session_tool_state(app->session,app->assistant,h->call_id,&state);
    if(rc!=COG_OK || state!=COG_TOOL_EXECUTION_STARTED || cog_node_data(app->assistant)->node_id!=h->request.parent_node)
        return COG_ERROR_INVALID_STATE;
    cog_store_owner_t owner;cog_store_checkpoint_t checkpoint;
    rc=cog_store_owner_read(app->store,h->parent_session,&owner,&checkpoint);
    if(rc!=COG_OK) return rc;
    if(owner.epoch!=h->request.owner.epoch || owner.instance_id!=h->request.owner.instance_id || checkpoint.generation==0U)
        return COG_ERROR_INVALID_STATE;
    h->last_now_ms=now;h->started=true;
    if(h->operation==CLI_DELEGATE && h->context->coordinator!=NULL) {
        rc=cli_runtime_start(h->context->coordinator,h->reservation,checkpoint);
        if(rc!=COG_OK && rc!=COG_ERROR_AGAIN) {
            cli_group_record_t *group=local_group(h);
            if(group==NULL) failed_result(h,rc);
        }
    } else if(h->context->worker!=NULL) {
        rc=cli_worker_delegation_step(h->context->worker,h);
        if(rc!=COG_OK && rc!=COG_ERROR_AGAIN) failed_result(h,rc);
    }
    return COG_OK;
}
static cog_result_t tool_cancel(void *opaque,uint64_t now)
{
    if(opaque==NULL) return COG_ERROR_NULL_PTR;
    cli_delegation_handle_t *h=opaque;if(now<h->last_now_ms) return COG_ERROR_INVALID_ARG;
    h->last_now_ms=now;
    if(h->done || h->cancelled) return COG_OK;
    h->cancelled=true;cog_result_t rc=COG_OK;
    if(h->operation==CLI_DELEGATE && h->reservation!=0U) {
        if(h->context->coordinator!=NULL) rc=cli_runtime_release(h->context->coordinator,h->reservation);
        else rc=cli_worker_delegation_step(h->context->worker,h);
    }
    if(!h->started && h->context->worker!=NULL && h->operation==CLI_DELEGATE &&
       h->reservation==0U && h->pending.length!=0U) rc=COG_ERROR_AGAIN;
    if(!h->started && (rc==COG_OK || rc==COG_ERROR_NOT_FOUND)) failed_result(h,COG_ERROR_ABORTED);
    return rc==COG_ERROR_AGAIN?COG_OK:rc;
}
static cog_result_t tool_poll(void *opaque,uint64_t now,bool *done,uint64_t *deadline)
{
    if(opaque==NULL || done==NULL || deadline==NULL) return COG_ERROR_NULL_PTR;
    cli_delegation_handle_t *h=opaque;
    if(!h->started && !h->cancelled) return COG_ERROR_INVALID_STATE;
    if(now<h->last_now_ms) return COG_ERROR_INVALID_ARG;
    h->last_now_ms=now;
    if(!h->done && now>=h->deadline_ms) {
        cog_result_t rc=tool_cancel(h,now);if(rc!=COG_OK) return rc;
    }
    if(!h->done) {
        cog_result_t rc=COG_OK;
        if(h->context->worker!=NULL) rc=cli_worker_delegation_step(h->context->worker,h);
        else if(h->operation!=CLI_DELEGATE) {
            if(h->cancelled) failed_result(h,COG_ERROR_ABORTED);
            else rc=cli_runtime_mailbox_step(h->context->coordinator,h);
        } else {
            cli_group_record_t *g=local_group(h);
            if(g==NULL) rc=COG_ERROR_INVALID_STATE;
            else if(g->phase==CLI_GROUP_GRANTED || g->phase==CLI_GROUP_ADMITTING)
                rc=cli_runtime_start(h->context->coordinator,h->reservation,g->started_checkpoint);
            else if(g->phase==CLI_GROUP_RESULT || g->phase==CLI_GROUP_SAVED) {
                if(g->output.length>=h->output.capacity) rc=COG_ERROR_LIMIT;
                else {
                    memcpy(h->output.data,g->output.data,g->output.length);
                    h->output.data[g->output.length]='\0';h->output.length=g->output.length;
                    bool effects=false;
                    for(size_t i=0U;i<g->submitted;++i) if(g->members[i].effects_uncertain) effects=true;
                    h->group=g->id;h->result=(cog_tool_output_t){g->aggregate,(const char *)h->output.data,
                        h->output.length,false,effects,0,false};
                    h->output_cached=true;h->done=true;
                }
            }
        }
        if(rc==COG_OK && h->cancelled && !h->started && h->context->worker!=NULL &&
           (h->reservation!=0U || h->pending.length==0U)) failed_result(h,COG_ERROR_ABORTED);
        if(rc!=COG_OK && rc!=COG_ERROR_AGAIN) {
            /* A live admitted group remains owned for actual cancel/drain;
               a transport loss can only report explicit unknown remote effects. */
            if(h->context->worker!=NULL && h->context->worker->shutdown) failed_result(h,COG_ERROR_INDETERMINATE);
            else if(h->operation!=CLI_DELEGATE || local_group(h)==NULL) failed_result(h,rc);
            else { (void)tool_cancel(h,now);return rc; }
        }
    }
    *done=h->done;*deadline=h->done?UINT64_MAX:h->deadline_ms;
    if(!h->done && h->pending.length!=0U && h->context->worker!=NULL) {
        cog_cli_worker_host_t *worker=h->context->worker;
        size_t lane=h->operation==CLI_DELEGATE?COG_CLI_LANE_DELEGATE-1U:COG_CLI_LANE_MAILBOX-1U;
        for(size_t i=0U;i<worker->executor_capacity;++i) if(worker->executors[i].app==h->context->app) {
            cli_control_lane_t *stream=&worker->executors[i].outbound[lane];
            if(stream->copied_wire==h->request_wire && stream->next_send_ms>now &&
               stream->next_send_ms<*deadline) *deadline=stream->next_send_ms;
            break;
        }
    }
    return COG_OK;
}
static cog_result_t tool_result(const void *opaque,cog_tool_output_t *result)
{
    if(opaque==NULL || result==NULL) return COG_ERROR_NULL_PTR;
    const cli_delegation_handle_t *h=opaque;
    if(!h->done || !h->output_cached) return COG_ERROR_AGAIN;
    *result=h->result;return COG_OK;
}
static cog_result_t tool_destroy(void *opaque)
{
    if(opaque==NULL) return COG_OK;
    cli_delegation_handle_t *h=opaque;
    if((h->started && !h->done) || (!h->started && !h->done &&
       h->context->worker!=NULL && h->pending.length!=0U)) return COG_ERROR_INVALID_STATE;
    if(!h->started && h->reservation!=0U) {
        h->cancelled=true;
        cog_result_t rc=h->context->coordinator!=NULL?
            cli_runtime_release(h->context->coordinator,h->reservation):cli_worker_delegation_step(h->context->worker,h);
        if(rc!=COG_OK && rc!=COG_ERROR_NOT_FOUND) return COG_ERROR_INVALID_STATE;
    }
    if(h->context->prepared==h) h->context->prepared=NULL;
    --h->context->live_handles;handle_storage_free(h);return COG_OK;
}
cog_result_t cli_delegation_saved(cog_cli_delegation_context_t *context,uint64_t node,uint16_t ordinal,
    cog_store_checkpoint_t checkpoint)
{
    if(context==NULL) return COG_ERROR_NULL_PTR;
    cli_delegation_handle_t *h=context->prepared;
    if(h==NULL || !h->done || !h->output_cached || h->request.parent_node!=node ||
       h->request.ordinal!=ordinal || checkpoint.generation==0U) return COG_ERROR_INVALID_STATE;
    if(h->operation==CLI_DELEGATE && h->reservation!=0U &&
       h->codec.length==16U) {
        cli_wire_reader_t saved={h->codec.data,16U,0U,true};
        if(cli_wire_get(&saved,8U)!=h->group || cli_wire_get(&saved,8U)!=checkpoint.generation)
            return COG_ERROR_INVALID_STATE;
    }
    if(h->result_saved) return COG_OK;
    if(h->operation!=CLI_DELEGATE || h->reservation==0U) { h->result_saved=true;return COG_OK; }
    if(context->coordinator!=NULL) {
        cli_group_record_t *group=local_group(h);
        if(group==NULL || (group->phase!=CLI_GROUP_RESULT && group->phase!=CLI_GROUP_SAVED)) return COG_ERROR_INVALID_STATE;
        if(group->result_acknowledged && group->result_checkpoint.generation!=checkpoint.generation) return COG_ERROR_INVALID_STATE;
        if(h->codec.capacity<16U) return COG_ERROR_LIMIT;
        cli_wire_writer_t saved={h->codec.data,0U,true};cli_wire_put(&saved,h->group,8U);
        cli_wire_put(&saved,checkpoint.generation,8U);h->codec.length=saved.used;
        group->result_checkpoint=checkpoint;group->result_acknowledged=true;group->phase=CLI_GROUP_SAVED;
        h->result_saved=true;return COG_OK;
    }
    /* The exact checkpoint is copied into the handle's reserved notification
       body before the worker bridge accepts its immutable wire. */
    if(h->codec.capacity<16U) return COG_ERROR_LIMIT;
    cli_wire_writer_t writer={h->codec.data,0U,true};cli_wire_put(&writer,h->group,8U);
    cli_wire_put(&writer,checkpoint.generation,8U);h->codec.length=writer.used;
    return cli_worker_delegation_step(context->worker,h);
}
static const cog_tool_async_vtable_t delegate_operations={delegate_new,tool_start,tool_poll,tool_cancel,tool_result,tool_destroy};
static const cog_tool_async_vtable_t send_operations={send_new,tool_start,tool_poll,tool_cancel,tool_result,tool_destroy};
static const cog_tool_async_vtable_t receive_operations={receive_new,tool_start,tool_poll,tool_cancel,tool_result,tool_destroy};
static const cog_tool_t delegation_descriptors[]={
    {"delegate","Run bounded child tasks", "{\"type\":\"object\",\"properties\":{\"tasks\":{\"type\":\"array\",\"minItems\":1,\"maxItems\":16,\"items\":{\"type\":\"object\",\"properties\":{\"task\":{\"type\":\"string\",\"minLength\":1}},\"required\":[\"task\"],\"additionalProperties\":false}}},\"required\":[\"tasks\"],\"additionalProperties\":false}",NULL,&delegate_operations,COG_TOOL_SIDE_EFFECTING},
    {"send_message","Copy a message to an admitted sibling", "{\"type\":\"object\",\"properties\":{\"agent_id\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":20},\"message\":{\"type\":\"string\",\"maxLength\":4095}},\"required\":[\"agent_id\",\"message\"],\"additionalProperties\":false}",NULL,&send_operations,COG_TOOL_SIDE_EFFECTING},
    {"receive_messages","Receive a bounded whole-message prefix", "{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}",NULL,&receive_operations,COG_TOOL_SIDE_EFFECTING}
};
const cog_tool_table_t cog_cli_delegation_tools={delegation_descriptors,sizeof(delegation_descriptors)/sizeof(delegation_descriptors[0])};

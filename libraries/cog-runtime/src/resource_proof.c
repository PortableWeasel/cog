#include "resource_proof.h"
#include <cog/instruction_codec.h>
#include <string.h>
#include <cog/tool.h>
static bool text_equal(const char *a,const char *b)
{ return a==NULL ? b==NULL : b!=NULL && strcmp(a,b)==0; }
static bool bytes_equal(const void *a,const void *b,size_t n)
{ return n==0U || (a!=NULL && b!=NULL && memcmp(a,b,n)==0); }
static const cog_node_data_t *node(const cog_session_snapshot_t *s,uint64_t id)
{ for(size_t i=0;i<s->node_count;i++)if(s->nodes[i].node_id==id)return &s->nodes[i];return NULL; }
static const cog_tool_ledger_record_t *ledger(const cog_session_snapshot_t *s,uint64_t id,const char *call)
{ for(size_t i=0;i<s->ledger_count;i++)if(s->ledger[i].assistant_node_id==id && text_equal(s->ledger[i].call_id,call))return &s->ledger[i];return NULL; }
static bool final(cog_tool_execution_state_t state)
{ return state!=COG_TOOL_EXECUTION_PROPOSED && state!=COG_TOOL_EXECUTION_STARTED && state!=COG_TOOL_EXECUTION_INDETERMINATE; }
static bool ancestor(const cog_session_snapshot_t *s,uint64_t parent,uint64_t child)
{
    for(size_t i=0;i<=s->node_count && child!=0U;i++) {
        if(child==parent)return true;
        const cog_node_data_t *n=node(s,child);if(n==NULL)return false;child=n->parent_id;
    }
    return false;
}
static size_t results(const cog_session_snapshot_t *s,uint64_t assistant,const char *call)
{
    size_t count=0;
    for(size_t i=0;i<s->node_count;i++) {
        const cog_node_data_t *n=&s->nodes[i];
        if(n->message==NULL || n->message->role!=COG_MESSAGE_ROLE_TOOL ||
           !text_equal(n->message->tool_call_id,call))continue;
        uint64_t at=n->parent_id;
        for(size_t k=0;k<s->node_count && at!=0U;k++) {
            const cog_node_data_t *p=node(s,at);if(p==NULL)break;
            if(p->message!=NULL && p->message->role==COG_MESSAGE_ROLE_ASSISTANT && p->message->tool_call_count!=0U) {
                if(p->node_id==assistant)++count;
                break;
            }
            at=p->parent_id;
        }
    }
    return count;
}
static bool audit_equal(const cog_tool_audit_record_t *a,const cog_tool_audit_record_t *b)
{ return a->sequence==b->sequence && a->previous==b->previous && a->next==b->next &&
    a->explicit_reconciliation==b->explicit_reconciliation && a->evidence_len==b->evidence_len &&
    bytes_equal(a->evidence,b->evidence,a->evidence_len); }
/* Only INSPECT's single appended recovery tail is synthetic. */
static bool persisted(const cog_tool_ledger_record_t *l,uint64_t watermark,
    cog_tool_execution_state_t *state,size_t *count)
{
    *state=l->state;*count=l->audit_count;
    if(l->audit_count==0U)return l->state==COG_TOOL_EXECUTION_PROPOSED;
    const cog_tool_audit_record_t *last=&l->audit[l->audit_count-1U];
    if(last->sequence>watermark) {
        if(l->state!=COG_TOOL_EXECUTION_INDETERMINATE || last->previous!=COG_TOOL_EXECUTION_STARTED ||
           last->next!=COG_TOOL_EXECUTION_INDETERMINATE || last->explicit_reconciliation || l->audit_count<2U)
            return false;
        --*count;*state=COG_TOOL_EXECUTION_STARTED;
    }
    for(size_t i=0;i<*count;i++)if(l->audit[i].sequence>watermark)return false;
    return *count==0U ? *state==COG_TOOL_EXECUTION_PROPOSED : l->audit[*count-1U].next==*state;
}
static cog_result_t owner_matches(cog_store_t *store,const char *id,cog_store_owner_t expected)
{
    cog_store_owner_t actual;cog_store_checkpoint_t latest;
    cog_result_t rc=cog_store_owner_read(store,id,&actual,&latest);
    if(rc==COG_OK && (actual.epoch!=expected.epoch || actual.instance_id!=expected.instance_id))rc=COG_ERROR_INVALID_STATE;
    return rc;
}
static cog_result_t load(cog_store_t *store,cog_store_owner_t owner,const char *id,uint64_t generation,
    cog_session_t **session,cog_store_load_report_t *report)
{
    cog_result_t rc=owner_matches(store,id,owner);
    if(rc==COG_OK)rc=cog_store_checkpoint_load(store,id,(cog_store_checkpoint_t){generation},COG_STORE_LOAD_INSPECT,session,report);
    if(rc==COG_OK && report->checkpoint_generation!=generation)rc=COG_ERROR_PROTOCOL;
    if(rc==COG_OK)rc=owner_matches(store,id,owner);
    return rc;
}
static bool capsule_binding(const cog_session_snapshot_t *s,uint64_t binding,
    const cog_cli_instruction_capsule_t *capsule)
{
    uint64_t context=0;
    for(size_t i=0;i<s->context_binding_count;i++)if(s->context_bindings[i].binding_id==binding)context=s->context_bindings[i].context_id;
    for(size_t i=0;i<s->context_count;i++)if(context!=0U && s->contexts[i].context_id==context) {
        const cog_session_context_t *c=&s->contexts[i];
        return text_equal(c->namespace_id,COG_INSTRUCTION_CONTEXT_NAMESPACE) && c->format==COG_INSTRUCTION_CONTEXT_FORMAT &&
            c->data_bytes==capsule->length && bytes_equal(c->data,capsule->bytes,c->data_bytes);
    }
    return false;
}
static const cog_message_tool_call_t *request_call(const cog_session_snapshot_t *s,
    const cog_cli_instruction_resource_t *request,const cog_node_data_t **assistant)
{
    *assistant=node(s,request->assistant_node_id);
    const cog_message_t *m=*assistant==NULL ? NULL : (*assistant)->message;
    if(m==NULL || m->role!=COG_MESSAGE_ROLE_ASSISTANT || request->call_ordinal>=m->tool_call_count)return NULL;
    const cog_message_tool_call_t *c=&m->tool_calls[request->call_ordinal];
    return text_equal(c->id,request->call_id) ? c : NULL;
}
cog_result_t cli_resource_request_prove(cog_store_t *store,cog_store_owner_t owner,const char *session_id,
    const cog_cli_instruction_resource_t *request,const cog_cli_instruction_capsule_t *old,
    const cog_prompt_tool_binding_t *bindings,size_t binding_count,const cog_instruction_limits_t *limits,
    cog_session_t **proof_session,cog_store_load_report_t *report)
{
    if(proof_session==NULL)return COG_ERROR_NULL_PTR;
    *proof_session=NULL;
    if(store==NULL || session_id==NULL || request==NULL || old==NULL || limits==NULL || report==NULL)return COG_ERROR_NULL_PTR;
    if(request->kind!=COG_CLI_INSTRUCTION_RESOURCE_REQUEST || request->request_checkpoint_generation==0U ||
       request->assistant_node_id==0U || request->call_id==NULL || old->length==0U || old->bytes==NULL ||
       owner.epoch==0U || owner.instance_id==0U)return COG_ERROR_INVALID_ARG;
    if(request->context.catalog_generation!=old->identity.catalog_generation ||
       request->context.snapshot_id!=old->identity.snapshot_id ||
       memcmp(request->context.sha256,old->identity.sha256,32U)!=0)return COG_ERROR_PROTOCOL;
    cog_instruction_snapshot_t *instructions=NULL;
    cog_result_t rc=cog_instruction_snapshot_decode(&instructions,limits,old->bytes,old->length,
        COG_INSTRUCTION_EXECUTE,bindings,binding_count);
    cog_instruction_manifest_t manifest;unsigned char digest[32];
    if(rc==COG_OK)rc=cog_instruction_snapshot_view(instructions,&manifest);
    if(rc==COG_OK)rc=cog_instruction_snapshot_fingerprint(instructions,digest);
    if(rc==COG_OK && (manifest.catalog_generation!=old->identity.catalog_generation || manifest.snapshot_id!=old->identity.snapshot_id ||
        memcmp(digest,old->identity.sha256,32U)!=0))rc=COG_ERROR_PROTOCOL;
    cog_session_t *session=NULL;cog_store_load_report_t loaded={0};cog_session_snapshot_t state;
    if(rc==COG_OK)rc=load(store,owner,session_id,request->request_checkpoint_generation,&session,&loaded);
    if(rc==COG_OK)rc=cog_session_snapshot_view(session,&state);
    const cog_node_data_t *assistant=NULL;const cog_message_tool_call_t *call=NULL;
    if(rc==COG_OK) {
        call=request_call(&state,request,&assistant);
        if(call==NULL || !ancestor(&state,request->assistant_node_id,state.active_node_id) ||
           assistant->context_binding_id!=state.selected_context_binding_id ||
           !capsule_binding(&state,state.selected_context_binding_id,old))rc=COG_ERROR_PROTOCOL;
    }
    if(rc==COG_OK) {
        cog_prompt_bundle_t effective;rc=cog_instruction_prompt_view(instructions,&effective);
        bool allowed=false;
        if(rc==COG_OK)for(size_t i=0;i<effective.tool_count;i++)
            if(text_equal(effective.tools[i].descriptor->name,call->name) && effective.tools[i].policy==COG_PROMPT_TOOL_ALLOW)
                allowed=true;
        if(rc==COG_OK && !allowed)rc=COG_ERROR_DENIED;
    }
    const cog_tool_ledger_record_t *target=rc==COG_OK ? ledger(&state,request->assistant_node_id,request->call_id) : NULL;
    if(rc==COG_OK && target==NULL)rc=COG_ERROR_PROTOCOL;
    for(size_t i=0;rc==COG_OK && i<state.ledger_count;i++) {
        const cog_tool_ledger_record_t *l=&state.ledger[i];cog_tool_execution_state_t before;size_t count;
        if(!persisted(l,loaded.source_audit_high_water,&before,&count)){rc=COG_ERROR_PROTOCOL;break;}
        if(l==target) {
            bool scope=request->operation==COG_CLI_INSTRUCTION_RESOURCE_SCOPE;
            bool control=(request->operation==COG_CLI_INSTRUCTION_RESOURCE_ACTIVATE && text_equal(call->name,"activate_skill")) ||
                (request->operation==COG_CLI_INSTRUCTION_RESOURCE_LOAD && text_equal(call->name,"read_skill_resource"));
            if((scope && before!=COG_TOOL_EXECUTION_PROPOSED) || (!scope && (!control || before!=COG_TOOL_EXECUTION_STARTED)) ||
                results(&state,l->assistant_node_id,l->call_id)!=0U)rc=COG_ERROR_DENIED;
        } else if(before==COG_TOOL_EXECUTION_STARTED || before==COG_TOOL_EXECUTION_INDETERMINATE)rc=COG_ERROR_DENIED;
        else if(!final(before) && l->assistant_node_id!=request->assistant_node_id)rc=COG_ERROR_PROTOCOL;
    }
    (void)cog_instruction_snapshot_free(instructions);
    if(rc==COG_OK){*proof_session=session;*report=loaded;}else cog_session_free(session);
    return rc;
}
static bool message_equal(const cog_message_t *a,const cog_message_t *b)
{
    if(a==NULL || b==NULL)return a==b;
    if(a->role!=b->role || a->status!=b->status || a->content_len!=b->content_len ||
       !bytes_equal(a->content,b->content,a->content_len) || !text_equal(a->name,b->name) ||
       !text_equal(a->tool_call_id,b->tool_call_id) || a->tool_call_count!=b->tool_call_count || a->opaque_count!=b->opaque_count)return false;
    for(size_t i=0;i<a->tool_call_count;i++) {
        const cog_message_tool_call_t *x=&a->tool_calls[i],*y=&b->tool_calls[i];
        if(!text_equal(x->id,y->id) || !text_equal(x->name,y->name) || x->arguments_len!=y->arguments_len ||
           !bytes_equal(x->arguments,y->arguments,x->arguments_len))return false;
    }
    for(size_t i=0;i<a->opaque_count;i++) {
        const cog_message_opaque_t *x=&a->opaque[i],*y=&b->opaque[i];
        if(!text_equal(x->namespace_id,y->namespace_id) || !text_equal(x->format,y->format) ||
           x->size!=y->size || !bytes_equal(x->data,y->data,x->size))return false;
    }
    return a->usage.tokens_known==b->usage.tokens_known && a->usage.input_tokens==b->usage.input_tokens &&
        a->usage.output_tokens==b->usage.output_tokens && a->usage.reasoning_tokens_known==b->usage.reasoning_tokens_known &&
        a->usage.reasoning_tokens==b->usage.reasoning_tokens && a->usage.cost_known==b->usage.cost_known &&
        memcmp(&a->usage.cost,&b->usage.cost,sizeof(a->usage.cost))==0 && text_equal(a->usage.currency,b->usage.currency);
}
static bool node_preserved(const cog_node_data_t *a,const cog_node_data_t *b)
{
    return b!=NULL && a->node_id==b->node_id && a->parent_id==b->parent_id &&
        a->context_binding_id==b->context_binding_id && a->role==b->role && a->content_len==b->content_len &&
        a->chunk_count==b->chunk_count && a->timing.request_sent_ms==b->timing.request_sent_ms &&
        a->timing.first_token_ms==b->timing.first_token_ms && a->timing.last_token_ms==b->timing.last_token_ms &&
        a->timing.duration_ms==b->timing.duration_ms && a->timing.ttft_ms==b->timing.ttft_ms &&
        memcmp(&a->timing.tokens_per_second,&b->timing.tokens_per_second,sizeof(a->timing.tokens_per_second))==0 &&
        a->cost.input_tokens==b->cost.input_tokens && a->cost.output_tokens==b->cost.output_tokens &&
        memcmp(&a->cost.cost_per_input_token,&b->cost.cost_per_input_token,sizeof(a->cost.cost_per_input_token))==0 && memcmp(&a->cost.cost_per_output_token,&b->cost.cost_per_output_token,sizeof(a->cost.cost_per_output_token))==0 &&
        memcmp(&a->cost.total_cost,&b->cost.total_cost,sizeof(a->cost.total_cost))==0 && a->cost.tokens_known==b->cost.tokens_known &&
        a->cost.total_cost_known==b->cost.total_cost_known &&
        text_equal(a->content,b->content) && text_equal(a->tool_name,b->tool_name) &&
        text_equal(a->tool_args,b->tool_args) && text_equal(a->tool_result,b->tool_result) &&
        text_equal(a->raw_request,b->raw_request) && text_equal(a->raw_response,b->raw_response) &&
        text_equal(a->model,b->model) && text_equal(a->provider,b->provider) && message_equal(a->message,b->message);
}
static cog_result_t commit_state_valid(const cog_session_snapshot_t *old,const cog_session_snapshot_t *now,
    const cog_cli_instruction_resource_t *request,const cog_store_load_report_t *old_report,
    const cog_store_load_report_t *report,const cog_cli_instruction_capsule_t *candidate)
{
    cog_result_t rc=COG_OK;
    if(now->ledger_count!=old->ledger_count || old->context_count==SIZE_MAX ||
       old->context_binding_count==SIZE_MAX || now->context_count!=old->context_count+1U ||
       now->context_binding_count!=old->context_binding_count+1U)return COG_ERROR_PROTOCOL;
    if(rc==COG_OK && (now->selected_context_binding_id==old->selected_context_binding_id ||
        !capsule_binding(now,now->selected_context_binding_id,candidate) ||
        !ancestor(now,old->active_node_id,now->active_node_id) ||
        !text_equal(old->parent_session_id,now->parent_session_id) || now->ledger_count!=old->ledger_count ||
        now->node_id_high_water<old->node_id_high_water || now->context_high_water<old->context_high_water ||
        now->context_binding_high_water<old->context_binding_high_water ||
        now->context_count!=old->context_count+1U || now->context_binding_count!=old->context_binding_count+1U))rc=COG_ERROR_PROTOCOL;
    if(rc==COG_OK && (now->contexts[old->context_count].context_id<=old->context_high_water ||
        now->context_bindings[old->context_binding_count].context_id!=now->contexts[old->context_count].context_id ||
        now->context_bindings[old->context_binding_count].binding_id!=now->selected_context_binding_id))rc=COG_ERROR_PROTOCOL;
    if(rc==COG_OK) {
        uint64_t cursor=now->selected_context_binding_id;
        for(size_t depth=0;cursor!=old->selected_context_binding_id && depth<=now->context_binding_count;depth++) {
            const cog_session_context_binding_t *b=NULL;
            for(size_t i=0;i<now->context_binding_count;i++)if(now->context_bindings[i].binding_id==cursor)b=&now->context_bindings[i];
            if(b==NULL || b->binding_id<=old->context_binding_high_water || b->anchor_node_id!=now->active_node_id) {
                rc=COG_ERROR_PROTOCOL;break;
            }
            cursor=b->previous_binding_id;
        }
        if(cursor!=old->selected_context_binding_id)rc=COG_ERROR_PROTOCOL;
    }
    for(size_t i=0;rc==COG_OK && i<old->node_count;i++)
        if(!node_preserved(&old->nodes[i],node(now,old->nodes[i].node_id)))rc=COG_ERROR_PROTOCOL;
    for(size_t i=0;rc==COG_OK && i<old->context_count;i++) {
        const cog_session_context_t *a=&old->contexts[i],*b=&now->contexts[i];
        if(a->context_id!=b->context_id || !text_equal(a->namespace_id,b->namespace_id) || a->format!=b->format ||
           a->data_bytes!=b->data_bytes || !bytes_equal(a->data,b->data,a->data_bytes))rc=COG_ERROR_PROTOCOL;
    }
    for(size_t i=0;rc==COG_OK && i<old->context_binding_count;i++) {
        const cog_session_context_binding_t *a=&old->context_bindings[i],*b=&now->context_bindings[i];
        if(a->binding_id!=b->binding_id || a->previous_binding_id!=b->previous_binding_id ||
           a->anchor_node_id!=b->anchor_node_id || a->context_id!=b->context_id)rc=COG_ERROR_PROTOCOL;
    }
    const cog_node_data_t *assistant=NULL;
    if(rc==COG_OK && request_call(old,request,&assistant)==NULL)rc=COG_ERROR_PROTOCOL;
    for(size_t i=0;rc==COG_OK && i<now->node_count;i++) {
        const cog_node_data_t *n=&now->nodes[i];
        if(node(old,n->node_id)!=NULL)continue;
        if(n->message==NULL || n->message->role!=COG_MESSAGE_ROLE_TOOL || assistant==NULL ||
           n->context_binding_id!=assistant->context_binding_id ||
           !ancestor(now,n->node_id,now->active_node_id) ||
           !ancestor(now,assistant->node_id,n->node_id))rc=COG_ERROR_PROTOCOL;
    }
    for(size_t i=0;rc==COG_OK && i<old->ledger_count;i++) {
        const cog_tool_ledger_record_t *a=&old->ledger[i],*b=ledger(now,a->assistant_node_id,a->call_id);
        cog_tool_execution_state_t before;size_t original_count;
        if(!persisted(a,old_report->source_audit_high_water,&before,&original_count) || b==NULL ||
           b->audit_count<original_count) {rc=COG_ERROR_PROTOCOL;break;}
        for(size_t j=0;j<original_count;j++)if(!audit_equal(&a->audit[j],&b->audit[j]))rc=COG_ERROR_PROTOCOL;
        if(rc!=COG_OK)break;
        if(final(before)) {
            if(b->state!=before || b->audit_count!=original_count)rc=COG_ERROR_PROTOCOL;
            continue;
        }
        bool requesting=a->assistant_node_id==request->assistant_node_id && text_equal(a->call_id,request->call_id);
        if(a->assistant_node_id!=request->assistant_node_id || !final(b->state) ||
           results(now,a->assistant_node_id,a->call_id)!=1U || b->audit_count!=original_count+1U) {
            rc=COG_ERROR_PROTOCOL;break;
        }
        const cog_tool_audit_record_t *transition=&b->audit[original_count];
        if(transition->previous!=before || transition->next!=b->state || transition->explicit_reconciliation ||
           transition->sequence<=old_report->source_audit_high_water || transition->sequence>report->source_audit_high_water) {
            rc=COG_ERROR_PROTOCOL;break;
        }
        if(before==COG_TOOL_EXECUTION_PROPOSED) {
            if(b->state!=COG_TOOL_EXECUTION_DENIED)rc=COG_ERROR_PROTOCOL;
        } else if(before!=COG_TOOL_EXECUTION_STARTED || !requesting ||
            request->operation==COG_CLI_INSTRUCTION_RESOURCE_SCOPE ||
            (b->state!=COG_TOOL_EXECUTION_SUCCEEDED && b->state!=COG_TOOL_EXECUTION_FAILED && b->state!=COG_TOOL_EXECUTION_CANCELLED))
            rc=COG_ERROR_PROTOCOL;
    }
    for(size_t i=0;rc==COG_OK && i<now->ledger_count;i++)
        if(!final(now->ledger[i].state) || results(now,now->ledger[i].assistant_node_id,now->ledger[i].call_id)!=1U)
            rc=COG_ERROR_PROTOCOL;
    return rc;
}
cog_result_t cli_resource_commit_prove(cog_store_t *store,cog_store_owner_t owner,const char *session_id,
    const cog_cli_instruction_resource_t *request,const cog_session_t *old_proof,
    const cog_store_load_report_t *old_report,const cog_cli_instruction_capsule_t *candidate,
    uint64_t checkpoint_generation,cog_session_t **saved_proof)
{
    if(saved_proof==NULL)return COG_ERROR_NULL_PTR;
    *saved_proof=NULL;
    if(store==NULL || session_id==NULL || request==NULL || old_proof==NULL || old_report==NULL || candidate==NULL)
        return COG_ERROR_NULL_PTR;
    if(checkpoint_generation<=request->request_checkpoint_generation ||
       old_report->checkpoint_generation!=request->request_checkpoint_generation || candidate->bytes==NULL || candidate->length==0U ||
       candidate->identity.catalog_generation!=request->context.catalog_generation ||
       candidate->identity.snapshot_id==request->context.snapshot_id || candidate->identity.snapshot_id==0U)return COG_ERROR_PROTOCOL;
    /* Reuse the pure outer encoder's canonical-prefix/hash checks without allocating. */
    static const unsigned char cgb1[]={'C','G','B','1'};
    cog_cli_instruction_bootstrap_t framing={cgb1,sizeof(cgb1),*candidate};size_t required=0;
    cog_result_t rc=cog_cli_instruction_bootstrap_encode(&framing,NULL,0U,&required);
    if(rc!=COG_ERROR_OVERFLOW)return rc==COG_OK ? COG_ERROR_PROTOCOL : rc;
    cog_session_snapshot_t old,now;
    rc=cog_session_snapshot_view(old_proof,&old);
    if(rc==COG_OK && !text_equal(old.session_id,session_id))rc=COG_ERROR_PROTOCOL;
    cog_session_t *saved=NULL;cog_store_load_report_t report={0};
    if(rc==COG_OK)rc=load(store,owner,session_id,checkpoint_generation,&saved,&report);
    if(rc==COG_OK)rc=cog_session_snapshot_view(saved,&now);
    if(rc==COG_OK)rc=commit_state_valid(&old,&now,request,old_report,&report,candidate);
    if(rc==COG_OK)*saved_proof=saved;else cog_session_free(saved);
    return rc;
}

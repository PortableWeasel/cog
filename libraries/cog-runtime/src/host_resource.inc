/* Coordinator CGIR service. Authentication/immutable attempt matching is done
 * by host_worker_message before entry; no UI-lane buffers retain these bytes. */
#include "resource_proof.h"
static cog_cli_control_message_t host_resource_envelope(cog_cli_host_runtime_t *r,
    cli_executor_slot_t *slot,cog_cli_control_kind_t kind,uint64_t wire,uint64_t reply) {
    return (cog_cli_control_message_t){.kind=kind,.lane=COG_CLI_LANE_RESOURCE,
        .runtime_incarnation=r->runtime_incarnation,.sender_incarnation=r->runtime_incarnation,
        .wire_id=wire,.reply_to=reply,.worker=slot->worker,.owner_peer=slot->owner_peer,
        .agent_id=slot->agent,.execution_epoch=slot->attempt,.session_id=slot->session};
}
static cog_result_t host_resource_capsule(cli_resource_exchange_t *x,
    const cog_instruction_snapshot_t *snapshot,cog_cli_instruction_capsule_t *capsule) {
    cog_instruction_manifest_t m;
    cog_result_t rc=cog_instruction_snapshot_view(snapshot,&m);
    size_t length=0U;
    if(rc==COG_OK)rc=cog_instruction_manifest_encode(&m,x->result_storage,x->result_capacity,&length);
    if(rc!=COG_OK)return rc;
    *capsule=(cog_cli_instruction_capsule_t){.identity={.catalog_generation=m.catalog_generation,
        .snapshot_id=m.snapshot_id},.bytes=x->result_storage,.length=length};
    return cog_instruction_snapshot_fingerprint(snapshot,capsule->identity.sha256);
}
static cog_result_t host_resource_status(cog_result_t rc) {
    switch(rc) {
    case COG_OK:case COG_ERROR_NOT_FOUND:case COG_ERROR_DENIED:case COG_ERROR_LIMIT:
    case COG_ERROR_IO:case COG_ERROR_ABORTED:case COG_ERROR_TIMEOUT:case COG_ERROR_UNSUPPORTED:
    case COG_ERROR_PROTOCOL:return rc;
    case COG_ERROR_NOMEM:case COG_ERROR_OVERFLOW:return COG_ERROR_LIMIT;
    default:return COG_ERROR_PROTOCOL;
    }
}
static cog_result_t host_resource_result(cog_cli_host_runtime_t *r,
    cli_executor_slot_t *slot,cog_result_t status) {
    cli_resource_exchange_t *x=slot->resource;cli_resource_owner_t *owner=slot->resource_owner;
    cog_cli_instruction_resource_t body={.kind=COG_CLI_INSTRUCTION_RESOURCE_RESULT,
        .context=x->request.context,.result=host_resource_status(status)};
    cog_result_t rc=COG_OK;
    if(status==COG_OK)rc=host_resource_capsule(x,owner->candidate==NULL?owner->current:owner->candidate,&body.candidate);
    if(rc!=COG_OK){body.result=host_resource_status(rc);memset(&body.candidate,0,sizeof(body.candidate));}
    cog_cli_control_message_t m=host_resource_envelope(r,slot,COG_CLI_CONTROL_RESOURCE_RESULT,
        owner->reply_wire,x->frames[0].wire);
    rc=cli_resource_exchange_build(x,&m,&body);
    if(rc==COG_OK){x->send_index=1U;x->send_pending=true;}
    return rc;
}
static cog_result_t host_resource_high_water(cog_runtime_t *runtime,
    cli_resource_owner_t *owner,uint64_t generation) {
    cog_session_snapshot_t state;
    cog_result_t rc=cog_session_snapshot_view(owner->proof,&state);
    if(rc!=COG_OK)return rc;
    for(size_t i=0U;i<state.context_count;i++) {
        const cog_session_context_t *context=&state.contexts[i];
        if(strcmp(context->namespace_id,COG_INSTRUCTION_CONTEXT_NAMESPACE)!=0)continue;
        if(context->format!=COG_INSTRUCTION_CONTEXT_FORMAT)return COG_ERROR_UNSUPPORTED;
        cog_instruction_snapshot_t *inspection=NULL;
        rc=cog_instruction_snapshot_decode(&inspection,&runtime->config.instruction_limits,
            context->data,context->data_bytes,COG_INSTRUCTION_INSPECT,NULL,0U);
        cog_instruction_manifest_t manifest;
        if(rc==COG_OK)rc=cog_instruction_snapshot_view(inspection,&manifest);
        if(rc==COG_OK && manifest.catalog_generation==generation && manifest.snapshot_id>owner->high_water)
            owner->high_water=manifest.snapshot_id;
        cog_instruction_snapshot_free(inspection);
        if(rc!=COG_OK)return rc;
    }
    return COG_OK;
}
static cog_result_t host_resource_begin(cog_cli_host_runtime_t *r,cli_executor_slot_t *slot) {
    cli_resource_exchange_t *x=slot->resource;cli_resource_owner_t *owner=slot->resource_owner;
    cog_runtime_t *runtime=r->instruction_owner;
    if(runtime==NULL)return COG_ERROR_UNSUPPORTED;
    cog_result_t rc=COG_OK;
    if(owner->current==NULL) {
        const cog_cli_configuration_t *configuration=slot->profile.source;
        if(configuration==NULL)configuration=slot->pending_profile;
        if(configuration==NULL && slot->agent==0U)configuration=r->current_mirror;
        const cog_cli_instruction_capsule_t *capsule=cli_configuration_instruction_capsule(configuration);
        if(capsule==NULL || capsule->length==0U)return COG_ERROR_DENIED;
        rc=cog_instruction_snapshot_decode(&owner->current,&runtime->config.instruction_limits,
            capsule->bytes,capsule->length,COG_INSTRUCTION_EXECUTE,
            runtime->bindings,runtime->config.tools.binding_count);
        if(rc!=COG_OK)return rc;
    }
    cog_cli_instruction_capsule_t old;
    rc=host_resource_capsule(x,owner->current,&old);
    if(rc==COG_OK)rc=cli_resource_request_prove(r->catalog,slot->owner,slot->session,
        &x->request,&old,runtime->bindings,runtime->config.tools.binding_count,
        &runtime->config.instruction_limits,&owner->proof,&owner->report);
    if(rc!=COG_OK)return rc;
    cog_session_snapshot_t proof;
    rc=cog_session_snapshot_view(owner->proof,&proof);
    const cog_message_tool_call_t *call=NULL;
    if(rc==COG_OK)for(size_t i=0U;i<proof.node_count;i++)
        if(proof.nodes[i].node_id==x->request.assistant_node_id && proof.nodes[i].message!=NULL &&
            x->request.call_ordinal<proof.nodes[i].message->tool_call_count)
            call=&proof.nodes[i].message->tool_calls[x->request.call_ordinal];
    if(rc==COG_OK && call==NULL)rc=COG_ERROR_PROTOCOL;
    cog_instruction_manifest_t retained;
    if(rc==COG_OK)rc=cog_instruction_snapshot_view(owner->current,&retained);
    if(rc==COG_OK) {
        bool enabled=false;
        for(size_t i=0U;i<retained.tool_count;i++)
            if(strcmp(retained.tools[i].name,call->name)==0 && retained.tools[i].policy!=COG_PROMPT_TOOL_DENY)
                enabled=true;
        if(!enabled)rc=COG_ERROR_DENIED;
    }
    if(rc==COG_OK)rc=cli_runtime_resource_request_matches(runtime,&x->request,call);
    if(rc==COG_OK)rc=host_resource_high_water(runtime,owner,old.identity.catalog_generation);
    if(rc!=COG_OK)return rc;
    if(owner->high_water==UINT64_MAX)return COG_ERROR_LIMIT;
    owner->candidate_id=++owner->high_water;owner->prepared=true;
    return COG_OK;
}
static cog_result_t host_resource_progress(cog_cli_host_runtime_t *r,cli_executor_slot_t *slot) {
    cli_resource_exchange_t *x=slot->resource;cli_resource_owner_t *owner=slot->resource_owner;
    if(x==NULL || owner==NULL || !x->active)return COG_ERROR_AGAIN;
    if(x->frozen)return COG_ERROR_INDETERMINATE;
    if(x->frames[1].length==0U) {
        cog_result_t rc=COG_OK;
        if(r->now_ms>=x->deadline_ms)rc=COG_ERROR_TIMEOUT;
        else if(r->shutdown || r->admission_frozen || slot->cancel_latched)rc=COG_ERROR_ABORTED;
        if(rc==COG_OK && !owner->prepared)rc=host_resource_begin(r,slot);
        if(rc==COG_OK && owner->job==NULL)rc=cli_runtime_resolver_new(&owner->job,
            r->instruction_owner,owner->current,(unsigned)x->request.operation,
            x->request.root_id,x->request.skill_name,x->request.path,owner->candidate_id);
        if(rc==COG_ERROR_AGAIN)return rc;
        bool done=false;
        if(rc==COG_OK)rc=cli_runtime_resolver_step(owner->job,&done);
        if(rc==COG_ERROR_AGAIN)return rc;
        if(rc==COG_OK && !done)return COG_ERROR_AGAIN;
        if(rc==COG_OK)rc=cli_runtime_resolver_take(owner->job,&owner->candidate);
        cog_result_t freed=cli_runtime_resolver_free(&owner->job);
        if(rc==COG_OK)rc=freed;
        rc=host_resource_result(r,slot,rc);
        if(rc!=COG_OK)return rc;
    } else if(r->now_ms>=x->deadline_ms) {
        /* Evidence/candidate remain pinned. No late receipt authorizes retry. */
        x->frozen=true;slot->effects_uncertain=true;return COG_ERROR_TIMEOUT;
    }
    if(x->send_pending) {
        cli_resource_frame_t *f=&x->frames[x->send_index];
        cli_host_bytes_t view={f->bytes,f->capacity,f->length};
        return host_copied_send(r,slot->worker,&view,f->wire,&x->copied_wire,&x->next_send_ms,false);
    }
    return COG_ERROR_AGAIN;
}
static cog_result_t host_resource_receive(cog_cli_host_runtime_t *r,cli_executor_slot_t *slot,
    const cog_cli_control_message_t *m,const cog_orchestrator_event_t *event) {
    cli_resource_exchange_t *x=slot->resource;cli_resource_owner_t *owner=slot->resource_owner;
    if(x==NULL || owner==NULL || r->instruction_owner==NULL)return COG_ERROR_UNSUPPORTED;
    size_t incoming=cli_resource_frame_index(m->kind);
    bool retained=incoming<CLI_RESOURCE_FRAME_COUNT &&
        ((x->frames[incoming].length!=0U && x->frames[incoming].wire==m->wire_id) ||
         m->wire_id<=x->retired_wire[incoming]);
    if(!retained && x->frozen)return COG_ERROR_INDETERMINATE;
    if(!retained && x->active && r->now_ms>=x->deadline_ms) {
        x->frozen=true;slot->effects_uncertain=true;return COG_ERROR_TIMEOUT;
    }
    cog_result_t rc=cli_resource_exchange_receive(x,m,event->data,event->data_len);
    if(rc==COG_ERROR_AGAIN) {
        size_t i=cli_resource_frame_index(m->kind);
        if(i+1U<CLI_RESOURCE_FRAME_COUNT && x->frames[i].wire==m->wire_id && x->frames[i+1U].length!=0U) {
            x->send_index=i+1U;x->send_pending=true;
        }
        return COG_OK;
    }
    if(rc!=COG_OK)return rc;
    if(m->kind==COG_CLI_CONTROL_RESOURCE_REQUEST) {
        cli_control_lane_t *lane=&slot->outbound[COG_CLI_LANE_RESOURCE-1U];
        if(lane->next_wire==0U || lane->next_wire>UINT64_MAX-2U)return COG_ERROR_LIMIT;
        owner->reply_wire=lane->next_wire;lane->next_wire+=2U;
        if(r->now_ms>UINT64_MAX-CLI_RESOURCE_TIMEOUT_MS)return COG_ERROR_LIMIT;
        x->deadline_ms=r->now_ms+CLI_RESOURCE_TIMEOUT_MS;
        if(slot->deadline_ms!=0U && slot->deadline_ms<x->deadline_ms)x->deadline_ms=slot->deadline_ms;
        return COG_OK;
    }
    if(m->kind==COG_CLI_CONTROL_RESOURCE_CONSUMED) {
        if(x->commit.outcome==COG_CLI_INSTRUCTION_COMMIT_UNCERTAIN) {
            x->frozen=true;x->send_pending=false;return COG_ERROR_INDETERMINATE;
        }
        rc=cli_resource_owner_retire(owner);
        if(rc==COG_OK)cli_resource_exchange_reset(x);
        return rc;
    }
    if(m->kind!=COG_CLI_CONTROL_RESOURCE_COMMIT)return COG_ERROR_PROTOCOL;
    x->send_pending=false;
    if(x->commit.outcome==COG_CLI_INSTRUCTION_COMMIT_SAVED) {
        if(owner->candidate==NULL || owner->proof==NULL)return COG_ERROR_PROTOCOL;
        rc=cli_resource_commit_prove(r->catalog,slot->owner,slot->session,&x->request,
            owner->proof,&owner->report,&x->result.candidate,
            x->commit.checkpoint_generation,&owner->saved_proof);
        if(rc!=COG_OK){x->frozen=true;slot->effects_uncertain=true;return rc;}
        cog_instruction_snapshot_free(owner->current);owner->current=owner->candidate;owner->candidate=NULL;
    } else if(x->commit.outcome==COG_CLI_INSTRUCTION_COMMIT_UNCERTAIN) {
        slot->effects_uncertain=true;
    }
    cog_cli_instruction_resource_t ready=x->commit;ready.kind=COG_CLI_INSTRUCTION_RESOURCE_READY;
    cog_cli_control_message_t reply=host_resource_envelope(r,slot,COG_CLI_CONTROL_RESOURCE_READY,
        owner->reply_wire+1U,m->wire_id);
    rc=cli_resource_exchange_build(x,&reply,&ready);
    if(rc==COG_OK){x->send_index=3U;x->send_pending=true;}
    return rc;
}

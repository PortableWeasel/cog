/* Native worker half of the one retained CGIR exchange. Included after native
 * envelope/copy-send helpers. No UI events or ordinary control-lane arenas. */
static cog_cli_control_message_t worker_resource_envelope(cog_cli_worker_host_t *worker,
    cli_executor_slot_t *slot,cog_cli_control_kind_t kind,uint64_t wire,uint64_t reply) {
    return (cog_cli_control_message_t){.kind=kind,.lane=COG_CLI_LANE_RESOURCE,
        .runtime_incarnation=worker->runtime_incarnation,.sender_incarnation=worker->fixed.worker.incarnation,
        .wire_id=wire,.reply_to=reply,.worker=slot->worker,.owner_peer=slot->owner_peer,
        .agent_id=slot->agent,.execution_epoch=slot->attempt,.session_id=slot->session};
}
static cog_result_t worker_resource_prepare(cog_cli_worker_host_t *worker,cli_executor_slot_t *slot) {
    if(slot->app==NULL || slot->app->runtime_owner==NULL)return COG_OK;
    cog_runtime_t *r=slot->app->runtime_owner;
    if(r->instructions==NULL)return COG_OK;
    if(slot->resource==NULL)return COG_ERROR_LIMIT;
    r->remote_instructions=true;
    cli_resource_exchange_t *x=slot->resource;
    if(x->frozen)return COG_ERROR_INDETERMINATE;
    if(x->active && worker->now_ms>=x->deadline_ms) {
        x->frozen=true;slot->app->recovery=true;return COG_ERROR_TIMEOUT;
    }
    if(!x->active) {
        if(r->instruction_job==NULL)return COG_OK;
        cli_control_lane_t *lane=&slot->outbound[COG_CLI_LANE_RESOURCE-1U];
        if(lane->next_wire==0U || lane->next_wire>UINT64_MAX-3U)return COG_ERROR_LIMIT;
        cog_store_owner_t owner;cog_store_checkpoint_t checkpoint;
        cog_result_t rc=cog_store_owner_read(slot->app->store,slot->session,&owner,&checkpoint);
        if(rc!=COG_OK)return rc;
        if(!same_owner(owner,slot->owner))return COG_ERROR_DENIED;
        cog_cli_instruction_resource_t request;
        rc=cli_runtime_remote_request(r,checkpoint.generation,&request);
        if(rc==COG_ERROR_AGAIN)return COG_OK;
        if(rc!=COG_OK)return rc;
        uint64_t wire=lane->next_wire;lane->next_wire+=3U;
        cog_cli_control_message_t m=worker_resource_envelope(worker,slot,
            COG_CLI_CONTROL_RESOURCE_REQUEST,wire,0U);
        rc=cli_resource_exchange_build(x,&m,&request);
        if(rc!=COG_OK){x->frozen=true;slot->app->recovery=true;return rc;}
        if(worker->now_ms>UINT64_MAX-CLI_RESOURCE_TIMEOUT_MS)return COG_ERROR_LIMIT;
        x->deadline_ms=worker->now_ms+CLI_RESOURCE_TIMEOUT_MS;
        if(slot->deadline_ms!=0U && slot->deadline_ms<x->deadline_ms)x->deadline_ms=slot->deadline_ms;
        uint64_t turn=slot->app->headless->absolute_deadline_ms;
        if(turn!=0U && turn<x->deadline_ms)x->deadline_ms=turn;
        x->send_index=0U;x->send_pending=true;
    }
    if(x->frames[1].length!=0U && x->frames[2].length==0U &&
        r->remote_outcome!=COG_CLI_INSTRUCTION_COMMIT_NONE) {
        cog_cli_instruction_resource_t commit={.kind=COG_CLI_INSTRUCTION_RESOURCE_COMMIT,
            .context=x->request.context,.candidate={.identity=x->result.candidate.identity},.outcome=r->remote_outcome,
            .checkpoint_generation=r->remote_checkpoint};
        cog_cli_control_message_t m=worker_resource_envelope(worker,slot,
            COG_CLI_CONTROL_RESOURCE_COMMIT,x->frames[0].wire+1U,x->frames[1].wire);
        cog_result_t rc=cli_resource_exchange_build(x,&m,&commit);
        if(rc!=COG_OK)return rc;
        x->send_index=2U;x->send_pending=true;
    }
    return COG_OK;
}
static cog_result_t worker_resource_receive(cog_cli_worker_host_t *worker,cli_executor_slot_t *slot,
    const cog_cli_control_message_t *message,size_t length) {
    if(slot->resource==NULL || slot->app==NULL || slot->app->runtime_owner==NULL)
        return COG_ERROR_INVALID_STATE;
    cli_resource_exchange_t *x=slot->resource;
    size_t incoming=cli_resource_frame_index(message->kind);
    bool retained=incoming<CLI_RESOURCE_FRAME_COUNT &&
        ((x->frames[incoming].length!=0U && x->frames[incoming].wire==message->wire_id) ||
         message->wire_id<=x->retired_wire[incoming]);
    if(!retained && x->frozen)return COG_ERROR_INDETERMINATE;
    if(!retained && x->active && worker->now_ms>=x->deadline_ms) {
        x->frozen=true;slot->app->recovery=true;return COG_ERROR_TIMEOUT;
    }
    cog_result_t rc=cli_resource_exchange_receive(x,message,worker->receive_secret,length);
    if(rc==COG_ERROR_AGAIN) {
        /* A retained reply re-arms its exact successor, never another save. */
        size_t i=cli_resource_frame_index(message->kind);
        if(i+1U<CLI_RESOURCE_FRAME_COUNT && x->frames[i].wire==message->wire_id && x->frames[i+1U].length!=0U) {
            x->send_index=i+1U;x->send_pending=true;
        }
        return COG_OK;
    }
    if(rc!=COG_OK)return rc;
    if(message->kind==COG_CLI_CONTROL_RESOURCE_RESULT) {
        x->send_pending=false;
        rc=cli_runtime_remote_result(slot->app->runtime_owner,&x->result);
        if(rc==COG_OK)rc=worker_resource_prepare(worker,slot);
        return rc;
    }
    if(message->kind!=COG_CLI_CONTROL_RESOURCE_READY)return COG_ERROR_PROTOCOL;
    cog_cli_instruction_resource_t consumed={.kind=COG_CLI_INSTRUCTION_RESOURCE_CONSUMED,
        .context=x->request.context};
    cog_cli_control_message_t m=worker_resource_envelope(worker,slot,
        COG_CLI_CONTROL_RESOURCE_CONSUMED,x->frames[0].wire+2U,x->frames[3].wire);
    rc=cli_resource_exchange_build(x,&m,&consumed);
    if(rc==COG_OK){x->send_index=4U;x->send_pending=true;}
    return rc;
}
static cog_result_t worker_resource_send(cog_cli_worker_host_t *worker,cli_executor_slot_t *slot) {
    cli_resource_exchange_t *x=slot->resource;
    if(x==NULL || !x->send_pending)return COG_ERROR_AGAIN;
    if(slot->agent!=0U) {
        const cli_control_lane_t *start=&slot->inbound[COG_CLI_LANE_DELEGATE-1U];
        if(!start->final_valid || start->final_reply_wire==0U ||
            start->copied_wire!=start->final_reply_wire)return COG_ERROR_AGAIN;
    }
    cli_resource_frame_t *f=&x->frames[x->send_index];
    cli_host_bytes_t view={f->bytes,f->capacity,f->length};
    cog_result_t rc=retained_send(worker,&view,f->wire,&x->copied_wire,&x->next_send_ms,
        x->send_index==4U);
    if(rc==COG_OK && x->send_index==4U) {
        rc=cli_runtime_remote_release(slot->app->runtime_owner,&x->ready);
        if(rc!=COG_OK){x->frozen=true;return rc;}
        cli_resource_exchange_reset(x);
    }
    return rc;
}

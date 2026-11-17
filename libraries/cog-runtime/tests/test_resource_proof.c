#define _POSIX_C_SOURCE 200809L
#include "../src/resource_proof.c"
#include <cog/instruction_codec.h>
#include <assert.h>
#include <cog/tool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
static void add(cog_session_t *s,uint64_t id,const cog_cli_instruction_capsule_t *c)
{
    cog_session_context_t v={id,COG_INSTRUCTION_CONTEXT_NAMESPACE,COG_INSTRUCTION_CONTEXT_FORMAT,c->bytes,c->length};
    assert(cog_session_context_add(s,&v)==COG_OK);
    cog_session_snapshot_t state;assert(cog_session_snapshot_view(s,&state)==COG_OK);
    cog_session_context_binding_t b={id,state.selected_context_binding_id,state.active_node_id,id};
    assert(cog_session_context_publish(s,&b)==COG_OK);
}
static const cog_tool_t tool={.name="activate_skill",.description="activate",.input_schema="{\"type\":\"object\"}"};
static void run_case(cog_prompt_tool_policy_t policy,bool omit,int uncertainty)
{
    char path[]="resource-proof-XXXXXX";assert(mkdtemp(path));
    cog_instruction_limits_t limits={8,8,4096,4096,4096,65536,32,8,8,8};
    cog_instruction_catalog_t *catalog=NULL;assert(cog_instruction_catalog_new(&catalog,&limits,1,NULL,0,NULL)==COG_OK);
    cog_prompt_tool_binding_t binding={&tool,policy};
    cog_prompt_tool_binding_t host_binding={&tool,COG_PROMPT_TOOL_ALLOW};
    cog_instruction_selection_t selection={.snapshot_id=1,.workspace_root_id="root",.scope="",.model_id="model",.template_id="template",.template_revision=1,
        .max_context_bytes=16384,.max_output_tokens=128,.max_tool_rounds=4,.bindings=omit ? NULL : &binding,.binding_count=omit ? 0U : 1U};
    cog_instruction_snapshot_t *snap=NULL;assert(cog_instruction_snapshot_new(&snap,catalog,&limits,&selection,NULL,0,NULL)==COG_OK);
    cog_instruction_manifest_t manifest;
    unsigned char oldbytes[16384],newbytes[16384];size_t n;
    cog_cli_instruction_capsule_t old={.identity={.catalog_generation=1,.snapshot_id=1},.bytes=oldbytes};
    assert(cog_instruction_snapshot_view(snap,&manifest)==COG_OK);
    assert(cog_instruction_manifest_encode(&manifest,oldbytes,sizeof(oldbytes),&n)==COG_OK);old.length=n;
    assert(cog_instruction_snapshot_fingerprint(snap,old.identity.sha256)==COG_OK);cog_instruction_snapshot_free(snap);
    selection.snapshot_id=2;assert(cog_instruction_snapshot_new(&snap,catalog,&limits,&selection,NULL,0,NULL)==COG_OK);
    cog_cli_instruction_capsule_t candidate={.identity={.catalog_generation=1,.snapshot_id=2},.bytes=newbytes};
    assert(cog_instruction_snapshot_view(snap,&manifest)==COG_OK);
    assert(cog_instruction_manifest_encode(&manifest,newbytes,sizeof(newbytes),&n)==COG_OK);candidate.length=n;
    assert(cog_instruction_snapshot_fingerprint(snap,candidate.identity.sha256)==COG_OK);cog_instruction_snapshot_free(snap);cog_instruction_catalog_free(catalog);
    cog_session_t *session=NULL; cog_session_config_t cfg={.id="session",.max_nodes=16,.max_payload_bytes=65536,
        .max_tool_calls_per_message=2,.max_contexts=4,.max_context_bindings=4,.max_context_bytes=65536};
    assert(cog_session_new(&session,&cfg)==COG_OK);add(session,1,&old);
    cog_store_t *store=NULL;cog_store_config_t sc={.base_dir=path,.require_owner=true,.max_record_bytes=262144};
    assert(cog_store_new(&store,&sc)==COG_OK);cog_store_owner_t owner={1,123};cog_store_owner_guard_t *guard=NULL;
    assert(cog_store_owner_compare_exchange(store,"session",(cog_store_owner_t){0},owner,(cog_store_checkpoint_t){0})==COG_OK);
    assert(cog_store_owner_acquire(store,"session",owner,&guard)==COG_OK);cog_store_checkpoint_t cp;
    assert(cog_store_save(store,session)==COG_OK);
    assert(cog_store_owner_read(store,"session",&owner,&cp)==COG_OK);
    cog_message_tool_call_t calls[]={{"request","activate_skill","{}",2},{"stale","activate_skill","{}",2}};
    cog_message_t assistant={.role=COG_MESSAGE_ROLE_ASSISTANT,.tool_calls=calls,.tool_call_count=2};
    cog_session_node_t *a=NULL;assert(cog_session_append(session,&assistant,&a)==COG_OK);
    assert(cog_store_save(store,session)==COG_OK);
    assert(cog_store_owner_read(store,"session",&owner,&cp)==COG_OK);
    assert(cog_session_tool_transition(session,a,"request",COG_TOOL_EXECUTION_PROPOSED,COG_TOOL_EXECUTION_STARTED)==COG_OK);
    if(uncertainty==1)assert(cog_session_tool_transition(session,a,"request",COG_TOOL_EXECUTION_STARTED,COG_TOOL_EXECUTION_INDETERMINATE)==COG_OK);
    if(uncertainty==2)assert(cog_session_tool_transition(session,a,"stale",COG_TOOL_EXECUTION_PROPOSED,COG_TOOL_EXECUTION_STARTED)==COG_OK);
    assert(cog_store_save(store,session)==COG_OK);
    assert(cog_store_owner_read(store,"session",&owner,&cp)==COG_OK);
    cog_cli_instruction_resource_t request={.kind=COG_CLI_INSTRUCTION_RESOURCE_REQUEST,.context=old.identity,
        .operation=COG_CLI_INSTRUCTION_RESOURCE_ACTIVATE,.assistant_node_id=cog_node_data(a)->node_id,.call_id="request",
        .request_checkpoint_generation=cp.generation};
    cog_session_t *proof=NULL,*saved=NULL;cog_store_load_report_t report;
    cog_result_t proved=cli_resource_request_prove(store,owner,"session",&request,&old,&host_binding,1,&limits,&proof,&report);
    if(policy!=COG_PROMPT_TOOL_ALLOW || omit || uncertainty!=0) {
        assert(proved==COG_ERROR_DENIED && proof==NULL);
        cog_session_free(session);assert(cog_store_owner_release(guard)==COG_OK);cog_store_free(store);return;
    }
    assert(proved==COG_OK);
    cog_session_snapshot_t p;assert(cog_session_snapshot_view(proof,&p)==COG_OK);
    assert(p.ledger[0].state==COG_TOOL_EXECUTION_INDETERMINATE && p.audit_high_water>report.source_audit_high_water);
    cog_session_t *bad=NULL;request.call_ordinal=1;
    assert(cli_resource_request_prove(store,owner,"session",&request,&old,&binding,1,&limits,&bad,&report)==COG_ERROR_PROTOCOL && bad==NULL);
    request.call_ordinal=0;
    assert(cli_resource_request_prove(store,(cog_store_owner_t){2,123},"session",&request,&old,&binding,1,&limits,&bad,&report)==COG_ERROR_INVALID_STATE);
    assert(cog_session_tool_transition(session,a,"request",COG_TOOL_EXECUTION_STARTED,COG_TOOL_EXECUTION_SUCCEEDED)==COG_OK);
    cog_message_t result={.role=COG_MESSAGE_ROLE_TOOL,.tool_call_id="request",.content="done",.content_len=4};
    assert(cog_session_append(session,&result,NULL)==COG_OK);
    assert(cog_session_tool_transition(session,a,"stale",COG_TOOL_EXECUTION_PROPOSED,COG_TOOL_EXECUTION_DENIED)==COG_OK);
    result.tool_call_id="stale";assert(cog_session_append(session,&result,NULL)==COG_OK);
    add(session,2,&candidate);assert(cog_store_save(store,session)==COG_OK);
    assert(cog_store_owner_read(store,"session",&owner,&cp)==COG_OK);
    assert(cli_resource_commit_prove(store,owner,"session",&request,proof,&report,&candidate,cp.generation,&saved)==COG_OK);
    cog_session_snapshot_t committed;assert(cog_session_snapshot_view(saved,&committed)==COG_OK);
    cog_store_load_report_t saved_report={.source_audit_high_water=committed.audit_high_water};
    assert(commit_state_valid(&p,&committed,&request,&report,&saved_report,&candidate)==COG_OK);
    cog_node_data_t nodes[16];memcpy(nodes,committed.nodes,committed.node_count*sizeof(*nodes));
    cog_session_snapshot_t forged=committed;forged.nodes=nodes;
    nodes[0].chunk_count++;
    assert(commit_state_valid(&p,&forged,&request,&report,&saved_report,&candidate)==COG_ERROR_PROTOCOL);
    nodes[0]=committed.nodes[0];nodes[0].timing.duration_ms++;
    assert(commit_state_valid(&p,&forged,&request,&report,&saved_report,&candidate)==COG_ERROR_PROTOCOL);
    memcpy(nodes,committed.nodes,committed.node_count*sizeof(*nodes));
    nodes[1].context_binding_id=2U;
    assert(commit_state_valid(&p,&forged,&request,&report,&saved_report,&candidate)==COG_ERROR_PROTOCOL);
    memcpy(nodes,committed.nodes,committed.node_count*sizeof(*nodes));
    nodes[2].parent_id=0U;
    assert(commit_state_valid(&p,&forged,&request,&report,&saved_report,&candidate)==COG_ERROR_PROTOCOL);
    forged=committed;forged.ledger_count++;
    assert(commit_state_valid(&p,&forged,&request,&report,&saved_report,&candidate)==COG_ERROR_PROTOCOL);
    forged=committed;forged.context_binding_count++;
    assert(commit_state_valid(&p,&forged,&request,&report,&saved_report,&candidate)==COG_ERROR_PROTOCOL);
    cog_session_context_binding_t revisions[4];memcpy(revisions,committed.context_bindings,committed.context_binding_count*sizeof(*revisions));
    forged=committed;forged.context_bindings=revisions;revisions[1].previous_binding_id=0U;
    assert(commit_state_valid(&p,&forged,&request,&report,&saved_report,&candidate)==COG_ERROR_PROTOCOL);
    cog_tool_ledger_record_t ledgers[2];memcpy(ledgers,committed.ledger,sizeof(ledgers));
    forged=committed;forged.ledger=ledgers;ledgers[1].state=COG_TOOL_EXECUTION_SUCCEEDED;
    assert(commit_state_valid(&p,&forged,&request,&report,&saved_report,&candidate)==COG_ERROR_PROTOCOL);
    cog_session_free(saved);saved=NULL;
    assert(cli_resource_commit_prove(store,owner,"session",&request,proof,&report,&candidate,request.request_checkpoint_generation,&saved)==COG_ERROR_PROTOCOL);
    candidate.identity.sha256[0]^=1U;
    assert(cli_resource_commit_prove(store,owner,"session",&request,proof,&report,&candidate,cp.generation,&saved)!=COG_OK);
    cog_session_free(proof);cog_session_free(session);assert(cog_store_owner_release(guard)==COG_OK);cog_store_free(store);
    /* The test store remains beneath CTest's build directory for failure inspection. */
}
int main(void)
{
    run_case(COG_PROMPT_TOOL_ALLOW,false,0);
    run_case(COG_PROMPT_TOOL_ALLOW,false,1);
    run_case(COG_PROMPT_TOOL_ALLOW,false,2);
    run_case(COG_PROMPT_TOOL_ASK,false,0);
    run_case(COG_PROMPT_TOOL_DENY,false,0);
    run_case(COG_PROMPT_TOOL_ALLOW,true,0);
    return 0;
}

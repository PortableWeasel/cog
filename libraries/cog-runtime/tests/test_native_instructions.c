#define _POSIX_C_SOURCE 200809L
#include "../src/execution_internal.h"
#include "../src/runtime_internal.h"
#include "../src/composition_internal.h"
#include <cog/openrouter.h>
#include <cog/runtime_instructions.h>
#include <cog/workspace.h>
#include <curl/curl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#define CHECK(x) do { if(!(x)){fprintf(stderr,"native instructions line %d: %s (rc=%d)\n",__LINE__,#x,(int)rc);failed=true;goto finish;} }while(0)
static uint64_t now_ms(void) {struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t)!=0)return 0;return (uint64_t)t.tv_sec*1000U+(uint64_t)t.tv_nsec/1000000U;}
static void diagnose(cog_cli_host_runtime_t *host,cli_executor_slot_t *slot) {
    const cog_cli_instruction_resource_t *request=&slot->resource->request;
    unsigned char hash[32]={0};
    cog_result_t rc=cog_instruction_snapshot_fingerprint(slot->resource_owner->current,hash);
    fprintf(stderr,"request cp=%llu node=%llu ordinal=%u fingerprint_rc=%d same=%d\n",
        (unsigned long long)request->request_checkpoint_generation,
        (unsigned long long)request->assistant_node_id,(unsigned)request->call_ordinal,
        (int)rc,memcmp(hash,request->context.sha256,32U)==0);
    cog_session_t *proof=NULL;cog_store_load_report_t report={0};
    rc=cog_store_checkpoint_load(host->catalog,slot->session,
        (cog_store_checkpoint_t){request->request_checkpoint_generation},COG_STORE_LOAD_INSPECT,&proof,&report);
    fprintf(stderr,"diagnostic load=%d cp=%llu audit=%llu\n",(int)rc,
        (unsigned long long)report.checkpoint_generation,(unsigned long long)report.source_audit_high_water);
    if(rc==COG_OK) {
        cog_session_snapshot_t state;
        rc=cog_session_snapshot_view(proof,&state);
        if(rc==COG_OK) {
            fprintf(stderr,"selected=%llu active=%llu contexts=%zu bindings=%zu ledger=%zu\n",
                (unsigned long long)state.selected_context_binding_id,(unsigned long long)state.active_node_id,
                state.context_count,state.context_binding_count,state.ledger_count);
            for(size_t i=0U;i<state.node_count;i++)if(state.nodes[i].node_id==request->assistant_node_id)
                fprintf(stderr,"assistant binding=%llu calls=%zu\n",
                    (unsigned long long)state.nodes[i].context_binding_id,
                    state.nodes[i].message==NULL?0U:state.nodes[i].message->tool_call_count);
            for(size_t i=0U;i<state.ledger_count;i++)fprintf(stderr,"ledger node=%llu state=%d audit=%zu\n",
                (unsigned long long)state.ledger[i].assistant_node_id,(int)state.ledger[i].state,state.ledger[i].audit_count);
        }
    }
    cog_session_free(proof);
}
int main(int argc,char **argv) {
    bool failed=false,global=false,submitted=false,complete=false,native_seen=false,saved_seen=false;uint64_t reported_result=0U;
    cog_result_t rc=COG_OK;cog_runtime_t *runtime=NULL;cog_net_ctx_t *net=NULL;
    cog_instruction_catalog_t *catalog=NULL;cog_instruction_snapshot_t *snapshot=NULL;
    CHECK(argc==7); /* worker executable, CA, URL, workspace, store, worker socket */
    CHECK(curl_global_init(CURL_GLOBAL_DEFAULT)==CURLE_OK);global=true;
    cog_net_config_t transport={.host_runtime_initialized=true,.dependency_memory=COG_NET_DEPENDENCY_MEMORY_DYNAMIC,
        .max_request_bytes=1048576,.max_connections=2,.ca_bundle=argv[2],.connect_timeout_ms=3000,.stall_timeout_ms=10000};
    rc=cog_net_new(&net,&transport);CHECK(rc==COG_OK);
    cog_openrouter_config_t provider={.net=net,.limits={.request_bytes=1048576,.parser_bytes=1048576,
        .writer_bytes=1048576,.max_json_depth=64,.max_frame_bytes=262144,.retained_delta_bytes=262144,
        .retained_delta_count=32,.max_text_bytes=65536,.max_tool_calls=16,.max_tool_id_bytes=256,
        .max_tool_name_bytes=256,.max_tool_arguments_bytes=65536,.max_opaque_bytes=65536}};
    static const cog_model_t model={.id="fixture/model",.capabilities=COG_MODEL_CAPABILITY_STREAMING|COG_MODEL_CAPABILITY_TOOLS};
    static const cog_model_table_t models={&model,1};
    cog_endpoint_t endpoint={.name="openrouter",.vtable=&cog_openrouter_provider_vtable,
        .models=&models,.base_url=argv[3],.api_key="synthetic-native",.implementation_config=&provider,.generation=1};
    cog_prompt_tool_binding_t bindings[]={
        {&cog_runtime_delegation_tools.tools[0],COG_PROMPT_TOOL_ALLOW},
        {&cog_runtime_activate_skill_tool,COG_PROMPT_TOOL_ALLOW},
        {&cog_runtime_read_skill_resource_tool,COG_PROMPT_TOOL_ALLOW},
        {&cog_workspace_tools.tools[0],COG_PROMPT_TOOL_ALLOW}};
    cog_runtime_scope_binding_t scope={&cog_workspace_tools.tools[0],"path",NULL,true};
    cog_instruction_source_root_t root={"root",argv[4]};
    cog_instruction_source_config_t sources={&root,1,{1,4096,8192,65536,16,16,2,16}};
    cog_runtime_workers_t workers={.executable=argv[1],.socket_path=argv[6],.workspace_root=argv[4],
        .net_config=&transport,.frame_bytes=2097152,.instruction_bytes=65536,.workers=1,.children=2,
        .request_budget=32,.tool_mask=1,.shutdown_ms=5000};
    cog_runtime_config_t config={.mode=COG_RUNTIME_NEW,.instruction_mode=COG_RUNTIME_INSTRUCTIONS_PINNED,
        .session_id="native-instructions",.store={.backend=&cog_store_file_backend,.base_dir=argv[5],.require_owner=true},
        .session={.max_nodes=1024,.max_payload_bytes=2097152,.max_tool_calls_per_message=16,
            .max_contexts=32,.max_context_bindings=64,.max_context_bytes=2097152},
        .limits={.max_input_bytes=4096,.max_queued_inputs=1,.max_view_bytes=1048576,
            .max_context_bytes=65536,.max_tool_output_bytes=8192,.max_instruction_bytes=65536,
            .max_events=128,.max_dispatch_per_step=1,.max_output_tokens=512,.max_tool_rounds=8,
            .max_turn_ms=45000,.max_tool_ms=35000,.max_retries=0,.max_retry_elapsed_ms=1000},
        .provider={.endpoint=&endpoint,.model_id=model.id,.prompt_template=&cog_openrouter_prompt_template,
            .compiler_limits={256,128,16,64,65536,262144,65536}},
        .tools={.table=&cog_workspace_tools,.bindings=bindings,.binding_count=4},
        .workspace_root_id="root",.scopes=&scope,.scope_count=1,.sources=&sources,.workers=&workers,
        .instruction_limits={32,16,4096,8192,4096,262144,128,16,16,16}};
    static const char role[]="---\ncog-agent: 1\nname: reader\ndescription: Read with skills\ntools: read_file activate_skill read_skill_resource\n---\nNAMED_CHILD_ROLE\n";
    static const char skill[]="---\nname: check\ndescription: Check resource\n---\nPRIVATE_SKILL_BODY\n";
    cog_instruction_source_t input[]={
        {COG_INSTRUCTION_HOST,"root","host.md","",NULL,0,"HOST_ROOT",9},
        {COG_INSTRUCTION_AGENT,"root","agents/reader.md","",NULL,0,role,sizeof(role)-1},
        {COG_INSTRUCTION_SKILL,"root","skills/check/SKILL.md","",NULL,0,skill,sizeof(skill)-1}};
    rc=cog_instruction_catalog_new(&catalog,&config.instruction_limits,1,input,3,NULL);CHECK(rc==COG_OK);
    cog_instruction_selection_t selection={.snapshot_id=1,.workspace_root_id="root",.scope="",
        .model_id=model.id,.template_id=cog_openrouter_prompt_template.id,.template_revision=cog_openrouter_prompt_template.revision,
        .max_context_bytes=65536,.max_output_tokens=512,.max_tool_rounds=8,.bindings=bindings,.binding_count=4};
    rc=cog_instruction_snapshot_new(&snapshot,catalog,&config.instruction_limits,&selection,NULL,0,NULL);CHECK(rc==COG_OK);
    config.instructions=snapshot;
    rc=cog_runtime_new(&runtime,&config);CHECK(rc==COG_OK);
    rc=cog_runtime_begin(runtime);CHECK(rc==COG_OK);
    uint64_t deadline=now_ms()+55000U;
    while(now_ms()<deadline && !complete) {
        cog_runtime_view_t view;
        rc=cog_runtime_view(runtime,&view);
        if(rc==COG_OK) {
            for(size_t i=0;i<view.item_count;i++)
                if(view.items[i].role==COG_MESSAGE_ROLE_ASSISTANT && view.items[i].text_bytes==11U &&
                    memcmp(view.items[i].text,"PARENT_DONE",11U)==0)complete=true;
            rc=cog_runtime_ack(runtime,view.serial);CHECK(rc==COG_OK);
            if(!submitted) {rc=cog_runtime_submit(runtime,"delegate named children",23U,false);CHECK(rc==COG_OK);submitted=true;}
        } else CHECK(rc==COG_ERROR_AGAIN);
        cog_runtime_step_t step;rc=cog_runtime_step(runtime,&step);
        if(rc!=COG_OK && rc!=COG_ERROR_AGAIN)for(size_t i=2U;i<runtime->host->bounds.executors;i++) {
            cli_executor_slot_t *slot=&runtime->host->executors[i];
            if(slot->resource==NULL || !slot->resource->active)continue;
            cli_resource_exchange_t *x=slot->resource;
            fprintf(stderr,"failure slot primary=%d frozen=%d phases=%zu,%zu,%zu,%zu,%zu commit=%d cp=%llu owner=%llu/%llu\n",
                (int)slot->primary,x->frozen,x->frames[0].length,x->frames[1].length,x->frames[2].length,
                x->frames[3].length,x->frames[4].length,(int)x->commit.outcome,
                (unsigned long long)x->commit.checkpoint_generation,
                (unsigned long long)slot->owner.epoch,(unsigned long long)slot->owner.instance_id);
            cog_store_owner_t actual={0};cog_store_checkpoint_t latest={0};
            cog_result_t read=cog_store_owner_read(runtime->host->catalog,slot->session,&actual,&latest);
            fprintf(stderr,"actual owner read=%d owner=%llu/%llu cp=%llu\n",(int)read,
                (unsigned long long)actual.epoch,(unsigned long long)actual.instance_id,
                (unsigned long long)latest.generation);
        }
        CHECK(rc==COG_OK || rc==COG_ERROR_AGAIN);
        CHECK(!step.done);
        for(size_t i=2;i<runtime->host->bounds.executors;i++) {
            cli_executor_slot_t *slot=&runtime->host->executors[i];
            if(slot->agent!=0 && slot->owner_peer!=0)native_seen=true;
            if(slot->resource!=NULL && slot->resource->result.result!=COG_OK &&
                slot->resource->frames[1].length!=0U &&
                slot->resource->frames[1].wire!=reported_result) {
                reported_result=slot->resource->frames[1].wire;
                fprintf(stderr,"native RESULT status=%d prepared=%d proof=%d candidate=%d\n",
                    (int)slot->resource->result.result,slot->resource_owner->prepared,
                    slot->resource_owner->proof!=NULL,slot->resource_owner->candidate!=NULL);
                if(slot->resource->result.result!=COG_OK)diagnose(runtime->host,slot);
            }
            if(slot->owner_peer!=0 && slot->resource_owner!=NULL && slot->resource_owner->current!=NULL) {
                cog_instruction_manifest_t m;
                CHECK(cog_instruction_snapshot_view(slot->resource_owner->current,&m)==COG_OK);
                if(m.snapshot_id>=4U && m.active_skill_count==1U)saved_seen=true;
            }
        }
        if(step.max_wait_ms!=0U)(void)poll(NULL,0,step.max_wait_ms>2U?2:(int)step.max_wait_ms);
    }
    CHECK(complete && native_seen && saved_seen);
finish:
    if(runtime!=NULL) {
        (void)cog_runtime_shutdown(runtime);bool done=false;
        uint64_t cleanup_deadline=now_ms()+10000U;
        while(now_ms()<cleanup_deadline && !done) {
            cog_runtime_view_t view;if(cog_runtime_view(runtime,&view)==COG_OK)(void)cog_runtime_ack(runtime,view.serial);
            cog_runtime_step_t step; (void)cog_runtime_step(runtime,&step);done=step.done;
            if(!done)(void)poll(NULL,0,2);
        }
        cog_result_t closed=cog_runtime_close(&runtime,COG_OK);
        if(closed!=COG_OK || runtime!=NULL)failed=true;
    }
    cog_instruction_snapshot_free(snapshot);cog_instruction_catalog_free(catalog);
    if(net!=NULL)cog_net_free(net);
    if(global)curl_global_cleanup();
    return failed?1:0;
}

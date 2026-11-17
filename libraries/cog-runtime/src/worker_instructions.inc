#include <cog/runtime_instructions.h>
/* Trusted builtin worker profile restoration. No source reader or discovery. */
#define WORKER_INSTRUCTION_TOOLS 16U
typedef struct worker_instruction_profile {
    cog_instruction_snapshot_t *snapshot;
    cog_instruction_manifest_t manifest;
    cog_prompt_tool_binding_t bindings[WORKER_INSTRUCTION_TOOLS];
    size_t binding_count;
    cog_instruction_limits_t limits;
    cog_provider_prompt_limits_t compiler_limits;
} worker_instruction_profile_t;
static cog_provider_limits_t worker_provider_limits(size_t bytes)
{
    return (cog_provider_limits_t){.request_bytes=bytes*4U,.parser_bytes=bytes*16U,
        .writer_bytes=bytes*8U,.max_json_depth=64U,.max_frame_bytes=bytes,
        .retained_delta_bytes=bytes,.retained_delta_count=16U,.max_text_bytes=bytes,
        .max_tool_calls=CLI_CALLS,.max_tool_id_bytes=CLI_ID_BYTES,
        .max_tool_name_bytes=CLI_ID_BYTES,.max_tool_arguments_bytes=CLI_ARGUMENT_BYTES,
        .max_opaque_bytes=bytes};
}
static cog_result_t worker_instruction_restore(cog_cli_worker_host_t *worker,
    const cog_cli_configuration_t *configuration,worker_instruction_profile_t *p)
{
    memset(p,0,sizeof(*p));
    const cog_cli_instruction_capsule_t *capsule=cli_configuration_instruction_capsule(configuration);
    if(capsule==NULL || capsule->length==0U) return COG_OK;
    size_t cap=worker->fixed.instruction_bytes;
    if(cap==0U) return COG_ERROR_UNSUPPORTED;
    if(capsule->length>cap || cap>SIZE_MAX/16U) return COG_ERROR_LIMIT;
    p->limits=(cog_instruction_limits_t){lesser(cap,1024U),lesser(cap,1024U),cap,cap,cap,cap*16U,lesser(cap,4096U),
        WORKER_INSTRUCTION_TOOLS,cap,cap};
    cog_instruction_snapshot_t *inspection=NULL;
    cog_result_t rc=cog_instruction_snapshot_decode(&inspection,&p->limits,capsule->bytes,
        capsule->length,COG_INSTRUCTION_INSPECT,NULL,0U);
    cog_instruction_manifest_t m;
    if(rc==COG_OK) rc=cog_instruction_snapshot_view(inspection,&m);
    const cog_cli_options_t *o=cog_cli_configuration_options(configuration);
    uint32_t mask=0U;
    if(rc==COG_OK) rc=cog_cli_configuration_tool_mask(configuration,&mask);
    if(rc==COG_OK && (m.catalog_generation!=capsule->identity.catalog_generation ||
        m.snapshot_id!=capsule->identity.snapshot_id || strcmp(m.model_id,o->model_id)!=0 ||
        strcmp(m.template_id,cog_openrouter_prompt_template.id)!=0 ||
        m.template_revision!=cog_openrouter_prompt_template.revision ||
        m.max_context_bytes>o->max_context_bytes || m.max_output_tokens>o->max_output_tokens ||
        m.max_tool_rounds>o->max_tool_rounds)) rc=COG_ERROR_PROTOCOL;
    if(rc==COG_OK && m.tool_count>WORKER_INSTRUCTION_TOOLS) rc=COG_ERROR_LIMIT;
    for(size_t i=0U;rc==COG_OK && i<m.tool_count;++i) {
        const cog_tool_t *descriptor=NULL;
        bool allowed=true;
        for(size_t j=0U;j<cog_workspace_tools.count;++j)
            if(strcmp(m.tools[i].name,cog_workspace_tools.tools[j].name)==0) {
                descriptor=&cog_workspace_tools.tools[j];allowed=j<4U && (mask&(1U<<j))!=0U;
            }
        for(size_t j=0U;j<cog_runtime_delegation_tools.count;++j)
            if(strcmp(m.tools[i].name,cog_runtime_delegation_tools.tools[j].name)==0)
                descriptor=&cog_runtime_delegation_tools.tools[j];
        if(strcmp(m.tools[i].name,cog_runtime_activate_skill_tool.name)==0)
            descriptor=&cog_runtime_activate_skill_tool;
        if(strcmp(m.tools[i].name,cog_runtime_read_skill_resource_tool.name)==0)
            descriptor=&cog_runtime_read_skill_resource_tool;
        if(descriptor==NULL) { rc=COG_ERROR_UNSUPPORTED;break; }
        cog_prompt_tool_policy_t policy=o->tool_policy==COG_CLI_TOOL_POLICY_ALLOW ? COG_PROMPT_TOOL_ALLOW :
            o->tool_policy==COG_CLI_TOOL_POLICY_ASK ? COG_PROMPT_TOOL_ASK : COG_PROMPT_TOOL_DENY;
        if(!allowed) policy=COG_PROMPT_TOOL_DENY;
        p->bindings[p->binding_count++]=(cog_prompt_tool_binding_t){descriptor,policy};
    }
    if(rc==COG_OK) rc=cog_instruction_snapshot_restore(&p->snapshot,&p->limits,&m,
        COG_INSTRUCTION_EXECUTE,p->bindings,p->binding_count,NULL);
    (void)cog_instruction_snapshot_free(inspection);
    if(rc==COG_OK) rc=cog_instruction_snapshot_view(p->snapshot,&p->manifest);
    if(rc==COG_OK) p->compiler_limits=(cog_provider_prompt_limits_t){
        .max_messages=CLI_NODES+lesser(cap,4096U),.max_fragments=lesser(cap,4096U),.max_tools=WORKER_INSTRUCTION_TOOLS,
        .max_json_depth=64U,.max_context_bytes=o->max_context_bytes,
        .max_request_bytes=cap*16U,.scratch_bytes=cap*16U};
    if(rc!=COG_OK) { (void)cog_instruction_snapshot_free(p->snapshot);p->snapshot=NULL; }
    return rc;
}
static cog_result_t worker_instruction_preflight(cog_cli_worker_host_t *worker,
    const cog_cli_configuration_t *configuration)
{
    worker_instruction_profile_t profile;
    cog_result_t rc=worker_instruction_restore(worker,configuration,&profile);
    if(rc!=COG_OK || profile.snapshot==NULL) return rc;
    cog_cli_configuration_t *bound=NULL;
    const cog_cli_options_t *o=cog_cli_configuration_options(configuration);
    rc=cog_cli_configuration_clone_new(&bound,configuration,o->net);
    cog_provider_limits_t limits=worker_provider_limits(o->max_context_bytes);
    if(rc==COG_OK) rc=cog_cli_configuration_bind(bound,&cog_openrouter_provider_vtable,
        NULL,NULL,&limits);
    cog_provider_prompt_compiler_t *compiler=NULL;
    cog_provider_t *provider=NULL;
    if(rc==COG_OK) rc=cog_provider_prompt_compiler_new(&compiler,&cog_openrouter_prompt_template,
        &profile.compiler_limits);
    cog_provider_prompt_input_t input={0};cog_provider_request_t request={0};
    if(rc==COG_OK) rc=cog_instruction_prompt_view(profile.snapshot,&input.prompt);
    input.request=(cog_provider_request_t){.model_id=o->model_id,.stream=true,
        .max_tokens=profile.manifest.max_output_tokens,
        .configuration_generation=cli_configuration_generation(configuration)};
    if(rc==COG_OK) rc=cog_provider_prompt_compile(compiler,&cog_openrouter_provider_vtable,&input,&request);
    if(rc==COG_OK) rc=cog_provider_connect(cog_cli_configuration_endpoint(bound),&provider);
    if(rc==COG_OK) rc=cog_provider_preflight(provider,&request,COG_PROVIDER_PREFLIGHT_COMPLETE);
    if(provider!=NULL) { cog_result_t closed=cog_provider_disconnect(provider);if(rc==COG_OK)rc=closed; }
    (void)cog_provider_prompt_compiler_free(compiler);
    cog_cli_configuration_free(bound);(void)cog_instruction_snapshot_free(profile.snapshot);
    return rc;
}
static cog_result_t worker_instruction_execution(cog_cli_worker_host_t *worker,
    cog_cli_app_t **out,cog_cli_execution_admission_t *admission)
{
    const cog_cli_instruction_capsule_t *capsule=cli_configuration_instruction_capsule(admission->configuration);
    if(capsule==NULL || capsule->length==0U) return cog_cli_execution_new(out,admission);
    worker_instruction_profile_t p;
    cog_result_t rc=worker_instruction_restore(worker,admission->configuration,&p);
    const cog_cli_options_t *o=cog_cli_configuration_options(admission->configuration);
    cog_provider_limits_t provider_limits=worker_provider_limits(o->max_context_bytes);
    if(rc==COG_OK) rc=cog_cli_configuration_bind(admission->configuration,
        &cog_openrouter_provider_vtable,NULL,NULL,&provider_limits);
    cog_runtime_config_t config={0};
    if(rc==COG_OK) {
        config.mode=admission->kind==COG_CLI_EXECUTION_STAGED ? COG_RUNTIME_RESUME : COG_RUNTIME_NEW;
        config.instruction_mode=COG_RUNTIME_INSTRUCTIONS_PINNED;
        config.session_id=admission->session_id;
        config.session=(cog_session_config_t){.max_nodes=CLI_NODES,
            .max_payload_bytes=o->max_context_bytes*16U,.max_tool_calls_per_message=CLI_CALLS,
            .max_contexts=16U,.max_context_bindings=16U,
            .max_context_bytes=worker->fixed.instruction_bytes*16U};
        config.store=(cog_store_config_t){.base_dir=o->store_dir,.require_owner=true,
            .max_record_bytes=o->max_context_bytes*16U};
        config.limits=(cog_runtime_limits_t){.max_input_bytes=o->max_context_bytes,
            .max_queued_inputs=1U,.max_view_bytes=o->max_context_bytes*4U,
            .max_context_bytes=o->max_context_bytes,.max_tool_output_bytes=o->max_tool_output_bytes,
            .max_instruction_bytes=worker->fixed.instruction_bytes,.max_events=64U,
            .max_dispatch_per_step=1U,.max_output_tokens=o->max_output_tokens,
            .max_tool_rounds=o->max_tool_rounds,.max_turn_ms=o->max_turn_ms,
            .max_tool_ms=o->max_tool_ms,.max_retries=o->max_retries,
            .max_retry_elapsed_ms=o->max_retry_elapsed_ms};
        config.provider=(cog_runtime_provider_config_t){
            cog_cli_configuration_endpoint(admission->configuration),o->model_id,
            &cog_openrouter_prompt_template,p.compiler_limits};
        config.tools=(cog_runtime_tool_config_t){.table=&cog_workspace_tools,
            .bindings=p.bindings,.binding_count=p.binding_count};
        config.instructions=config.mode==COG_RUNTIME_NEW ? p.snapshot : NULL;config.instruction_limits=p.limits;
        config.workspace_root_id=p.manifest.workspace_root_id;
        const cog_runtime_scope_binding_t scopes[]={
            {&cog_workspace_tools.tools[0],"path",NULL,true},
            {&cog_workspace_tools.tools[1],"path","",false},
            {&cog_workspace_tools.tools[2],"path",NULL,true},
            {&cog_workspace_tools.tools[3],"cwd","",false}};
        config.scopes=scopes;config.scope_count=sizeof(scopes)/sizeof(scopes[0]);
        cog_runtime_t *runtime=NULL;
        rc=cli_runtime_new_admitted(&runtime,&config,admission);
        admission->configuration=NULL;
        if(rc==COG_OK) *out=runtime->app;
    }
    (void)cog_instruction_snapshot_free(p.snapshot);
    if(rc!=COG_OK) { cog_cli_configuration_free(admission->configuration);admission->configuration=NULL; }
    return rc;
}

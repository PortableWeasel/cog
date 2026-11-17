/** Exact immutable CGB2 configuration attachment and replay regressions. */
#include "../src/configuration.c"
#include "../src/instruction_hash.h"
#include <assert.h>
#include <cog/runtime_instructions.h>
int main(void)
{
    cog_cli_configuration_t *c=calloc(1U,sizeof(*c));assert(c);
    c->generation=7U;c->tool_mask=0U;
    c->options=(cog_cli_options_t){.backend=COG_CLI_BACKEND_REAL,
        .store_mode=COG_CLI_STORE_DURABLE,.model_id="test/model",.endpoint_name="openrouter",
        .base_url="https://example.test/api",.credential_ref="host:test",.store_dir="/state",
        .tool_policy=COG_CLI_TOOL_POLICY_ALLOW,.max_output_tokens=100U,.max_tool_rounds=3U,
        .max_turn_ms=1000U,.max_tool_ms=500U,.max_tool_output_bytes=4096U,
        .max_context_bytes=8192U,.max_retry_elapsed_ms=100U};
    strcpy(c->secret,"test-secret");
    unsigned char capsule[12]={'C','G','I','M',1,0,0,0,1,0,0,0};
    cog_cli_instruction_capsule_t value={.identity={.catalog_generation=3U,.snapshot_id=5U},
        .bytes=capsule,.length=sizeof(capsule)};
    nt_hash(capsule,sizeof(capsule),value.identity.sha256);
    assert(cli_configuration_instruction_attach(c,&value)==COG_OK);
    assert(cli_configuration_instruction_attach(c,&value)==COG_ERROR_INVALID_STATE);
    assert(c->instructions.bytes!=capsule);capsule[0]='X';
    cog_cli_configuration_t *clone=NULL;assert(cog_cli_configuration_clone_new(&clone,c,NULL)==COG_OK);
    assert(clone->instructions.bytes!=c->instructions.bytes);
    cog_cli_bootstrap_probe_t probe={.session_id="root",.owner={1,2},.checkpoint={3}};
    cog_net_config_t net={.host_runtime_initialized=true,.dependency_memory=COG_NET_DEPENDENCY_MEMORY_DYNAMIC};
    unsigned char bytes[8192],again[8192];size_t n=0,m=0;
    memset(bytes,0xA5,sizeof(bytes));
    assert(cog_cli_configuration_bootstrap_encode(c,&probe,&net,bytes,1,&n)==COG_ERROR_OVERFLOW);
    assert(bytes[0]==0xA5);
    assert(cog_cli_configuration_bootstrap_encode(c,&probe,&net,bytes,sizeof(bytes),&n)==COG_OK);
    assert(memcmp(bytes,"CGB2",4)==0);
    cog_cli_configuration_free(c);c=NULL;
    assert(cog_cli_configuration_bootstrap_encode(clone,&probe,&net,again,sizeof(again),&m)==COG_OK);
    assert(m==n && memcmp(bytes,again,n)==0);
    cog_cli_configuration_t *decoded=NULL;cog_cli_bootstrap_probe_t decoded_probe;
    assert(cog_cli_configuration_bootstrap_new(&decoded,bytes,n,(cog_net_ctx_t *)(void *)&net,&decoded_probe)==COG_OK);
    assert(cli_configuration_generation(decoded)==7U);
    assert(decoded->instructions.identity.snapshot_id==5U);
    assert(decoded->instructions.bytes!=(void *)(bytes+n-12U));
    assert(cog_cli_configuration_bootstrap_encode(decoded,&decoded_probe,&net,again,sizeof(again),&m)==COG_OK);
    assert(m==n && memcmp(bytes,again,n)==0);
    for(size_t i=0;i<n;++i) {
        cog_cli_configuration_t *bad=NULL;
        assert(cog_cli_configuration_bootstrap_new(&bad,bytes,i,(cog_net_ctx_t *)(void *)&net,&decoded_probe)!=COG_OK);
        assert(bad==NULL);
    }
    bytes[n-17U]^=1U;
    cog_cli_configuration_t *bad=NULL;
    assert(cog_cli_configuration_bootstrap_new(&bad,bytes,n,(cog_net_ctx_t *)(void *)&net,&decoded_probe)==COG_ERROR_PROTOCOL);
    assert(bad==NULL);
    /* A clone replacement cannot mutate the retained original receipt bytes. */
    cog_cli_configuration_t *replacement=NULL;
    assert(cog_cli_configuration_clone_new(&replacement,clone,NULL)==COG_OK);
    cog_cli_instruction_capsule_t newer=*cli_configuration_instruction_capsule(clone);
    newer.identity.snapshot_id++;
    assert(cli_configuration_instruction_replace(replacement,&newer)==COG_OK);
    assert(cli_configuration_instruction_capsule(clone)->identity.snapshot_id==5U);
    assert(cli_configuration_instruction_capsule(replacement)->identity.snapshot_id==6U);
    cog_cli_configuration_free(replacement);
    static const cog_model_t model={.id="test/model",.capabilities=COG_MODEL_CAPABILITY_STREAMING|COG_MODEL_CAPABILITY_TOOLS};
    static const cog_model_table_t models={&model,1U};
    cog_openrouter_config_t adapter={0};
    cog_endpoint_t endpoint={.name="host-label",.base_url="https://example.test/api",.api_key="typed-secret",
        .vtable=&cog_openrouter_provider_vtable,.models=&models,.implementation_config=&adapter,.generation=8U};
    cog_runtime_workers_t workers={.workspace_root="relative-workspace"};
    cog_prompt_tool_binding_t bindings[]={
        {&cog_workspace_tools.tools[0],COG_PROMPT_TOOL_ALLOW},
        {&cog_runtime_activate_skill_tool,COG_PROMPT_TOOL_ALLOW}};
    cog_runtime_config_t typed={.mode=COG_RUNTIME_NEW,.instruction_mode=COG_RUNTIME_INSTRUCTIONS_PINNED,
        .session_id="root",.store={.backend=&cog_store_file_backend,.base_dir="relative-store"},
        .workers=&workers,.provider={.endpoint=&endpoint,.model_id="test/model",.prompt_template=&cog_openrouter_prompt_template},
        .tools={.table=&cog_workspace_tools,.bindings=bindings,.binding_count=2U},
        .limits={.max_context_bytes=8192U,.max_output_tokens=100U,.max_tool_rounds=3U,.max_turn_ms=1000U,
            .max_tool_ms=500U,.max_tool_output_bytes=4096U,.max_retry_elapsed_ms=100U}};
    cog_cli_configuration_t *exported=NULL;
    assert(cli_configuration_runtime_new(&exported,&typed)==COG_OK);
    assert(strcmp(exported->secret,"typed-secret")==0 && strcmp(exported->options.endpoint_name,"openrouter")==0);
    assert(exported->tool_mask==1U && exported->options.tool_policy==COG_CLI_TOOL_POLICY_ALLOW);
    assert(cli_configuration_runtime_paths(exported,"/workspace","/state")==COG_OK);
    assert(cog_cli_configuration_bootstrap_encode(exported,&probe,&net,again,sizeof(again),&m)==COG_OK);
    cog_cli_configuration_t *exported_copy=NULL;
    assert(cog_cli_configuration_bootstrap_new(&exported_copy,again,m,(cog_net_ctx_t *)(void *)&net,&decoded_probe)==COG_OK);
    assert(strcmp(exported_copy->secret,"typed-secret")==0 && strcmp(exported_copy->options.workspace_root,"/workspace")==0);
    assert(exported_copy->tool_mask==1U && exported_copy->generation==8U);
    cog_cli_configuration_free(exported_copy);cog_cli_configuration_free(exported);
    typed.instruction_mode=COG_RUNTIME_INSTRUCTIONS_DISABLED;bindings[1].policy=COG_PROMPT_TOOL_ASK;
    assert(cli_configuration_runtime_new(&exported,&typed)==COG_ERROR_UNSUPPORTED && exported==NULL);
    cog_cli_configuration_free(decoded);cog_cli_configuration_free(clone);
    return 0;
}

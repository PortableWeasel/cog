#define _POSIX_C_SOURCE 200809L
#include "../src/fixture_provider.h"
#include "../src/projection.h"
#include <cog/runtime.h>
#include <assert.h>
#include <stdlib.h>

/* Projection construction occurs after the resolved root executor owns a
 * session/provider, but before host_new publishes the host pointer. */
cog_result_t __wrap_cog_cli_projection_new(cog_cli_projection_t **out,
    const cog_cli_projection_config_t *config);
cog_result_t __wrap_cog_cli_projection_new(cog_cli_projection_t **out,
    const cog_cli_projection_config_t *config)
{
    assert(out!=NULL && config!=NULL);
    *out=NULL;
    return COG_ERROR_NOMEM;
}
int main(void)
{
    static const cog_mock_script_t script={0};
    static const cog_model_t model={.id="fixture"};
    static const cog_model_table_t models={&model,1U};
    cog_endpoint_t endpoint={.name="fixture",.vtable=&cog_mock_provider_vtable,
        .models=&models,.implementation_config=&script,.generation=1U};
    cog_prompt_tool_binding_t binding={&cog_runtime_delegation_tools.tools[0],COG_PROMPT_TOOL_ALLOW};
    char directory[]="runtime-constructor-failure-XXXXXX";
    assert(mkdtemp(directory)!=NULL);
    cog_runtime_config_t config={.mode=COG_RUNTIME_NEW,.instruction_mode=COG_RUNTIME_INSTRUCTIONS_DISABLED,
        .session_id="failure",.store={.backend=&cog_store_file_backend,.base_dir=directory,.require_owner=true},
        .session={.max_nodes=32U,.max_payload_bytes=262144U,.max_tool_calls_per_message=4U},
        .provider={.endpoint=&endpoint,.model_id="fixture"},.tools={.bindings=&binding,.binding_count=1U},
        .limits={.max_input_bytes=4096U,.max_queued_inputs=1U,.max_view_bytes=1048576U,
            .max_context_bytes=65536U,.max_tool_output_bytes=4096U,.max_instruction_bytes=65536U,
            .max_events=64U,.max_dispatch_per_step=1U,.max_output_tokens=128U,.max_tool_rounds=4U,
            .max_turn_ms=10000U,.max_tool_ms=1000U,.max_retry_elapsed_ms=1000U}};
    cog_runtime_t *runtime=NULL;
    assert(cog_runtime_new(&runtime,&config)==COG_ERROR_NOMEM && runtime==NULL);
    assert(cog_runtime_close(&runtime,COG_OK)==COG_OK);
    config.session_id="second-failure";
    assert(cog_runtime_new(&runtime,&config)==COG_ERROR_NOMEM && runtime==NULL);
    return 0;
}

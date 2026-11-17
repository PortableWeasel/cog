#include "../src/execution_internal.h"
#include "../src/runtime_internal.h"
#include <cog/workspace.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
int main(void) {
    cog_runtime_t r={0};cog_cli_app_t *app=calloc(1U,sizeof(*app));assert(app!=NULL);
    r.app=app;app->runtime_owner=&r;r.remote_instructions=true;
    r.config.instruction_limits=(cog_instruction_limits_t){32,16,4096,8192,4096,131072,64,16,16,16};
    cog_prompt_tool_binding_t binding={&cog_workspace_tools.tools[0],COG_PROMPT_TOOL_ALLOW};
    r.bindings=&binding;r.config.tools.binding_count=1U;
    cog_runtime_scope_binding_t scope={binding.descriptor,"path",NULL,true};
    r.config.scopes=&scope;r.config.scope_count=1U;r.config.workspace_root_id="root";
    r.json_storage=calloc(CLI_ARGUMENT_BYTES*3U,1U);assert(r.json_storage!=NULL);
    assert(cli_runtime_instruction_init(&r)==COG_OK);
    cog_instruction_source_t source={COG_INSTRUCTION_HOST,"root","host.md","",NULL,0,"HOST",4};
    cog_instruction_catalog_t *catalog=NULL;
    assert(cog_instruction_catalog_new(&catalog,&r.config.instruction_limits,1U,&source,1U,NULL)==COG_OK);
    cog_instruction_selection_t selection={.snapshot_id=1,.workspace_root_id="root",.scope="",
        .model_id="test",.template_id="test",.template_revision=1,.max_context_bytes=65536,
        .max_output_tokens=128,.max_tool_rounds=4,.bindings=&binding,.binding_count=1};
    assert(cog_instruction_snapshot_new(&r.instructions,catalog,&r.config.instruction_limits,
        &selection,NULL,0U,NULL)==COG_OK);
    cog_session_config_t cfg={.id="native-test",.max_nodes=16,.max_payload_bytes=32768,
        .max_tool_calls_per_message=4};
    assert(cog_session_new(&app->session,&cfg)==COG_OK);
    static const char arguments[]="{\"path\":\"file.txt\"}";
    app->call_views[0]=(cog_message_tool_call_t){.id="call",.name="read_file",
        .arguments=arguments,.arguments_len=sizeof(arguments)-1U};
    app->call_count=1;
    cog_message_t message={.role=COG_MESSAGE_ROLE_ASSISTANT,.tool_calls=app->call_views,.tool_call_count=1};
    assert(cog_session_append(app->session,&message,&app->assistant)==COG_OK);
    assert(cli_runtime_scope(app,&app->call_views[0])==COG_ERROR_AGAIN);
    cog_cli_instruction_resource_t request;
    assert(cli_runtime_remote_request(&r,9U,&request)==COG_OK);
    assert(request.operation==COG_CLI_INSTRUCTION_RESOURCE_SCOPE && strcmp(request.path,"")==0);
    assert(cli_runtime_resource_request_matches(&r,&request,&app->call_views[0])==COG_OK);
    assert(cli_runtime_remote_hold(&r));
    cog_instruction_manifest_t m;assert(cog_instruction_snapshot_view(r.instructions,&m)==COG_OK);
    unsigned char encoded[4096];size_t bytes;
    assert(cog_instruction_manifest_encode(&m,encoded,sizeof(encoded),&bytes)==COG_OK);
    cog_cli_instruction_resource_t result={.kind=COG_CLI_INSTRUCTION_RESOURCE_RESULT,.context=request.context,
        .candidate={request.context,encoded,bytes}};
    assert(cli_runtime_remote_result(&r,&result)==COG_OK);
    assert(r.remote_outcome==COG_CLI_INSTRUCTION_COMMIT_UNCHANGED && cli_runtime_remote_hold(&r));
    cog_cli_instruction_resource_t ready={.kind=COG_CLI_INSTRUCTION_RESOURCE_READY,.context=request.context,
        .candidate={.identity=request.context},.outcome=COG_CLI_INSTRUCTION_COMMIT_UNCHANGED};
    assert(cli_runtime_remote_release(&r,&ready)==COG_OK);
    assert(!cli_runtime_remote_hold(&r));
    /* Re-entering original proposal admits normally instead of another SCOPE. */
    assert(cli_runtime_scope(app,&app->call_views[0])==COG_OK);
    cog_tool_execution_state_t state;
    assert(cog_session_tool_state(app->session,app->assistant,"call",&state)==COG_OK);
    assert(state==COG_TOOL_EXECUTION_PROPOSED);
    assert(r.scope_allowed && !r.scope_replaced && r.control_change==NULL);
    r.scope_allowed=false;
    assert(cli_runtime_scope(app,&app->call_views[0])==COG_ERROR_AGAIN);
    assert(cli_runtime_remote_request(&r,10U,&request)==COG_OK);
    result=(cog_cli_instruction_resource_t){.kind=COG_CLI_INSTRUCTION_RESOURCE_RESULT,
        .context=request.context,.result=COG_ERROR_DENIED};
    assert(cli_runtime_remote_result(&r,&result)==COG_OK);
    assert(r.remote_outcome==COG_CLI_INSTRUCTION_COMMIT_REFUSED && cli_runtime_remote_hold(&r));
    ready.outcome=COG_CLI_INSTRUCTION_COMMIT_REFUSED;
    memset(&ready.candidate,0,sizeof(ready.candidate));
    assert(cli_runtime_remote_release(&r,&ready)==COG_OK);
    assert(cli_runtime_scope(app,&app->call_views[0])==COG_ERROR_DENIED);
    assert(r.scope_refused && r.control_change==NULL);
    assert(cli_runtime_instruction_dispose(&r)==COG_OK);
    cog_instruction_snapshot_free(r.instructions);cog_instruction_catalog_free(catalog);
    cog_session_free(app->session);free(r.json_storage);free(app);return 0;
}

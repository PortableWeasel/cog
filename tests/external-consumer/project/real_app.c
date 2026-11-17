/** @file real_app.c @brief Installed public REAL TLS app and cooperative core. */
#include "installed_proof.h"
#include <cog_cli/cli.h>
#include <stdio.h>
#include <string.h>

#define REQUIRE(x) do { if(!(x)){fprintf(stderr,"installed REAL:%d: %s\n",__LINE__,#x);failed=1;goto cleanup;} }while(0)
int installed_real_app_proof(const char *directory,const char *url,const char *certificate)
{
    int failed=0;cog_net_ctx_t *net=NULL;cog_cli_app_t *app=NULL;cog_session_t *saved=NULL;
    char workspace[4096],store[4096],config[4096];
    int n=snprintf(workspace,sizeof(workspace),"%s/real-workspace",directory);
    REQUIRE(n>0 && (size_t)n<sizeof(workspace));
    n=snprintf(store,sizeof(store),"%s/real-store",directory);REQUIRE(n>0 && (size_t)n<sizeof(store));
    n=snprintf(config,sizeof(config),"%s/empty-config",directory);REQUIRE(n>0 && (size_t)n<sizeof(config));
    cog_net_config_t transport={.host_runtime_initialized=true,
        .dependency_memory=COG_NET_DEPENDENCY_MEMORY_DYNAMIC,.ca_bundle=certificate,
        .max_connections=2U,.max_request_bytes=1048576U,.connect_timeout_ms=3000U,.stall_timeout_ms=3000U};
    REQUIRE(cog_net_new(&net,&transport)==COG_OK);
    cog_cli_options_t options={.backend=COG_CLI_BACKEND_REAL,.net=net,.model_id="fixture/model",
        .base_url=url,.credential_ref="env:COG_INSTALLED_PROOF_KEY",.config_path=config,
        .workspace_root=workspace,.store_dir=store,.store_mode=COG_CLI_STORE_DURABLE,
        .session_id="installed-real",.prompt="installed effect",.tool_policy=COG_CLI_TOOL_POLICY_ALLOW,
        .max_context_bytes=65536U,.max_tool_output_bytes=4096U,.max_output_tokens=128U,
        .max_tool_rounds=1U,.max_turn_ms=10000U,.max_tool_ms=3000U,.max_retries=0U,
        .explicit_options=COG_CLI_OPT_BACKEND|COG_CLI_OPT_MODEL|COG_CLI_OPT_BASE_URL|COG_CLI_OPT_CREDENTIAL|
            COG_CLI_OPT_WORKSPACE|COG_CLI_OPT_STORE_DIR|COG_CLI_OPT_STORE_MODE|COG_CLI_OPT_POLICY|
            COG_CLI_OPT_CONTEXT|COG_CLI_OPT_TOOL_OUTPUT|COG_CLI_OPT_OUTPUT_TOKENS|COG_CLI_OPT_TOOL_ROUNDS|
            COG_CLI_OPT_TURN_MS|COG_CLI_OPT_TOOL_MS|COG_CLI_OPT_RETRIES};
    REQUIRE(cog_cli_app_new(&app,&options)==COG_OK);
    REQUIRE(cog_cli_app_run(app)==COG_OK);
    cog_message_t history[8];size_t count=0U;
    REQUIRE(cog_session_copy_history(cog_cli_app_session(app),NULL,history,8U,&count)==COG_OK && count==4U);
    REQUIRE(history[1].tool_call_count==1U && history[2].role==COG_MESSAGE_ROLE_TOOL &&
        strcmp(history[1].tool_calls[0].id,"installed-effect")==0 &&
        strcmp(history[2].tool_call_id,"installed-effect")==0 &&
        history[3].status==COG_MESSAGE_COMPLETE && strcmp(history[3].content,"INSTALLED_REAL_DONE")==0);
    cog_session_snapshot_t snapshot;
    REQUIRE(cog_session_snapshot_view(cog_cli_app_session(app),&snapshot)==COG_OK &&
        snapshot.ledger_count==1U && snapshot.ledger[0].state==COG_TOOL_EXECUTION_SUCCEEDED);
    cog_store_load_report_t report;
    REQUIRE(cog_store_load_ex(cog_cli_app_store(app),"installed-real",COG_STORE_LOAD_INSPECT,&saved,&report)==COG_OK);
    REQUIRE(cog_session_snapshot_view(saved,&snapshot)==COG_OK && snapshot.ledger_count==1U &&
        snapshot.ledger[0].state==COG_TOOL_EXECUTION_SUCCEEDED);
    REQUIRE(cog_net_active_count(net)==0U);
    /* Exercise public cooperative driver definitions after app_run ended. Stop
       before the first slice so no application input/model/effect is admitted. */
    cog_core_t *core=cog_cli_app_core(app);cog_core_step_info_t step={0};
    REQUIRE(cog_core_drive_begin(core)==COG_OK);cog_core_stop(core);
    REQUIRE(cog_core_step(core,&step)==COG_OK && step.done);
    REQUIRE(cog_core_drive_end(core)==COG_OK);
cleanup:
    cog_session_free(saved);cog_cli_app_free(app);cog_net_free(net);
    if(!failed)puts("installed public REAL TLS/tool/durable/core lifecycle: PASS");
    return failed;
}

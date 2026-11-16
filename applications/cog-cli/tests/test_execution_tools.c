/** @file test_execution_tools.c @brief Real ASK/STARTED/publication/tool save join. */
#define _POSIX_C_SOURCE 200809L
#include "app_internal.h"
#include "execution_internal.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"line %d: %s\n",__LINE__,#x); return 1; } } while (0)
int main(int argc,char **argv)
{
    CHECK(argc==3);
    static const char args[]="{\"command\":\"printf once >> effect; printf done\"}";
    static const cog_provider_delta_t deltas[]={
        {.kind=COG_PROVIDER_DELTA_TOOL_BEGIN,.tool_call_id="call-1",.tool_name="run_command"},
        {.kind=COG_PROVIDER_DELTA_TOOL_ARGUMENTS,.tool_call_id="call-1",.data=args,.data_len=sizeof(args)-1U},
        {.kind=COG_PROVIDER_DELTA_TOOL_END,.tool_call_id="call-1"},
        {.kind=COG_PROVIDER_DELTA_FINISH,.finish_reason=COG_FINISH_REASON_TOOL_CALLS,.outcome=COG_OK},
        {.kind=COG_PROVIDER_DELTA_TEXT,.data="finished",.data_len=8U},
        {.kind=COG_PROVIDER_DELTA_FINISH,.finish_reason=COG_FINISH_REASON_STOP,.outcome=COG_OK}};
    char program[]="fixture",mock[]="--mock";char *input[]={program,mock};int status=0;
    cog_cli_options_t options;CHECK(cog_cli_options_parse(&options,2,input,&status)==COG_OK);
    options.store_mode=COG_CLI_STORE_DURABLE;options.store_dir=argv[1];options.workspace_root=argv[2];
    options.tool_policy=COG_CLI_TOOL_POLICY_ASK;options.prompt="execute one identified call";
    options.interactive=false;options.mock_deltas=deltas;options.mock_delta_count=sizeof(deltas)/sizeof(deltas[0]);
    options.explicit_options|=COG_CLI_OPT_STORE_MODE|COG_CLI_OPT_STORE_DIR|COG_CLI_OPT_WORKSPACE|COG_CLI_OPT_POLICY;
    cog_cli_configuration_t *configuration=NULL;
    CHECK(cog_cli_configuration_new(&configuration,&options,1U)==COG_OK);
    cog_cli_execution_admission_t admission={.kind=COG_CLI_EXECUTION_ROOT,.configuration=configuration,.configuration_generation=1U};
    cog_cli_app_t *app=NULL;CHECK(cog_cli_execution_new(&app,&admission)==COG_OK);
    CHECK(cog_cli_execution_begin(app)==COG_OK);
    char effect[4096];int written=snprintf(effect,sizeof(effect),"%s/effect",argv[2]);
    CHECK(written>0 && (size_t)written<sizeof(effect));
    bool allowed=false,started_barrier=false,finished=false;uint64_t sequence=0U;
    for(size_t turn=0U;turn<4096U && !finished;++turn){
        uint64_t serial=0U;cog_cli_display_snapshot_t view;
        cog_result_t copied=cog_cli_execution_copy_view(app,&serial,&view);
        if(copied==COG_OK){
            cog_session_snapshot_t snapshot;CHECK(cog_session_snapshot_view(app->session,&snapshot)==COG_OK);
            if(snapshot.ledger_count==1U && snapshot.ledger[0].state==COG_TOOL_EXECUTION_STARTED && !started_barrier){
                CHECK(allowed && access(effect,F_OK)!=0);
                for(size_t hold=0U;hold<8U;++hold){cog_core_step_info_t held;CHECK(cog_cli_execution_step(app,&held)==COG_OK);CHECK(access(effect,F_OK)!=0);}
                started_barrier=true;
            }
            CHECK(cog_cli_execution_commit_view(app,serial,71U,++sequence)==COG_OK);
        }else CHECK(copied==COG_ERROR_AGAIN);
        cog_cli_policy_view_t policy;uint64_t prompt=0U;
        if(!allowed && cog_cli_execution_copy_policy(app,&prompt,&policy)==COG_OK){
            CHECK(access(effect,F_OK)!=0);
            cog_cli_command_t command={.kind=COG_CLI_COMMAND_POLICY,.observed_generation=71U,
                .observed_sequence=sequence,.target_session_id=policy.session_id,.node_id=policy.assistant_node,
                .call_ordinal=policy.call_ordinal,.nonce=1U,.decision=2U};
            CHECK(cog_cli_execution_apply_command(app,&command,app->headless->expected_owner,prompt)==COG_OK);
            allowed=true;
        }
        cog_core_step_info_t info;CHECK(cog_cli_execution_step(app,&info)==COG_OK && !info.done);
        cog_session_node_t *leaf=cog_session_active(app->session);
        if(allowed && !app->active && leaf!=NULL && cog_node_data(leaf)->message->role==COG_MESSAGE_ROLE_ASSISTANT &&
            cog_node_data(leaf)->message->status==COG_MESSAGE_COMPLETE)finished=true;
        const struct timespec delay={0,1000000L};(void)nanosleep(&delay,NULL);
    }
    CHECK(finished && started_barrier);
    FILE *file=fopen(effect,"r");CHECK(file!=NULL);char text[16]={0};CHECK(fread(text,1U,15U,file)==4U);CHECK(fclose(file)==0);
    CHECK(strcmp(text,"once")==0);
    cog_session_t *loaded=NULL;CHECK(cog_store_load(app->store,cog_session_id(app->session),&loaded)==COG_OK);
    cog_session_snapshot_t persisted;CHECK(cog_session_snapshot_view(loaded,&persisted)==COG_OK);
    CHECK(persisted.ledger_count==1U && persisted.ledger[0].state==COG_TOOL_EXECUTION_SUCCEEDED);
    cog_session_free(loaded);
    CHECK(cog_cli_app_shutdown(app)==COG_OK);bool done=false;
    for(size_t i=0U;i<256U&&!done;++i){cog_core_step_info_t info;CHECK(cog_cli_execution_step(app,&info)==COG_OK);done=info.done;}
    CHECK(done && cog_cli_execution_close(&app,COG_OK)==COG_OK && app==NULL);
    puts("headless actual ASK/tool publication: PASS");return 0;
}

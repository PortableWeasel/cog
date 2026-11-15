/** @file test_app_limits.c @brief Tool/turn budgets, schema refusal and stop. */
#define _POSIX_C_SOURCE 200809L
#include "cog_cli/cli.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#define REQUIRE(x) do{if(!(x)){fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x);return 1;}}while(0)
static void *stopper(void *arg)
{struct timespec t={0,50000000L};(void)nanosleep(&t,NULL);cog_cli_app_stop(arg);return NULL;}
static int run_tool(const char *name,const char *args,bool stop,bool timeout,bool truncated,bool valid)
{
    char root[]="/tmp/cog-k-limit-XXXXXX";REQUIRE(mkdtemp(root)!=NULL);
    char store[512];REQUIRE(snprintf(store,sizeof(store),"%s/store",root)>0);
    cog_provider_delta_t deltas[]={
        {.kind=COG_PROVIDER_DELTA_TOOL_BEGIN,.tool_call_id="call",.tool_name=name},
        {.kind=COG_PROVIDER_DELTA_TOOL_ARGUMENTS,.tool_call_id="call",.data=args,.data_len=strlen(args)},
        {.kind=COG_PROVIDER_DELTA_TOOL_END,.tool_call_id="call"},
        {.kind=COG_PROVIDER_DELTA_FINISH,.finish_reason=COG_FINISH_REASON_TOOL_CALLS,.outcome=COG_OK},
        {.kind=COG_PROVIDER_DELTA_TEXT,.data="final",.data_len=5},
        {.kind=COG_PROVIDER_DELTA_FINISH,.finish_reason=COG_FINISH_REASON_STOP,.outcome=COG_OK}};
    cog_cli_options_t opts={.backend=COG_CLI_BACKEND_MOCK,.prompt="fixture",.mock_deltas=deltas,.mock_delta_count=6,
        .workspace_root=root,.store_mode=COG_CLI_STORE_DURABLE,.store_dir=store,.tool_policy=COG_CLI_TOOL_POLICY_ALLOW,
        .max_tool_output_bytes=truncated?16U:65536U,.max_tool_ms=timeout?20U:5000U,
        .explicit_options=COG_CLI_OPT_BACKEND|COG_CLI_OPT_WORKSPACE|COG_CLI_OPT_POLICY|COG_CLI_OPT_STORE_MODE|COG_CLI_OPT_STORE_DIR|COG_CLI_OPT_TOOL_OUTPUT|COG_CLI_OPT_TOOL_MS};
    cog_cli_app_t *app=NULL;REQUIRE(cog_cli_app_new(&app,&opts)==COG_OK);
    pthread_t thread;
    if(stop)REQUIRE(pthread_create(&thread,NULL,stopper,app)==0);
    REQUIRE(cog_cli_app_run(app)==COG_OK);
    if(stop)REQUIRE(pthread_join(thread,NULL)==0);
    cog_mock_state_t state;REQUIRE(cog_mock_provider_state(cog_cli_app_provider(app),&state)==COG_OK);
    REQUIRE(state.send_count==(stop?1U:2U));
    const cog_message_t *history=NULL;size_t count=0;
    REQUIRE(cog_session_get_history(cog_cli_app_session(app),cog_session_active(cog_cli_app_session(app)),&history,&count)==COG_OK);
    REQUIRE(count==(stop?3U:4U));
    if(stop)REQUIRE(strstr(history[2].content,"status=aborted")!=NULL);
    if(timeout)REQUIRE(strstr(history[2].content,"status=timeout")!=NULL);
    if(truncated)REQUIRE(strstr(history[2].content,"truncated=true")!=NULL);
    if(!valid)REQUIRE(strstr(history[2].content,"invalid-argument")!=NULL);
    if(strcmp(name,"run_command")==0 && !stop && !timeout && !truncated) {
        REQUIRE(strstr(history[2].content,"PATH=/usr/bin:/bin")!=NULL);
        REQUIRE(strstr(history[2].content,"OPENROUTER_API_KEY")==NULL);
    }
    cog_cli_app_free(app);
    char cleanup[1024];REQUIRE(snprintf(cleanup,sizeof(cleanup),"rm -rf -- '%s'",root)>0);REQUIRE(system(cleanup)==0);
    return 0;
}
static int rounds(void)
{
    char root[]="/tmp/cog-k-rounds-XXXXXX";REQUIRE(mkdtemp(root)!=NULL);
    char store[512],effect[512];REQUIRE(snprintf(store,sizeof(store),"%s/store",root)>0);
    REQUIRE(snprintf(effect,sizeof(effect),"%s/effect",root)>0);
    const char *args="{\"command\":\"printf once >> effect\"}";
    cog_provider_delta_t deltas[10]={0};
    for(size_t i=0;i<2;i++) {
        const char *id=i==0?"first":"second";
        deltas[i*4U]=(cog_provider_delta_t){.kind=COG_PROVIDER_DELTA_TOOL_BEGIN,.tool_call_id=id,.tool_name="run_command"};
        deltas[i*4U+1U]=(cog_provider_delta_t){.kind=COG_PROVIDER_DELTA_TOOL_ARGUMENTS,.tool_call_id=id,.data=args,.data_len=strlen(args)};
        deltas[i*4U+2U]=(cog_provider_delta_t){.kind=COG_PROVIDER_DELTA_TOOL_END,.tool_call_id=id};
        deltas[i*4U+3U]=(cog_provider_delta_t){.kind=COG_PROVIDER_DELTA_FINISH,.finish_reason=COG_FINISH_REASON_TOOL_CALLS,.outcome=COG_OK};
    }
    deltas[8]=(cog_provider_delta_t){.kind=COG_PROVIDER_DELTA_TEXT,.data="must not send",.data_len=13};
    deltas[9]=(cog_provider_delta_t){.kind=COG_PROVIDER_DELTA_FINISH,.finish_reason=COG_FINISH_REASON_STOP,.outcome=COG_OK};
    cog_cli_options_t opts={.backend=COG_CLI_BACKEND_MOCK,.prompt="fixture",.mock_deltas=deltas,.mock_delta_count=10,
        .workspace_root=root,.store_mode=COG_CLI_STORE_DURABLE,.store_dir=store,.tool_policy=COG_CLI_TOOL_POLICY_ALLOW,.max_tool_rounds=1,
        .explicit_options=COG_CLI_OPT_BACKEND|COG_CLI_OPT_WORKSPACE|COG_CLI_OPT_POLICY|COG_CLI_OPT_STORE_MODE|COG_CLI_OPT_STORE_DIR|COG_CLI_OPT_TOOL_ROUNDS};
    cog_cli_app_t *app=NULL;REQUIRE(cog_cli_app_new(&app,&opts)==COG_OK);
    REQUIRE(cog_cli_app_run(app)==COG_ERROR_LIMIT);
    cog_mock_state_t state;REQUIRE(cog_mock_provider_state(cog_cli_app_provider(app),&state)==COG_OK && state.send_count==2U);
    FILE *file=fopen(effect,"r");REQUIRE(file!=NULL);char text[16]={0};REQUIRE(fread(text,1,15,file)==4U);REQUIRE(fclose(file)==0);
    REQUIRE(strcmp(text,"once")==0);
    cog_cli_app_free(app);char cleanup[1024];REQUIRE(snprintf(cleanup,sizeof(cleanup),"rm -rf -- '%s'",root)>0);REQUIRE(system(cleanup)==0);
    return 0;
}
static int turn_timeout(void)
{
    const cog_mock_step_t step={COG_MOCK_STEP_CHUNK,"too late",COG_OK,0,false,0,NULL};
    const cog_mock_script_t script={&step,1,0,COG_OK,100};
    cog_cli_options_t opts={.backend=COG_CLI_BACKEND_MOCK,.prompt="fixture",.script=&script,.max_turn_ms=10,
        .explicit_options=COG_CLI_OPT_BACKEND|COG_CLI_OPT_TURN_MS};
    cog_cli_app_t *app=NULL;REQUIRE(cog_cli_app_new(&app,&opts)==COG_OK);
    REQUIRE(cog_cli_app_run(app)==COG_ERROR_TIMEOUT);
    cog_cli_display_snapshot_t snapshot;REQUIRE(cog_cli_app_display_snapshot(app,&snapshot)==COG_OK);
    REQUIRE(snapshot.item_count==2U && snapshot.items[1].status==COG_MESSAGE_INCOMPLETE);
    cog_cli_app_free(app);return 0;
}
int main(void)
{
    REQUIRE(setenv("OPENROUTER_API_KEY","synthetic-must-not-inherit",1)==0);
    REQUIRE(run_tool("run_command","{\"command\":\"env\"}",false,false,false,true)==0);
    REQUIRE(run_tool("run_command","{\"command\":\"sleep 10\"}",true,false,false,true)==0);
    REQUIRE(run_tool("run_command","{\"command\":\"sleep 10\"}",false,true,false,true)==0);
    REQUIRE(run_tool("run_command","{\"command\":\"printf 012345678901234567890123456789\"}",false,false,true,true)==0);
    REQUIRE(run_tool("read_file","{\"path\":\"missing\",\"extra\":true}",false,false,false,false)==0);
    REQUIRE(rounds()==0);REQUIRE(turn_timeout()==0);return 0;
}

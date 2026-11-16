/** @file test_client_ask_output.c @brief An undeliverable ASK cannot admit effects. */
#define _POSIX_C_SOURCE 200809L
#include "app_internal.h"
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static bool repeat_prompt;
static unsigned stage;
static int saved_error,input_write;
cog_result_t __real_cog_cli_service(void *arg,uint64_t now,uint64_t *deadline);
cog_result_t __wrap_cog_cli_service(void *arg,uint64_t now,uint64_t *deadline);
static void fail_error_output(void)
{
    assert(fflush(stderr)==0);int full=open("/dev/full",O_WRONLY);assert(full>=0);
    assert(dup2(full,STDERR_FILENO)>=0);assert(close(full)==0);
}
static cog_result_t type_line(cog_cli_app_t *app,const char *text)
{
    size_t n=strlen(text);assert(write(input_write,text,n)==(ssize_t)n);
    for(size_t i=0U;i<n;i++){cog_result_t rc=cog_cli_idle(app,0U);if(rc!=COG_OK)return rc;}
    return COG_OK;
}
cog_result_t __wrap_cog_cli_service(void *arg,uint64_t now,uint64_t *deadline)
{
    cog_cli_app_t *app=arg;
    if(app->request_serial==2U && !app->active)cog_cli_app_stop(app);
    if(cog_core_state(app->core)==CLI_POLICY && stage==0U && !repeat_prompt){
        fail_error_output();stage=1U;
    }
    if(app->policy_prompt && stage<2U){
        if(repeat_prompt){
            fail_error_output();stage=1U;
            cog_result_t rc=type_line(app,"arbitrary input\n");
            if(rc!=COG_OK)return rc;
        }
        stage=2U;
        /* This already available explicit decision must not rescue failed
         * delivery. Ordinary approval never authorizes a hidden request. */
        cog_result_t rc=type_line(app,"allow\n");if(rc!=COG_OK)return rc;
    }
    return __real_cog_cli_service(arg,now,deadline);
}
int main(int argc,char **argv)
{
    assert(argc==2);repeat_prompt=strcmp(argv[1],"repeat")==0;
    assert(repeat_prompt || strcmp(argv[1],"initial")==0);
    char root[]="/tmp/cog-ask-output-XXXXXX",store[512],effect[512],command[600];
    assert(mkdtemp(root)!=NULL);assert(snprintf(store,sizeof(store),"%s/store",root)>0);
    assert(snprintf(effect,sizeof(effect),"%s/effect",root)>0);
    const cog_provider_delta_t script[]={
        {.kind=COG_PROVIDER_DELTA_TOOL_BEGIN,.tool_call_id="ask",.tool_name="run_command"},
        {.kind=COG_PROVIDER_DELTA_TOOL_ARGUMENTS,.tool_call_id="ask",.data="{\"command\":\"printf BAD >> effect\"}",
            .data_len=sizeof("{\"command\":\"printf BAD >> effect\"}")-1U},
        {.kind=COG_PROVIDER_DELTA_TOOL_END,.tool_call_id="ask"},
        {.kind=COG_PROVIDER_DELTA_FINISH,.finish_reason=COG_FINISH_REASON_TOOL_CALLS},
        {.kind=COG_PROVIDER_DELTA_FINISH,.finish_reason=COG_FINISH_REASON_STOP}};
    cog_cli_options_t opts={.backend=COG_CLI_BACKEND_MOCK,.prompt="approval output failure",.interactive=true,
        .workspace_root=root,.store_mode=COG_CLI_STORE_DURABLE,.store_dir=store,
        .tool_policy=COG_CLI_TOOL_POLICY_ASK,.mock_deltas=script,.mock_delta_count=sizeof(script)/sizeof(script[0]),
        .explicit_options=COG_CLI_OPT_BACKEND|COG_CLI_OPT_WORKSPACE|COG_CLI_OPT_STORE_MODE|COG_CLI_OPT_STORE_DIR|COG_CLI_OPT_POLICY};
    int input[2];assert(pipe(input)==0);int saved_input=dup(STDIN_FILENO);saved_error=dup(STDERR_FILENO);
    assert(saved_input>=0 && saved_error>=0);assert(dup2(input[0],STDIN_FILENO)>=0);assert(close(input[0])==0);input_write=input[1];
    cog_cli_app_t *app=NULL;assert(cog_cli_app_new(&app,&opts)==COG_OK);
    cog_result_t rc=cog_cli_app_run(app);
    assert(dup2(saved_error,STDERR_FILENO)>=0);clearerr(stderr);assert(close(saved_error)==0);
    bool effects=access(effect,F_OK)==0;
    cog_session_snapshot_t snapshot;assert(cog_session_snapshot_view(app->session,&snapshot)==COG_OK);
    bool started=false;for(size_t i=0U;i<snapshot.ledger_count;i++)for(size_t j=0U;j<snapshot.ledger[i].audit_count;j++)
        if(snapshot.ledger[i].audit[j].next==COG_TOOL_EXECUTION_STARTED)started=true;
    fprintf(stderr,"ASK %s: rc=%d effects=%s started=%s\n",argv[1],(int)rc,effects?"yes":"no",started?"yes":"no");
    cog_cli_app_free(app);assert(dup2(saved_input,STDIN_FILENO)>=0);assert(close(saved_input)==0);assert(close(input_write)==0);
    assert(snprintf(command,sizeof(command),"rm -rf -- '%s'",root)>0);assert(system(command)==0);
    assert(rc==COG_ERROR_IO && !effects && !started);return 0;
}

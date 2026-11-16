/** @file test_client_acceptance.c @brief Application policy and budget joins. */
#define _POSIX_C_SOURCE 200809L
#include "app_internal.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define TEXT(s) {.kind=COG_PROVIDER_DELTA_TEXT,.data=s,.data_len=sizeof(s)-1U}
#define BEGIN(id) {.kind=COG_PROVIDER_DELTA_TOOL_BEGIN,.tool_call_id=id,.tool_name="run_command"}
#define ARGS(id,s) {.kind=COG_PROVIDER_DELTA_TOOL_ARGUMENTS,.tool_call_id=id,.data=s,.data_len=sizeof(s)-1U}
#define END(id) {.kind=COG_PROVIDER_DELTA_TOOL_END,.tool_call_id=id}
#define FINISH(reason) {.kind=COG_PROVIDER_DELTA_FINISH,.finish_reason=reason,.outcome=COG_OK}
static cog_result_t (*control)(cog_cli_app_t *app);
static bool virtual_clock;
static uint64_t virtual_now, last_deadline;
static uint32_t before_retry_ms;
static bool elapsed_applied;
cog_result_t __real_cog_cli_service(void *arg,uint64_t now,uint64_t *deadline);
cog_result_t __wrap_cog_cli_service(void *arg,uint64_t now,uint64_t *deadline);
cog_result_t __wrap_cog_cli_service(void *arg,uint64_t now,uint64_t *deadline)
{
    if(control!=NULL){cog_result_t rc=control(arg);if(rc!=COG_OK)return rc;}
    cog_result_t rc=__real_cog_cli_service(arg,virtual_clock?virtual_now:now,deadline);
    cog_cli_app_t *app=arg;
    if(rc==COG_OK && virtual_clock && !elapsed_applied && app->delta_ready &&
        app->delta.kind==COG_PROVIDER_DELTA_FINISH && app->delta.outcome==COG_ERROR_UNAVAILABLE){
        elapsed_applied=true;virtual_now=app->started_at+before_retry_ms;
        rc=__real_cog_cli_service(arg,virtual_now,deadline);
    }
    last_deadline=*deadline;return rc;
}
static cog_cli_options_t options(const char *root,const char *store)
{
    return (cog_cli_options_t){.backend=COG_CLI_BACKEND_MOCK,.prompt="hello",.session_id="acceptance",
        .workspace_root=root,.store_mode=COG_CLI_STORE_DURABLE,.store_dir=store,
        .tool_policy=COG_CLI_TOOL_POLICY_ASK,.explicit_options=COG_CLI_OPT_BACKEND|
        COG_CLI_OPT_WORKSPACE|COG_CLI_OPT_STORE_MODE|COG_CLI_OPT_STORE_DIR|COG_CLI_OPT_POLICY};
}
static void cleanup(const char *root)
{
    char command[512];int n=snprintf(command,sizeof(command),"rm -rf -- '%s'",root);
    assert(n>0 && (size_t)n<sizeof(command));assert(system(command)==0);
}
static void absent(const char *root)
{
    char path[512];assert(snprintf(path,sizeof(path),"%s/effect",root)>0);assert(access(path,F_OK)!=0);
}
static void effect_once(const char *root)
{
    char path[512],bytes[32]={0};assert(snprintf(path,sizeof(path),"%s/effect",root)>0);
    FILE *file=fopen(path,"r");assert(file!=NULL);assert(fread(bytes,1,sizeof(bytes),file)==4U);
    assert(fclose(file)==0);assert(memcmp(bytes,"once",4U)==0);
}
static const cog_provider_delta_t policy_script[]={BEGIN("allow-one"),
    ARGS("allow-one","{\"command\":\"printf once >> effect\"}"),END("allow-one"),
    {.kind=COG_PROVIDER_DELTA_TOOL_BEGIN,.tool_index=1U,.tool_call_id="deny-two",.tool_name="run_command"},
    {.kind=COG_PROVIDER_DELTA_TOOL_ARGUMENTS,.tool_index=1U,.tool_call_id="deny-two",
        .data="{\"command\":\"printf BAD >> effect\"}",.data_len=sizeof("{\"command\":\"printf BAD >> effect\"}")-1U},
    {.kind=COG_PROVIDER_DELTA_TOOL_END,.tool_index=1U,.tool_call_id="deny-two"},
    FINISH(COG_FINISH_REASON_TOOL_CALLS),TEXT("done"),FINISH(COG_FINISH_REASON_STOP)};
static unsigned policy_stage;
static int input_write;
static void type_line(cog_cli_app_t *app,const char *text)
{
    size_t length=strlen(text);assert(write(input_write,text,length)==(ssize_t)length);
    for(size_t i=0U;i<length;i++)assert(cog_cli_idle(app,0U)==COG_OK);
}
static cog_result_t policy_control(cog_cli_app_t *app)
{
    if(app->policy_prompt && policy_stage==0U){
        absent(app->opts.workspace_root);
        assert(cog_cli_app_queue(app,"queued text is not approval",27U)==COG_OK);
        type_line(app,"arbitrary user text\n");
        assert(app->policy_decision==COG_TOOLKIT_POLICY_UNDECIDED);
        absent(app->opts.workspace_root);policy_stage=1U;
    }else if(app->policy_prompt && policy_stage==1U){
        absent(app->opts.workspace_root);type_line(app,"allow\n");policy_stage=2U;
    }else if(app->policy_prompt && app->tool_index==1U && policy_stage==2U){
        effect_once(app->opts.workspace_root);type_line(app,"deny\n");policy_stage=3U;
    }else if(app->request_serial==2U && !app->active){
        assert(policy_stage==3U);cog_cli_app_stop(app);
    }
    return COG_OK;
}
/* Capture persisted state at the actual next send, before a borrowed request
 * can be overwritten. This checks the durability ordering, not just final RAM. */
static cog_cli_app_t *sending_app;
static bool expect_allowed;
static unsigned verified_sends;
cog_result_t __real_cog_provider_send(cog_provider_t *provider,const cog_provider_request_t *request);
cog_result_t __wrap_cog_provider_send(cog_provider_t *provider,const cog_provider_request_t *request);
cog_result_t __wrap_cog_provider_send(cog_provider_t *provider,const cog_provider_request_t *request)
{
    if(sending_app!=NULL && sending_app->request_serial==1U){
        assert(request->message_count==4U);
        assert(strcmp(request->messages[2].tool_call_id,"allow-one")==0);
        assert(strcmp(request->messages[3].tool_call_id,"deny-two")==0);
        cog_session_t *loaded=NULL;assert(cog_store_load(sending_app->store,"acceptance",&loaded)==COG_OK);
        cog_session_snapshot_t snapshot;assert(cog_session_snapshot_view(loaded,&snapshot)==COG_OK);
        assert(snapshot.ledger_count==2U);
        assert(snapshot.ledger[0].state==(expect_allowed?COG_TOOL_EXECUTION_SUCCEEDED:COG_TOOL_EXECUTION_DENIED));
        assert(snapshot.ledger[1].state==COG_TOOL_EXECUTION_DENIED);
        assert(snapshot.ledger[0].audit_count==(expect_allowed?2U:1U));
        assert(snapshot.ledger[1].audit_count==1U);
        cog_session_free(loaded);verified_sends++;
    }
    return __real_cog_provider_send(provider,request);
}
static void ask(bool interactive)
{
    char root[]="/tmp/cog-accept-policy-XXXXXX",store[512];assert(mkdtemp(root)!=NULL);
    assert(snprintf(store,sizeof(store),"%s/store",root)>0);
    cog_cli_options_t opts=options(root,store);opts.interactive=interactive;
    opts.mock_deltas=policy_script;opts.mock_delta_count=sizeof(policy_script)/sizeof(policy_script[0]);
    int descriptors[2],saved=-1;
    if(interactive){assert(pipe(descriptors)==0);saved=dup(STDIN_FILENO);assert(saved>=0);
        assert(dup2(descriptors[0],STDIN_FILENO)>=0);assert(close(descriptors[0])==0);input_write=descriptors[1];}
    cog_cli_app_t *app=NULL;assert(cog_cli_app_new(&app,&opts)==COG_OK);
    sending_app=app;expect_allowed=interactive;verified_sends=0U;policy_stage=0U;
    control=interactive?policy_control:NULL;
    assert(cog_cli_app_run(app)==COG_OK);control=NULL;sending_app=NULL;
    assert(verified_sends==1U);if(interactive)effect_once(root);else absent(root);
    cog_mock_state_t state;assert(cog_mock_provider_state(app->provider,&state)==COG_OK && state.send_count==2U);
    cog_cli_app_free(app);
    if(interactive){assert(dup2(saved,STDIN_FILENO)>=0);assert(close(saved)==0);assert(close(input_write)==0);}
    cleanup(root);
}
static void semantic_finish(cog_finish_reason_t reason)
{
    char root[]="/tmp/cog-accept-finish-XXXXXX",store[512];assert(mkdtemp(root)!=NULL);
    assert(snprintf(store,sizeof(store),"%s/store",root)>0);
    const cog_provider_delta_t script[]={TEXT("accepted"),BEGIN("never"),
        ARGS("never","{\"command\":\"printf BAD >> effect\"}"),END("never"),FINISH(reason)};
    cog_cli_options_t opts=options(root,store);opts.tool_policy=COG_CLI_TOOL_POLICY_ALLOW;
    opts.mock_deltas=script;opts.mock_delta_count=sizeof(script)/sizeof(script[0]);
    cog_cli_app_t *app=NULL;assert(cog_cli_app_new(&app,&opts)==COG_OK);
    assert(cog_cli_app_run(app)==COG_ERROR_LIMIT);absent(root);
    cog_cli_display_snapshot_t display;assert(cog_cli_app_display_snapshot(app,&display)==COG_OK);
    assert(display.item_count==2U && display.items[1].status==COG_MESSAGE_INCOMPLETE);
    assert(display.items[1].text_len==8U && memcmp(display.items[1].text,"accepted",8U)==0);
    cog_session_snapshot_t snapshot;assert(cog_session_snapshot_view(app->session,&snapshot)==COG_OK);
    assert(snapshot.ledger_count==0U);
    cog_mock_state_t state;assert(cog_mock_provider_state(app->provider,&state)==COG_OK && state.send_count==1U);
    cog_cli_app_free(app);cleanup(root);
}
static unsigned backoff_visits;
static cog_result_t retry_control(cog_cli_app_t *app)
{
    if(cog_core_state(app->core)==CLI_BACKOFF){
        if(backoff_visits++==0U)return COG_OK;
        assert(app->started_at+before_retry_ms==virtual_now);
        assert(app->retry_at==virtual_now+10U);
        assert(last_deadline==app->retry_at);
        virtual_now=app->retry_at;
    }
    return COG_OK;
}
/* At elapsed zero, a delay exactly equal to either remaining budget is
 * refused. One additional millisecond admits exactly one retry. */
static void retry_boundary(uint32_t elapsed,uint32_t retry_ms,uint32_t turn_ms,bool admitted)
{
    const cog_mock_step_t steps[]={
        {COG_MOCK_STEP_ERROR,NULL,COG_ERROR_UNAVAILABLE,429,true,10U,"synthetic"},
        {COG_MOCK_STEP_CHUNK,"ok",COG_OK,0,false,0U,NULL},
        {COG_MOCK_STEP_COMPLETE,NULL,COG_OK,0,false,0U,NULL}};
    const cog_mock_script_t script={.steps=steps,.step_count=3U};
    cog_cli_options_t opts={.backend=COG_CLI_BACKEND_MOCK,.prompt="retry",.script=&script,
        .max_retry_elapsed_ms=retry_ms,.max_turn_ms=turn_ms,.max_retries=4U,
        .explicit_options=COG_CLI_OPT_BACKEND|COG_CLI_OPT_RETRY_MS|COG_CLI_OPT_TURN_MS|COG_CLI_OPT_RETRIES};
    cog_cli_app_t *app=NULL;assert(cog_cli_app_new(&app,&opts)==COG_OK);
    /* The real core drives dispatch; only its explicit app-service clock input
     * is controlled. No wall-clock sleeps or error/retry results are replaced. */
    virtual_clock=true;before_retry_ms=elapsed;elapsed_applied=false;backoff_visits=0U;
    assert(cog_cli_clock(&virtual_now)==COG_OK);control=retry_control;
    assert(cog_cli_app_run(app)==(admitted?COG_OK:COG_ERROR_UNAVAILABLE));
    control=NULL;virtual_clock=false;
    cog_mock_state_t state;assert(cog_mock_provider_state(app->provider,&state)==COG_OK);
    assert(state.send_count==(admitted?2U:1U));
    cog_cli_display_snapshot_t display;assert(cog_cli_app_display_snapshot(app,&display)==COG_OK);
    assert(display.item_count==2U && display.items[1].status==(admitted?COG_MESSAGE_COMPLETE:COG_MESSAGE_INCOMPLETE));
    cog_cli_app_free(app);
}
static const unsigned char opaque_bytes[]={0U,1U,255U,2U};
static const cog_provider_delta_t context_script[]={TEXT("working"),
    {.kind=COG_PROVIDER_DELTA_OPAQUE,.opaque={"fixture.ns","binary-v1",opaque_bytes,sizeof(opaque_bytes)}},
    BEGIN("context-call"),ARGS("context-call","{\"command\":\"printf done\"}"),END("context-call"),
    FINISH(COG_FINISH_REASON_TOOL_CALLS),TEXT("finished"),FINISH(COG_FINISH_REASON_STOP)};
static void cumulative_context(bool one_over)
{
    char root[]="/tmp/cog-accept-context-XXXXXX",store[512];assert(mkdtemp(root)!=NULL);
    assert(snprintf(store,sizeof(store),"%s/store",root)>0);
    cog_cli_options_t opts=options(root,store);opts.tool_policy=COG_CLI_TOOL_POLICY_ALLOW;
    opts.mock_deltas=context_script;opts.mock_delta_count=sizeof(context_script)/sizeof(context_script[0]);
    cog_cli_app_t *app=NULL;assert(cog_cli_app_new(&app,&opts)==COG_OK);assert(cog_cli_app_run(app)==COG_OK);
    cog_message_t history[8];size_t count=0U;
    assert(cog_session_copy_history(app->session,NULL,history,8U,&count)==COG_OK && count==4U);
    assert(history[1].opaque_count==1U && history[1].tool_call_count==1U);
    assert(history[1].opaque[0].size==sizeof(opaque_bytes));
    assert(memcmp(history[1].opaque[0].data,opaque_bytes,sizeof(opaque_bytes))==0);
    /* Concrete fixture oracle: schema + five known texts + call identity/name/
     * arguments + result identity/name + exact opaque namespace/format/blob.
     * No implementation accounting helper is used to derive the boundary. */
    size_t exact=app->tools_json_len+sizeof("hello")-1U+sizeof("working")-1U+
        sizeof("finished")-1U+sizeof("next")-1U+history[2].content_len+
        2U*(sizeof("context-call")-1U)+2U*(sizeof("run_command")-1U)+
        sizeof("{\"command\":\"printf done\"}")-1U+
        sizeof("fixture.ns")-1U+sizeof("binary-v1")-1U+sizeof(opaque_bytes);
    cog_session_snapshot_t before;assert(cog_session_snapshot_view(app->session,&before)==COG_OK);
    size_t original_nodes=before.node_count;
    cog_cli_app_free(app);
    opts.prompt="next";opts.mock_deltas=NULL;opts.mock_delta_count=0U;opts.chunks="boundary";
    opts.session_id=NULL;opts.session_action=COG_CLI_SESSION_RESUME;opts.source_session_id="acceptance";
    opts.max_context_bytes=exact-(one_over?1U:0U);opts.explicit_options|=COG_CLI_OPT_CONTEXT;
    assert(cog_cli_app_new(&app,&opts)==COG_OK);
    assert(cog_cli_app_run(app)==(one_over?COG_ERROR_LIMIT:COG_OK));
    cog_mock_state_t state;assert(cog_mock_provider_state(app->provider,&state)==COG_OK);
    assert(state.send_count==(one_over?0U:1U));
    cog_session_snapshot_t after;assert(cog_session_snapshot_view(app->session,&after)==COG_OK);
    assert(after.node_count>=original_nodes+1U && after.ledger_count==1U);
    assert(after.ledger[0].state==COG_TOOL_EXECUTION_SUCCEEDED);
    /* The complete old branch remains inspectable even after failed admission. */
    cog_cli_display_snapshot_t display;assert(cog_cli_app_display_snapshot(app,&display)==COG_OK);
    assert(display.item_count>=5U && display.items[1].text_len==7U && display.items[3].text_len==8U);
    if(!one_over){assert(state.last_request->message_count==5U);
        assert(state.last_request->messages[1].opaque_count==1U);
        assert(memcmp(state.last_request->messages[1].opaque[0].data,opaque_bytes,sizeof(opaque_bytes))==0);}
    cog_cli_app_free(app);cleanup(root);
}
int main(void)
{
    ask(false);ask(true);semantic_finish(COG_FINISH_REASON_LENGTH);
    semantic_finish(COG_FINISH_REASON_CONTENT_FILTER);
    retry_boundary(0U,10U,100U,false);retry_boundary(0U,100U,10U,false);
    retry_boundary(0U,11U,11U,true);
    retry_boundary(5U,15U,100U,false);retry_boundary(5U,100U,15U,false);
    retry_boundary(5U,16U,16U,true);cumulative_context(false);cumulative_context(true);return 0;
}

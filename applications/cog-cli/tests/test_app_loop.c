/** @file test_app_loop.c @brief K real-library application contract checks. */
#define _POSIX_C_SOURCE 200809L
#include "app_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); return 1; } } while (0)
#define TEXT(s) {.kind=COG_PROVIDER_DELTA_TEXT,.data=s,.data_len=sizeof(s)-1U}
#define BEGIN(id_,name_) {.kind=COG_PROVIDER_DELTA_TOOL_BEGIN,.tool_call_id=id_,.tool_name=name_}
#define ARGS(id_,s) {.kind=COG_PROVIDER_DELTA_TOOL_ARGUMENTS,.tool_call_id=id_,.data=s,.data_len=sizeof(s)-1U}
#define END(id_) {.kind=COG_PROVIDER_DELTA_TOOL_END,.tool_call_id=id_}
#define FINISH(reason) {.kind=COG_PROVIDER_DELTA_FINISH,.finish_reason=reason,.outcome=COG_OK}
static cog_cli_options_t options(void)
{
    return (cog_cli_options_t){.backend=COG_CLI_BACKEND_MOCK,
        .explicit_options=COG_CLI_OPT_BACKEND,.prompt="hello"};
}
static int remove_tree(const char *root)
{
    /* Disposable mkdtemp test roots contain only generated fixture names. */
    char command[1024];
    int n = snprintf(command,sizeof(command),"rm -rf -- '%s'",root);
    return n > 0 && (size_t)n < sizeof(command) ? system(command) : -1;
}
static int stream_and_partial(void)
{
    const cog_mock_step_t steps[] = {
        {COG_MOCK_STEP_CHUNK,"part",COG_OK,0,false,0,NULL},
        {COG_MOCK_STEP_ERROR,NULL,COG_ERROR_UNAVAILABLE,500,true,1,"redacted failure"},
        {COG_MOCK_STEP_CHUNK,"new",COG_OK,0,false,0,NULL}};
    const cog_mock_script_t script={steps,3,0,COG_OK,0};
    cog_cli_options_t opts=options(); opts.script=&script;
    cog_cli_app_t *app=NULL; REQUIRE(cog_cli_app_new(&app,&opts)==COG_OK);
    REQUIRE(cog_cli_app_run(app)==COG_ERROR_UNAVAILABLE);
    cog_mock_state_t state; REQUIRE(cog_mock_provider_state(app->provider,&state)==COG_OK);
    REQUIRE(state.send_count==1U);
    cog_cli_display_snapshot_t display;
    REQUIRE(cog_cli_app_display_snapshot(app,&display)==COG_OK);
    REQUIRE(display.item_count==2U && display.items[1].status==COG_MESSAGE_INCOMPLETE);
    REQUIRE(display.items[1].text_len==4U);
    cog_message_t history[8]; size_t count=0U;
    REQUIRE(cog_session_copy_history(app->session,NULL,history,8,&count)==COG_ERROR_INVALID_STATE);
    char prompt[]="continue";
    REQUIRE(cog_cli_app_submit(app,prompt,8)==COG_OK); memset(prompt,'x',8);
    REQUIRE(cog_cli_app_run(app)==COG_OK);
    REQUIRE(cog_session_copy_history(app->session,NULL,history,8,&count)==COG_OK && count==3U);
    REQUIRE(strcmp(history[1].content,"continue")==0 && strcmp(history[2].content,"new")==0);
    cog_session_snapshot_t snapshot; REQUIRE(cog_session_snapshot_view(app->session,&snapshot)==COG_OK);
    REQUIRE(snapshot.node_count==4U);
    bool retained=false;
    for(size_t i=0;i<snapshot.node_count;i++) if(snapshot.nodes[i].message->status==COG_MESSAGE_INCOMPLETE) retained=true;
    REQUIRE(retained);
    cog_cli_app_free(app); return 0;
}
static int cancel_then_queue(void)
{
    const cog_mock_step_t steps[]={
        {COG_MOCK_STEP_CHUNK,"partial",COG_OK,0,false,0,NULL},
        {COG_MOCK_STEP_CANCEL,NULL,COG_OK,0,false,0,NULL},
        {COG_MOCK_STEP_CHUNK,"continued",COG_OK,0,false,0,NULL}};
    const cog_mock_script_t script={steps,3,0,COG_OK,0};
    cog_cli_options_t opts=options();opts.script=&script;opts.queue_text="prequeued work";
    cog_cli_app_t *app=NULL;REQUIRE(cog_cli_app_new(&app,&opts)==COG_OK);
    REQUIRE(cog_cli_app_run(app)==COG_OK);
    cog_mock_state_t state;REQUIRE(cog_mock_provider_state(app->provider,&state)==COG_OK && state.send_count==1U);
    REQUIRE(app->continuation_required);
    bool queued=false;
    for(size_t i=0;i<CLI_INPUTS;i++)if(app->inputs[i].used && app->inputs[i].queued)queued=true;
    REQUIRE(queued);
    cog_cli_display_snapshot_t display;REQUIRE(cog_cli_app_display_snapshot(app,&display)==COG_OK);
    REQUIRE(display.item_count==2U && display.items[1].status==COG_MESSAGE_CANCELLED);
    REQUIRE(cog_cli_app_submit(app,"new authorization",17)==COG_OK);
    REQUIRE(cog_cli_app_run(app)==COG_OK);
    cog_message_t history[8];size_t count=0;
    REQUIRE(cog_session_copy_history(app->session,NULL,history,8,&count)==COG_OK && count==5U);
    REQUIRE(strcmp(history[1].content,"new authorization")==0);
    REQUIRE(strcmp(history[2].content,"continued")==0);
    REQUIRE(strcmp(history[3].content,"prequeued work")==0);
    cog_session_snapshot_t snapshot;REQUIRE(cog_session_snapshot_view(app->session,&snapshot)==COG_OK);
    bool retained=false;
    for(size_t i=0;i<snapshot.node_count;i++)
        if(snapshot.nodes[i].message->status==COG_MESSAGE_CANCELLED && snapshot.nodes[i].content_len==7U)retained=true;
    REQUIRE(retained);
    REQUIRE(cog_mock_provider_state(app->provider,&state)==COG_OK && state.send_count==3U);
    cog_cli_app_free(app);return 0;
}
static int retries(void)
{
    const cog_mock_step_t errors[] = {
        {COG_MOCK_STEP_ERROR,NULL,COG_ERROR_UNAVAILABLE,429,true,1,"rate limited"},
        {COG_MOCK_STEP_ERROR,NULL,COG_ERROR_UNAVAILABLE,429,true,1,"rate limited"},
        {COG_MOCK_STEP_CHUNK,"must not send",COG_OK,0,false,0,NULL}};
    const cog_mock_script_t script={errors,3,0,COG_OK,0};
    for(unsigned int zero=0;zero<2;zero++) {
        cog_cli_options_t opts=options(); opts.script=&script;
        if(zero!=0U) {opts.explicit_options|=COG_CLI_OPT_RETRIES;opts.max_retries=0;}
        cog_cli_app_t *app=NULL; REQUIRE(cog_cli_app_new(&app,&opts)==COG_OK);
        REQUIRE(cog_cli_app_run(app)==COG_ERROR_UNAVAILABLE);
        cog_mock_state_t state; REQUIRE(cog_mock_provider_state(app->provider,&state)==COG_OK);
        REQUIRE(state.send_count==(zero!=0U?1U:2U));
        REQUIRE(state.error_count==state.send_count);
        cog_cli_app_free(app);
    }
    return 0;
}
static const cog_provider_delta_t tool_script[]={
    TEXT("working"),BEGIN("call-1","run_command"),
    ARGS("call-1","{\"command\":\"printf once >> effect; printf done\"}"),END("call-1"),
    FINISH(COG_FINISH_REASON_TOOL_CALLS),TEXT("finished"),FINISH(COG_FINISH_REASON_STOP)};
static int tool_round(bool allow, bool store)
{
    char root[]="/tmp/cog-k-tools-XXXXXX"; REQUIRE(mkdtemp(root)!=NULL);
    char durable[512]; REQUIRE(snprintf(durable,sizeof(durable),"%s/state/parents/store",root)>0);
    cog_cli_options_t opts=options(); opts.mock_deltas=tool_script;
    opts.mock_delta_count=sizeof(tool_script)/sizeof(tool_script[0]); opts.workspace_root=root;
    opts.tool_policy=allow?COG_CLI_TOOL_POLICY_ALLOW:COG_CLI_TOOL_POLICY_DENY;
    opts.explicit_options|=COG_CLI_OPT_WORKSPACE|COG_CLI_OPT_POLICY;
    if(store) {opts.store_mode=COG_CLI_STORE_DURABLE;opts.store_dir=durable;
        opts.explicit_options|=COG_CLI_OPT_STORE_MODE|COG_CLI_OPT_STORE_DIR;}
    cog_cli_app_t *app=NULL; REQUIRE(cog_cli_app_new(&app,&opts)==COG_OK);
    REQUIRE(cog_cli_app_run(app)==COG_OK);
    cog_message_t history[8]; size_t count=0U;
    REQUIRE(cog_session_copy_history(app->session,NULL,history,8,&count)==COG_OK && count==4U);
    REQUIRE(history[1].role==COG_MESSAGE_ROLE_ASSISTANT && history[1].tool_call_count==1U);
    REQUIRE(strcmp(history[1].content,"working")==0 && strcmp(history[2].tool_call_id,"call-1")==0);
    char effect[512]; REQUIRE(snprintf(effect,sizeof(effect),"%s/effect",root)>0);
    if(allow&&store) {
        FILE *file=fopen(effect,"r"); REQUIRE(file!=NULL); char data[16]={0};
        REQUIRE(fread(data,1,15,file)==4U); REQUIRE(fclose(file)==0); REQUIRE(strcmp(data,"once")==0);
        REQUIRE(strstr(history[2].content,"status=ok")!=NULL && strstr(history[2].content,"done")!=NULL);
    } else {REQUIRE(access(effect,F_OK)!=0); REQUIRE(strstr(history[2].content,"denied")!=NULL);}
    cog_mock_state_t state; REQUIRE(cog_mock_provider_state(app->provider,&state)==COG_OK);
    REQUIRE(state.send_count==2U && state.last_request->message_count==3U);
    REQUIRE((state.last_request->tools_json!=NULL)==store);
    cog_session_snapshot_t snapshot; REQUIRE(cog_session_snapshot_view(app->session,&snapshot)==COG_OK);
    REQUIRE(snapshot.ledger_count==1U && snapshot.ledger[0].audit_count==(allow&&store?2U:1U));
    REQUIRE(snapshot.ledger[0].state==(allow&&store?COG_TOOL_EXECUTION_SUCCEEDED:COG_TOOL_EXECUTION_DENIED));
    if(store) {
        cog_session_t *loaded=NULL; REQUIRE(cog_store_load(app->store,cog_session_id(app->session),&loaded)==COG_OK);
        REQUIRE(cog_session_copy_history(loaded,NULL,history,8,&count)==COG_OK && count==4U);
        cog_session_free(loaded);
        cog_cli_app_t *conflict=NULL; opts.session_id=cog_session_id(app->session);
        REQUIRE(cog_cli_app_new(&conflict,&opts)==COG_ERROR_INVALID_STATE && conflict==NULL);
    }
    cog_cli_app_free(app); REQUIRE(remove_tree(root)==0); return 0;
}
static int display_codec(void)
{
    cog_cli_display_item_t item={7,COG_MESSAGE_ROLE_ASSISTANT,COG_MESSAGE_INCOMPLETE,"é",2};
    cog_cli_display_snapshot_t input={"fixture",2,3,&item,1},out={0};
    unsigned char bytes[256], unchanged[256]; memset(bytes,0xa5,sizeof(bytes));
    memcpy(unchanged,bytes,sizeof(bytes)); size_t required=0,ni=0,nt=0;
    REQUIRE(cog_cli_display_snapshot_encode(&input,bytes,1,&required)==COG_ERROR_OVERFLOW);
    REQUIRE(memcmp(bytes,unchanged,sizeof(bytes))==0);
    REQUIRE(cog_cli_display_snapshot_encode(&input,bytes,sizeof(bytes),&required)==COG_OK);
    cog_cli_display_item_t items[2]; char text[64]; memset(text,0x5a,sizeof(text));
    REQUIRE(cog_cli_display_snapshot_decode(bytes,required,&out,items,0,text,1,&ni,&nt)==COG_ERROR_OVERFLOW);
    REQUIRE(out.session_id==NULL && (unsigned char)text[0]==0x5aU && ni==1U);
    REQUIRE(cog_cli_display_snapshot_decode(bytes,required,&out,items,2,text,sizeof(text),&ni,&nt)==COG_OK);
    REQUIRE(strcmp(out.session_id,"fixture")==0 && out.generation==2U && out.through_sequence==3U);
    REQUIRE(out.item_count==1U && strcmp(out.items[0].text,"é")==0);
    bytes[required-1U]=0xffU;
    REQUIRE(cog_cli_display_snapshot_decode(bytes,required,&out,items,2,text,sizeof(text),&ni,&nt)==COG_ERROR_PROTOCOL);
    return 0;
}
static int bounds_and_cancel(void)
{
    cog_cli_options_t opts=options(); opts.mock_mode=COG_CLI_MOCK_CANCEL;
    cog_cli_app_t *app=NULL; REQUIRE(cog_cli_app_new(&app,&opts)==COG_OK);
    REQUIRE(cog_cli_app_run(app)==COG_OK);
    cog_cli_display_snapshot_t display; REQUIRE(cog_cli_app_display_snapshot(app,&display)==COG_OK);
    REQUIRE(display.item_count==2U && display.items[1].status==COG_MESSAGE_CANCELLED);
    cog_cli_app_free(app);
    opts=options();opts.max_context_bytes=4096;opts.explicit_options|=COG_CLI_OPT_CONTEXT;
    REQUIRE(cog_cli_app_new(&app,&opts)==COG_OK);
    char prompt[4097]; memset(prompt,'a',sizeof(prompt));
    REQUIRE(cog_cli_app_submit(app,prompt,sizeof(prompt))==COG_ERROR_LIMIT);
    REQUIRE(cog_cli_app_submit(app,"a\0b",3)==COG_ERROR_INVALID_ARG);
    for(size_t i=0;i<CLI_INPUTS;i++) REQUIRE(cog_cli_app_queue(app,"q",1)==COG_OK);
    REQUIRE(cog_cli_app_queue(app,"q",1)==COG_ERROR_OVERFLOW);
    cog_cli_app_free(app);
    opts=options();opts.mock_deltas=tool_script;opts.mock_delta_count=0;
    REQUIRE(cog_cli_app_new(&app,&opts)==COG_ERROR_INVALID_ARG && app==NULL);
    return 0;
}
int main(void)
{
    REQUIRE(stream_and_partial()==0);REQUIRE(cancel_then_queue()==0);REQUIRE(retries()==0);
    REQUIRE(tool_round(true,true)==0);REQUIRE(tool_round(false,true)==0);
    REQUIRE(tool_round(true,false)==0);REQUIRE(display_codec()==0);REQUIRE(bounds_and_cancel()==0);
    return 0;
}

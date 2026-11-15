/** @file test_session_ux.c @brief Durable application session and recovery UX. */
#define _POSIX_C_SOURCE 200809L
#include "app_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x);return 1;}}while(0)
static cog_cli_options_t options(const char *store)
{
    return (cog_cli_options_t){.backend=COG_CLI_BACKEND_MOCK,.store_mode=COG_CLI_STORE_DURABLE,
        .store_dir=store,.explicit_options=COG_CLI_OPT_BACKEND|COG_CLI_OPT_STORE_MODE|COG_CLI_OPT_STORE_DIR};
}
static cog_result_t refuse_secret(void *arg,const char *ref,char *out,size_t cap,size_t *required)
{(void)arg;(void)ref;(void)out;(void)cap;(void)required;return COG_ERROR_DENIED;}
static int clean(const char *root)
{char command[1024];CHECK(snprintf(command,sizeof(command),"rm -rf -- '%s'",root)>0);CHECK(system(command)==0);return 0;}
static int capture(cog_cli_app_t *app,char *text,size_t capacity,cog_result_t expected)
{
    FILE *output=tmpfile();CHECK(output!=NULL);app->out=output;
    CHECK(cog_cli_app_run(app)==expected);CHECK(fflush(output)==0);rewind(output);
    size_t n=fread(text,1,capacity-1U,output);text[n]='\0';app->out=stdout;CHECK(fclose(output)==0);return 0;
}
static int basics(const char *store)
{
    cog_cli_options_t o=options(store);o.session_id="story";o.prompt="first";o.chunks="one";
    cog_cli_app_t *app=NULL;CHECK(cog_cli_app_new(&app,&o)==COG_OK);CHECK(cog_cli_app_run(app)==COG_OK);
    cog_store_owner_t owner; cog_store_checkpoint_t checkpoint;
    CHECK(cog_store_owner_read(app->store,"story",&owner,&checkpoint)==COG_OK && owner.epoch==1U);
    cog_cli_app_free(app);
    o=options(store);o.backend=COG_CLI_BACKEND_REAL;o.session_action=COG_CLI_SESSION_LIST;o.credential_read=refuse_secret;
    CHECK(cog_cli_app_new(&app,&o)==COG_OK);CHECK(app->provider==NULL && app->core==NULL && app->session==NULL);
    CHECK(cog_cli_app_submit(app,"x",1)==COG_ERROR_INVALID_STATE);
    CHECK(cog_cli_app_queue(app,"x",1)==COG_ERROR_INVALID_STATE);
    CHECK(cog_cli_app_cancel(app)==COG_ERROR_INVALID_STATE);
    cog_cli_display_snapshot_t display;CHECK(cog_cli_app_display_snapshot(app,&display)==COG_ERROR_NOT_FOUND);
    char output[1024];CHECK(capture(app,output,sizeof(output),COG_OK)==0 && strcmp(output,"story\n")==0);
    cog_cli_app_free(app);
    o=options(store);o.session_action=COG_CLI_SESSION_RESUME;o.source_session_id="story";o.prompt="second";o.chunks="two";
    CHECK(cog_cli_app_new(&app,&o)==COG_OK);
    CHECK(cog_store_owner_read(app->store,"story",&owner,&checkpoint)==COG_OK && owner.epoch==2U);
    cog_cli_app_t *other=NULL;CHECK(cog_cli_app_new(&other,&o)==COG_ERROR_AGAIN && other==NULL);
    CHECK(cog_cli_app_run(app)==COG_OK);
    cog_message_t history[16];size_t count=0;
    CHECK(cog_session_copy_history(app->session,NULL,history,16,&count)==COG_OK && count==4U);
    CHECK(strcmp(history[0].content,"first")==0 && strcmp(history[1].content,"one")==0);
    CHECK(strcmp(history[2].content,"second")==0 && strcmp(history[3].content,"two")==0);
    cog_mock_state_t state;CHECK(cog_mock_provider_state(app->provider,&state)==COG_OK && state.send_count==1U && state.last_request->message_count==3U);
    uint64_t old_leaf=cog_node_data(cog_session_active(app->session))->node_id;
    cog_cli_app_free(app);
    o=options(store);o.session_action=COG_CLI_SESSION_FORK;o.source_session_id="story";o.fork_node_id=1;o.prompt="branch";o.chunks="three";
    CHECK(cog_cli_app_new(&app,&o)==COG_OK);CHECK(cog_cli_app_run(app)==COG_OK);
    CHECK(cog_session_copy_history(app->session,NULL,history,16,&count)==COG_OK && count==3U);
    CHECK(strcmp(history[0].content,"first")==0 && strcmp(history[1].content,"branch")==0 && strcmp(history[2].content,"three")==0);
    cog_session_snapshot_t snapshot;CHECK(cog_session_snapshot_view(app->session,&snapshot)==COG_OK && snapshot.node_count==6U);
    CHECK(cog_cli_session_node(app,old_leaf)!=NULL);
    CHECK(cog_cli_app_queue(app,"queued",6)==COG_OK);CHECK(cog_cli_app_fork(app,1)==COG_ERROR_AGAIN);
    cog_cli_app_free(app);
    o=options(store);o.session_action=COG_CLI_SESSION_RESUME;o.source_session_id="missing";
    CHECK(cog_cli_app_new(&app,&o)==COG_ERROR_NOT_FOUND && app==NULL);
    return 0;
}
static int partial(const char *store)
{
    static const cog_mock_step_t steps[]={{COG_MOCK_STEP_CHUNK,"partial",COG_OK,0,false,0,NULL},
        {COG_MOCK_STEP_ERROR,NULL,COG_ERROR_UNAVAILABLE,500,false,0,"sanitized"}};
    const cog_mock_script_t script={steps,2,0,COG_OK,0};
    cog_cli_options_t o=options(store);o.session_id="partial";o.prompt="old";o.script=&script;
    cog_cli_app_t *app=NULL;CHECK(cog_cli_app_new(&app,&o)==COG_OK);CHECK(cog_cli_app_run(app)==COG_ERROR_UNAVAILABLE);cog_cli_app_free(app);
    o=options(store);o.session_action=COG_CLI_SESSION_RESUME;o.source_session_id="partial";o.queue_text="prequeued";o.chunks="continued";
    CHECK(cog_cli_app_new(&app,&o)==COG_OK && app->continuation_required);
    CHECK(cog_cli_app_run(app)==COG_OK);
    cog_mock_state_t state;CHECK(cog_mock_provider_state(app->provider,&state)==COG_OK && state.send_count==0U);
    CHECK(cog_cli_app_submit(app,"fresh authorization",19)==COG_OK);CHECK(cog_cli_app_run(app)==COG_OK);
    CHECK(cog_mock_provider_state(app->provider,&state)==COG_OK && state.send_count==2U);
    cog_message_t history[16];size_t count;
    CHECK(cog_session_copy_history(app->session,NULL,history,16,&count)==COG_OK && count==5U);
    CHECK(strcmp(history[1].content,"fresh authorization")==0 && strcmp(history[3].content,"prequeued")==0);
    const cog_message_t *old=cog_node_data(cog_cli_session_node(app,2))->message;
    CHECK(old->status==COG_MESSAGE_INCOMPLETE && strcmp(old->content,"partial")==0);
    cog_cli_app_free(app);return 0;
}
static int seed(const char *base)
{
    CHECK(mkdir(base,0700)==0 || access(base,F_OK)==0);
    cog_store_config_t config={.base_dir=base,.require_owner=true};cog_store_t *store=NULL;
    CHECK(cog_store_new(&store,&config)==COG_OK);
    cog_session_config_t cfg={.id="uncertain"};cog_session_t *session=NULL;
    CHECK(cog_session_new(&session,&cfg)==COG_OK);
    const cog_message_t user={.role=COG_MESSAGE_ROLE_USER,.content="fixture",.content_len=7};
    CHECK(cog_session_append(session,&user,NULL)==COG_OK);
    static const char args[]="{\"command\":\"printf forbidden >> effect\"}";
    const cog_message_tool_call_t calls[]={{"first","run_command",args,sizeof(args)-1U},{"second","run_command",args,sizeof(args)-1U}};
    static const unsigned char opaque_data[]="{\"reasoning_details\":[]}";
    const cog_message_opaque_t opaque={"openrouter.chat-completions","json-v1",opaque_data,sizeof(opaque_data)-1U};
    const cog_message_t proposal={.role=COG_MESSAGE_ROLE_ASSISTANT,.content="pending",.content_len=7,
        .tool_calls=calls,.tool_call_count=2,.opaque=&opaque,.opaque_count=1};
    cog_session_node_t *assistant=NULL;CHECK(cog_session_append(session,&proposal,&assistant)==COG_OK);
    cog_store_owner_t owner={1,991};cog_store_owner_guard_t *guard=NULL;
    CHECK(cog_store_owner_compare_exchange(store,"uncertain",(cog_store_owner_t){0},owner,(cog_store_checkpoint_t){0})==COG_OK);
    CHECK(cog_store_owner_acquire(store,"uncertain",owner,&guard)==COG_OK);
    CHECK(cog_session_tool_transition(session,assistant,"first",COG_TOOL_EXECUTION_PROPOSED,COG_TOOL_EXECUTION_STARTED)==COG_OK);
    CHECK(cog_session_tool_transition(session,assistant,"second",COG_TOOL_EXECUTION_PROPOSED,COG_TOOL_EXECUTION_STARTED)==COG_OK);
    CHECK(cog_store_save(store,session)==COG_OK);CHECK(cog_store_owner_release(guard)==COG_OK);cog_session_free(session);
    cfg.id="usage";CHECK(cog_session_new(&session,&cfg)==COG_OK);CHECK(cog_session_append(session,&user,NULL)==COG_OK);
    const cog_message_t answer={.role=COG_MESSAGE_ROLE_ASSISTANT,.content="known",.content_len=5,
        .usage={.tokens_known=true,.input_tokens=17,.output_tokens=3,.cost_known=true,.cost=0.001,.currency="USD"}};
    CHECK(cog_session_append(session,&answer,NULL)==COG_OK);owner.instance_id=992;
    CHECK(cog_store_owner_compare_exchange(store,"usage",(cog_store_owner_t){0},owner,(cog_store_checkpoint_t){0})==COG_OK);
    CHECK(cog_store_owner_acquire(store,"usage",owner,&guard)==COG_OK);CHECK(cog_store_save(store,session)==COG_OK);
    CHECK(cog_store_owner_release(guard)==COG_OK);cog_session_free(session);cog_store_free(store);return 0;
}
static int recovery(const char *store)
{
    CHECK(seed(store)==0);
    cog_cli_options_t o=options(store);o.session_action=COG_CLI_SESSION_RESUME;o.source_session_id="uncertain";
    cog_cli_app_t *app=NULL;CHECK(cog_cli_app_new(&app,&o)==COG_ERROR_INDETERMINATE && app==NULL);
    o.session_action=COG_CLI_SESSION_RECOVER;o.backend=COG_CLI_BACKEND_REAL;o.previous_processes_fenced=true;o.credential_read=refuse_secret;
    CHECK(cog_cli_app_new(&app,&o)==COG_OK && app->provider==NULL && app->core==NULL && app->workspace==NULL);
    cog_cli_app_t *competing=NULL;CHECK(cog_cli_app_new(&competing,&o)==COG_ERROR_AGAIN && competing==NULL);
    CHECK(cog_cli_app_fork(app,1)==COG_ERROR_INVALID_STATE);
    CHECK(cog_cli_app_submit(app,"unsafe",6)==COG_ERROR_INVALID_STATE);
    CHECK(cog_cli_app_run(app)==COG_ERROR_INDETERMINATE);
    CHECK(cog_cli_app_reconcile(app,999,"first",COG_TOOL_EXECUTION_ACKNOWLEDGED,"evidence",8)==COG_ERROR_NOT_FOUND);
    CHECK(cog_cli_app_reconcile(app,2,"missing",COG_TOOL_EXECUTION_ACKNOWLEDGED,"evidence",8)==COG_ERROR_NOT_FOUND);
    CHECK(cog_cli_app_reconcile(app,2,"first",COG_TOOL_EXECUTION_ACKNOWLEDGED,"",0)==COG_ERROR_INVALID_ARG);
    const char *evidence="fixture fenced; effects unknown";
    CHECK(cog_cli_app_reconcile(app,2,"first",COG_TOOL_EXECUTION_ACKNOWLEDGED,evidence,strlen(evidence))==COG_OK);
    CHECK(cog_cli_app_reconcile(app,2,"first",COG_TOOL_EXECUTION_ACKNOWLEDGED,evidence,strlen(evidence))==COG_ERROR_INVALID_STATE);
    CHECK(cog_cli_app_run(app)==COG_ERROR_INDETERMINATE);cog_cli_app_free(app);
    CHECK(cog_cli_app_new(&app,&o)==COG_OK);
    evidence="fixture operator verified no tool ever started";
    CHECK(cog_cli_app_reconcile(app,2,"second",COG_TOOL_EXECUTION_FAILED,evidence,strlen(evidence))==COG_OK);
    CHECK(cog_cli_app_submit(app,"still offline",13)==COG_ERROR_INVALID_STATE);cog_cli_app_free(app);
    o=options(store);o.session_action=COG_CLI_SESSION_RESUME;o.source_session_id="uncertain";o.prompt="after";o.chunks="safe";
    CHECK(cog_cli_app_new(&app,&o)==COG_OK);CHECK(cog_cli_app_run(app)==COG_OK);
    cog_message_t history[16];size_t count;
    CHECK(cog_session_copy_history(app->session,NULL,history,16,&count)==COG_OK && count==6U);
    CHECK(history[1].opaque_count==1U && history[1].opaque[0].size==24U);
    CHECK(strstr(history[2].content,"effects remain unknown")!=NULL);
    cog_session_snapshot_t snapshot;CHECK(cog_session_snapshot_view(app->session,&snapshot)==COG_OK && snapshot.ledger_count==2U);
    CHECK(snapshot.ledger[0].state==COG_TOOL_EXECUTION_ACKNOWLEDGED && snapshot.ledger[0].audit_count==3U);
    CHECK(snapshot.ledger[1].state==COG_TOOL_EXECUTION_FAILED && snapshot.ledger[1].audit_count==3U);
    cog_cli_app_free(app);return 0;
}
static int metadata_edges(const char *base)
{
    char missing[512];CHECK(snprintf(missing,sizeof(missing),"%s/missing/parents",base)>0);
    cog_cli_options_t o=options(missing);o.session_action=COG_CLI_SESSION_LIST;
    cog_cli_app_t *app=NULL;CHECK(cog_cli_app_new(&app,&o)==COG_OK);CHECK(cog_cli_app_run(app)==COG_OK);
    CHECK(access(missing,F_OK)!=0);CHECK(cog_cli_app_shutdown(app)==COG_OK);cog_cli_app_free(app);
    char legacy[512],path[512];CHECK(snprintf(legacy,sizeof(legacy),"%s/legacy",base)>0);CHECK(mkdir(legacy,0700)==0);
    CHECK(snprintf(path,sizeof(path),"%s/meta.json",legacy)>0);FILE *file=fopen(path,"w");CHECK(file!=NULL);
    CHECK(fputs("{\"version\":1,\"id\":\"bGVnYWN5\",\"parent_id\":null,\"max_nodes\":8,\"node_count\":0,\"next_node_id\":1,\"active_node_id\":0}",file)>=0);CHECK(fclose(file)==0);
    CHECK(snprintf(path,sizeof(path),"%s/nodes.jsonl",legacy)>0);file=fopen(path,"w");CHECK(file!=NULL);CHECK(fclose(file)==0);
    o=options(base);o.session_action=COG_CLI_SESSION_RESUME;o.source_session_id="legacy";
    CHECK(cog_cli_app_new(&app,&o)==COG_ERROR_DENIED && app==NULL);
    o.previous_processes_fenced=true;o.prompt="migrated";o.chunks="ok";
    CHECK(cog_cli_app_new(&app,&o)==COG_OK);CHECK(cog_cli_app_run(app)==COG_OK);
    cog_store_load_report_t report;cog_session_t *loaded=NULL;
    CHECK(cog_store_load_ex(app->store,"legacy",COG_STORE_LOAD_REPLAY,&loaded,&report)==COG_OK && report.source_version==2U);
    cog_session_free(loaded);cog_cli_app_free(app);return 0;
}
int main(int argc,char **argv)
{
    if(argc==3 && strcmp(argv[1],"seed")==0)return seed(argv[2]);
    char root[]="/tmp/cog-l-ux-XXXXXX";CHECK(mkdtemp(root)!=NULL);
    char store[512];CHECK(snprintf(store,sizeof(store),"%s/state",root)>0);
    CHECK(basics(store)==0);CHECK(partial(store)==0);CHECK(recovery(store)==0);CHECK(metadata_edges(store)==0);
    CHECK(clean(root)==0);return 0;
}

/** @file test_app_reload.c @brief Actual bounded app/provider reload composition. */
#define _POSIX_C_SOURCE 200809L
#include "app_internal.h"
#include <cog/config.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef COG_RELOAD_REAL_PROFILE
#include <curl/curl.h>
#endif
#define CHECK(x) do {if(!(x)){fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x);return 1;}}while(0)
#ifdef COG_RELOAD_WRAPPERS
/* These interpositions run the real constructors/transactions/destructors.
   Explicit owner-thread test control runs between core dispatches. */
static unsigned commit_fault, connect_fault, disconnect_fault;
static cog_cli_app_t *fault_app;
static cog_result_t (*owner_control)(cog_cli_app_t *app);
static bool controlled;
typedef struct connection {cog_provider_t *provider;const cog_endpoint_t *endpoint;bool consumed;} connection_t;
static connection_t connections[256];static size_t connection_count;
typedef struct sent_request {cog_provider_t *provider;uint64_t generation;char model[64];} sent_request_t;
static sent_request_t sends[256];static size_t send_count;
static uint64_t pending_generations[32];static size_t pending_count;
cog_result_t __real_cog_cli_service(void *arg,uint64_t now,uint64_t *deadline);
cog_result_t __wrap_cog_cli_service(void *arg,uint64_t now,uint64_t *deadline);
cog_result_t __real_cog_config_commit(cog_config_txn_t *txn);
cog_result_t __wrap_cog_config_commit(cog_config_txn_t *txn);
cog_result_t __real_cog_provider_connect(const cog_endpoint_t *endpoint,cog_provider_t **provider);
cog_result_t __wrap_cog_provider_connect(const cog_endpoint_t *endpoint,cog_provider_t **provider);
cog_result_t __real_cog_provider_disconnect(cog_provider_t *provider);
cog_result_t __wrap_cog_provider_disconnect(cog_provider_t *provider);
cog_result_t __real_cog_provider_send(cog_provider_t *provider,const cog_provider_request_t *request);
cog_result_t __wrap_cog_provider_send(cog_provider_t *provider,const cog_provider_request_t *request);
cog_result_t __real_cog_provider_preflight(cog_provider_t *provider,const cog_provider_request_t *request,cog_provider_preflight_stage_t stage);
cog_result_t __wrap_cog_provider_preflight(cog_provider_t *provider,const cog_provider_request_t *request,cog_provider_preflight_stage_t stage);
void __real_cog_cli_configuration_free(cog_cli_configuration_t *configuration);
void __wrap_cog_cli_configuration_free(cog_cli_configuration_t *configuration);
cog_result_t __wrap_cog_cli_service(void *arg,uint64_t now,uint64_t *deadline)
{
    if(owner_control!=NULL){cog_result_t rc=owner_control(arg);if(rc!=COG_OK)return rc;}
    return __real_cog_cli_service(arg,now,deadline);
}
cog_result_t __wrap_cog_config_commit(cog_config_txn_t *txn)
{
    unsigned mode=commit_fault;commit_fault=0U;
    cog_config_txn_t *held=NULL;
    if(mode==1U)assert(cog_config_set(txn,"unexpected","rejected")==COG_OK);
    if(mode==2U){
        /* Actually exhaust the fixed64-node pool after shrinking this candidate.
           Rejection cannot restore the seven-node running snapshot: real LIMIT
           finalizes this exact app transaction, not a fabricated return value. */
        assert(fault_app!=NULL);
        assert(cog_config_delete(txn,"models")==COG_OK);
        assert(cog_config_delete(txn,"providers")==COG_OK);
        assert(cog_config_begin(fault_app->reload_tree,&held)==COG_OK);
        for(unsigned i=0U;i<64U;i++){
            char path[32];assert(snprintf(path,sizeof(path),"exhaust.n%u",i)>0);
            cog_result_t rc=cog_config_set(held,path,"v");
            if(rc==COG_ERROR_LIMIT)break;
            assert(rc==COG_OK);
        }
    }
    cog_result_t rc=__real_cog_config_commit(txn);
    if(mode==1U)assert(rc==COG_ERROR_INVALID_ARG);
    if(mode==2U){assert(rc==COG_ERROR_LIMIT);assert(cog_config_set(txn,"probe","v")==COG_ERROR_INVALID_STATE);}
    cog_config_rollback(held);return rc;
}
cog_result_t __wrap_cog_provider_connect(const cog_endpoint_t *endpoint,cog_provider_t **provider)
{
    if(connect_fault!=0U){connect_fault=0U;*provider=NULL;return COG_ERROR_NOMEM;}
    cog_result_t rc=__real_cog_provider_connect(endpoint,provider);
    if(rc==COG_OK){assert(connection_count<256U);connections[connection_count++]=(connection_t){*provider,endpoint,false};}
    return rc;
}
cog_result_t __wrap_cog_provider_disconnect(cog_provider_t *provider)
{
    for(size_t i=connection_count;i>0U;i--)if(connections[i-1U].provider==provider && provider!=NULL){
        connection_t *connection=&connections[i-1U];assert(!connection->consumed);
        if(connection->endpoint->api_key!=NULL)assert(connection->endpoint->api_key[0]!='\0');
        connection->consumed=true;break;
    }
    cog_result_t rc=__real_cog_provider_disconnect(provider);
    if(provider!=NULL && disconnect_fault!=0U){disconnect_fault=0U;return COG_ERROR_IO;}
    return rc;
}
void __wrap_cog_cli_configuration_free(cog_cli_configuration_t *configuration)
{
    const cog_endpoint_t *endpoint=cog_cli_configuration_endpoint(configuration);
    if(endpoint!=NULL)for(size_t i=0U;i<connection_count;i++)
        if(connections[i].endpoint==endpoint)assert(connections[i].consumed);
    __real_cog_cli_configuration_free(configuration);
}
cog_result_t __wrap_cog_provider_send(cog_provider_t *provider,const cog_provider_request_t *request)
{
    assert(send_count<256U);sent_request_t *sent=&sends[send_count++];sent->provider=provider;
    sent->generation=request->configuration_generation;
    assert(strlen(request->model_id)<sizeof(sent->model));strcpy(sent->model,request->model_id);
    return __real_cog_provider_send(provider,request);
}
cog_result_t __wrap_cog_provider_preflight(cog_provider_t *provider,const cog_provider_request_t *request,cog_provider_preflight_stage_t stage)
{
    if(stage==COG_PROVIDER_PREFLIGHT_PENDING_TOOLS){assert(pending_count<32U);pending_generations[pending_count++]=request->configuration_generation;}
    return __real_cog_provider_preflight(provider,request,stage);
}
static void reset_controls(void)
{
    assert(commit_fault==0U && connect_fault==0U && disconnect_fault==0U);
    owner_control=NULL;controlled=false;fault_app=NULL;connection_count=0U;send_count=0U;pending_count=0U;
}
#endif
static cog_cli_options_t options(void)
{
    return (cog_cli_options_t){.backend=COG_CLI_BACKEND_MOCK,.model_id="old/model",
        .explicit_options=COG_CLI_OPT_BACKEND|COG_CLI_OPT_MODEL,.chunks="answer"};
}
static int basic(void)
{
    cog_cli_options_t opts=options();opts.prompt="first";
    cog_cli_app_t *app=NULL;CHECK(cog_cli_app_new(&app,&opts)==COG_OK);
    cog_provider_t *old=app->provider;uint64_t generation=999U;
    char model[]="new/model";cog_cli_reload_options_t next={.model_id=model};
    CHECK(cog_cli_app_reload(app,&next,&generation)==COG_OK && generation==2U);
    CHECK(app->provider==old && cog_core_provider(app->core)==old);
    CHECK(app->future_provider!=NULL && app->future_provider!=old && app->configuration_generation==1U);
    cog_mock_state_t state;CHECK(cog_mock_provider_state(old,&state)==COG_OK && state.send_count==0U);
    CHECK(cog_mock_provider_state(app->future_provider,&state)==COG_OK && state.send_count==0U);
    CHECK(cog_cli_app_reload(app,&next,&generation)==COG_ERROR_AGAIN && generation==2U);
    memset(model,'x',sizeof(model)-1U);
    CHECK(cog_cli_app_run(app)==COG_OK && app->configuration_generation==2U);
    CHECK(app->future_provider==NULL && app->retiring_provider==NULL && !app->reload_notice_pending);
    CHECK(cog_core_provider(app->core)==app->provider);
    CHECK(cog_mock_provider_state(app->provider,&state)==COG_OK && state.send_count==1U);
    CHECK(state.last_request->configuration_generation==2U && strcmp(state.last_request->model_id,"new/model")==0);
    CHECK(state.last_request->message_count==1U && strcmp(state.last_request->messages[0].content,"first")==0);
    cog_config_txn_t *txn=NULL;const char *value=NULL;
    CHECK(cog_config_begin(app->reload_tree,&txn)==COG_OK);
    CHECK(cog_config_get(txn,"models.default",&value)==COG_OK && strcmp(value,"new/model")==0);
    cog_config_rollback(txn);cog_cli_app_free(app);return 0;
}
static int bounds(void)
{
    cog_cli_options_t opts=options();cog_cli_app_t *app=NULL;
    CHECK(cog_cli_app_new(&app,&opts)==COG_OK);
    uint64_t output=777U;const cog_cli_reload_options_t valid={.model_id="next/model"};
    CHECK(cog_cli_app_reload(NULL,&valid,&output)==COG_ERROR_NULL_PTR);
    CHECK(cog_cli_app_reload(app,NULL,&output)==COG_ERROR_NULL_PTR);
    CHECK(cog_cli_app_reload(app,&valid,NULL)==COG_ERROR_NULL_PTR);
    CHECK(cog_cli_app_reload(app,&(cog_cli_reload_options_t){0},&output)==COG_ERROR_INVALID_ARG);
    CHECK(cog_cli_app_reload(app,&(cog_cli_reload_options_t){.base_url="https://fixture.invalid"},&output)==COG_ERROR_UNSUPPORTED);
    CHECK(app->reload_attempts==0U && output==777U);
    for(unsigned i=0U;i<CLI_RELOAD_ATTEMPTS;i++) {
        CHECK(cog_cli_app_reload(app,&(cog_cli_reload_options_t){.model_id=""},&output)==COG_ERROR_INVALID_ARG);
        CHECK(app->reload_attempts==i+1U && app->configuration_generation==1U && output==777U);
    }
    CHECK(cog_cli_app_reload(app,&valid,&output)==COG_ERROR_LIMIT && output==777U);
    cog_mock_state_t state;CHECK(cog_mock_provider_state(app->provider,&state)==COG_OK && state.send_count==0U);
    cog_cli_app_free(app);
    opts.prompt="last eligible attempt";CHECK(cog_cli_app_new(&app,&opts)==COG_OK);
    for(unsigned i=0U;i<CLI_RELOAD_ATTEMPTS-1U;i++)
        CHECK(cog_cli_app_reload(app,&(cog_cli_reload_options_t){.model_id=""},&output)==COG_ERROR_INVALID_ARG);
    CHECK(cog_cli_app_reload(app,&valid,&output)==COG_OK && output==65U);
    CHECK(cog_cli_app_run(app)==COG_OK);
    CHECK(cog_mock_provider_state(app->provider,&state)==COG_OK && state.last_request->configuration_generation==65U);
    CHECK(cog_cli_app_reload(app,&valid,&output)==COG_ERROR_LIMIT && output==65U);
    cog_cli_app_free(app);return 0;
}
static int queue_before_notice(void)
{
    cog_cli_options_t opts=options();cog_cli_app_t *app=NULL;CHECK(cog_cli_app_new(&app,&opts)==COG_OK);
    /* Fill all but the final slot with harmless stale notifications, then put
       copied input before publication's deferred real notice. */
    const cog_event_t stale={.id=COG_EVENT_CONFIG_CHANGED,.kind=COG_EVENT_CONFIG_CHANGED,.source_id=999U};
    for(size_t i=0U;i<63U;i++)CHECK(cog_core_enqueue(app->core,&stale)==COG_OK);
    CHECK(cog_cli_app_submit(app,"queued before notice",20U)==COG_OK);
    uint64_t generation=0U;CHECK(cog_cli_app_reload(app,&(cog_cli_reload_options_t){.model_id="queued/model"},&generation)==COG_OK);
    CHECK(cog_cli_reload_service(app)==COG_OK && app->reload_notice_pending && !app->reload_notice_queued);
    CHECK(cog_cli_app_run(app)==COG_OK);
    cog_mock_state_t state;CHECK(cog_mock_provider_state(app->provider,&state)==COG_OK && state.send_count==1U);
    CHECK(state.last_request->configuration_generation==2U && strcmp(state.last_request->model_id,"queued/model")==0);
    CHECK(!app->reload_notice_pending && !app->reload_notice_queued && app->request_serial==1U);
    cog_cli_app_free(app);return 0;
}
#ifdef COG_RELOAD_WRAPPERS
static cog_result_t retained_control(cog_cli_app_t *app)
{
    if(controlled || !app->active || !app->provider_active)return COG_OK;
    controlled=true;size_t first=0U,again=0U;cog_provider_delta_t delta;
    assert(cog_provider_next(app->provider,&delta,NULL,0U,&first)==COG_ERROR_OVERFLOW && first>0U);
    cog_provider_t *old=app->provider;cog_provider_error_t before={0},error={0};
    cog_result_t error_status=cog_provider_get_error(old,&before);
    uint64_t sequence=app->sequence,serial=app->request_serial,now=app->now;
    uint64_t generation=0U;
    assert(cog_cli_app_reload(app,&(cog_cli_reload_options_t){.model_id="retained/new"},&generation)==COG_OK);
    assert(generation==2U && app->provider==old && app->configuration_generation==1U);
    assert(app->now==now && app->sequence==sequence && app->request_serial==serial);
    assert(cog_provider_next(old,&delta,NULL,0U,&again)==COG_ERROR_OVERFLOW && again==first);
    assert(cog_provider_get_error(old,&error)==error_status);assert(error.code==before.code && error.message==before.message);
    assert(cog_cli_app_queue(app,"next",4U)==COG_OK);return COG_OK;
}
static int retained_turn(void)
{
    reset_controls();cog_cli_options_t opts=options();opts.prompt="old";
    cog_cli_app_t *app=NULL;CHECK(cog_cli_app_new(&app,&opts)==COG_OK);
    owner_control=retained_control;CHECK(cog_cli_app_run(app)==COG_OK && controlled);owner_control=NULL;
    CHECK(send_count==2U && sends[0].provider!=sends[1].provider);
    CHECK(sends[0].generation==1U && strcmp(sends[0].model,"old/model")==0);
    CHECK(sends[1].generation==2U && strcmp(sends[1].model,"retained/new")==0);
    CHECK(app->request_serial==2U && app->configuration_generation==2U);
    cog_cli_app_free(app);return 0;
}
static cog_result_t backoff_control(cog_cli_app_t *app)
{
    if(controlled || cog_core_state(app->core)!=CLI_BACKOFF)return COG_OK;
    controlled=true;uint64_t generation=0U;
    assert(cog_cli_app_reload(app,&(cog_cli_reload_options_t){.model_id="backoff/new"},&generation)==COG_OK);
    assert(app->retries==1U && app->configuration_generation==1U);
    assert(cog_cli_app_queue(app,"next",4U)==COG_OK);return COG_OK;
}
static int backoff_turn(void)
{
    reset_controls();static const cog_mock_step_t steps[]={
        {COG_MOCK_STEP_ERROR,NULL,COG_ERROR_UNAVAILABLE,429,true,1U,"fixture"},
        {COG_MOCK_STEP_CHUNK,"retried",COG_OK,0,false,0U,NULL}};
    const cog_mock_script_t script={steps,2U,0U,COG_OK,0U};
    cog_cli_options_t opts=options();opts.prompt="old";opts.script=&script;
    cog_cli_app_t *app=NULL;CHECK(cog_cli_app_new(&app,&opts)==COG_OK);
    owner_control=backoff_control;CHECK(cog_cli_app_run(app)==COG_OK && controlled);owner_control=NULL;
    CHECK(send_count==4U && sends[0].generation==1U && sends[1].generation==1U);
    CHECK(sends[2].generation==2U && sends[3].generation==2U);
    CHECK(strcmp(sends[1].model,"old/model")==0 && strcmp(sends[3].model,"backoff/new")==0);
    cog_cli_app_free(app);return 0;
}
static int transaction_failures(void)
{
    for(unsigned mode=0U;mode<4U;mode++){
        reset_controls();cog_cli_options_t opts=options();opts.prompt="old stays usable";
        cog_cli_app_t *app=NULL;CHECK(cog_cli_app_new(&app,&opts)==COG_OK);fault_app=app;
        cog_provider_t *old=app->provider;cog_cli_configuration_t *configuration=app->configuration;
        if(mode==0U)connect_fault=1U;else commit_fault=mode==2U?2U:1U;
        if(mode==3U)disconnect_fault=1U;
        const cog_result_t expected[]={COG_ERROR_NOMEM,COG_ERROR_INVALID_ARG,COG_ERROR_LIMIT,COG_ERROR_INVALID_ARG};
        uint64_t output=765U;
        CHECK(cog_cli_app_reload(app,&(cog_cli_reload_options_t){.model_id="rejected/model"},&output)==expected[mode]);
        CHECK(output==765U && app->provider==old && app->configuration==configuration && app->future_provider==NULL);
        CHECK(app->reload_attempts==1U && app->configuration_generation==1U);
        cog_config_txn_t *txn=NULL;const char *value=NULL;CHECK(cog_config_begin(app->reload_tree,&txn)==COG_OK);
        CHECK(cog_config_get(txn,"models.default",&value)==COG_OK && strcmp(value,"old/model")==0);cog_config_rollback(txn);
        if(mode==3U){
            CHECK(app->reload_cleanup_error==COG_ERROR_IO && app->shutdown);
            CHECK(cog_cli_app_submit(app,"unsafe",6U)==COG_ERROR_INDETERMINATE);
            bool handled=false;CHECK(cog_cli_reload_command(app,"/reload model retry",&handled)==COG_ERROR_IO && handled);
            CHECK(cog_cli_app_run(app)==COG_ERROR_IO && send_count==0U);
        }else{
            CHECK(app->reload_cleanup_error==COG_OK && cog_cli_app_run(app)==COG_OK);
            CHECK(send_count==1U && sends[0].generation==1U && strcmp(sends[0].model,"old/model")==0);
        }
        cog_cli_app_free(app);
    }
    return 0;
}
static int consuming_retirement(void)
{
    reset_controls();cog_cli_options_t opts=options();cog_cli_app_t *app=NULL;
    CHECK(cog_cli_app_new(&app,&opts)==COG_OK);uint64_t generation=0U;
    CHECK(cog_cli_app_reload(app,&(cog_cli_reload_options_t){.model_id="committed/new"},&generation)==COG_OK);
    cog_provider_t *next=app->future_provider;disconnect_fault=1U;
    CHECK(cog_cli_reload_activate(app)==COG_ERROR_IO);
    CHECK(app->provider==next && cog_core_provider(app->core)==next && app->configuration_generation==2U);
    CHECK(app->future_provider==NULL && app->retiring_provider==NULL && app->retiring_configuration==NULL);
    CHECK(app->reload_cleanup_error==COG_ERROR_IO && app->shutdown);
    CHECK(cog_cli_app_run(app)==COG_ERROR_IO && send_count==0U);
    cog_cli_app_free(app);return 0;
}
static int eof_boundaries(void)
{
    for(unsigned mode=0U;mode<2U;mode++){
        reset_controls();cog_cli_options_t opts=options();opts.interactive=true;
        cog_cli_app_t *app=NULL;CHECK(cog_cli_app_new(&app,&opts)==COG_OK);
        uint64_t generation=0U;
        const char *line=mode==0U?"/reload model eof/new":"last input";
        if(mode!=0U)CHECK(cog_cli_app_reload(app,&(cog_cli_reload_options_t){.model_id="last/new"},&generation)==COG_OK);
        int fds[2];CHECK(pipe(fds)==0);size_t length=strlen(line);
        CHECK(write(fds[1],line,length)==(ssize_t)length);CHECK(close(fds[1])==0);
        int saved=dup(STDIN_FILENO);CHECK(saved>=0 && dup2(fds[0],STDIN_FILENO)>=0);CHECK(close(fds[0])==0);
        for(size_t i=0U;i<=length;i++)CHECK(cog_cli_idle(app,0U)==COG_OK);
        CHECK(dup2(saved,STDIN_FILENO)>=0 && close(saved)==0);
        CHECK(app->stdin_eof && app->future_provider!=NULL);
        CHECK(cog_cli_app_run(app)==COG_OK);
        if(mode==0U){CHECK(send_count==0U && app->configuration_generation==1U && app->future_provider!=NULL);}
        else{CHECK(send_count==1U && sends[0].generation==2U && app->future_provider==NULL);}
        cog_cli_app_free(app);
    }
    return 0;
}
#define BEGIN_CALL {.kind=COG_PROVIDER_DELTA_TOOL_BEGIN,.tool_call_id="effect",.tool_name="run_command"}
#define CALL_ARGS {.kind=COG_PROVIDER_DELTA_TOOL_ARGUMENTS,.tool_call_id="effect",.data="{\"command\":\"printf once >> effect\"}",.data_len=sizeof("{\"command\":\"printf once >> effect\"}")-1U}
#define END_CALL {.kind=COG_PROVIDER_DELTA_TOOL_END,.tool_call_id="effect"}
#define FINISH(r) {.kind=COG_PROVIDER_DELTA_FINISH,.finish_reason=r,.outcome=COG_OK}
static cog_result_t tool_control(cog_cli_app_t *app)
{
    if(controlled || app->execution==NULL)return COG_OK;
    controlled=true;uint64_t generation=0U;cog_toolkit_execution_t *execution=app->execution;
    cog_store_owner_guard_t *guard=app->guard;cog_workspace_t *workspace=app->workspace;
    assert(cog_cli_app_reload(app,&(cog_cli_reload_options_t){.model_id="tool/new"},&generation)==COG_OK);
    assert(app->execution==execution && app->guard==guard && app->workspace==workspace);
    assert(app->opts.tool_policy==COG_CLI_TOOL_POLICY_ALLOW && app->configuration_generation==1U);
    return COG_OK;
}
static int tool_turn(void)
{
    reset_controls();char root[]="/tmp/cog-r12-tool-XXXXXX";CHECK(mkdtemp(root)!=NULL);
    static const cog_provider_delta_t deltas[]={BEGIN_CALL,CALL_ARGS,END_CALL,FINISH(COG_FINISH_REASON_TOOL_CALLS),
        {.kind=COG_PROVIDER_DELTA_TEXT,.data="done",.data_len=4U},FINISH(COG_FINISH_REASON_STOP)};
    cog_cli_options_t opts=options();opts.prompt="execute once";opts.store_dir=root;opts.workspace_root=root;
    opts.store_mode=COG_CLI_STORE_DURABLE;opts.tool_policy=COG_CLI_TOOL_POLICY_ALLOW;
    opts.explicit_options|=COG_CLI_OPT_STORE_DIR|COG_CLI_OPT_WORKSPACE|COG_CLI_OPT_STORE_MODE|COG_CLI_OPT_POLICY;
    opts.mock_deltas=deltas;opts.mock_delta_count=sizeof(deltas)/sizeof(deltas[0]);
    cog_cli_app_t *app=NULL;CHECK(cog_cli_app_new(&app,&opts)==COG_OK);
    owner_control=tool_control;CHECK(cog_cli_app_run(app)==COG_OK && controlled);owner_control=NULL;
    CHECK(send_count==2U && sends[0].generation==1U && sends[1].generation==1U);
    CHECK(strcmp(sends[1].model,"old/model")==0 && app->configuration_generation==2U);
    cog_mock_state_t state;CHECK(cog_mock_provider_state(app->provider,&state)==COG_OK && state.send_count==0U);
    char path[512];CHECK(snprintf(path,sizeof(path),"%s/effect",root)>0);FILE *file=fopen(path,"r");CHECK(file!=NULL);
    char bytes[16]={0};CHECK(fread(bytes,1,sizeof(bytes),file)==4U && memcmp(bytes,"once",4U)==0);CHECK(fclose(file)==0);
    cog_session_snapshot_t snapshot;CHECK(cog_session_snapshot_view(app->session,&snapshot)==COG_OK);
    CHECK(snapshot.ledger_count==1U && snapshot.ledger[0].state==COG_TOOL_EXECUTION_SUCCEEDED);
    cog_cli_app_free(app);char cleanup[600];CHECK(snprintf(cleanup,sizeof(cleanup),"rm -rf -- '%s'",root)>0);CHECK(system(cleanup)==0);
    return 0;
}
static cog_result_t policy_control(cog_cli_app_t *app)
{
    if(controlled || !app->policy_prompt || app->future_provider==NULL)return COG_OK;
    controlled=true;
    cog_toolkit_policy_decision_t decision=app->policy_decision;
    assert(decision==COG_TOOLKIT_POLICY_UNDECIDED && app->opts.tool_policy==COG_CLI_TOOL_POLICY_ASK);
    assert(app->configuration_generation==1U);
    return COG_OK;
}
static int policy_turn(void)
{
    reset_controls();char root[]="/tmp/cog-r12-policy-XXXXXX";CHECK(mkdtemp(root)!=NULL);
    static const cog_provider_delta_t deltas[]={BEGIN_CALL,CALL_ARGS,END_CALL,FINISH(COG_FINISH_REASON_TOOL_CALLS),FINISH(COG_FINISH_REASON_STOP)};
    cog_cli_options_t opts=options();opts.prompt="ask first";opts.store_dir=root;opts.workspace_root=root;
    opts.interactive=true;opts.store_mode=COG_CLI_STORE_DURABLE;
    opts.explicit_options|=COG_CLI_OPT_STORE_DIR|COG_CLI_OPT_WORKSPACE|COG_CLI_OPT_STORE_MODE;
    opts.mock_deltas=deltas;opts.mock_delta_count=sizeof(deltas)/sizeof(deltas[0]);
    cog_cli_app_t *app=NULL;CHECK(cog_cli_app_new(&app,&opts)==COG_OK);
    int fds[2];CHECK(pipe(fds)==0);static const char command[]="/reload model policy/new\n";
    CHECK(write(fds[1],command,sizeof(command)-1U)==(ssize_t)(sizeof(command)-1U));CHECK(close(fds[1])==0);
    int saved=dup(STDIN_FILENO);CHECK(saved>=0 && dup2(fds[0],STDIN_FILENO)>=0);CHECK(close(fds[0])==0);
    owner_control=policy_control;
    /* Actual terminal command must be recognized ahead of the allow/deny
       answer parser. Subsequent EOF explicitly denies the unchanged old call. */
    cog_result_t result=cog_cli_app_run(app);owner_control=NULL;
    CHECK(dup2(saved,STDIN_FILENO)>=0 && close(saved)==0);
    CHECK(result==COG_OK && controlled && app->stdin_eof);
    CHECK(send_count==2U && sends[0].generation==1U && sends[1].generation==1U);
    cog_session_snapshot_t snapshot;CHECK(cog_session_snapshot_view(app->session,&snapshot)==COG_OK);
    CHECK(snapshot.ledger_count==1U && snapshot.ledger[0].state==COG_TOOL_EXECUTION_DENIED);
    char path[512];CHECK(snprintf(path,sizeof(path),"%s/effect",root)>0 && access(path,F_OK)!=0);
    CHECK(app->configuration_generation==1U && app->future_provider!=NULL);
    cog_cli_app_free(app);char cleanup[600];CHECK(snprintf(cleanup,sizeof(cleanup),"rm -rf -- '%s'",root)>0);CHECK(system(cleanup)==0);
    return 0;
}
static int restored_proposal(void)
{
    reset_controls();char root[]="/tmp/cog-r12-restored-XXXXXX";CHECK(mkdtemp(root)!=NULL);
    cog_cli_options_t opts=options();opts.store_dir=root;opts.workspace_root=root;opts.session_id="pending";
    opts.store_mode=COG_CLI_STORE_DURABLE;opts.tool_policy=COG_CLI_TOOL_POLICY_ALLOW;
    opts.explicit_options|=COG_CLI_OPT_STORE_DIR|COG_CLI_OPT_WORKSPACE|COG_CLI_OPT_STORE_MODE|COG_CLI_OPT_POLICY;
    cog_cli_app_t *app=NULL;CHECK(cog_cli_app_new(&app,&opts)==COG_OK);
    const cog_message_t user={.role=COG_MESSAGE_ROLE_USER,.content="prior",.content_len=5U};
    CHECK(cog_session_append(app->session,&user,NULL)==COG_OK);
    static const char args[]="{\"command\":\"printf once >> effect\"}";
    const cog_message_tool_call_t call={"restored","run_command",args,sizeof(args)-1U};
    const cog_message_t assistant={.role=COG_MESSAGE_ROLE_ASSISTANT,.tool_calls=&call,.tool_call_count=1U};
    CHECK(cog_session_append(app->session,&assistant,NULL)==COG_OK);
    CHECK(cog_store_save(app->store,app->session)==COG_OK);cog_cli_app_free(app);
    opts.session_id=NULL;opts.session_action=COG_CLI_SESSION_RESUME;opts.source_session_id="pending";
    CHECK(cog_cli_app_new(&app,&opts)==COG_OK && app->resuming_policy && app->active);
    CHECK(pending_count==1U && pending_generations[0]==1U);
    uint64_t generation=0U;
    CHECK(cog_cli_app_reload(app,&(cog_cli_reload_options_t){.model_id="restored/new"},&generation)==COG_OK);
    CHECK(pending_count==2U && pending_generations[1]==2U && app->configuration_generation==1U);
    CHECK(cog_cli_app_run(app)==COG_OK && send_count==1U && sends[0].generation==1U);
    CHECK(app->configuration_generation==2U && cog_cli_app_submit(app,"fresh",5U)==COG_OK);
    CHECK(cog_cli_app_run(app)==COG_OK && send_count==2U && sends[1].generation==2U);
    char path[512];CHECK(snprintf(path,sizeof(path),"%s/effect",root)>0);FILE *file=fopen(path,"r");CHECK(file!=NULL);
    char bytes[16]={0};CHECK(fread(bytes,1,sizeof(bytes),file)==4U && memcmp(bytes,"once",4U)==0);CHECK(fclose(file)==0);
    cog_session_snapshot_t snapshot;CHECK(cog_session_snapshot_view(app->session,&snapshot)==COG_OK);
    CHECK(snapshot.ledger_count==1U && snapshot.ledger[0].state==COG_TOOL_EXECUTION_SUCCEEDED);
    cog_cli_app_free(app);char cleanup[600];CHECK(snprintf(cleanup,sizeof(cleanup),"rm -rf -- '%s'",root)>0);CHECK(system(cleanup)==0);
    return 0;
}
static cog_result_t partial_control(cog_cli_app_t *app)
{
    if(controlled || app->text_len==0U)return COG_OK;
    controlled=true;uint64_t generation=0U;
    assert(cog_cli_app_reload(app,&(cog_cli_reload_options_t){.model_id="partial/new"},&generation)==COG_OK);
    assert(cog_cli_app_queue(app,"prequeued",9U)==COG_OK);return COG_OK;
}
static int partial_authority(void)
{
    reset_controls();static const cog_mock_step_t steps[]={
        {COG_MOCK_STEP_CHUNK,"partial",COG_OK,0,false,0U,NULL},
        {COG_MOCK_STEP_ERROR,NULL,COG_ERROR_UNAVAILABLE,500,false,0U,"fixture"}};
    const cog_mock_script_t script={steps,2U,0U,COG_OK,0U};
    cog_cli_options_t opts=options();opts.prompt="old";opts.script=&script;
    cog_cli_app_t *app=NULL;CHECK(cog_cli_app_new(&app,&opts)==COG_OK);
    owner_control=partial_control;CHECK(cog_cli_app_run(app)==COG_ERROR_UNAVAILABLE && controlled);owner_control=NULL;
    CHECK(send_count==1U && app->continuation_required);
    CHECK(cog_cli_app_run(app)==COG_OK && send_count==1U); /* Publication grants no authorization. */
    CHECK(cog_cli_app_submit(app,"fresh",5U)==COG_OK);
    CHECK(cog_cli_app_run(app)==COG_ERROR_UNAVAILABLE && send_count==2U && sends[1].generation==2U);
    const cog_message_t *old=cog_node_data(cog_cli_session_node(app,2U))->message;
    CHECK(old->status==COG_MESSAGE_INCOMPLETE && strcmp(old->content,"partial")==0);
    cog_cli_app_free(app);return 0;
}
static int future_capacity(void)
{
    reset_controls();cog_cli_options_t opts=options();opts.prompt="old";
    opts.chunks="01234567890123456789012345678901234567890123456789012345678";
    opts.max_context_bytes=64U;opts.explicit_options|=COG_CLI_OPT_CONTEXT;
    cog_cli_app_t *app=NULL;CHECK(cog_cli_app_new(&app,&opts)==COG_OK);
    owner_control=retained_control;CHECK(cog_cli_app_run(app)==COG_ERROR_LIMIT && controlled);owner_control=NULL;
    CHECK(send_count==1U && app->configuration_generation==2U);
    cog_session_snapshot_t snapshot;CHECK(cog_session_snapshot_view(app->session,&snapshot)==COG_OK);
    CHECK(snapshot.node_count==3U && snapshot.nodes[1].message->status==COG_MESSAGE_COMPLETE);
    CHECK(snapshot.nodes[1].message->content_len==strlen(opts.chunks));
    CHECK(memcmp(snapshot.nodes[1].message->content,opts.chunks,strlen(opts.chunks))==0);
    cog_cli_app_free(app);return 0;
}
#ifdef COG_RELOAD_REAL_PROFILE
typedef struct secret_context {cog_cli_app_t *app;const char *secret;unsigned calls;} secret_context_t;
static cog_result_t secret(void *arg,const char *reference,char *output,size_t capacity,size_t *required)
{
    secret_context_t *context=arg;(void)reference;context->calls++;
    if(context->app!=NULL){
        uint64_t generation=998U;
        assert(cog_cli_app_reload(context->app,&(cog_cli_reload_options_t){.model_id="recursive"},&generation)==COG_ERROR_INVALID_STATE);
        assert(generation==998U && cog_cli_app_run(context->app)==COG_ERROR_INVALID_STATE);
        assert(cog_cli_app_submit(context->app,"recursive",9U)==COG_ERROR_INVALID_STATE);
    }
    *required=strlen(context->secret)+1U;if(*required>capacity)return COG_ERROR_OVERFLOW;
    memcpy(output,context->secret,*required);return COG_OK;
}
static int real_compatibility(void)
{
    CHECK(curl_global_init(CURL_GLOBAL_DEFAULT)==CURLE_OK);cog_net_ctx_t *net=NULL;
    const cog_net_config_t net_options={.host_runtime_initialized=true,.dependency_memory=COG_NET_DEPENDENCY_MEMORY_DYNAMIC};
    CHECK(cog_net_new(&net,&net_options)==COG_OK);
    for(unsigned mode=0U;mode<5U;mode++){
        reset_controls();secret_context_t resolver={NULL,"SYNTHETIC_OLD",0U};
        cog_cli_options_t opts={.backend=COG_CLI_BACKEND_REAL,.model_id="synthetic/old",.base_url="https://127.0.0.1:1/v1",
            .credential_ref="host:fixture",.credential_read=secret,.credential_arg=&resolver,.net=net,
            .store_mode=COG_CLI_STORE_DISABLED,.explicit_options=COG_CLI_OPT_BACKEND|COG_CLI_OPT_MODEL|COG_CLI_OPT_BASE_URL|COG_CLI_OPT_CREDENTIAL|COG_CLI_OPT_STORE_MODE};
        cog_cli_app_t *app=NULL;CHECK(cog_cli_app_new(&app,&opts)==COG_OK && resolver.calls==1U);
        cog_provider_t *old=app->provider;const cog_endpoint_t *old_endpoint=cog_cli_configuration_endpoint(app->configuration);
        const cog_message_t user={.role=COG_MESSAGE_ROLE_USER,.content="fixture",.content_len=7U};
        CHECK(cog_session_append(app->session,&user,NULL)==COG_OK);
        const char *payload=mode==2U?"{\"reasoning_details\":[{\"type\":\"reasoning.encrypted\",\"data\":42}]}":"{\"reasoning_details\":[{\"type\":\"reasoning.encrypted\",\"data\":\"signed bytes\",\"unknown\":{\"keep\":true}}]}";
        const cog_message_opaque_t opaque={(mode==1U || mode==3U)?"foreign.required":"openrouter.chat-completions","json-v1",(const unsigned char *)payload,strlen(payload)};
        const cog_message_t assistant={.role=COG_MESSAGE_ROLE_ASSISTANT,.content="prior",.content_len=5U,.opaque=&opaque,.opaque_count=1U,
            .status=mode==4U?COG_MESSAGE_INCOMPLETE:COG_MESSAGE_COMPLETE};
        CHECK(cog_session_append(app->session,&assistant,NULL)==COG_OK);
        if(mode==3U)CHECK(cog_cli_app_fork(app,1U)==COG_OK && app->fork_parent!=NULL);
        resolver.app=app;resolver.secret="SYNTHETIC_NEW";
        uint64_t generation=765U;
        const cog_cli_reload_options_t replacement={.model_id="synthetic/new",.credential_ref="host:fixture",.credential_read=secret,.credential_arg=&resolver};
        const cog_result_t expected[]={COG_OK,COG_ERROR_UNSUPPORTED,COG_ERROR_PROTOCOL,COG_OK,COG_ERROR_INVALID_STATE};
        cog_result_t result=cog_cli_app_reload(app,&replacement,&generation);
        if(result!=expected[mode])fprintf(stderr,"real preflight mode=%u actual=%s expected=%s\n",mode,cog_cli_result_name(result),cog_cli_result_name(expected[mode]));
        CHECK(result==expected[mode]);
        CHECK(resolver.calls==2U && app->provider==old && app->configuration_generation==1U && app->reload_attempts==1U);
        CHECK(strcmp(old_endpoint->api_key,"SYNTHETIC_OLD")==0 && cog_net_active_count(net)==0U && send_count==0U);
        if(expected[mode]==COG_OK){
            CHECK(generation==2U && app->future_provider!=NULL);
            CHECK(cog_cli_reload_activate(app)==COG_OK && cog_core_provider(app->core)==app->provider);
            CHECK(strcmp(cog_cli_configuration_endpoint(app->configuration)->api_key,"SYNTHETIC_NEW")==0);
            CHECK(cog_cli_configuration_options(app->configuration)->credential_read==NULL);
        }else CHECK(generation==765U && app->future_provider==NULL && !app->reload_notice_pending);
        cog_cli_app_free(app);
    }
    cog_net_free(net);curl_global_cleanup();return 0;
}
#endif
#endif
int main(void)
{
    CHECK(basic()==0);CHECK(bounds()==0);CHECK(queue_before_notice()==0);
#ifdef COG_RELOAD_WRAPPERS
    CHECK(retained_turn()==0);CHECK(backoff_turn()==0);CHECK(transaction_failures()==0);CHECK(consuming_retirement()==0);
    CHECK(eof_boundaries()==0);CHECK(tool_turn()==0);CHECK(policy_turn()==0);CHECK(restored_proposal()==0);
    CHECK(partial_authority()==0);CHECK(future_capacity()==0);
#ifdef COG_RELOAD_REAL_PROFILE
    CHECK(real_compatibility()==0);
#endif
#endif
    return 0;
}

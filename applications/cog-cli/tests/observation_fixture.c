/** @file observation_fixture.c @brief Actual TLS/provider/tool observation oracle. */
#define _POSIX_C_SOURCE 200809L
#include "observation_fixture.h"
#include "app_internal.h"
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x);return 1;}}while(0)
typedef struct copied_row {
    uint64_t id, timestamp, opened, in, out;
    unsigned connected, terminal, retries, stalled;
    cog_result_t status;uint16_t http;
    char model[128], session[128];
} copied_row_t;
typedef struct observation_capture {
    cog_obs_sink_t sink;cog_obs_sink_table_t table;
    copied_row_t rows[8];size_t count, records, spans, failed_spans;
    unsigned providers, tools, known_usage, unknown_usage, tls, connect;
    bool in_callback, bad, cancel, cancelled, new_generation;
} observation_capture_t;
static observation_capture_t *capture;
cog_result_t __real_cog_obs_new(cog_obs_t **out,const cog_obs_config_t *config);
cog_result_t __wrap_cog_obs_new(cog_obs_t **out,const cog_obs_config_t *config);
cog_result_t __real_cog_cli_service(void *arg,uint64_t now,uint64_t *deadline);
cog_result_t __wrap_cog_cli_service(void *arg,uint64_t now,uint64_t *deadline);
static bool copy_label(char *output,size_t capacity,const char *input)
{
    if(input==NULL){output[0]='\0';return true;}
    size_t length=strlen(input);if(length>=capacity)return false;
    memcpy(output,input,length+1U);return true;
}
static cog_result_t record_sink(void *arg,const char *record,size_t length)
{
    observation_capture_t *state=arg;
    if(state->in_callback || ++state->records>10000U || length>=1024U)return COG_ERROR_LIMIT;
    static const char *const forbidden[]={"synthetic","localhost","example.txt","APP_WIRE_OK","APP_RELOAD_OK",
        "partial-cancel","Bearer","env:OPENROUTER_API_KEY","reasoning_details"};
    for(size_t i=0U;i<sizeof(forbidden)/sizeof(forbidden[0]);i++)if(strstr(record,forbidden[i])!=NULL){state->bad=true;return COG_ERROR_PROTOCOL;}
    if(strstr(record,"provider.completed")!=NULL){state->providers++;if(strstr(record,"duration_ns=")==NULL)state->bad=true;}
    if(strstr(record,"tool.completed")!=NULL){
        state->tools++;
        if(strstr(record,"tool=\"edit_file\"")==NULL || strstr(record,"execution_started=true")==NULL ||
            strstr(record,"duration_ns=")==NULL || strstr(record,"call_ordinal=1")==NULL)state->bad=true;
    }
    if(strstr(record,"provider.usage")!=NULL){
        if(strstr(record,"input_tokens_available=true")!=NULL){
            state->known_usage++;if(strstr(record," input_tokens=2")==NULL || strstr(record," output_tokens=3")==NULL)state->bad=true;
        }else{state->unknown_usage++;if(strstr(record," input_tokens=")!=NULL || strstr(record," output_tokens=")!=NULL)state->bad=true;}
        if(strstr(record,"cost_available=false")==NULL || strstr(record," cost=")!=NULL)state->bad=true;
    }
    if(strstr(record,"configuration_generation=2")!=NULL)state->new_generation=true;
    if(strstr(record,"transport_class=\"tls\"")!=NULL)state->tls++;
    if(strstr(record,"transport_class=\"connect\"")!=NULL)state->connect++;
    return state->bad?COG_ERROR_PROTOCOL:COG_OK;
}
static cog_result_t event_sink(void *arg,const cog_obs_conn_event_t *event)
{
    observation_capture_t *state=arg;
    if(state->in_callback || event->conn_id==0U)return COG_ERROR_INVALID_STATE;
    copied_row_t *row=NULL;
    for(size_t i=0U;i<state->count;i++)if(state->rows[i].id==event->conn_id)row=&state->rows[i];
    if(event->kind==COG_OBS_CONN_EVENT_OPEN){
        if(row!=NULL || state->count==8U)return COG_ERROR_OVERFLOW;
        row=&state->rows[state->count++];row->id=event->conn_id;row->opened=event->timestamp_ns;
        if(!copy_label(row->session,sizeof(row->session),event->session_id) || !copy_label(row->model,sizeof(row->model),event->model))return COG_ERROR_LIMIT;
    }
    if(row==NULL || row->terminal!=0U || event->timestamp_ns<row->timestamp)return COG_ERROR_PROTOCOL;
    if(event->endpoint==NULL || strcmp(event->endpoint,"openrouter")!=0 || event->provider==NULL || strcmp(event->provider,"openrouter")!=0)return COG_ERROR_PROTOCOL;
    if(event->kind==COG_OBS_CONN_EVENT_CONNECTED){if(row->connected++!=0U)return COG_ERROR_PROTOCOL;}
    if(event->kind==COG_OBS_CONN_EVENT_RETRY){
        if(event->timestamp_ns!=row->opened || row->connected!=0U || event->retry_attempt==0U)return COG_ERROR_PROTOCOL;
        row->retries++;
    }
    if(event->kind==COG_OBS_CONN_EVENT_PROGRESS){
        if(event->bytes_in_total<row->in || event->bytes_out_total<row->out ||
            event->bytes_in_delta!=event->bytes_in_total-row->in || event->bytes_out_delta!=event->bytes_out_total-row->out)return COG_ERROR_PROTOCOL;
        row->in=event->bytes_in_total;row->out=event->bytes_out_total;
    }else if(event->bytes_in_delta!=0U || event->bytes_out_delta!=0U)return COG_ERROR_PROTOCOL;
    if(event->kind==COG_OBS_CONN_EVENT_STALLED)row->stalled++;
    if(event->kind==COG_OBS_CONN_EVENT_CLOSED || event->kind==COG_OBS_CONN_EVENT_FAILED){
        if(row->in!=event->bytes_in_total || row->out!=event->bytes_out_total)return COG_ERROR_PROTOCOL;
        row->terminal++;row->status=event->status;row->http=event->http_status;
    }
    row->timestamp=event->timestamp_ns;return COG_OK;
}
static cog_result_t span_sink(void *arg,const cog_obs_span_record_t *span)
{
    observation_capture_t *state=arg;
    if(state->in_callback || ++state->spans>16U || span->duration_ns==0U ||
        (strcmp(span->name,"provider.attempt")!=0 && strcmp(span->name,"tool.attempt")!=0))return COG_ERROR_PROTOCOL;
    if(span->status!=COG_OK)state->failed_spans++;
    return COG_OK;
}
cog_result_t __wrap_cog_obs_new(cog_obs_t **out,const cog_obs_config_t *config)
{
    if(capture==NULL)return __real_cog_obs_new(out,config);
    cog_obs_config_t replacement=*config;
    replacement.sinks=&capture->table;replacement.trace_sink=span_sink;replacement.trace_user=capture;
    replacement.conn_event_sink=event_sink;replacement.conn_event_user=capture;
    return __real_cog_obs_new(out,&replacement);
}
cog_result_t __wrap_cog_cli_service(void *arg,uint64_t now,uint64_t *deadline)
{
    cog_cli_app_t *app=arg;
    if(capture!=NULL && capture->cancel && !capture->cancelled && app->text_len!=0U){
        capture->cancelled=true;cog_result_t result=cog_cli_app_cancel(app);if(result!=COG_OK)return result;
    }
    return __real_cog_cli_service(arg,now,deadline);
}
static cog_result_t feed(void *user,const cog_net_ctx_t *net,const cog_net_observation_t *value)
{
    capture->in_callback=true;cog_result_t result=cog_cli_observer_capture(user,net,value);capture->in_callback=false;return result;
}
int cog_test_observation_runtime(void)
{
    observation_capture_t state={0};state.sink=(cog_obs_sink_t){.name="copied",.deliver=record_sink,.user=&state,.min_severity=COG_OBS_TRACE};
    state.table=(cog_obs_sink_table_t){&state.sink,1U};capture=&state;
    const char *mode=getenv("COG_OBSERVATION_MODE");if(mode==NULL)mode="normal";
    bool normal=strcmp(mode,"normal")==0 || strcmp(mode,"retry")==0;
    bool retry=strcmp(mode,"retry")==0;state.cancel=strcmp(mode,"cancel")==0;
    CHECK(curl_global_init(CURL_GLOBAL_DEFAULT)==CURLE_OK);
    cog_cli_observer_t *observer=NULL;CHECK(cog_cli_observer_new(&observer,4U)==COG_OK);
    cog_net_ctx_t *net=NULL;cog_net_config_t cfg={.host_runtime_initialized=true,.dependency_memory=COG_NET_DEPENDENCY_MEMORY_DYNAMIC,
        .ca_bundle=strcmp(mode,"tls")==0?NULL:getenv("COG_NET_TEST_CA"),.max_connections=4U,
        .stall_timeout_ms=strcmp(mode,"stall")==0?100U:30000U,.observer=feed,.observer_user=observer};
    CHECK(cog_net_new(&net,&cfg)==COG_OK);
    char session[128];memset(session,'s',127U);session[127]='\0';
    cog_cli_options_t opts={.prompt="synthetic fixture edit",.backend=COG_CLI_BACKEND_REAL,.model_id="synthetic/model",
        .session_id=session,.base_url=getenv("COG_APP_TEST_URL"),.net=net,.verbose=true,
        .store_mode=COG_CLI_STORE_DURABLE,.store_dir=getenv("COG_APP_TEST_STORE"),.workspace_root=getenv("COG_APP_TEST_ROOT"),
        .tool_policy=COG_CLI_TOOL_POLICY_ALLOW,.max_output_tokens=512U,.max_retries=retry?1U:0U,
        .explicit_options=COG_CLI_OPT_BACKEND|COG_CLI_OPT_MODEL|COG_CLI_OPT_BASE_URL|COG_CLI_OPT_STORE_MODE|
            COG_CLI_OPT_STORE_DIR|COG_CLI_OPT_WORKSPACE|COG_CLI_OPT_POLICY|COG_CLI_OPT_OUTPUT_TOKENS|COG_CLI_OPT_RETRIES};
    cog_cli_app_t *app=NULL;CHECK(cog_cli_app_new(&app,&opts)==COG_OK);
    CHECK(cog_cli_observer_bind(observer,app->obs)==COG_OK);app->observer=observer;
    cog_result_t result=cog_cli_app_run(app);
    if(normal){
        CHECK(result==COG_OK);uint64_t generation=0U;char model[128];memset(model,'b',127U);model[127]='\0';
        CHECK(cog_cli_app_reload(app,&(cog_cli_reload_options_t){.model_id=model},&generation)==COG_OK && generation==2U);
        memset(model,'x',127U);CHECK(cog_cli_app_submit(app,"fresh after reload",18U)==COG_OK);
        CHECK(cog_cli_app_run(app)==COG_OK && state.new_generation);
        CHECK(state.count==(retry?4U:3U) && state.providers==state.count && state.tools==1U);
        CHECK(state.known_usage==2U && state.unknown_usage==(retry?2U:1U) && state.spans==state.providers+1U);
        CHECK(state.failed_spans==(retry?1U:0U));
        for(size_t i=0U;i<state.count;i++){
            CHECK(state.rows[i].terminal==1U && state.rows[i].connected==1U && state.rows[i].in>0U && state.rows[i].out>0U);
            CHECK(state.rows[i].status==COG_OK && state.rows[i].http==(retry && i==0U?429U:200U));
            CHECK(strlen(state.rows[i].session)==127U && state.rows[i].session[0]=='s');
            CHECK(i+1U==state.count?strlen(state.rows[i].model)==127U && state.rows[i].model[0]=='b':state.rows[i].model[0]=='\0');
            CHECK(state.rows[i].retries==(retry && i==1U?1U:0U));
        }
        cog_session_snapshot_t snapshot;CHECK(cog_session_snapshot_view(app->session,&snapshot)==COG_OK);
        CHECK(snapshot.ledger_count==1U && snapshot.ledger[0].state==COG_TOOL_EXECUTION_SUCCEEDED);
        const cog_message_t *last=cog_node_data(cog_session_active(app->session))->message;
        CHECK(strcmp(last->content,"APP_RELOAD_OK")==0 && !last->usage.tokens_known);
    }else{
        CHECK(state.count==1U && state.rows[0].terminal==1U && state.tools==0U);
        if(strcmp(mode,"stall")==0)CHECK(result==COG_ERROR_TIMEOUT && state.rows[0].stalled==1U && state.rows[0].status==COG_ERROR_TIMEOUT);
        else if(strcmp(mode,"tls")==0)CHECK(result!=COG_OK && state.rows[0].connected==0U && state.tls==1U);
        else if(strcmp(mode,"connect")==0)CHECK(result!=COG_OK && state.rows[0].connected==0U && state.connect==1U);
        else CHECK(state.cancelled && result==COG_OK && state.rows[0].status==COG_ERROR_ABORTED);
        CHECK(state.providers==1U && state.failed_spans==1U && state.unknown_usage==1U);
    }
    size_t depth=99U;CHECK(cog_obs_trace_depth(app->obs,&depth)==COG_OK && depth==0U);
    CHECK(!state.bad && cog_net_observer_status(net)==COG_OK);
    cog_cli_app_free(app);CHECK(cog_cli_observer_status(observer)==COG_OK);
    cog_net_free(net);cog_cli_observer_free(observer);curl_global_cleanup();capture=NULL;return 0;
}

/** @file test_client_wire_acceptance.c @brief Real app cancellation/fork joins. */
#define _POSIX_C_SOURCE 200809L
#include "app_internal.h"
#include <cog/openrouter.h>
#include <curl/curl.h>
#include <assert.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static const char *mode;
static cog_cli_app_t *current;
static unsigned sends,cancelled,finished,backoffs,service_calls;
static uint64_t start,virtual_now;
static bool expired;
static unsigned traffic;
static unsigned full_reservation;
static cog_endpoint_t retained_endpoint;
static cog_openrouter_config_t retained_configuration;
cog_result_t __real_cog_provider_connect(const cog_endpoint_t *endpoint,cog_provider_t **out);
cog_result_t __wrap_cog_provider_connect(const cog_endpoint_t *endpoint,cog_provider_t **out);
cog_result_t __wrap_cog_provider_connect(const cog_endpoint_t *endpoint,cog_provider_t **out)
{
    if(strcmp(mode,"cancel-retained")!=0)return __real_cog_provider_connect(endpoint,out);
    /* Test-only constructor reservation policy; copied endpoint/configuration
     * retain the original generation's string borrows through app teardown. */
    retained_endpoint=*endpoint;retained_configuration=*(const cog_openrouter_config_t *)endpoint->implementation_config;
    retained_configuration.limits.retained_delta_count=1U;
    retained_endpoint.implementation_config=&retained_configuration;
    return __real_cog_provider_connect(&retained_endpoint,out);
}
cog_result_t __real_cog_provider_poll(cog_provider_t *provider,uint64_t now,uint64_t *deadline);
cog_result_t __wrap_cog_provider_poll(cog_provider_t *provider,uint64_t now,uint64_t *deadline);
cog_result_t __wrap_cog_provider_poll(cog_provider_t *provider,uint64_t now,uint64_t *deadline)
{
    cog_result_t rc=__real_cog_provider_poll(provider,now,deadline);
    if(rc==COG_OK && strcmp(mode,"cancel-retained")==0 && current!=NULL && current->provider_active && cancelled==0U){
        cog_provider_delta_t delta={0};size_t needed=0U;
        cog_result_t peek=cog_provider_next(provider,&delta,NULL,0U,&needed);
        if(peek==COG_ERROR_OVERFLOW){
            /* OVERFLOW preserves this ordinary delta. The one-entry ordinary
             * reservation is provably occupied, independently of app copies. */
            assert(needed>0U && cog_net_active_count(current->opts.net)==1U);
            full_reservation++;cancelled++;return cog_cli_app_cancel(current);
        }
        assert(peek==COG_ERROR_AGAIN);
    }
    return rc;
}
static int entries(const char *path)
{
    DIR *dir=opendir(path);assert(dir!=NULL);int n=0;struct dirent *entry;
    while((entry=readdir(dir))!=NULL)if(entry->d_name[0]!='.')n++;
    assert(closedir(dir)==0);return n;
}
/* Test-only DNS routing, on the genuine net-owned easy constructor. All DNS
 * resolution, socket work, cancellation and destruction remain in admitted
 * curl/c-ares. No preload, replacement completion or resolver thread. */
CURL *__real_curl_easy_init(void);
CURL *__wrap_curl_easy_init(void);
CURL *__wrap_curl_easy_init(void)
{
    CURL *easy=__real_curl_easy_init();const char *dns=getenv("COG_ACCEPT_DNS");
    if(easy!=NULL && dns!=NULL)assert(curl_easy_setopt(easy,CURLOPT_DNS_SERVERS,dns)==CURLE_OK);
    return easy;
}
cog_result_t __real_cog_provider_send(cog_provider_t *provider,const cog_provider_request_t *request);
cog_result_t __wrap_cog_provider_send(cog_provider_t *provider,const cog_provider_request_t *request);
cog_result_t __wrap_cog_provider_send(cog_provider_t *provider,const cog_provider_request_t *request)
{
    cog_result_t rc=__real_cog_provider_send(provider,request);
    if(rc==COG_OK)sends++;
    if(strcmp(mode,"budget")==0 && sends==3U){
        assert(current!=NULL && current->retries==1U && current->effects);
        assert(current->started_at==start && request->message_count==3U);
        size_t queued=0U;
        for(size_t i=0U;i<CLI_INPUTS;i++)if(current->inputs[i].used && current->inputs[i].queued)queued++;
        assert(queued==7U && traffic>=4U);
        /* Remaining whole-turn time includes the original retry and tool. */
        virtual_now=start+(uint64_t)current->opts.max_turn_ms-1U;
    }
    return rc;
}
cog_result_t __real_cog_cli_service(void *arg,uint64_t now,uint64_t *deadline);
cog_result_t __wrap_cog_cli_service(void *arg,uint64_t now,uint64_t *deadline);
cog_result_t __wrap_cog_cli_service(void *arg,uint64_t now,uint64_t *deadline)
{
    cog_cli_app_t *app=arg;service_calls++;
    bool budget=strcmp(mode,"budget")==0;
    if(budget){
        if(start==0U)start=virtual_now=now;
        if(cog_core_state(app->core)==CLI_BACKOFF && backoffs++!=0U)virtual_now=app->retry_at;
        now=virtual_now;
        if(app->active){
            /* Repeated valid USER_INPUT events refer to one live queued slot.
             * They exercise the real active-turn handler and fair service,
             * keeping ordinary queue work present throughout retry/tool I/O. */
            for(size_t i=0U;i<CLI_INPUTS;i++)if(app->inputs[i].used){
                cog_event_t event={.id=COG_EVENT_USER_INPUT,.kind=COG_EVENT_USER_INPUT,
                    .payload=app->inputs[i].text,.payload_len=app->inputs[i].length};
                for(unsigned j=0U;j<4U;j++){
                    cog_result_t queued=cog_core_enqueue(app->core,&event);
                    assert(queued==COG_OK || queued==COG_ERROR_OVERFLOW);
                    if(queued==COG_OK)traffic++;
                }
                break;
            }
        }
    }
    cog_result_t rc=__real_cog_cli_service(arg,now,deadline);
    if(rc!=COG_OK)return rc;
    if(budget && sends==3U && !expired){
        assert(app->started_at==start && *deadline==start+(uint64_t)app->opts.max_turn_ms);
        assert(app->now==*deadline-1U);expired=true;virtual_now=*deadline;
        return __real_cog_cli_service(arg,virtual_now,deadline);
    }
    if(strncmp(mode,"cancel-",7U)!=0 || cancelled!=0U || strcmp(mode,"cancel-retained")==0)return COG_OK;
    bool ready=false;
    if(strcmp(mode,"cancel-stream")==0)ready=app->text_len!=0U;
    else if(strcmp(mode,"cancel-finish")==0)ready=app->delta_ready && app->delta.kind==COG_PROVIDER_DELTA_FINISH && app->delta.outcome==COG_OK;
    else {const char *path=getenv("COG_ACCEPT_READY");ready=path!=NULL && access(path,F_OK)==0;}
    if(ready){assert(app->provider_active);
        assert(cog_net_active_count(app->opts.net)==(strcmp(mode,"cancel-finish")==0?0U:1U));
        if(strcmp(mode,"cancel-finish")==0)finished++;
        cancelled++;return cog_cli_app_cancel(app);}
    return COG_OK;
}
static void marker(const char *expected)
{
    char path[1024],bytes[64]={0};assert(snprintf(path,sizeof(path),"%s/effect",getenv("COG_APP_TEST_ROOT"))>0);
    FILE *file=fopen(path,"r");assert(file!=NULL);size_t n=fread(bytes,1,sizeof(bytes),file);
    assert(fclose(file)==0);assert(n==strlen(expected) && memcmp(bytes,expected,n)==0);
}
static void closed_guard(cog_cli_options_t opts)
{
    /* A new owner epoch can acquire the same store after teardown, with no
     * network work. This detects retained app ownership as well as net slots. */
    opts.prompt=NULL;opts.interactive=false;opts.session_id=NULL;
    opts.fork_node_id=0U;
    opts.session_action=COG_CLI_SESSION_RESUME;opts.source_session_id="wire-acceptance";
    cog_cli_app_t *app=NULL;assert(cog_cli_app_new(&app,&opts)==COG_OK);cog_cli_app_free(app);
}
int main(void)
{
    mode=getenv("COG_ACCEPT_MODE");assert(mode!=NULL);
    int fds=entries("/proc/self/fd"),threads=entries("/proc/self/task");assert(threads==1);
    assert(curl_global_init(CURL_GLOBAL_DEFAULT)==CURLE_OK);
    cog_net_ctx_t *net=NULL;cog_net_config_t cfg={.host_runtime_initialized=true,
        .dependency_memory=COG_NET_DEPENDENCY_MEMORY_DYNAMIC,.ca_bundle=getenv("COG_NET_TEST_CA"),
        .chunks_per_request=1U,.chunk_bytes=16384U};
    assert(cog_net_new(&net,&cfg)==COG_OK);
    cog_cli_options_t opts={.backend=COG_CLI_BACKEND_REAL,.model_id="synthetic/model",
        .prompt="ancestor",.session_id="wire-acceptance",.net=net,.base_url=getenv("COG_APP_TEST_URL"),
        .store_mode=COG_CLI_STORE_DURABLE,.store_dir=getenv("COG_APP_TEST_STORE"),
        .workspace_root=getenv("COG_APP_TEST_ROOT"),.tool_policy=COG_CLI_TOOL_POLICY_ALLOW,
        .max_output_tokens=512U,.max_retries=3U,.max_turn_ms=1000U,
        .explicit_options=COG_CLI_OPT_BACKEND|COG_CLI_OPT_MODEL|COG_CLI_OPT_BASE_URL|
        COG_CLI_OPT_STORE_MODE|COG_CLI_OPT_STORE_DIR|COG_CLI_OPT_WORKSPACE|COG_CLI_OPT_POLICY|
        COG_CLI_OPT_OUTPUT_TOKENS|COG_CLI_OPT_RETRIES|COG_CLI_OPT_TURN_MS};
    if(strcmp(mode,"budget")!=0)opts.max_turn_ms=10000U;
    cog_cli_app_t *app=NULL;assert(cog_cli_app_new(&app,&opts)==COG_OK);current=app;
    if(strcmp(mode,"budget")==0)for(unsigned i=0U;i<7U;i++)assert(cog_cli_app_queue(app,"queued during whole turn",24U)==COG_OK);
    cog_result_t rc=cog_cli_app_run(app);current=NULL;
    assert(!app->provider_active && app->execution==NULL && cog_net_active_count(net)==0U);
    assert(entries("/proc/self/task")==threads);
    if(strcmp(mode,"fork")==0){
        assert(rc==COG_OK && sends==2U);marker("once");
        cog_session_snapshot_t snapshot;assert(cog_session_snapshot_view(app->session,&snapshot)==COG_OK);
        assert(snapshot.ledger_count==1U && snapshot.ledger[0].state==COG_TOOL_EXECUTION_SUCCEEDED);
        uint64_t fork_id=0U,old_leaf=snapshot.active_node_id,old_assistant=snapshot.ledger[0].assistant_node_id;
        for(size_t i=0U;i<snapshot.node_count;i++)if(snapshot.nodes[i].message!=NULL && snapshot.nodes[i].message->role==COG_MESSAGE_ROLE_TOOL)
            fork_id=snapshot.nodes[i].node_id;
        assert(fork_id!=0U);cog_cli_app_free(app);
        opts.session_id=NULL;opts.session_action=COG_CLI_SESSION_FORK;
        opts.source_session_id="wire-acceptance";opts.fork_node_id=fork_id;opts.prompt="branch";
        assert(cog_cli_app_new(&app,&opts)==COG_OK);marker("once");
        assert(cog_cli_app_run(app)==COG_OK && sends==4U);marker("oncebranch");
        assert(cog_session_snapshot_view(app->session,&snapshot)==COG_OK && snapshot.ledger_count==2U);
        assert(snapshot.ledger[0].assistant_node_id==old_assistant);
        assert(snapshot.ledger[1].assistant_node_id!=old_assistant);
        assert(strcmp(snapshot.ledger[0].call_id,snapshot.ledger[1].call_id)==0);
        bool retained=false;for(size_t i=0U;i<snapshot.node_count;i++)if(snapshot.nodes[i].node_id==old_leaf){
            retained=true;assert(strcmp(snapshot.nodes[i].message->content,"OLD_BRANCH")==0);}
        assert(retained && cog_net_active_count(net)==0U);
    }else if(strcmp(mode,"budget")==0){
        assert(rc==COG_ERROR_TIMEOUT && sends==3U && expired && backoffs>=2U && service_calls>8U);
        assert(app->started_at==start && app->retries==1U);marker("once");
        cog_session_snapshot_t snapshot;assert(cog_session_snapshot_view(app->session,&snapshot)==COG_OK);
        assert(snapshot.ledger_count==1U && snapshot.ledger[0].state==COG_TOOL_EXECUTION_SUCCEEDED);
    }else{
        assert(rc==COG_OK && sends==1U && cancelled==1U);
        cog_cli_display_snapshot_t display;assert(cog_cli_app_display_snapshot(app,&display)==COG_OK);
        assert(display.item_count==2U);
        bool success=strcmp(mode,"cancel-finish")==0;
        assert(display.items[1].status==(success?COG_MESSAGE_COMPLETE:COG_MESSAGE_CANCELLED));
        if(success)assert(finished==1U && display.items[1].text_len==8U);
        if(strcmp(mode,"cancel-stream")==0)assert(display.items[1].text_len==7U);
        if(strcmp(mode,"cancel-retained")==0)assert(full_reservation==1U && display.items[1].text_len==8U);
    }
    cog_cli_app_free(app);closed_guard(opts);cog_net_free(net);curl_global_cleanup();
    assert(entries("/proc/self/task")==threads && entries("/proc/self/fd")==fds);return 0;
}

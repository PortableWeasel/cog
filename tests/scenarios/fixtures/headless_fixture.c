/** @file headless_fixture.c @brief Actual bounded host fixture operations. */
#define _POSIX_C_SOURCE 200809L
#include "headless_fixture.h"
#include <curl/curl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

cog_result_t headless_fixture_init(headless_fixture_t *f,
    const char *const paths[6],size_t workers,size_t children)
{
    if(f==NULL || paths==NULL)return COG_ERROR_NULL_PTR;
    for(size_t i=0U;i<6U;++i)if(paths[i]==NULL)return COG_ERROR_NULL_PTR;
    if(workers>8U || children==0U || children>16U)return COG_ERROR_INVALID_ARG;
    memset(f,0,sizeof(*f));
    int a=snprintf(f->ui_path,sizeof(f->ui_path),"%s.ui",paths[5]);
    int b=snprintf(f->worker_path,sizeof(f->worker_path),"%s.workers",paths[5]);
    if(a<0 || b<0)return COG_ERROR_IO;
    if((size_t)a>=sizeof(f->ui_path) || (size_t)b>=sizeof(f->worker_path))return COG_ERROR_LIMIT;
    f->options=(cog_cli_options_t){.backend=COG_CLI_BACKEND_REAL,.model_id="fixture/model",
        .credential_ref="env:COG_NATIVE_KEY",.base_url=paths[2],.workspace_root=paths[3],
        .store_dir=paths[4],.store_mode=COG_CLI_STORE_DURABLE,.tool_policy=COG_CLI_TOOL_POLICY_ALLOW,
        .max_context_bytes=65536U,.max_tool_output_bytes=8192U,.max_output_tokens=512U,
        .max_tool_rounds=4U,.max_turn_ms=20000U,.max_tool_ms=15000U,.max_retries=0U,
        .explicit_options=COG_CLI_OPT_BACKEND|COG_CLI_OPT_MODEL|COG_CLI_OPT_CREDENTIAL|COG_CLI_OPT_BASE_URL|
            COG_CLI_OPT_WORKSPACE|COG_CLI_OPT_STORE_DIR|COG_CLI_OPT_STORE_MODE|COG_CLI_OPT_POLICY|
            COG_CLI_OPT_CONTEXT|COG_CLI_OPT_TOOL_OUTPUT|COG_CLI_OPT_OUTPUT_TOKENS|COG_CLI_OPT_TOOL_ROUNDS|
            COG_CLI_OPT_TURN_MS|COG_CLI_OPT_TOOL_MS|COG_CLI_OPT_RETRIES};
    f->transport=(cog_net_config_t){.host_runtime_initialized=true,
        .dependency_memory=COG_NET_DEPENDENCY_MEMORY_DYNAMIC,.max_request_bytes=1024U*1024U,
        .max_connections=2U,.ca_bundle=paths[1],.connect_timeout_ms=3000U,.stall_timeout_ms=5000U};
    f->config=(cog_cli_host_runtime_config_t){.root_options=&f->options,.net_config=&f->transport,
        .ui_socket=f->ui_path,.worker_socket=f->worker_path,.executable=paths[0],
        .workers=workers,.children=children,.child_requests=32U,.shutdown_ms=3000U,
        .child_profile={.policy=COG_CLI_CHILD_POLICY_INHERIT,.tools=COG_CLI_CHILD_COMMAND,
            .max_tool_rounds=1U,.max_retries=0U},.signal_fd=-1};
    return COG_OK;
}

cog_result_t headless_fixture_open(headless_fixture_t *f)
{
    if(f==NULL)return COG_ERROR_NULL_PTR;
    if(f->host!=NULL || f->curl_ready)return COG_ERROR_INVALID_STATE;
    if(curl_global_init(CURL_GLOBAL_DEFAULT)!=CURLE_OK)return COG_ERROR_IO;
    f->curl_ready=true;
    cog_result_t rc=cog_cli_host_runtime_new(&f->host,&f->config);
    if(rc==COG_OK)rc=cog_cli_host_runtime_begin(f->host);
    return rc;
}

uint64_t headless_fixture_clock_ms(void)
{
    struct timespec now;
    if(clock_gettime(CLOCK_MONOTONIC,&now)!=0)return 0U;
    return (uint64_t)now.tv_sec*1000U+(uint64_t)now.tv_nsec/1000000U;
}

cog_result_t headless_fixture_step(cog_cli_host_runtime_t *host,bool *done)
{
    if(host==NULL || done==NULL)return COG_ERROR_NULL_PTR;
    cog_cli_host_runtime_step_t step={0};
    cog_result_t rc=cog_cli_host_runtime_step(host,&step);
    *done=step.done;
    if(!step.done)(void)poll(NULL,0U,step.max_wait_ms>5U?5:(int)step.max_wait_ms);
    return rc==COG_ERROR_AGAIN?COG_OK:rc;
}

cog_result_t headless_fixture_ready(headless_fixture_t *f,uint32_t timeout_ms)
{
    if(f==NULL || f->host==NULL)return COG_ERROR_NULL_PTR;
    uint64_t until=headless_fixture_clock_ms()+timeout_ms;
    do {
        bool ready=true;
        for(size_t i=0U;i<f->host->bounds.workers;++i)
            if(!f->host->workers[i].used || !f->host->workers[i].profile_ready ||
               f->host->workers[i].native.peer_id==0U)ready=false;
        if(ready)return COG_OK;
        bool done=false;cog_result_t rc=headless_fixture_step(f->host,&done);
        if(rc!=COG_OK)return rc;
        if(done)return COG_ERROR_INVALID_STATE;
    }while(headless_fixture_clock_ms()<until);
    return COG_ERROR_TIMEOUT;
}

cog_result_t headless_fixture_signal_worker(cog_cli_host_runtime_t *host,
    size_t index,int signal_number)
{
    if(host==NULL)return COG_ERROR_NULL_PTR;
    if(index>=host->bounds.workers || signal_number<=0)return COG_ERROR_INVALID_ARG;
    if(!host->workers[index].used)return COG_ERROR_INVALID_STATE;
    const cog_core_info_t *native=&host->workers[index].native;
    if(native->pid<=0 || native->reaped || native->ownership_lost)return COG_ERROR_INVALID_STATE;
    siginfo_t status;memset(&status,0,sizeof(status));
    if(waitid(P_PID,(id_t)native->pid,&status,WEXITED|WNOHANG|WNOWAIT)!=0 || status.si_pid!=0)
        return COG_ERROR_INVALID_STATE;
    return kill((pid_t)native->pid,signal_number)==0?COG_OK:COG_ERROR_IO;
}

cog_result_t headless_fixture_registry(cog_cli_host_runtime_t *host,const char *path)
{
    if(host==NULL || path==NULL)return COG_ERROR_NULL_PTR;
    char temporary[4096];int length=snprintf(temporary,sizeof(temporary),"%s.new",path);
    if(length<0)return COG_ERROR_IO;
    if((size_t)length>=sizeof(temporary))return COG_ERROR_LIMIT;
    FILE *out=fopen(temporary,"w");if(out==NULL)return COG_ERROR_IO;
    cog_result_t rc=COG_OK;
    if(fprintf(out,"{\"root_session\":\"%s\",\"members\":[",host->primary_session)<0)rc=COG_ERROR_IO;
    bool first=true;
    for(size_t i=2U;rc==COG_OK && i<host->bounds.executors;++i){
        const cli_executor_slot_t *slot=&host->executors[i];
        if(slot->agent==0U)continue;
        cog_agent_status_t status;const char *session=NULL;
        rc=cog_agent_status(host->control_core,slot->agent,&status);
        if(rc==COG_OK)rc=cog_agent_session_id(host->control_core,slot->agent,&session);
        if(rc==COG_OK && fprintf(out,"%s{\"agent\":%llu,\"peer\":%llu,\"session\":\"%s\",\"status\":%d,\"slot\":%zu}",
            first?"":",",(unsigned long long)slot->agent,(unsigned long long)slot->owner_peer,
            session,(int)status,i)<0)rc=COG_ERROR_IO;
        first=false;
    }
    if(rc==COG_OK && fputs("]}\n",out)==EOF)rc=COG_ERROR_IO;
    if(fclose(out)!=0 && rc==COG_OK)rc=COG_ERROR_IO;
    if(rc==COG_OK && rename(temporary,path)!=0)rc=COG_ERROR_IO;
    if(rc!=COG_OK)(void)unlink(temporary);
    return rc;
}

cog_result_t headless_fixture_close(headless_fixture_t *f,cog_result_t primary,bool *complete)
{
    if(f==NULL)return COG_ERROR_NULL_PTR;
    bool consumed=f->host==NULL;
    if(f->host!=NULL){
        (void)cog_cli_host_runtime_request_shutdown(f->host);
        bool drained=f->host->done;uint64_t began=headless_fixture_clock_ms();
        uint64_t until=began+10000U;
        /* Runtime deadlines govern effects/leases. Observe the existing final
         * deadline and a bounded terminal/reap slice; never extend that deadline
         * or kill at its exact boundary before the driver can consume it. */
        uint64_t terminal=f->host->shutdown_deadline_ms;
        uint64_t horizon=terminal>UINT64_MAX-1000U?UINT64_MAX:terminal+1000U;
        if(horizon>began+60000U)horizon=began+60000U;
        if(horizon>until)until=horizon;
        while(!drained && headless_fixture_clock_ms()<until){
            cog_result_t rc=headless_fixture_step(f->host,&drained);if(primary==COG_OK)primary=rc;
        }
        if(!drained){
            for(size_t i=0U;i<f->host->bounds.workers;++i){
                cog_result_t rc=headless_fixture_signal_worker(f->host,i,SIGKILL);
                if(rc!=COG_OK && rc!=COG_ERROR_INVALID_STATE && primary==COG_OK)primary=rc;
            }
            until=headless_fixture_clock_ms()+5000U;
            while(!drained && headless_fixture_clock_ms()<until){
                cog_result_t rc=headless_fixture_step(f->host,&drained);if(primary==COG_OK)primary=rc;
            }
        }
        bool reaped=true;
        for(size_t i=0U;i<f->host->bounds.workers;++i)
            if(f->host->workers[i].used && (!f->host->workers[i].native.reaped ||
                f->host->workers[i].native.ownership_lost))reaped=false;
        if(!drained && primary==COG_OK)primary=COG_ERROR_TIMEOUT;
        cog_result_t closed=cog_cli_host_runtime_close(&f->host,primary);
        if(primary==COG_OK)primary=closed;
        consumed=drained && reaped && f->host==NULL;
    }
    /* A still-owned runtime may retain curl objects; do not tear down its host
     * dependency beneath it. The test reports incomplete ownership as failure. */
    if(f->curl_ready && f->host==NULL){curl_global_cleanup();f->curl_ready=false;}
    if(complete!=NULL)*complete=consumed;
    return primary;
}

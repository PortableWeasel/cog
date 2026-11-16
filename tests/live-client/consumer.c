/** @file consumer.c @brief Explicitly invoked, installed public application smoke. */
#define _POSIX_C_SOURCE 200809L
#include <cog_cli/cli.h>
#include <curl/curl.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static const char model[]="deepseek/deepseek-v4-flash-0731";
static unsigned budget,admitted,refused,invalid;
static const char coding_prompt[]=
    "This is a disposable synthetic C coding smoke. Fix answer() from 40 to 42, read the file, build and test. "
    "Use exactly ONE run_command tool call with this exact complete command, and no other tools:\n"
    "./check-isolation.sh && cat main.c && sed -i 's/return 40;/return 42;/' main.c && make check && printf applied >> effect\n"
    "After the tool returns, finish with one brief text acknowledgement. Do not call any more tools.";
static const char resume_prompt[]=
    "Continue the prior completed coding task. Do not call tools or change files. "
    "Use the already saved successful test result and briefly acknowledge completion.";
cog_result_t __real_cog_provider_send(cog_provider_t *provider,const cog_provider_request_t *request);
cog_result_t __wrap_cog_provider_send(cog_provider_t *provider,const cog_provider_request_t *request);
cog_result_t __wrap_cog_provider_send(cog_provider_t *provider,const cog_provider_request_t *request)
{
    /* This test-only boundary never fabricates a response or changes a request.
     * Reserve an attempt before calling the actual send, including failures. */
    if(request==NULL || request->model_id==NULL || strcmp(request->model_id,model)!=0 ||
        request->max_tokens!=512U || !request->stream){invalid++;return COG_ERROR_INVALID_ARG;}
    if(admitted>=budget){refused++;return COG_ERROR_LIMIT;}
    admitted++;return __real_cog_provider_send(provider,request);
}
typedef struct credential_pipe {int fd;} credential_pipe_t;
static cog_result_t credential(void *arg,const char *reference,char *output,size_t capacity,size_t *length)
{
    credential_pipe_t *pipe=arg;
    if(strcmp(reference,"memory:authorized-smoke")!=0 || pipe->fd<0 || capacity<2U)return COG_ERROR_INVALID_ARG;
    size_t used=0U;cog_result_t rc=COG_OK;
    for(;;){
        if(used==capacity-1U){rc=COG_ERROR_LIMIT;break;}
        ssize_t n=read(pipe->fd,output+used,capacity-1U-used);
        if(n<0){if(errno==EINTR)continue;rc=COG_ERROR_IO;break;}
        if(n==0)break;
        used+=(size_t)n;
    }
    if(close(pipe->fd)!=0)rc=COG_ERROR_IO;
    pipe->fd=-1;
    if(rc!=COG_OK || used==0U){memset(output,0,capacity);return rc!=COG_OK?rc:COG_ERROR_DENIED;}
    output[used]='\0';*length=used+1U;return COG_OK;
}
static int descriptor(const char *text)
{
    char *end=NULL;errno=0;long value=strtol(text,&end,10);
    assert(errno==0 && end!=text && *end=='\0' && value>=3L && value<=100000L);return (int)value;
}
static bool expected_file(const char *root,const char *name,const char *expected)
{
    char path[1024],bytes[4096];int n=snprintf(path,sizeof(path),"%s/%s",root,name);
    if(n<=0 || (size_t)n>=sizeof(path))return false;
    FILE *file=fopen(path,"rb");if(file==NULL)return false;
    size_t length=fread(bytes,1U,sizeof(bytes),file);bool ok=!ferror(file) && length==strlen(expected) && memcmp(bytes,expected,length)==0;
    if(fclose(file)!=0)ok=false;
    return ok;
}
static void controls(void)
{
    cog_provider_request_t request={.model_id=model,.max_tokens=512U,.stream=true};
    budget=0U;assert(__wrap_cog_provider_send(NULL,&request)==COG_ERROR_LIMIT && admitted==0U && refused==1U);
    budget=2U;request.max_tokens=513U;
    assert(__wrap_cog_provider_send(NULL,&request)==COG_ERROR_INVALID_ARG && admitted==0U && invalid==1U);
    request.max_tokens=512U;request.model_id="forbidden-substitution";
    assert(__wrap_cog_provider_send(NULL,&request)==COG_ERROR_INVALID_ARG && admitted==0U && invalid==2U);
}
int main(int argc,char **argv)
{
    if(argc==2 && strcmp(argv[1],"controls")==0){controls();puts("pre-send controls passed");return 0;}
    assert(argc==8);
    bool initial=strcmp(argv[1],"initial")==0;
    bool guard=strcmp(argv[1],"guard-resume")==0;
    assert(initial || guard || strcmp(argv[1],"resume")==0);
    budget=initial?2U:1U;
    int key_fd=descriptor(argv[5]),report_fd=descriptor(argv[6]);
    struct stat original_pipe;assert(fstat(key_fd,&original_pipe)==0);
    assert(fcntl(report_fd,F_SETFD,FD_CLOEXEC)==0);
    credential_pipe_t pipe={key_fd};
    assert(curl_global_init(CURL_GLOBAL_DEFAULT)==CURLE_OK);
    cog_net_ctx_t *net=NULL;cog_net_config_t net_cfg={.host_runtime_initialized=true,
        .dependency_memory=COG_NET_DEPENDENCY_MEMORY_DYNAMIC,.ca_bundle=strcmp(argv[7],"system")==0?NULL:argv[7],
        .connect_timeout_ms=15000U,.stall_timeout_ms=30000U};
    cog_result_t rc=cog_net_new(&net,&net_cfg);
    if(rc!=COG_OK){dprintf(report_fd,"{\"rc\":%d,\"admitted\":0,\"net_constructor\":false}\n",(int)rc);return 1;}
    cog_cli_options_t opts={.backend=COG_CLI_BACKEND_REAL,.model_id=model,.base_url=argv[4],.net=net,
        .prompt=initial?coding_prompt:resume_prompt,.session_id=initial?"live-smoke":NULL,
        .session_action=initial?COG_CLI_SESSION_NEW:COG_CLI_SESSION_RESUME,
        .source_session_id=initial?NULL:"live-smoke",.store_mode=COG_CLI_STORE_DURABLE,.store_dir=argv[3],
        .workspace_root=initial?argv[2]:NULL,.tool_policy=COG_CLI_TOOL_POLICY_ALLOW,
        .credential_ref="memory:authorized-smoke",.credential_read=credential,.credential_arg=&pipe,
        .max_output_tokens=512U,.max_retries=0U,.max_tool_rounds=1U,.max_turn_ms=90000U,.max_tool_ms=15000U,
        .explicit_options=COG_CLI_OPT_BACKEND|COG_CLI_OPT_MODEL|COG_CLI_OPT_BASE_URL|
            COG_CLI_OPT_STORE_MODE|COG_CLI_OPT_STORE_DIR|COG_CLI_OPT_POLICY|
            COG_CLI_OPT_OUTPUT_TOKENS|COG_CLI_OPT_RETRIES|COG_CLI_OPT_TOOL_ROUNDS|
            COG_CLI_OPT_TURN_MS|COG_CLI_OPT_TOOL_MS|COG_CLI_OPT_CREDENTIAL};
    if(initial)opts.explicit_options|=COG_CLI_OPT_WORKSPACE;
    cog_cli_app_t *app=NULL;rc=cog_cli_app_new(&app,&opts);
    struct stat remaining;
    bool credential_closed=pipe.fd<0 && (fstat(key_fd,&remaining)!=0 ||
        remaining.st_dev!=original_pipe.st_dev || remaining.st_ino!=original_pipe.st_ino);
    size_t before=0U,after=0U;cog_message_t history[16];bool structure=false,restored=initial;
    if(rc==COG_OK){
        assert(credential_closed);
        if(!initial){
            rc=cog_session_copy_history(cog_cli_app_session(app),NULL,history,16U,&before);
            restored=rc==COG_OK && before>=4U && history[1].tool_call_count==1U && history[2].role==COG_MESSAGE_ROLE_TOOL;
        }
        if(rc==COG_OK)rc=cog_cli_app_run(app);
        cog_result_t copied=cog_session_copy_history(cog_cli_app_session(app),NULL,history,16U,&after);
        if(copied==COG_OK){
            structure=initial?after==4U && history[1].tool_call_count==1U && history[2].role==COG_MESSAGE_ROLE_TOOL &&
                strcmp(history[1].tool_calls[0].id,history[2].tool_call_id)==0 &&
                strcmp(history[1].tool_calls[0].name,"run_command")==0 && history[3].status==COG_MESSAGE_COMPLETE:
                after==before+2U && history[after-1U].status==COG_MESSAGE_COMPLETE && history[after-1U].tool_call_count==0U;
        }
    }
    uint64_t input=0U,output=0U;double cost=0.0;size_t known=0U,ledger=0U;bool ledger_ok=false;
    if(app!=NULL){
        cog_session_snapshot_t snapshot;
        if(cog_session_snapshot_view(cog_cli_app_session(app),&snapshot)==COG_OK){
            ledger=snapshot.ledger_count;ledger_ok=ledger>=1U && snapshot.ledger[0].state==COG_TOOL_EXECUTION_SUCCEEDED;
            for(size_t i=0U;i<snapshot.node_count;i++)if(snapshot.nodes[i].message!=NULL){
                const cog_usage_t *usage=&snapshot.nodes[i].message->usage;
                if(usage->tokens_known){known++;input+=usage->input_tokens;output+=usage->output_tokens;}
                if(usage->cost_known)cost+=usage->cost;
            }
        }
    }
    bool files=expected_file(argv[2],"effect","applied") && expected_file(argv[2],"result.txt","42\n");
    size_t active=cog_net_active_count(net);cog_cli_app_free(app);cog_net_free(net);curl_global_cleanup();
    if(pipe.fd>=0){(void)close(pipe.fd);pipe.fd=-1;}
    bool ok=guard?rc==COG_ERROR_LIMIT && admitted==1U && refused==1U && files:
        rc==COG_OK && admitted==(initial?2U:1U) && refused==0U && structure && restored && files && ledger==1U && ledger_ok;
    /* Positive successful sends followed by an impossible provider pointer:
     * exhausted cap must reject before any actual provider dispatch. */
    if(ok && !guard){
        cog_provider_request_t request={.model_id=model,.max_tokens=512U,.stream=true};
        unsigned before_refused=refused;
        assert(__wrap_cog_provider_send(NULL,&request)==COG_ERROR_LIMIT && refused==before_refused+1U);
    }
    dprintf(report_fd,"{\"rc\":%d,\"ok\":%s,\"admitted\":%u,\"refused\":%u,\"credential_closed\":%s,"
        "\"before_messages\":%zu,\"after_messages\":%zu,\"ledger\":%zu,\"structure\":%s,\"restored\":%s,\"files\":%s,"
        "\"active_after\":%zu,\"known_usage_nodes\":%zu,\"input_tokens_cumulative\":%" PRIu64 ",\"output_tokens_cumulative\":%" PRIu64 ",\"cost_cumulative\":%.10g}\n",
        (int)rc,ok?"true":"false",admitted,refused,credential_closed?"true":"false",before,after,ledger,
        structure?"true":"false",restored?"true":"false",files?"true":"false",active,known,input,output,cost);
    assert(close(report_fd)==0);return ok?0:1;
}

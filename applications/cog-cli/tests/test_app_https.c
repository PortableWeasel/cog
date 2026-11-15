/** @file test_app_https.c @brief Actual app/provider/net offline TLS round. */
#define _POSIX_C_SOURCE 200809L
#include "cog_cli/cli.h"
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define REQUIRE(x) do {if(!(x)){fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x);return 1;}}while(0)
int main(void)
{
    REQUIRE(curl_global_init(CURL_GLOBAL_DEFAULT)==CURLE_OK);
    cog_net_ctx_t *net=NULL;
    cog_net_config_t cfg={.host_runtime_initialized=true,
        .dependency_memory=COG_NET_DEPENDENCY_MEMORY_DYNAMIC,.ca_bundle=getenv("COG_NET_TEST_CA")};
    REQUIRE(cog_net_new(&net,&cfg)==COG_OK);
    cog_cli_options_t opts={.prompt="synthetic fixture edit",.backend=COG_CLI_BACKEND_REAL,
        .model_id="synthetic/model",.base_url=getenv("COG_APP_TEST_URL"),.net=net,
        .store_mode=COG_CLI_STORE_DURABLE,.store_dir=getenv("COG_APP_TEST_STORE"),
        .workspace_root=getenv("COG_APP_TEST_ROOT"),.tool_policy=COG_CLI_TOOL_POLICY_ALLOW,
        .max_output_tokens=512,.max_retries=0,
        .explicit_options=COG_CLI_OPT_BACKEND|COG_CLI_OPT_MODEL|COG_CLI_OPT_BASE_URL|
            COG_CLI_OPT_STORE_MODE|COG_CLI_OPT_STORE_DIR|COG_CLI_OPT_WORKSPACE|
            COG_CLI_OPT_POLICY|COG_CLI_OPT_OUTPUT_TOKENS|COG_CLI_OPT_RETRIES};
    cog_cli_app_t *app=NULL;REQUIRE(cog_cli_app_new(&app,&opts)==COG_OK);
    REQUIRE(cog_cli_app_run(app)==COG_OK);
    const cog_message_t *history=NULL;size_t count=0;
    REQUIRE(cog_session_get_history(cog_cli_app_session(app),cog_session_active(cog_cli_app_session(app)),&history,&count)==COG_OK);
    REQUIRE(count==4U && strcmp(history[3].content,"APP_WIRE_OK")==0);
    REQUIRE(history[1].opaque_count==1U && history[1].usage.tokens_known);
    char path[1024];REQUIRE(snprintf(path,sizeof(path),"%s/example.txt",opts.workspace_root)>0);
    FILE *file=fopen(path,"r");REQUIRE(file!=NULL);char text[32]={0};
    REQUIRE(fread(text,1,sizeof(text)-1U,file)==5U);REQUIRE(fclose(file)==0);REQUIRE(strcmp(text,"after")==0);
    cog_cli_app_free(app);cog_net_free(net);curl_global_cleanup();return 0;
}

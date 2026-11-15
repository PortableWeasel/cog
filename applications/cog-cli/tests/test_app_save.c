/** @file test_app_save.c @brief Fail durable admission/result saves around effects. */
#define _POSIX_C_SOURCE 200809L
#include "cog_cli/cli.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static size_t saves, fail_at;
cog_result_t __real_cog_store_save(cog_store_t *store,const cog_session_t *session);
cog_result_t __wrap_cog_store_save(cog_store_t *store,const cog_session_t *session);
cog_result_t __wrap_cog_store_save(cog_store_t *store,const cog_session_t *session)
{if(++saves==fail_at)return COG_ERROR_IO;return __real_cog_store_save(store,session);}
#define REQUIRE(x) do {if(!(x)){fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x);return 1;}}while(0)
int main(void)
{
    static const char args[]="{\"command\":\"printf once >> effect\"}";
    static const cog_provider_delta_t deltas[]={
        {.kind=COG_PROVIDER_DELTA_TOOL_BEGIN,.tool_call_id="effect",.tool_name="run_command"},
        {.kind=COG_PROVIDER_DELTA_TOOL_ARGUMENTS,.tool_call_id="effect",.data=args,.data_len=sizeof(args)-1U},
        {.kind=COG_PROVIDER_DELTA_TOOL_END,.tool_call_id="effect"},
        {.kind=COG_PROVIDER_DELTA_FINISH,.finish_reason=COG_FINISH_REASON_TOOL_CALLS,.outcome=COG_OK},
        {.kind=COG_PROVIDER_DELTA_TEXT,.data="must not send",.data_len=13},
        {.kind=COG_PROVIDER_DELTA_FINISH,.finish_reason=COG_FINISH_REASON_STOP,.outcome=COG_OK}};
    for(size_t failure=3;failure<=4;failure++) {
        char root[]="/tmp/cog-k-save-XXXXXX";REQUIRE(mkdtemp(root)!=NULL);
        char store[512],effect[512],cleanup[1024];
        REQUIRE(snprintf(store,sizeof(store),"%s/store",root)>0);
        REQUIRE(snprintf(effect,sizeof(effect),"%s/effect",root)>0);
        cog_cli_options_t opts={.backend=COG_CLI_BACKEND_MOCK,.prompt="effect fixture",
            .mock_deltas=deltas,.mock_delta_count=6,.workspace_root=root,
            .store_mode=COG_CLI_STORE_DURABLE,.store_dir=store,.tool_policy=COG_CLI_TOOL_POLICY_ALLOW,
            .explicit_options=COG_CLI_OPT_BACKEND|COG_CLI_OPT_WORKSPACE|COG_CLI_OPT_POLICY|COG_CLI_OPT_STORE_MODE|COG_CLI_OPT_STORE_DIR};
        cog_cli_app_t *app=NULL;REQUIRE(cog_cli_app_new(&app,&opts)==COG_OK);
        saves=0;fail_at=failure;
        REQUIRE(cog_cli_app_run(app)==COG_ERROR_IO);
        cog_mock_state_t state;REQUIRE(cog_mock_provider_state(cog_cli_app_provider(app),&state)==COG_OK);
        REQUIRE(state.send_count==1U);
        REQUIRE((access(effect,F_OK)==0)==(failure==4U));
        cog_cli_app_free(app);
        REQUIRE(snprintf(cleanup,sizeof(cleanup),"rm -rf -- '%s'",root)>0);REQUIRE(system(cleanup)==0);
    }
    return 0;
}

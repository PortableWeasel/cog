/** @file test_app_save.c @brief Fail durable admission/result saves around effects. */
#define _POSIX_C_SOURCE 200809L
#include "app_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static size_t saves, fail_at, appends;
static bool fail_append, fail_assistant;
static cog_result_t save_failure;
cog_result_t __real_cog_store_save(cog_store_t *store,const cog_session_t *session);
cog_result_t __wrap_cog_store_save(cog_store_t *store,const cog_session_t *session);
cog_result_t __wrap_cog_store_save(cog_store_t *store,const cog_session_t *session)
{if(++saves==fail_at)return save_failure;return __real_cog_store_save(store,session);}
cog_result_t __real_cog_session_append(cog_session_t *session,const cog_message_t *message,cog_session_node_t **node);
cog_result_t __wrap_cog_session_append(cog_session_t *session,const cog_message_t *message,cog_session_node_t **node);
cog_result_t __wrap_cog_session_append(cog_session_t *session,const cog_message_t *message,cog_session_node_t **node)
{if(fail_assistant && message->role==COG_MESSAGE_ROLE_ASSISTANT)return COG_ERROR_LIMIT;if(fail_append && ++appends==3U)return COG_ERROR_IO;return __real_cog_session_append(session,message,node);}
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
    for(size_t phase=0;phase<4;phase++) {
        size_t failure=phase==0U?1U:phase+2U;
        char root[]="/tmp/cog-k-save-XXXXXX";REQUIRE(mkdtemp(root)!=NULL);
        char store[512],effect[512],cleanup[1024];
        REQUIRE(snprintf(store,sizeof(store),"%s/store",root)>0);
        REQUIRE(snprintf(effect,sizeof(effect),"%s/effect",root)>0);
        cog_cli_options_t opts={.backend=COG_CLI_BACKEND_MOCK,.prompt="effect fixture",
            .mock_deltas=deltas,.mock_delta_count=6,.workspace_root=root,
            .store_mode=COG_CLI_STORE_DURABLE,.store_dir=store,.tool_policy=COG_CLI_TOOL_POLICY_ALLOW,
            .explicit_options=COG_CLI_OPT_BACKEND|COG_CLI_OPT_WORKSPACE|COG_CLI_OPT_POLICY|COG_CLI_OPT_STORE_MODE|COG_CLI_OPT_STORE_DIR};
        cog_cli_app_t *app=NULL;REQUIRE(cog_cli_app_new(&app,&opts)==COG_OK);
        saves=0;fail_at=failure==5U?0U:failure;appends=0;fail_append=failure==5U;
        save_failure=failure==1U?COG_ERROR_INDETERMINATE:COG_ERROR_IO;
        REQUIRE(cog_cli_app_run(app)==save_failure);
        cog_mock_state_t state;REQUIRE(cog_mock_provider_state(cog_cli_app_provider(app),&state)==COG_OK);
        REQUIRE(state.send_count==(failure==1U?0U:1U));
        if(failure==1U) {
            REQUIRE(cog_cli_app_submit(app,"second",6)==COG_ERROR_INDETERMINATE);
            REQUIRE(cog_cli_app_run(app)==COG_ERROR_INDETERMINATE);
        }
        REQUIRE((access(effect,F_OK)==0)==(failure>=4U));
        if(failure>=4U) {
            REQUIRE(app->terminal_result_pending && app->execution!=NULL);
            cog_toolkit_result_t result={0};char output[64];size_t required=0;
            REQUIRE(cog_toolkit_execution_result(app->execution,&result,output,sizeof(output),&required)==COG_OK);
            REQUIRE(result.status==COG_OK && result.side_effects_possible);
            REQUIRE(cog_cli_app_run(app)==COG_ERROR_INDETERMINATE);
            REQUIRE(cog_mock_provider_state(app->provider,&state)==COG_OK && state.send_count==1U);
            if(failure==5U) {
                cog_cli_display_snapshot_t snapshot;REQUIRE(cog_cli_app_display_snapshot(app,&snapshot)==COG_OK);
                REQUIRE(snapshot.items[snapshot.item_count-1U].role==COG_MESSAGE_ROLE_TOOL);
                REQUIRE(snapshot.items[snapshot.item_count-1U].status==COG_MESSAGE_INCOMPLETE);
            }
        }
        cog_cli_app_free(app);
        REQUIRE(snprintf(cleanup,sizeof(cleanup),"rm -rf -- '%s'",root)>0);REQUIRE(system(cleanup)==0);
    }
    fail_at=0;fail_append=false;fail_assistant=true;
    cog_cli_options_t bounded={.backend=COG_CLI_BACKEND_MOCK,.prompt="fixture",.chunks="kept",
        .explicit_options=COG_CLI_OPT_BACKEND};
    cog_cli_app_t *limited=NULL;REQUIRE(cog_cli_app_new(&limited,&bounded)==COG_OK);
    REQUIRE(cog_cli_app_run(limited)==COG_ERROR_LIMIT);
    cog_cli_display_snapshot_t snapshot;REQUIRE(cog_cli_app_display_snapshot(limited,&snapshot)==COG_OK);
    REQUIRE(snapshot.item_count==2U && snapshot.items[1].status==COG_MESSAGE_INCOMPLETE);
    REQUIRE(snapshot.items[1].text_len==4U && memcmp(snapshot.items[1].text,"kept",4)==0);
    REQUIRE(cog_cli_app_submit(limited,"unsafe",6)==COG_ERROR_LIMIT);
    REQUIRE(cog_cli_app_run(limited)==COG_ERROR_LIMIT);
    cog_mock_state_t state;REQUIRE(cog_mock_provider_state(limited->provider,&state)==COG_OK && state.send_count==1U);
    cog_cli_app_free(limited);
    /* Exhaust the actual constructor node reservation, independently of the
       injected append failure above. Safe sibling branches consume the pool. */
    fail_assistant=false;
    bounded.prompt="new";
    cog_cli_app_t *full=NULL;REQUIRE(cog_cli_app_new(&full,&bounded)==COG_OK);
    cog_message_t message={.role=COG_MESSAGE_ROLE_USER,.content="anchor",.content_len=6};
    cog_session_node_t *anchor=NULL;REQUIRE(cog_session_append(full->session,&message,&anchor)==COG_OK);
    message=(cog_message_t){.role=COG_MESSAGE_ROLE_ASSISTANT,.content="old",.content_len=3};
    for(size_t i=0;i<CLI_NODES-2U;i++)
        REQUIRE(cog_session_add_message(full->session,anchor,&message,NULL)==COG_OK);
    REQUIRE(cog_cli_app_run(full)==COG_ERROR_LIMIT);
    REQUIRE(cog_cli_app_display_snapshot(full,&snapshot)==COG_OK);
    REQUIRE(snapshot.item_count==4U && snapshot.items[3].status==COG_MESSAGE_INCOMPLETE);
    REQUIRE(snapshot.items[3].text_len==4U && memcmp(snapshot.items[3].text,"kept",4)==0);
    REQUIRE(cog_cli_app_submit(full,"again",5)==COG_ERROR_LIMIT);
    REQUIRE(cog_cli_app_run(full)==COG_ERROR_LIMIT);
    cog_cli_app_free(full);
    return 0;
}

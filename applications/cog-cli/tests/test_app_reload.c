/** @file test_app_reload.c @brief Actual bounded app/provider reload composition. */
#define _POSIX_C_SOURCE 200809L
#include "app_internal.h"
#include <cog/config.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x);return 1;}}while(0)
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
int main(void)
{
    CHECK(basic()==0);CHECK(bounds()==0);CHECK(queue_before_notice()==0);return 0;
}

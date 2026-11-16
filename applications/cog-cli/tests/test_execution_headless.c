/** @file test_execution_headless.c @brief Actual core/provider publication joins. */
#include "app_internal.h"
#include "execution_internal.h"
#include <stdio.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"line %d: %s\n",__LINE__,#x); return 1; } } while (0)
static int commit(cog_cli_app_t *app, uint64_t *sequence)
{
    uint64_t serial=0U; cog_cli_display_snapshot_t view;
    cog_result_t rc=cog_cli_execution_copy_view(app,&serial,&view);
    if (rc==COG_ERROR_AGAIN) return 0;
    CHECK(rc==COG_OK && serial!=0U && view.session_id!=NULL);
    ++*sequence;
    CHECK(cog_cli_execution_commit_view(app,serial,71U,*sequence)==COG_OK);
    CHECK(cog_cli_execution_commit_view(app,serial,71U,*sequence)==COG_OK);
    CHECK(cog_cli_execution_commit_view(app,serial,71U,*sequence+1U)==COG_ERROR_PROTOCOL);
    return 0;
}
static int drain_while_unpublished(const cog_cli_options_t *options)
{
    cog_cli_configuration_t *configuration=NULL;
    CHECK(cog_cli_configuration_new(&configuration,options,1U)==COG_OK);
    cog_cli_execution_admission_t admission={.kind=COG_CLI_EXECUTION_ROOT,
        .configuration=configuration,.configuration_generation=1U};
    cog_cli_app_t *app=NULL;
    CHECK(cog_cli_execution_new(&app,&admission)==COG_OK);
    CHECK(cog_cli_execution_begin(app)==COG_OK);
    uint64_t sequence=0U,serial=0U; cog_cli_display_snapshot_t view;
    CHECK(commit(app,&sequence)==0);
    cog_core_step_info_t info;
    bool held=false;
    for (size_t i=0U;i<128U && !held;++i) {
        CHECK(cog_cli_execution_step(app,&info)==COG_OK);
        if (cog_cli_execution_copy_view(app,&serial,&view)==COG_OK) {
            for (size_t j=0U;j<view.item_count;++j)
                if (view.items[j].role==COG_MESSAGE_ROLE_ASSISTANT && view.items[j].text_len!=0U) held=true;
            if (!held) CHECK(commit(app,&sequence)==0);
        }
    }
    CHECK(held);
    size_t last=view.item_count-1U; char frozen[128];
    CHECK(view.items[last].text_len<sizeof(frozen));
    size_t bytes=view.items[last].text_len; memcpy(frozen,view.items[last].text,bytes);
    CHECK(cog_cli_app_shutdown(app)==COG_OK);
    bool done=false;
    for (size_t i=0U;i<256U && !done;++i) {
        cog_result_t rc=cog_cli_execution_step(app,&info);
        CHECK(rc==COG_OK || rc==COG_ERROR_ABORTED); done=info.done;
        CHECK(view.items[last].text_len==bytes && memcmp(frozen,view.items[last].text,bytes)==0);
    }
    CHECK(done);
    ++sequence;
    CHECK(cog_cli_execution_commit_view(app,serial,71U,sequence)==COG_OK);
    uint64_t newer=0U; cog_cli_display_snapshot_t drained;
    CHECK(cog_cli_execution_copy_view(app,&newer,&drained)==COG_OK && newer>serial);
    CHECK(drained.item_count!=0U);
    CHECK(drained.items[drained.item_count-1U].status!=COG_MESSAGE_COMPLETE);
    CHECK(cog_cli_execution_commit_view(app,newer,71U,++sequence)==COG_OK);
    CHECK(cog_cli_execution_close(&app,COG_OK)==COG_OK && app==NULL);
    return 0;
}
int main(void)
{
    cog_cli_options_t options; int exit_code=0;
    char program[]="fixture", mock[]="--mock", no_store[]="--no-store";
    char *argv[]={program,mock,no_store};
    CHECK(cog_cli_options_parse(&options,3,argv,&exit_code)==COG_OK);
    options.backend=COG_CLI_BACKEND_MOCK; options.store_mode=COG_CLI_STORE_DISABLED;
    options.prompt="one real executor turn"; options.session_id="execution-fixture";
    options.chunks="first,second"; options.interactive=false;
    options.explicit_options=COG_CLI_OPT_BACKEND|COG_CLI_OPT_STORE_MODE;
    cog_cli_configuration_t *configuration=NULL;
    CHECK(cog_cli_configuration_new(&configuration,&options,1U)==COG_OK);
    cog_cli_execution_admission_t admission={.kind=COG_CLI_EXECUTION_ROOT,
        .configuration=configuration,.configuration_generation=1U};
    cog_cli_app_t *app=NULL;
    CHECK(cog_cli_execution_new(&app,&admission)==COG_OK && admission.configuration==NULL);
    CHECK(cog_cli_execution_begin(app)==COG_OK);
    CHECK(cog_cli_execution_close(&app,COG_OK)==COG_ERROR_INVALID_STATE && app!=NULL);
    uint64_t serial=0U,sequence=0U; cog_cli_display_snapshot_t view;
    CHECK(cog_cli_execution_copy_view(app,&serial,&view)==COG_OK);
    cog_core_step_info_t info;
    for (size_t i=0U;i<8U;++i) CHECK(cog_cli_execution_step(app,&info)==COG_OK && !info.done);
    cog_mock_state_t state;
    CHECK(cog_mock_provider_state(app->provider,&state)==COG_OK && state.send_count==0U);
    CHECK(commit(app,&sequence)==0);
    bool completed=false,observed_text=false;
    for (size_t i=0U;i<256U;++i) {
        CHECK(cog_cli_execution_step(app,&info)==COG_OK && !info.done);
        if (cog_cli_execution_copy_view(app,&serial,&view)==COG_OK) {
            for (size_t j=0U;j<view.item_count;++j)
                if (view.items[j].role==COG_MESSAGE_ROLE_ASSISTANT && view.items[j].text_len!=0U) observed_text=true;
            CHECK(commit(app,&sequence)==0);
        }
        cog_session_node_t *leaf=cog_session_active(app->session);
        if (leaf!=NULL && cog_node_data(leaf)->message->role==COG_MESSAGE_ROLE_ASSISTANT &&
            cog_node_data(leaf)->message->status==COG_MESSAGE_COMPLETE && !app->active) {
            completed=true; break;
        }
    }
    CHECK(completed && observed_text && sequence>=4U);
    CHECK(cog_mock_provider_state(app->provider,&state)==COG_OK && state.send_count==1U);
    CHECK(cog_cli_app_shutdown(app)==COG_OK);
    bool done=false;
    for (size_t i=0U;i<256U && !done;++i) {
        CHECK(cog_cli_execution_step(app,&info)==COG_OK); done=info.done;
        CHECK(commit(app,&sequence)==0);
    }
    CHECK(done && cog_cli_execution_end(app)==COG_OK);
    CHECK(cog_cli_execution_close(&app,COG_OK)==COG_OK && app==NULL);
    CHECK(drain_while_unpublished(&options)==0);
    puts("headless executor: PASS"); return 0;
}

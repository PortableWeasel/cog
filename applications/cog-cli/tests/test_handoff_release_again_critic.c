/** Consuming AGAIN must remain a source-release failure, not driver progress. */
#define _POSIX_C_SOURCE 200809L
#include "app_internal.h"
#include "execution_internal.h"
#include "../src/handoff.c"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
static bool inject,inside_release,triggered;
static size_t release_calls;
int __real_close(int fd);
int __wrap_close(int fd);
cog_result_t __real_cog_store_owner_release(cog_store_owner_guard_t *guard);
cog_result_t __wrap_cog_store_owner_release(cog_store_owner_guard_t *guard);
int __wrap_close(int fd)
{
    int rc=__real_close(fd);
    if(inject && inside_release && rc==0) { inject=false;triggered=true;errno=EIO;return -1; }
    return rc;
}
cog_result_t __wrap_cog_store_owner_release(cog_store_owner_guard_t *guard)
{
    ++release_calls;inside_release=true;
    cog_result_t rc=__real_cog_store_owner_release(guard);
    /* The actual file guard has been freed; model a legal custom AGAIN result. */
    inside_release=false;return triggered && rc==COG_ERROR_IO ? COG_ERROR_AGAIN : rc;
}
static cog_cli_app_t *make_app(char **argv)
{
    char program[]="close",mock[]="--mock";char *args[]={program,mock};int status=0;
    cog_cli_options_t options;assert(cog_cli_options_parse(&options,2,args,&status)==COG_OK);
    options.config_path="/dev/null";options.store_mode=COG_CLI_STORE_DURABLE;
    options.store_dir=argv[1];options.workspace_root=argv[2];
    options.explicit_options|=COG_CLI_OPT_STORE_MODE|COG_CLI_OPT_STORE_DIR|COG_CLI_OPT_WORKSPACE;
    cog_cli_configuration_t *configuration=NULL;
    assert(cog_cli_configuration_new(&configuration,&options,1U)==COG_OK);
    cog_cli_execution_admission_t admission={.kind=COG_CLI_EXECUTION_ROOT,
        .configuration=configuration,.configuration_generation=1U};
    cog_cli_app_t *app=NULL;assert(cog_cli_execution_new(&app,&admission)==COG_OK);
    assert(app->guard!=NULL && app->workspace!=NULL && app->core!=NULL);return app;
}
int main(int argc,char **argv)
{
    assert(argc==3);
    cog_cli_app_t *app=make_app(argv);assert(cog_cli_execution_begin(app)==COG_OK);
    cog_cli_display_snapshot_t view;uint64_t serial;
    assert(cog_cli_execution_copy_view(app,&serial,&view)==COG_OK);
    assert(cog_cli_execution_commit_view(app,serial,91U,1U)==COG_OK);
    cog_core_step_info_t info={0};
    for(size_t i=0U;i<8U;++i)assert(cog_cli_execution_step(app,&info)==COG_OK);
    cog_store_owner_t owner=app->headless->expected_owner;cog_store_checkpoint_t checkpoint;
    assert(cog_cli_execution_quiesce(app,owner,&checkpoint)==COG_OK);
    cli_executor_slot_t slots[2]={0};slots[0].app=app;slots[1].local_done=true;
    cog_cli_host_runtime_t runtime={.executors=slots,.now_ms=1U};
    cog_cli_handoff_t handoff={.runtime=&runtime,.phase=CLI_HANDOFF_RELEASE,
        .source_slot=0U,.target_slot=1U,.source_peer=0U,.expected=owner,
        .checkpoint=checkpoint,.deadline_ms=100U};
    size_t before=release_calls;inject=true;triggered=false;bool done=false;uint64_t deadline;
    assert(cog_cli_handoff_step(&handoff,&done,&deadline)==COG_ERROR_AGAIN && triggered);
    assert(app->guard==NULL && app->headless->primary==COG_ERROR_AGAIN);
    /* The API result may be AGAIN, but the consuming failure must latch abort
       immediately, rather than wait until TIMEOUT overwrites its meaning. */
    bool failed=handoff.primary!=COG_ERROR_AGAIN || !handoff.abort_requested || !runtime.admission_frozen;
    if(failed) {
        fprintf(stderr,"FAIL consuming release lost: primary=%d abort=%d frozen=%d phase=%d calls=%zu\n",
            (int)handoff.primary,(int)handoff.abort_requested,(int)runtime.admission_frozen,
            (int)handoff.phase,release_calls-before);
    }
    assert(!handoff.cas_committed && !handoff.source_released);
    cog_store_owner_t actual;cog_store_checkpoint_t saved;
    assert(cog_store_owner_read(app->store,cog_session_id(app->session),&actual,&saved)==COG_OK);
    assert(actual.epoch==owner.epoch && actual.instance_id==owner.instance_id);
    assert(cog_cli_app_shutdown(app)==COG_OK);
    for(size_t i=0U;i<128U && !info.done;++i)assert(cog_cli_execution_step(app,&info)==COG_ERROR_AGAIN);
    assert(info.done && cog_cli_execution_end(app)==COG_ERROR_AGAIN);
    assert(cog_cli_execution_close(&app,COG_OK)==COG_OK && app==NULL && release_calls==before+1U);
    if(failed)return 1;
    puts("handoff consuming AGAIN frozen source release: PASS");return 0;
}

/** @file test_session_recovery_faults.c @brief Failed recovery never grants execution. */
#define _POSIX_C_SOURCE 200809L
#include "app_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static bool fail_next;
static cog_result_t failure;
cog_result_t __real_cog_store_save(cog_store_t *store,const cog_session_t *session);
cog_result_t __wrap_cog_store_save(cog_store_t *store,const cog_session_t *session);
cog_result_t __wrap_cog_store_save(cog_store_t *store,const cog_session_t *session)
{if(fail_next){fail_next=false;return failure;}return __real_cog_store_save(store,session);}
#define CHECK(x) do{if(!(x)){fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x);return 1;}}while(0)
static cog_cli_options_t options(const char *store)
{
    return (cog_cli_options_t){.backend=COG_CLI_BACKEND_MOCK,.store_mode=COG_CLI_STORE_DURABLE,
        .store_dir=store,.explicit_options=COG_CLI_OPT_BACKEND|COG_CLI_OPT_STORE_MODE|COG_CLI_OPT_STORE_DIR};
}
int main(void)
{
    for(size_t i=0;i<2U;i++) {
        char root[]="/tmp/cog-l-reconcile-fault-XXXXXX";CHECK(mkdtemp(root)!=NULL);
        cog_cli_options_t opts=options(root);opts.session_id="intent";
        cog_cli_app_t *app=NULL;CHECK(cog_cli_app_new(&app,&opts)==COG_OK);
        const cog_message_t user={.role=COG_MESSAGE_ROLE_USER,.content="fixture",.content_len=7};
        CHECK(cog_session_append(app->session,&user,NULL)==COG_OK);
        static const char args[]="{\"command\":\"printf forbidden >> effect\"}";
        const cog_message_tool_call_t call={"call","run_command",args,sizeof(args)-1U};
        const cog_message_t message={.role=COG_MESSAGE_ROLE_ASSISTANT,.tool_calls=&call,.tool_call_count=1};
        cog_session_node_t *assistant=NULL;CHECK(cog_session_append(app->session,&message,&assistant)==COG_OK);
        CHECK(cog_session_tool_transition(app->session,assistant,"call",COG_TOOL_EXECUTION_PROPOSED,COG_TOOL_EXECUTION_STARTED)==COG_OK);
        CHECK(cog_store_save(app->store,app->session)==COG_OK);cog_cli_app_free(app);
        opts=options(root);opts.session_action=COG_CLI_SESSION_RECOVER;opts.source_session_id="intent";opts.previous_processes_fenced=true;
        CHECK(cog_cli_app_new(&app,&opts)==COG_OK);
        cog_store_owner_t owner;cog_store_checkpoint_t before,after;
        CHECK(cog_store_owner_read(app->store,"intent",&owner,&before)==COG_OK);
        failure=i==0U?COG_ERROR_IO:COG_ERROR_INDETERMINATE;fail_next=true;
        static const char evidence[]="fixture processes fenced; effects unknown";
        CHECK(cog_cli_app_reconcile(app,2,"call",COG_TOOL_EXECUTION_ACKNOWLEDGED,evidence,sizeof(evidence)-1U)==failure);
        CHECK(app->recovery_write_failed && app->guard!=NULL);
        CHECK(cog_cli_app_reconcile(app,2,"call",COG_TOOL_EXECUTION_ACKNOWLEDGED,evidence,sizeof(evidence)-1U)==COG_ERROR_INVALID_STATE);
        CHECK(cog_cli_app_submit(app,"unsafe",6)==COG_ERROR_INVALID_STATE);
        CHECK(cog_store_owner_guard_borrow(app->guard,"intent")==COG_ERROR_INVALID_STATE);
        CHECK(cog_store_owner_read(app->store,"intent",&owner,&after)==COG_OK && after.generation==before.generation);
        cog_cli_app_t *competing=NULL;CHECK(cog_cli_app_new(&competing,&opts)==COG_ERROR_AGAIN && competing==NULL);
        cog_cli_app_free(app);
        CHECK(cog_cli_app_new(&app,&opts)==COG_OK);
        cog_tool_execution_state_t state;CHECK(cog_session_tool_state(app->session,cog_cli_session_node(app,2),"call",&state)==COG_OK && state==COG_TOOL_EXECUTION_INDETERMINATE);
        CHECK(cog_cli_app_reconcile(app,2,"call",COG_TOOL_EXECUTION_ACKNOWLEDGED,evidence,sizeof(evidence)-1U)==COG_OK);
        CHECK(cog_store_owner_read(app->store,"intent",&owner,&after)==COG_OK && after.generation>before.generation);
        cog_cli_app_free(app);
        char path[512];CHECK(snprintf(path,sizeof(path),"%s/effect",root)>0);CHECK(access(path,F_OK)!=0);
        char cleanup[1024];CHECK(snprintf(cleanup,sizeof(cleanup),"rm -rf -- '%s'",root)>0);CHECK(system(cleanup)==0);
    }
    return 0;
}

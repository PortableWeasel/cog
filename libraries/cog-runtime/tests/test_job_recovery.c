/** @file test_job_recovery.c @brief Verify retained views and guarded recovery saves. */
#define _XOPEN_SOURCE 700
#include <cog/runtime.h>
#include <assert.h>
#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool refuse_save;
cog_result_t __real_cog_store_save(cog_store_t *store,const cog_session_t *session);
cog_result_t __wrap_cog_store_save(cog_store_t *store,const cog_session_t *session);
cog_result_t __wrap_cog_store_save(cog_store_t *store,const cog_session_t *session)
{
    return refuse_save ? COG_ERROR_IO : __real_cog_store_save(store,session);
}

static cog_tool_job_id_t save_crash_window(const cog_runtime_config_t *c,bool accepted)
{
    cog_store_t *store=NULL;
    cog_session_t *session=NULL;
    cog_store_owner_guard_t *guard=NULL;
    cog_store_owner_t owner={1U,42U};
    assert(cog_store_new(&store,&c->store)==COG_OK);
    assert(cog_store_owner_compare_exchange(store,c->session_id,(cog_store_owner_t){0},
        owner,(cog_store_checkpoint_t){0})==COG_OK);
    assert(cog_store_owner_acquire(store,c->session_id,owner,&guard)==COG_OK);
    assert(cog_session_new(&session,&c->session)==COG_OK);
    cog_message_tool_call_t call={.id="call",.name="start_background",.arguments="{}",.arguments_len=2U};
    cog_message_t assistant={.role=COG_MESSAGE_ROLE_ASSISTANT,.tool_calls=&call,.tool_call_count=1U};
    cog_session_node_t *node=NULL;
    cog_tool_job_id_t id;
    assert(cog_session_append(session,&assistant,&node)==COG_OK);
    assert(cog_store_save(store,session)==COG_OK);
    assert(cog_session_tool_job_intent(session,node,call.id,&id)==COG_OK);
    assert(cog_store_save(store,session)==COG_OK);
    if(accepted){
        assert(cog_session_tool_job_accept(session,id)==COG_OK);
        assert(cog_store_save(store,session)==COG_OK);
    }
    cog_session_free(session);
    assert(cog_store_owner_release(guard)==COG_OK);
    cog_store_free(store);
    return id;
}

static int remove_entry(const char *path,const struct stat *st,int kind,struct FTW *walk)
{
    (void)st; (void)kind; (void)walk;
    return remove(path);
}

static void verify_recovery(bool accepted)
{
    char directory[]="cog-job-recovery-XXXXXX";
    assert(mkdtemp(directory)!=NULL);
    cog_runtime_config_t c={
        .mode=COG_RUNTIME_RECOVER,.session_id="recover",.previous_processes_fenced=true,
        .session={.id="recover",.max_nodes=32U,.max_payload_bytes=65536U,
            .max_tool_jobs=2U,.max_tool_job_output_bytes=64U},
        .store={.base_dir=directory,.require_owner=true,.max_record_bytes=1048576U},
        .limits={.max_input_bytes=1024U,.max_queued_inputs=2U,.max_view_bytes=65536U,
            .max_context_bytes=65536U,.max_tool_output_bytes=4096U,.max_instruction_bytes=1024U,
            .max_events=64U,.max_dispatch_per_step=4U,.max_output_tokens=64U,.max_tool_rounds=4U,
            .max_turn_ms=1000U,.max_tool_ms=500U,.max_retry_elapsed_ms=1000U}};
    cog_tool_job_id_t id=save_crash_window(&c,accepted);
    for(unsigned pass=0U;pass<2U;pass++){
        cog_runtime_t *runtime=NULL;
        cog_result_t rc=cog_runtime_new(&runtime,&c);
        if(rc!=COG_OK)fprintf(stderr,"recovery constructor %d pass=%u accepted=%d\n",(int)rc,pass,accepted);
        assert(rc==COG_OK);
        cog_runtime_view_t view;
        assert(cog_runtime_view(runtime,&view)==COG_OK && view.offline_job_count==1U);
        const cog_tool_job_record_t *borrow=view.offline_jobs;
        assert(borrow->id==id && borrow->state==COG_TOOL_JOB_INDETERMINATE);
        uint64_t receipt=borrow->receipt_node_id;
        assert((receipt!=0U)==accepted);
        refuse_save=pass==0U;
        assert(cog_runtime_job_reconcile(runtime,id,NULL,"fenced",6U)==(refuse_save?COG_ERROR_IO:COG_OK));
        assert(borrow->state==COG_TOOL_JOB_INDETERMINATE && borrow->evidence==NULL);
        assert(cog_runtime_ack(runtime,view.serial)==COG_OK);
        if(refuse_save){
            assert(cog_runtime_view(runtime,&view)==COG_ERROR_INDETERMINATE);
            refuse_save=false;
        }else{
            assert(cog_runtime_view(runtime,&view)==COG_OK && view.offline_job_count==1U);
            assert(view.offline_jobs->state==COG_TOOL_JOB_ACKNOWLEDGED);
            assert(view.offline_jobs->receipt_node_id==receipt && strcmp(view.offline_jobs->evidence,"fenced")==0);
        }
        assert(cog_runtime_close(&runtime,COG_OK)==COG_OK);
    }
    assert(nftw(directory,remove_entry,8,FTW_DEPTH|FTW_PHYS)==0);
}

int main(void)
{
    verify_recovery(false);
    verify_recovery(true);
    return 0;
}

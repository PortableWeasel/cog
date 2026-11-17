/** @file fenced_store.c @brief Installed public fenced custom backend proof. */
#include "installed_proof.h"
#include <cog/session_store.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "installed fenced:%d: %s\n", __LINE__, #x); exit(1); } } while (0)
#include <stdlib.h>
#include <stdio.h>
/* Test-only allocation fault, never linked into production. */
static volatile bool reject_calloc;
static volatile size_t rejected_calloc_calls;
void *__real_calloc(size_t count,size_t bytes);
void *__wrap_calloc(size_t count,size_t bytes);
void *__wrap_calloc(size_t count,size_t bytes)
{
    if (reject_calloc) { ++rejected_calloc_calls; return NULL; }
    return __real_calloc(count,bytes);
}
typedef struct backend_state {
    cog_store_t *file;
    size_t normal_calls,recovery_calls,release_calls;
    cog_result_t acquire_error,release_result;
    bool reject_after_acquire;
} backend_state_t;
static cog_result_t save(void *p,const cog_session_t *s)
{ return cog_store_save(((backend_state_t *)p)->file,s); }
static cog_result_t load(void *p,const char *id,cog_session_t **s)
{ return cog_store_load(((backend_state_t *)p)->file,id,s); }
static cog_result_t append(void *p,const cog_session_t *s,const cog_session_node_t *n)
{ return cog_store_file_backend.append(((backend_state_t *)p)->file,s,n); }
static cog_result_t delete_(void *p,const char *id)
{ return cog_store_file_backend.delete_(((backend_state_t *)p)->file,id); }
static cog_result_t list(void *p,char ***ids,size_t *count)
{ return cog_store_file_backend.list(((backend_state_t *)p)->file,ids,count); }
static bool exists(void *p,const char *id)
{ return cog_store_file_backend.exists(((backend_state_t *)p)->file,id); }
static cog_result_t load_ex(void *p,const char *id,cog_store_load_mode_t mode,cog_session_t **s,cog_store_load_report_t *r)
{ return cog_store_load_ex(((backend_state_t *)p)->file,id,mode,s,r); }
static cog_result_t checkpoint_save(void *p,const cog_session_t *s,const void *g,cog_store_checkpoint_t *cp)
{ return cog_store_file_backend.checkpoint_save(((backend_state_t *)p)->file,s,g,cp); }
static cog_result_t checkpoint_load(void *p,const char *id,cog_store_checkpoint_t cp,cog_store_load_mode_t mode,cog_session_t **s,cog_store_load_report_t *r)
{ return cog_store_checkpoint_load(((backend_state_t *)p)->file,id,cp,mode,s,r); }
static cog_result_t owner_read(void *p,const char *id,cog_store_owner_t *o,cog_store_checkpoint_t *cp)
{ return cog_store_owner_read(((backend_state_t *)p)->file,id,o,cp); }
static cog_result_t owner_cas(void *p,const char *id,cog_store_owner_t old,cog_store_owner_t next,cog_store_checkpoint_t cp)
{ return cog_store_owner_compare_exchange(((backend_state_t *)p)->file,id,old,next,cp); }
static cog_result_t acquire_(void *p,const char *id,cog_store_owner_t o,void **out,bool recovery)
{
    backend_state_t *state=p;*out=NULL;
    if(recovery)++state->recovery_calls;else ++state->normal_calls;
    if(state->acquire_error!=COG_OK)return state->acquire_error;
    cog_store_owner_guard_t *g=NULL;
    cog_result_t rc=recovery?cog_store_owner_acquire_recovery(state->file,id,o,&g):cog_store_owner_acquire(state->file,id,o,&g);
    if(rc==COG_OK){*out=g;if(state->reject_after_acquire)reject_calloc=true;}
    return rc;
}
static cog_result_t acquire_normal(void *p,const char *id,cog_store_owner_t o,void **g)
{ return acquire_(p,id,o,g,false); }
static cog_result_t acquire_recovery(void *p,const char *id,cog_store_owner_t o,void **g)
{ return acquire_(p,id,o,g,true); }
static cog_result_t release_(void *p,void *g)
{
    backend_state_t *state=p;++state->release_calls;
    if(state->release_result==COG_ERROR_INVALID_STATE)return COG_ERROR_INVALID_STATE;
    cog_result_t rc=cog_store_owner_release(g);CHECK(rc==COG_OK);
    return state->release_result;
}
static const cog_store_vtable_t backend={.name="actual-file-proxy",.save=save,.load=load,.append=append,
    .delete_=delete_,.list=list,.exists=exists,.load_ex=load_ex,.checkpoint_save=checkpoint_save,
    .checkpoint_load=checkpoint_load,.owner_read=owner_read,.owner_compare_exchange=owner_cas,
    .owner_acquire=acquire_normal,.owner_release=release_,.owner_acquire_recovery=acquire_recovery};
int installed_fenced_store_proof(const char *directory)
{
    backend_state_t state={0};
    CHECK(cog_store_new(&state.file,&(cog_store_config_t){.base_dir=directory,.require_owner=true})==COG_OK);
    cog_store_t *store=NULL;CHECK(cog_store_new(&store,&(cog_store_config_t){.backend=&backend,.backend_context=&state,.require_owner=true})==COG_OK);
    cog_store_owner_t owner={1U,91U};cog_store_checkpoint_t cp={0};
    CHECK(cog_store_owner_compare_exchange(store,"guard",(cog_store_owner_t){0},owner,cp)==COG_OK);
    cog_store_owner_guard_t *g=NULL;
    reject_calloc=true;
    CHECK(cog_store_owner_acquire(store,"guard",owner,&g)==COG_ERROR_NOMEM && g==NULL);
    CHECK(cog_store_owner_acquire_recovery(store,"guard",owner,&g)==COG_ERROR_NOMEM && g==NULL);
    reject_calloc=false;CHECK(rejected_calloc_calls==2U);CHECK(state.normal_calls==0U && state.recovery_calls==0U && state.release_calls==0U);
    state.acquire_error=COG_ERROR_IO;
    CHECK(cog_store_owner_acquire(store,"guard",owner,&g)==COG_ERROR_IO && g==NULL);
    CHECK(cog_store_owner_acquire_recovery(store,"guard",owner,&g)==COG_ERROR_IO && g==NULL);
    CHECK(state.normal_calls==1U && state.recovery_calls==1U && state.release_calls==0U);
    state.acquire_error=COG_OK;state.reject_after_acquire=true;state.release_result=COG_ERROR_INVALID_STATE;
    CHECK(cog_store_owner_acquire(store,"guard",owner,&g)==COG_OK && g!=NULL);
    reject_calloc=false;state.reject_after_acquire=false;
    CHECK(cog_store_owner_guard_borrow(g,"guard")==COG_OK);
    CHECK(cog_store_owner_release(g)==COG_ERROR_INVALID_STATE && state.release_calls==0U);
    CHECK(cog_store_owner_guard_return(g)==COG_OK);
    state.release_result=COG_ERROR_INVALID_STATE;
    CHECK(cog_store_owner_release(g)==COG_ERROR_INVALID_STATE && state.release_calls==1U);
    CHECK(cog_store_owner_guard_borrow(g,"guard")==COG_OK && cog_store_owner_guard_return(g)==COG_OK);
    state.release_result=COG_ERROR_IO;
    CHECK(cog_store_owner_release(g)==COG_ERROR_IO && state.release_calls==2U);g=NULL;
    /* Actual lock and public wrapper are both gone, despite the reported IO. */
    CHECK(cog_store_owner_acquire(store,"guard",owner,&g)==COG_OK);
    state.release_result=COG_ERROR_AGAIN;
    CHECK(cog_store_owner_release(g)==COG_ERROR_AGAIN && state.release_calls==3U);g=NULL;
    CHECK(cog_store_owner_acquire(store,"guard",owner,&g)==COG_OK);
    state.release_result=COG_OK;CHECK(cog_store_owner_release(g)==COG_OK);g=NULL;
    /* Recovery success also publishes without an allocation after callback. */
    owner=(cog_store_owner_t){1U,92U};cp=(cog_store_checkpoint_t){0};
    CHECK(cog_store_owner_compare_exchange(store,"recovery",(cog_store_owner_t){0},owner,cp)==COG_OK);
    CHECK(cog_store_owner_acquire(store,"recovery",owner,&g)==COG_OK);
    cog_session_t *session=NULL;CHECK(cog_session_new(&session,&(cog_session_config_t){
        .id="recovery",.max_nodes=16U,.max_payload_bytes=65536U})==COG_OK);
    const cog_message_tool_call_t call={.id="call",.name="effect",.arguments="{}",.arguments_len=2U};
    const cog_message_t message={.role=COG_MESSAGE_ROLE_ASSISTANT,.tool_calls=&call,.tool_call_count=1U};
    cog_session_node_t *node=NULL;CHECK(cog_session_append(session,&message,&node)==COG_OK);
    CHECK(cog_session_tool_transition(session,node,"call",COG_TOOL_EXECUTION_PROPOSED,COG_TOOL_EXECUTION_STARTED)==COG_OK);
    CHECK(cog_store_save(store,session)==COG_OK);
    cog_store_owner_t recorded;CHECK(cog_store_owner_read(store,"recovery",&recorded,&cp)==COG_OK);
    CHECK(cog_store_owner_release(g)==COG_OK);g=NULL;
    cog_store_owner_t next={2U,93U};CHECK(cog_store_owner_compare_exchange(store,"recovery",owner,next,cp)==COG_OK);
    state.reject_after_acquire=true;state.release_result=COG_ERROR_INVALID_STATE;
    CHECK(cog_store_owner_acquire_recovery(store,"recovery",next,&g)==COG_OK && g!=NULL);
    reject_calloc=false;state.reject_after_acquire=false;
    CHECK(cog_store_owner_guard_borrow(g,"recovery")==COG_ERROR_INVALID_STATE);
    state.release_result=COG_ERROR_IO;CHECK(cog_store_owner_release(g)==COG_ERROR_IO);g=NULL;
    cog_session_free(session);
    size_t releases=state.release_calls;cog_store_free(store);cog_store_free(state.file);
    CHECK(state.release_calls==releases);puts("custom guard ownership/reservation: PASS");return 0;
}

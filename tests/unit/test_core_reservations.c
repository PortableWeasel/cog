/** @file test_core_reservations.c @brief Constructor fault unwind and no hidden slice allocation. */
#include <cog/core.h>
#include <cog/agent.h>
#include "../../libraries/cog-core/src/core_internal.h"
#include "../../libraries/cog-ipc/private/core_ipc.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

void *__real_malloc(size_t n); void *__wrap_malloc(size_t n);
void *__real_calloc(size_t n,size_t s); void *__wrap_calloc(size_t n,size_t s);
void *__real_realloc(void *p,size_t n); void *__wrap_realloc(void *p,size_t n);
static size_t calls,fail_at;
static bool armed,deny;
static bool refused(void)
{
    assert(!deny);
    return armed && ++calls==fail_at;
}
void *__wrap_malloc(size_t n) { return refused()?NULL:__real_malloc(n); }
void *__wrap_calloc(size_t n,size_t s) { return refused()?NULL:__real_calloc(n,s); }
void *__wrap_realloc(void *p,size_t n) { return refused()?NULL:__real_realloc(p,n); }
static cog_handler_result_t handle(cog_core_t *core,const cog_event_t *event)
{
    (void)event;cog_core_stop(core);
    return (cog_handler_result_t){COG_TRANSITION_SELF,COG_OK};
}
static const cog_state_t transitions[]={COG_TRANSITION_SELF,COG_TRANSITION_END};
static const cog_dispatch_entry_t table[]={
    COG_DISPATCH_ENTRY(0,COG_EVENT_USER_INPUT,handle,transitions),
    COG_DISPATCH_ENTRY(0,COG_EVENT_SUBAGENT_SPAWN,handle,transitions)
};
int main(void)
{
    cog_session_t *session;assert(cog_session_new(&session,NULL)==COG_OK);
    cog_core_config_t cfg={0};cfg.session=session;cfg.dispatch_table=table;cfg.dispatch_table_size=2U;
    cfg.require_peer_credentials=true;cfg.ipc_peer_capacity=3U;
    cog_core_t *core;armed=true;calls=0U;fail_at=0U;
    assert(cog_core_new(&core,&cfg)==COG_OK);armed=false;
    size_t allocations=calls;assert(allocations>=10U);cog_core_free(core);
    for(size_t i=1U;i<=allocations;++i) {
        cog_ipc_endpoint_t *ep;assert(cog_ipc_endpoint_new(&ep)==COG_OK);
        cfg.ipc_endpoint=ep;calls=0U;fail_at=i;armed=true;core=NULL;
        assert(cog_core_new(&core,&cfg)==COG_ERROR_NOMEM && core==NULL);armed=false;
        /* Failed core construction never consumes the caller's endpoint. */
        assert(cog_ipc_internal_role(ep)==0);assert(cog_ipc_endpoint_free(ep)==COG_OK);
    }
    cfg.ipc_endpoint=NULL;assert(cog_core_new(&core,&cfg)==COG_OK);
    cog_agent_config_t child_cfg={0};child_cfg.task="reserved child";
    cog_agent_id_t id;assert(cog_agent_spawn(core,&child_cfg,&id)==COG_OK);
    deny=true;
    cog_session_t *child;assert(cog_agent_session_borrow(core,id,&child)==COG_OK);
    assert(cog_agent_session_return(core,id,child)==COG_OK);
    assert(cog_core_drive_begin(core)==COG_OK);
    cog_core_step_info_t info;assert(cog_core_step(core,&info)==COG_OK && info.done);
    assert(cog_core_drive_end(core)==COG_OK);
    cog_event_t event={COG_EVENT_USER_INPUT,COG_EVENT_USER_INPUT,NULL,0U,0U};
    assert(cog_core_enqueue(core,&event)==COG_OK && cog_core_run(core)==COG_OK);
    deny=false;cog_core_free(core);cog_session_free(session);
    puts("core constructor faults and reserved non-accept slice work PASS");return 0;
}

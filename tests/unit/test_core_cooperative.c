/** @file test_core_cooperative.c @brief Shared manual/run engine and exact borrow lifetime. */
#define _POSIX_C_SOURCE 200809L
#include <cog/core.h>
#include <cog/agent.h>
#include "../../libraries/cog-core/src/core_internal.h"
#include "../../libraries/cog-core/src/session_internal.h"
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

int __real_clock_gettime(clockid_t id, struct timespec *time);
int __wrap_clock_gettime(clockid_t id, struct timespec *time);
static unsigned clock_mode;
int __wrap_clock_gettime(clockid_t id, struct timespec *time)
{
    if (id == CLOCK_MONOTONIC && clock_mode != 0U) {
        if (clock_mode == 1U) { errno = EIO; return -1; }
        time->tv_sec = clock_mode == 2U ? -1 : clock_mode == 4U ? (time_t)LLONG_MAX :
                       clock_mode == 6U ? 99 : 100;
        time->tv_nsec = clock_mode == 3U ? 1000000000L : 0L;
        return 0;
    }
    return __real_clock_gettime(id, time);
}
typedef struct fixture {
    cog_core_t *core;
    cog_session_t *session;
    size_t handled, services, idle, stop_at, fail_at, last_service, maximum_gap;
    bool loop, reentry, due, stop_service;
} fixture_t;
static void invalid_reentry(cog_core_t *core)
{
    cog_core_step_info_t info, before;
    memset(&info, 0xa5, sizeof(info)); memcpy(&before,&info,sizeof(info));
    assert(cog_core_drive_begin(core) == COG_ERROR_INVALID_STATE);
    assert(cog_core_step(core,&info) == COG_ERROR_INVALID_STATE);
    assert(memcmp(&info,&before,sizeof(info)) == 0);
    assert(cog_core_drive_end(core) == COG_ERROR_INVALID_STATE);
    assert(cog_core_run(core) == COG_ERROR_INVALID_STATE);
}
static cog_handler_result_t handle(cog_core_t *core, const cog_event_t *event)
{
    fixture_t *f = cog_core_user(core);
    if (f->reentry) invalid_reentry(core);
    ++f->handled;
    size_t gap = f->handled-f->last_service;
    if (gap > f->maximum_gap) f->maximum_gap = gap;
    if (f->loop) assert(cog_core_enqueue(core,event) == COG_OK);
    if (f->handled == f->stop_at) cog_core_stop(core);
    return (cog_handler_result_t){COG_TRANSITION_SELF,
        f->handled == f->fail_at ? COG_ERROR_PROTOCOL : COG_OK};
}
static const cog_state_t transitions[] = {COG_TRANSITION_SELF,COG_TRANSITION_END};
static const cog_dispatch_entry_t table[] = {
    COG_DISPATCH_ENTRY(0,COG_EVENT_USER_INPUT,handle,transitions),
    COG_DISPATCH_ENTRY(0,COG_EVENT_SUBAGENT_SPAWN,handle,transitions),
    COG_DISPATCH_ENTRY(0,COG_EVENT_SUBAGENT_CANCEL,handle,transitions)
};
static cog_result_t service(void *arg, uint64_t now, uint64_t *deadline)
{
    fixture_t *f = arg;
    if (f->reentry) invalid_reentry(f->core);
    ++f->services; f->last_service = f->handled;
    *deadline = f->due ? now : UINT64_MAX;
    if (f->stop_service) cog_core_stop(f->core);
    return COG_OK;
}
static cog_result_t idle(void *arg, uint32_t budget)
{
    fixture_t *f = arg; ++f->idle;
    assert(budget <= 7U);
    if (f->reentry) invalid_reentry(f->core);
    cog_core_stop(f->core);
    return COG_OK;
}
static void make(fixture_t *f, size_t limit)
{
    cog_core_config_t cfg = {0};
    assert(cog_session_new(&f->session,NULL) == COG_OK);
    cfg.session=f->session; cfg.dispatch_table=table; cfg.dispatch_table_size=3U;
    cfg.user=f; cfg.service_fn=service; cfg.service_arg=f; cfg.idle_fn=idle; cfg.idle_arg=f;
    cfg.step_event_limit=limit; cfg.service_interval=3U; cfg.max_idle_wait_ms=7U;
    cfg.max_agents=4U; cfg.max_agent_message_bytes=256U;
    assert(cog_core_new(&f->core,&cfg) == COG_OK);
}
static void drop(fixture_t *f) { cog_core_free(f->core); cog_session_free(f->session); }
static void enqueue(fixture_t *f)
{
    cog_event_t e = {COG_EVENT_USER_INPUT,COG_EVENT_USER_INPUT,NULL,0U,0U};
    assert(cog_core_enqueue(f->core,&e) == COG_OK);
}
static void drive(void)
{
    for (unsigned manual=0U;manual<2U;++manual) {
        fixture_t f = {0}; make(&f,2U); f.loop=true; f.reentry=true; f.stop_at=11U;
        enqueue(&f); enqueue(&f);
        if (!manual) assert(cog_core_run(f.core)==COG_OK);
        else {
            assert(cog_core_drive_begin(f.core)==COG_OK);
            assert(cog_core_drive_end(f.core)==COG_ERROR_INVALID_STATE);
            cog_core_step_info_t info;
            do {
                assert(cog_core_step(f.core,&info)==COG_OK);
                assert(info.dispatched<=2U && f.idle==0U);
                if (!info.done) assert(info.max_wait_ms==0U && info.next_deadline_ms<=f.core->last_now);
            } while (!info.done);
            assert(cog_core_step(f.core,&info)==COG_ERROR_INVALID_STATE);
            assert(cog_core_drive_end(f.core)==COG_OK);
            assert(cog_core_drive_end(f.core)==COG_ERROR_INVALID_STATE);
        }
        assert(f.handled==11U && f.maximum_gap<=3U);
        assert(!cog_core_is_running(f.core));
        f.loop=false; f.stop_at=0U; f.fail_at=f.handled+1U;
        assert(cog_core_drive_begin(f.core)==COG_OK);
        cog_core_step_info_t info;
        assert(cog_core_step(f.core,&info)==COG_ERROR_PROTOCOL && info.done);
        size_t retained=cog_event_queue_count(f.core->event_queue);
        assert(retained!=0U);
        assert(cog_core_drive_end(f.core)==COG_ERROR_PROTOCOL);
        assert(cog_event_queue_count(f.core->event_queue)==retained);
        drop(&f);
    }
    fixture_t f={0}; make(&f,0U); f.reentry=true;
    assert(f.core->step_event_limit==16U);
    assert(cog_core_drive_begin(f.core)==COG_OK);
    cog_core_step_info_t info;
    assert(cog_core_step(f.core,&info)==COG_OK && !info.done && info.dispatched==0U);
    assert(info.max_wait_ms==7U && info.next_deadline_ms==UINT64_MAX && f.idle==0U);
    f.due=true;
    assert(cog_core_step(f.core,&info)==COG_OK && info.max_wait_ms==0U);
    cog_core_stop(f.core);
    assert(cog_core_step(f.core,&info)==COG_OK && info.done && info.dispatched==0U);
    assert(cog_core_drive_end(f.core)==COG_OK);
    assert(cog_core_run(f.core)==COG_OK && f.idle==1U);
    drop(&f);
}
static void clock_failures(void)
{
    for (unsigned mode=1U;mode<=4U;++mode) {
        fixture_t f={0};make(&f,1U);enqueue(&f);clock_mode=mode;
        assert(cog_core_drive_begin(f.core)==COG_OK);
        cog_core_step_info_t info;
        assert(cog_core_step(f.core,&info)==COG_ERROR_IO && info.done && info.dispatched==0U);
        assert(cog_core_drive_end(f.core)==COG_ERROR_IO);
        assert(f.handled==0U && cog_event_queue_count(f.core->event_queue)==1U);
        clock_mode=0U;drop(&f);
    }
    fixture_t f={0};make(&f,1U); clock_mode=5U;
    assert(cog_core_drive_begin(f.core)==COG_OK);
    cog_core_step_info_t info; assert(cog_core_step(f.core,&info)==COG_OK);
    cog_core_stop(f.core); assert(cog_core_step(f.core,&info)==COG_OK && info.done);
    assert(cog_core_drive_end(f.core)==COG_OK);
    clock_mode=6U; assert(cog_core_drive_begin(f.core)==COG_OK);
    assert(cog_core_step(f.core,&info)==COG_ERROR_IO && info.done);
    assert(cog_core_drive_end(f.core)==COG_ERROR_IO);
    clock_mode=0U;drop(&f);
}
static void borrow(void)
{
    fixture_t f={0};make(&f,1U);
    cog_agent_config_t cfg={0}; cfg.task="actual child";cfg.parent_session_id=cog_session_id(f.session);
    cog_agent_id_t id;assert(cog_agent_spawn(f.core,&cfg,&id)==COG_OK);
    cog_session_t *child=NULL,*unchanged=f.session;
    assert(cog_agent_session_borrow(NULL,id,&unchanged)==COG_ERROR_NULL_PTR && unchanged==f.session);
    assert(cog_agent_session_borrow(f.core,0U,&unchanged)==COG_ERROR_NOT_FOUND && unchanged==f.session);
    assert(cog_agent_session_borrow(f.core,id,&child)==COG_OK);
    const cog_agent_entry_t *entry=cog_agent_internal_get(f.core,id);
    assert(child==entry->session && strcmp(cog_session_id(child),entry->session_id)==0);
    assert(strcmp(cog_session_parent_id(child),cfg.parent_session_id)==0);
    assert(entry->intent_leases==1U && entry->execution_borrowed);
    assert(cog_agent_session_borrow(f.core,id,&unchanged)==COG_ERROR_INVALID_STATE && unchanged==f.session);
    assert(cog_agent_session_return(f.core,id,f.session)==COG_ERROR_INVALID_STATE);
    cog_core_config_t child_cfg={0}; child_cfg.session=child;child_cfg.dispatch_table=table;child_cfg.dispatch_table_size=3U;
    cog_core_t *child_core;
    assert(cog_core_new(&child_core,&child_cfg)==COG_OK);
    assert(cog_agent_session_return(f.core,id,child)==COG_ERROR_INVALID_STATE);
    cog_core_free(child_core);
    assert(cog_agent_session_return(f.core,id,child)==COG_OK);
    assert(cog_agent_session_return(f.core,id,child)==COG_ERROR_INVALID_STATE);
    assert(cog_agent_session_borrow(f.core,id,&child)==COG_OK);
    assert(cog_core_drive_begin(f.core)==COG_OK);
    cog_core_step_info_t info;assert(cog_core_step(f.core,&info)==COG_OK);
    assert(entry->intent_leases==0U);
    cog_core_stop(f.core);assert(cog_core_step(f.core,&info)==COG_OK && info.done);
    assert(cog_core_drive_end(f.core)==COG_OK);
    assert(cog_agent_internal_start(f.core,id)==COG_OK);
    assert(cog_agent_ack_update(f.core,id,entry->execution_epoch,entry->sequence)==COG_OK);
    assert(cog_agent_internal_complete(f.core,id,"done",4U)==COG_OK);
    assert(cog_agent_ack_update(f.core,id,entry->execution_epoch,entry->sequence)==COG_OK);
    assert(cog_agent_release(f.core,id)==COG_ERROR_INVALID_STATE);
    assert(cog_agent_session_return(f.core,id,child)==COG_OK);
    assert(cog_agent_session_borrow(f.core,id,&unchanged)==COG_ERROR_INVALID_STATE);
    assert(cog_agent_release(f.core,id)==COG_OK);
    cfg.owner_peer=99U;cfg.execution_epoch=1U;
    assert(cog_agent_spawn(f.core,&cfg,&id)==COG_OK);
    assert(cog_agent_session_borrow(f.core,id,&unchanged)==COG_ERROR_INVALID_STATE);
    cfg.owner_peer=0U;assert(cog_agent_spawn(f.core,&cfg,&id)==COG_OK);
    assert(cog_agent_cancel(f.core,id)==COG_OK);
    assert(cog_agent_session_borrow(f.core,id,&unchanged)==COG_ERROR_ABORTED && unchanged==f.session);
    /* Free discards still-queued spawn/cancel leases without handler execution. */
    size_t handled=f.handled;drop(&f);assert(f.handled==handled);
}
static _Atomic(cog_core_t *) signal_target;
static volatile sig_atomic_t signal_seen;
static void interrupt_handler(int number)
{
    (void)number;
    cog_core_stop(atomic_load_explicit(&signal_target,memory_order_relaxed));
    signal_seen=1;
}
static void budgets_and_signal(void)
{
    fixture_t f={0};make(&f,1024U);f.loop=true;f.stop_at=1025U;enqueue(&f);
    assert(cog_core_drive_begin(f.core)==COG_OK);
    cog_core_step_info_t info;
    assert(cog_core_step(f.core,&info)==COG_OK && info.dispatched==1024U && !info.done);
    assert(info.max_wait_ms==0U);
    assert(cog_core_step(f.core,&info)==COG_OK && info.dispatched==1U && info.done);
    assert(cog_core_drive_end(f.core)==COG_OK);drop(&f);
    memset(&f,0,sizeof(f));make(&f,1U);
    cog_core_config_t cfg={0};cfg.session=f.session;cfg.dispatch_table=table;
    cfg.dispatch_table_size=3U;cfg.step_event_limit=1025U;
    cog_core_t *invalid=f.core;
    assert(cog_core_new(&invalid,&cfg)==COG_ERROR_INVALID_ARG && invalid==NULL);
    assert(atomic_is_lock_free(&signal_target) && atomic_is_lock_free(&f.core->running) &&
           atomic_is_lock_free(&f.core->stop_requested));
    atomic_store_explicit(&signal_target,f.core,memory_order_relaxed);
    struct sigaction action={0},previous;action.sa_handler=interrupt_handler;
    assert(sigemptyset(&action.sa_mask)==0 && sigaction(SIGUSR1,&action,&previous)==0);
    assert(cog_core_drive_begin(f.core)==COG_OK);enqueue(&f);
    assert(raise(SIGUSR1)==0 && signal_seen==1 && !cog_core_is_running(f.core));
    assert(cog_core_step(f.core,&info)==COG_OK && info.done && info.dispatched==0U);
    assert(cog_core_drive_end(f.core)==COG_OK);
    assert(sigaction(SIGUSR1,&previous,NULL)==0);
    atomic_store_explicit(&signal_target,NULL,memory_order_relaxed);drop(&f);
}
int main(void)
{
    drive();clock_failures();borrow();budgets_and_signal();
    puts("cooperative driver and exact child borrow PASS");return 0;
}

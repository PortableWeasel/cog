/**
 * @file test_07_aux_crash.c
 * @brief Scenario 07: Aux core crash -> detection -> recovery.
 *
 * Drives the real cog-orchestrator against the repo's aux_core fixture
 * process (tests/integration/fixtures/aux_core.c) over a local Unix
 * domain socket, and proves the scenario-07 contract from
 * docs/scenarios/07-aux-crash.md:
 *
 *   1. crash detection: a fixture that heartbeats once and then exits
 *      with a crash is detected as COG_HEALTH_DEAD via the real health
 *      monitor and exact native reap;
 *   2. sessions retained: the crashed core's registered sessions are
 *      still reported by cog_orchestrator_sessions after the crash, so
 *      recovery can find the affected subagents;
 *   3. recovery primitives: a subagent that was running on the crashed
 *      core (spawned on the real core via cog_agent_spawn) is marked
 *      FAILED via an authenticated attempt update — the action the parent takes
 *      when the orchestrator reports the dead core's sessions — and a
 *      fresh replacement agent can be spawned and completed from
 *      scratch;
 *   4. auto-replace only after actual reap: with auto_replace enabled, a
 *      SIGKILLed core is not replaced while its process is still
 *      unreaped; the replacement appears only after the orchestrator's
 *      service reaps the dead child (waitpid WNOHANG), retained events
 *      are acknowledged and the slot can hold a fresh managed/peer identity.
 *
 * The fixture binary path is supplied at compile time via
 * COG_AUX_CORE_BIN (falling back to argv[1]).  Every spawned pid is
 * owned by this test and reaped by the orchestrator; no process is
 * killed by name and no unrelated process is signalled.
 *
 * These are preserved component assertions; O9 owns actual joined application
 * execution and crash-result propagation. No automatic task replay is implied.
 *
 * Ticket: 0d8ff50 (Stage 12.6c — Scenario tests 06-10).
 */
#define _POSIX_C_SOURCE 200809L

#include "cog/orchestrator.h"

#include "orchestrator_internal.h"

#include "cog_cli/cli.h"
#include "cog_cli/mock_provider.h"

#include "agent_internal.h"
#include "agent_test_helpers.h"

#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifndef COG_AUX_CORE_BIN
#define COG_AUX_CORE_BIN NULL
#endif

/* ------------------------------------------------------------------ */
/* Test harness                                                        */
/* ------------------------------------------------------------------ */

static unsigned g_failures = 0U;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            (void)fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__,         \
                          __LINE__, #cond);                             \
            ++g_failures;                                               \
        }                                                               \
    } while (0)

#define CHECK_EQ_INT(a, b)                                              \
    do {                                                                \
        long long _a = (long long)(a);                                  \
        long long _b = (long long)(b);                                  \
        if (_a != _b) {                                                 \
            (void)fprintf(stderr, "FAIL %s:%d: %s == %s (%lld != %lld)\n", \
                          __FILE__, __LINE__, #a, #b, _a, _b);          \
            ++g_failures;                                               \
        }                                                               \
    } while (0)


#define FX_CHECK(x) CHECK(x)
static uint64_t fx_now(void)
{
    struct timespec ts;FX_CHECK(clock_gettime(CLOCK_MONOTONIC,&ts)==0);
    return (uint64_t)ts.tv_sec*1000U+(uint64_t)ts.tv_nsec/1000000U;
}
static void fx_pause(void) { (void)nanosleep(&(struct timespec){0,1000000L},NULL); }
static bool fx_same(cog_worker_id_t a,cog_worker_id_t b)
{ return a.incarnation==b.incarnation&&a.number==b.number; }
static cog_result_t fx_service(cog_orchestrator_t *o)
{
    bool done=false;uint64_t deadline;
    cog_result_t rc=cog_orchestrator_service(o,fx_now(),&done,&deadline);
    FX_CHECK(rc==COG_OK||rc==COG_ERROR_AGAIN||rc==COG_ERROR_TIMEOUT);
    return rc;
}
/* Failed prerequisites drain every owned lifetime before stopping this test.
 * Never continue a stress iteration with an all-zero identity or PID. */
static void fx_abort(cog_orchestrator_t *o)
{
    if(o!=NULL) {
        bool done=false;uint64_t deadline,end=fx_now()+4000;
        (void)cog_orchestrator_request_shutdown(o,fx_now(),1);
        while(!done&&fx_now()<end) { (void)cog_orchestrator_service(o,fx_now(),&done,&deadline);fx_pause(); }
        if(done) cog_orchestrator_free(o);
        else (void)fprintf(stderr,"fixture fatal: native cleanup did not complete\n");
    }
    exit(EXIT_FAILURE);
}
static cog_core_info_t fx_info(cog_orchestrator_t *o,cog_worker_id_t id)
{
    cog_core_info_t info[16],found={0};size_t count=0;
    FX_CHECK(cog_orchestrator_list(o,info,16,&count)==COG_OK);
    for(size_t i=0;i<count&&i<16;++i) if(fx_same(info[i].worker_id,id)) return info[i];
    return found;
}
static bool fx_signal_worker(cog_orchestrator_t *o,cog_worker_id_t id,int sig)
{
    if(id.incarnation==0||id.number==0) return false;
    cog_core_info_t info=fx_info(o,id);
    if(!fx_same(info.worker_id,id)||info.pid<=0||info.reaped||info.ownership_lost) return false;
    return kill((pid_t)info.pid,sig)==0;
}
static bool fx_wait(cog_orchestrator_t *o,cog_worker_id_t id,bool reaped)
{
    uint64_t end=fx_now()+4000;
    do {
        (void)fx_service(o);cog_core_info_t info=fx_info(o,id);
        if(reaped?info.reaped:info.health==COG_HEALTH_HEALTHY) return true;
        fx_pause();
    } while(fx_now()<end);
    return false;
}
static void fx_ack(cog_orchestrator_t *o)
{
    unsigned char data[256];cog_orchestrator_event_t event;size_t required;
    for(int cls=COG_ORCHESTRATOR_CONTROL_EVENTS;cls<=COG_ORCHESTRATOR_MESSAGE_EVENTS;++cls) {
        cog_result_t rc;
        while((rc=cog_orchestrator_copy_event(o,(cog_orchestrator_event_class_t)cls,&event,data,sizeof(data),&required))==COG_OK)
            FX_CHECK(cog_orchestrator_ack_event(o,event.event_id)==COG_OK);
        FX_CHECK(rc==COG_ERROR_AGAIN);
    }
}
static cog_result_t fx_release(cog_orchestrator_t *o,cog_worker_id_t id)
{
    cog_result_t rc;uint64_t end=fx_now()+2000;
    do {
        fx_ack(o);rc=cog_orchestrator_release_worker(o,id);
        if(rc!=COG_ERROR_INVALID_STATE) return rc;
        (void)fx_service(o);fx_pause();
    } while(fx_now()<end);
    return rc;
}
static void fx_command(cog_orchestrator_t *o,cog_worker_id_t id,unsigned char mode,unsigned char active)
{
    unsigned char bytes[6]={'F','X','M','1',mode,active};
    cog_ipc_message_t message={.data=bytes,.capacity=sizeof(bytes),.len=sizeof(bytes)};
    FX_CHECK(cog_orchestrator_send(o,id,&message)==COG_OK);
    uint64_t end=fx_now()+2000;bool received=false;
    while(fx_now()<end&&!received) {
        unsigned char data[256];cog_orchestrator_event_t event;size_t required;
        (void)fx_service(o);
        cog_result_t rc=cog_orchestrator_copy_event(o,COG_ORCHESTRATOR_MESSAGE_EVENTS,&event,data,sizeof(data),&required);
        if(rc==COG_OK) {
            FX_CHECK(fx_same(event.worker_id,id)&&required==6&&memcmp(data,bytes,6)==0);
            FX_CHECK(cog_orchestrator_ack_event(o,event.event_id)==COG_OK);received=true;
        } else FX_CHECK(rc==COG_ERROR_AGAIN);
        fx_pause();
    }
    FX_CHECK(received);
}
static cog_orchestrator_t *fx_new(char *path,size_t capacity,bool replace)
{
    static unsigned serial=0;
    int n=snprintf(path,capacity,"orch-%ld-%u.sock",(long)getpid(),++serial);
    FX_CHECK(n>0&&(size_t)n<capacity);(void)unlink(path);
    cog_orchestrator_config_t cfg={0};cfg.socket_path=path;cfg.max_aux_cores=8;
    cfg.health_interval_ms=200;cfg.unhealthy_threshold=3;cfg.default_core_capacity=8;
    cfg.shutdown_timeout_ms=200;cfg.auto_replace=replace;
    cfg.max_frame_bytes=256;cfg.message_bytes=4096;cfg.message_events=8;
    cog_orchestrator_t *o=NULL;
    cog_result_t rc=cog_orchestrator_new(&o,&cfg);FX_CHECK(rc==COG_OK);
    if(rc!=COG_OK||o==NULL) fx_abort(o);
    return o;
}
static cog_worker_id_t fx_spawn(cog_orchestrator_t *o)
{
    cog_worker_id_t id={0};
    cog_result_t rc=cog_orchestrator_spawn(o,COG_AUX_CORE_BIN,&id);FX_CHECK(rc==COG_OK);
    if(rc!=COG_OK||id.incarnation==0||id.number==0) fx_abort(o);
    bool ready=fx_wait(o,id,false);FX_CHECK(ready);if(!ready) fx_abort(o);
    cog_core_info_t info=fx_info(o,id);FX_CHECK(info.peer_id!=0&&info.pid>0&&info.capacity==8);
    return id;
}
static void fx_close_at(cog_orchestrator_t *o,char *path,cog_result_t expected,const char *test)
{
    FX_CHECK(cog_orchestrator_request_shutdown(o,fx_now(),300)==COG_OK);
    bool done=false;uint64_t deadline,end=fx_now()+4000;cog_result_t rc=COG_ERROR_AGAIN;
    while(!done&&fx_now()<end) { rc=cog_orchestrator_service(o,fx_now(),&done,&deadline);fx_pause(); }
    FX_CHECK(done);
    if(rc!=expected) (void)fprintf(stderr,"%s shutdown: actual %d expected %d done %d\n",test,(int)rc,(int)expected,(int)done);
    FX_CHECK(rc==expected);
    cog_core_info_t infos[16];size_t count=0;FX_CHECK(cog_orchestrator_list(o,infos,16,&count)==COG_OK);
    fx_ack(o);
    for(size_t i=0;i<count&&i<16;++i) {
        FX_CHECK(infos[i].reaped&&!infos[i].ownership_lost);
        /* Native done is not logical endpoint/event retirement. Continue the
           actual owner service and ACK copied events before bounded release. */
        cog_result_t released=fx_release(o,infos[i].worker_id);
        if(released!=COG_OK) (void)fprintf(stderr,"%s release worker %llu: actual %d reaped %d lost %d\n",test,(unsigned long long)infos[i].worker_id.number,(int)released,(int)infos[i].reaped,(int)infos[i].ownership_lost);
        FX_CHECK(released==COG_OK);
    }
    if(done) cog_orchestrator_free(o);
    (void)unlink(path);
}
#define fx_close(o,p,e) fx_close_at(o,p,e,__func__)
static cog_worker_id_t fx_replacement(cog_orchestrator_t *o,cog_worker_id_t old)
{
    uint64_t end=fx_now()+4000;
    do {
        fx_ack(o);(void)fx_service(o);cog_core_info_t infos[16];size_t count=0;
        FX_CHECK(cog_orchestrator_list(o,infos,16,&count)==COG_OK);
        if(count==1&&!fx_same(infos[0].worker_id,old)&&infos[0].health==COG_HEALTH_HEALTHY) return infos[0].worker_id;
        fx_pause();
    } while(fx_now()<end);
    FX_CHECK(false);fx_abort(o);return (cog_worker_id_t){0};
}

static void scenario_crash_detection_sessions(void)
{
    char path[96];cog_orchestrator_t *o=fx_new(path,sizeof(path),false);
    cog_worker_id_t id=fx_spawn(o);cog_peer_id_t peer=fx_info(o,id).peer_id;
    CHECK(cog_orchestrator_register_session(o,peer,"crash-sess")==COG_OK);
    fx_command(o,id,1,0);CHECK(fx_wait(o,id,true));CHECK(cog_orchestrator_health(o,id)==COG_HEALTH_DEAD);
    const char *sessions[8];size_t count=0;
    CHECK(cog_orchestrator_sessions(o,peer,sessions,8,&count)==COG_OK);
    CHECK_EQ_INT(count,1);CHECK(count==1&&strcmp(sessions[0],"crash-sess")==0);
    CHECK(cog_orchestrator_release_worker(o,id)==COG_ERROR_INVALID_STATE);
    CHECK(cog_orchestrator_unregister_session(o,peer,"crash-sess")==COG_OK);
    fx_close(o,path,COG_OK);
}
static void scenario_auto_replace_after_reap(void)
{
    char path[96];cog_orchestrator_t *o=fx_new(path,sizeof(path),true);
    cog_worker_id_t old=fx_spawn(o);cog_core_info_t before=fx_info(o,old);
    bool signalled=fx_signal_worker(o,old,SIGKILL);CHECK(signalled);if(!signalled) fx_abort(o);
    /* No service means no reap, replacement or mutation of native identity. */
    cog_core_info_t after=fx_info(o,old);CHECK(!after.reaped&&after.pid==before.pid&&after.peer_id==before.peer_id);
    CHECK(fx_wait(o,old,true));CHECK(fx_info(o,old).reaped);
    /* Consumer ownership, not death alone, controls reservation retirement. */
    CHECK(cog_orchestrator_release_worker(o,old)==COG_ERROR_INVALID_STATE);fx_ack(o);
    cog_worker_id_t replacement=fx_replacement(o,old);
    CHECK(!fx_same(old,replacement)&&fx_info(o,replacement).peer_id!=before.peer_id);
    fx_close(o,path,COG_OK);
}
static void scenario_recovery_restart(void)
{
    cog_cli_options_t opts;
    cog_cli_app_t *app = NULL;
    cog_core_t *core;
    cog_agent_config_t cfg;
    cog_agent_id_t affected = 0U;
    cog_agent_id_t replacement = 0U;
    cog_agent_status_t st;
    char result[32];
    size_t result_len = 0U;

    (void)memset(&opts, 0, sizeof(opts));
    opts.model_id = "mock-model";
    opts.endpoint_name = "mock";
    opts.interactive = false;
    opts.backend = COG_CLI_BACKEND_MOCK;
    opts.explicit_options |= COG_CLI_OPT_BACKEND | COG_CLI_OPT_MODEL | COG_CLI_OPT_ENDPOINT;
    CHECK(cog_cli_app_new(&app, &opts) == COG_OK);
    if (app == NULL) {
        return;
    }
    core = cog_cli_app_core(app);
    CHECK(core != NULL);

    /* A subagent that was running on the crashed core. */
    (void)memset(&cfg, 0, sizeof(cfg));
    cfg.task = "was on crashed core";
    cfg.parent_session_id = cog_session_id(cog_cli_app_session(app));
    cfg.force_local = true;
    CHECK(cog_agent_spawn(core, &cfg, &affected) == COG_OK);
    CHECK(scenario_agent_step(core, affected, COG_AGENT_STATUS_RUNNING, COG_OK, "", 0U) == COG_OK);
    CHECK(cog_agent_status(core, affected, &st) == COG_OK);
    CHECK_EQ_INT((long long)st, (long long)COG_AGENT_STATUS_RUNNING);

    /* Recovery action: the parent marks the subagent that was running
     * on the crashed core as FAILED (the orchestrator reports the dead
     * peer's sessions via cog_orchestrator_sessions; the parent applies
     * the fail transition). */
    CHECK(scenario_agent_step(core, affected, COG_AGENT_STATUS_FAILED, COG_ERROR_IO, "", 0U) == COG_OK);
    CHECK(cog_agent_status(core, affected, &st) == COG_OK);
    CHECK_EQ_INT((long long)st, (long long)COG_AGENT_STATUS_FAILED);
    /* A failed terminal result is readable, including an empty result. */
    CHECK(cog_agent_result(core, affected, result, sizeof(result),
                           &result_len) == COG_OK);

    /* Restart from scratch: a fresh replacement agent runs to completion
     * and delivers its result to the parent. */
    (void)memset(&cfg, 0, sizeof(cfg));
    cfg.task = "replacement task";
    cfg.parent_session_id = cog_session_id(cog_cli_app_session(app));
    cfg.force_local = true;
    CHECK(cog_agent_spawn(core, &cfg, &replacement) == COG_OK);
    CHECK(replacement != affected);
    CHECK(scenario_agent_step(core, replacement, COG_AGENT_STATUS_RUNNING, COG_OK, "", 0U) == COG_OK);
    CHECK(scenario_agent_step(core, replacement, COG_AGENT_STATUS_COMPLETED, COG_OK, "recovered", 9U)
          == COG_OK);
    CHECK(cog_agent_status(core, replacement, &st) == COG_OK);
    CHECK_EQ_INT((long long)st, (long long)COG_AGENT_STATUS_COMPLETED);
    CHECK(cog_agent_result(core, replacement, result, sizeof(result),
                           &result_len) == COG_OK);
    CHECK_EQ_INT((long long)result_len, 9LL);
    CHECK(memcmp(result, "recovered", 9U) == 0);

    cog_cli_app_free(app);
}

int main(int argc, char **argv)
{
    const char *aux_bin = COG_AUX_CORE_BIN;

    if (aux_bin == NULL && argc > 1) {
        aux_bin = argv[1];
    }
    if (aux_bin == NULL) {
        (void)fprintf(stderr,
                      "test_07_aux_crash: COG_AUX_CORE_BIN not defined "
                      "and no argv[1] fixture path given\n");
        return 2;
    }

    (void)printf("Scenario 07: aux core crash -> detection -> recovery\n");
    scenario_crash_detection_sessions();
    scenario_recovery_restart();
    scenario_auto_replace_after_reap();

    if (g_failures != 0U) {
        (void)fprintf(stderr, "test_07_aux_crash: %u failure(s)\n",
                      g_failures);
        return 1;
    }
    (void)printf("test_07_aux_crash: all checks passed\n");
    return 0;
}

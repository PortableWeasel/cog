/**
 * @file test_10_fan_out_fan_in.c
 * @brief Scenario 10: Fan-out/fan-in across aux cores.
 *
 * Drives the real cog-orchestrator (against the repo's aux_core fixture
 * over a local Unix domain socket) and the real cog-core subagent
 * registry, and proves the scenario-10 contract from
 * docs/scenarios/10-fan-out-fan-in.md:
 *
 *   1. parallel placement across aux cores: cog_orchestrator_place
 *      distributes new subagents across healthy aux cores, preferring
 *      the least-loaded core as reported by actual binary worker heartbeats;
 *   2. fan-out grouping: cog_agent_spawn with a shared group_id creates
 *      a fan-out group of subagents, each with its own child session;
 *   3. ordered fan-in: each subagent runs to completion and
 *      cog_agent_result returns the collected results in spawn order;
 *   4. broadcast cancel: cog_ipc_broadcast reaches every connected peer,
 *      and cog_agent_cancel / cog_agent_is_cancelled mark the fan-out
 *      agents cancelled so late results are rejected (the
 *      cog_agent_is_cancelled gap fix).
 *
 * The fixture binary path is supplied at compile time via
 * COG_AUX_CORE_BIN (falling back to argv[1]).  Every spawned pid is
 * owned by this test and reaped by the orchestrator; all sockets are
 * local temporary paths.
 *
 * These are preserved component assertions; O9 owns actual joined application
 * fan-out execution and cancellation.
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

#include <pthread.h>
#include <stdatomic.h>
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
/* Assertion helpers.                                                  */
/* ------------------------------------------------------------------ */

static int failures = 0;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            (void)fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__,         \
                          __LINE__, #cond);                             \
            ++failures;                                                 \
        }                                                               \
    } while (0)

#define CHECK_EQ_INT(a, b)                                              \
    do {                                                                \
        long long _a = (long long)(a);                                  \
        long long _b = (long long)(b);                                  \
        if (_a != _b) {                                                 \
            (void)fprintf(stderr, "FAIL %s:%d: %s == %s (%lld != %lld)\n", \
                          __FILE__, __LINE__, #a, #b, _a, _b);          \
            ++failures;                                                 \
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
static cog_core_info_t fx_info(cog_orchestrator_t *o,cog_worker_id_t id)
{
    cog_core_info_t info[16],found={0};size_t count=0;
    FX_CHECK(cog_orchestrator_list(o,info,16,&count)==COG_OK);
    for(size_t i=0;i<count&&i<16;++i) if(fx_same(info[i].worker_id,id)) return info[i];
    return found;
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
    cog_orchestrator_t *o=NULL;FX_CHECK(cog_orchestrator_new(&o,&cfg)==COG_OK);return o;
}
static cog_worker_id_t fx_spawn(cog_orchestrator_t *o)
{
    cog_worker_id_t id={0};
    FX_CHECK(cog_orchestrator_spawn(o,COG_AUX_CORE_BIN,&id)==COG_OK);
    FX_CHECK(id.incarnation!=0&&id.number!=0);FX_CHECK(fx_wait(o,id,false));
    cog_core_info_t info=fx_info(o,id);FX_CHECK(info.peer_id!=0&&info.pid>0&&info.capacity==8);
    return id;
}
static void fx_close(cog_orchestrator_t *o,char *path,cog_result_t expected)
{
    FX_CHECK(cog_orchestrator_request_shutdown(o,fx_now(),300)==COG_OK);
    bool done=false;uint64_t deadline,end=fx_now()+4000;cog_result_t rc=COG_ERROR_AGAIN;
    while(!done&&fx_now()<end) { rc=cog_orchestrator_service(o,fx_now(),&done,&deadline);fx_pause(); }
    FX_CHECK(done);FX_CHECK(rc==expected);
    cog_core_info_t infos[16];size_t count=0;FX_CHECK(cog_orchestrator_list(o,infos,16,&count)==COG_OK);
    fx_ack(o);
    for(size_t i=0;i<count&&i<16;++i) {
        FX_CHECK(infos[i].reaped&&!infos[i].ownership_lost);
        FX_CHECK(cog_orchestrator_release_worker(o,infos[i].worker_id)==COG_OK);
    }
    if(done) cog_orchestrator_free(o);
    (void)unlink(path);
}

static void fx_load(cog_orchestrator_t *o,cog_worker_id_t id,unsigned char load)
{
    fx_command(o,id,4,load);uint64_t end=fx_now()+2000;
    while(fx_now()<end&&fx_info(o,id).active_subagents!=load) { (void)fx_service(o);fx_pause(); }
    CHECK(fx_info(o,id).active_subagents==load);
}
static void scenario_placement_across_cores(void)
{
    char path[96];cog_orchestrator_t *o=fx_new(path,sizeof(path),false);
    cog_worker_id_t first=fx_spawn(o),second=fx_spawn(o);
    cog_peer_id_t p1=fx_info(o,first).peer_id,p2=fx_info(o,second).peer_id;
    CHECK(p1!=p2&&cog_orchestrator_place(o,0)==p1);
    fx_load(o,first,5);CHECK(cog_orchestrator_place(o,0)==p2);
    fx_load(o,second,5);CHECK(cog_orchestrator_place(o,0)==p1);
    fx_load(o,first,8);CHECK(cog_orchestrator_place(o,0)==p2);
    cog_core_info_t infos[4];size_t count=0;CHECK(cog_orchestrator_list(o,infos,4,&count)==COG_OK);
    CHECK_EQ_INT(count,2);CHECK(fx_info(o,first).active_subagents==8&&fx_info(o,second).active_subagents==5);
    fx_close(o,path,COG_OK);
}
/* ------------------------------------------------------------------ */
/* 2+3. Fan-out with group_id and ordered fan-in results               */
/* ------------------------------------------------------------------ */

static void scenario_fan_out_fan_in(void)
{
    cog_cli_options_t opts;
    cog_cli_app_t *app = NULL;
    cog_core_t *core;
    cog_agent_config_t cfg;
    cog_agent_id_t ids[3];
    cog_agent_status_t st;
    const char *sids[3];
    const char *parent_sid;
    static const char *const results[3] = { "r1", "r2", "r3" };
    static const size_t result_lens[3] = { 2U, 2U, 2U };
    char buf[32];
    size_t len = 0U;
    size_t i;

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
    parent_sid = cog_session_id(cog_cli_app_session(app));
    CHECK(parent_sid != NULL);

    /* Fan-out: three subagents in one group, each with its own child
     * session linked to the parent. */
    for (i = 0U; i < 3U; ++i) {
        (void)memset(&cfg, 0, sizeof(cfg));
        cfg.task = "fan-out task";
        cfg.parent_session_id = parent_sid;
        cfg.force_local = true;
        cfg.group_id = 7U;
        CHECK(cog_agent_spawn(core, &cfg, &ids[i]) == COG_OK);
        CHECK(ids[i] != 0U);
        CHECK(cog_agent_session_id(core, ids[i], &sids[i]) == COG_OK);
        CHECK(sids[i] != NULL);
        CHECK(cog_agent_internal_get(core, ids[i]) != NULL);
        CHECK_EQ_INT((long long)cog_agent_internal_get(core, ids[i])->group_id,
                     7LL);
    }
    /* Distinct child sessions. */
    CHECK(strcmp(sids[0], sids[1]) != 0);
    CHECK(strcmp(sids[1], sids[2]) != 0);
    CHECK(strcmp(sids[0], sids[2]) != 0);

    /* Run each to completion and collect the results in spawn order. */
    for (i = 0U; i < 3U; ++i) {
        CHECK(scenario_agent_step(core, ids[i], COG_AGENT_STATUS_RUNNING, COG_OK, "", 0U) == COG_OK);
        CHECK(scenario_agent_step(core, ids[i], COG_AGENT_STATUS_COMPLETED, COG_OK, results[i],
                                  result_lens[i]) == COG_OK);
        CHECK(cog_agent_status(core, ids[i], &st) == COG_OK);
        CHECK_EQ_INT((long long)st, (long long)COG_AGENT_STATUS_COMPLETED);
    }
    for (i = 0U; i < 3U; ++i) {
        CHECK(cog_agent_result(core, ids[i], buf, sizeof(buf), &len) == COG_OK);
        CHECK_EQ_INT((long long)len, (long long)result_lens[i]);
        CHECK(memcmp(buf, results[i], result_lens[i]) == 0);
    }

    cog_cli_app_free(app);
}

/* ------------------------------------------------------------------ */
/* 4. Broadcast cancel + late-result rejection                         */
/* ------------------------------------------------------------------ */

static int make_socket_path(char *path,size_t size)
{
    int n=snprintf(path,size,"scn10-%ld.sock",(long)getpid());
    if(n<0||(size_t)n>=size) return -1;
    (void)unlink(path);return 0;
}
static cog_result_t ipc_step(cog_ipc_endpoint_t *ep)
{ uint64_t deadline;return cog_ipc_endpoint_service(ep,fx_now(),&deadline); }
typedef struct peer_ctx { char path[128];char received[64];int ok; } peer_ctx_t;
static atomic_uint peers_done;
static void *peer_thread(void *arg)
{
    peer_ctx_t *p=arg;cog_ipc_endpoint_t *ep=NULL;
    if(cog_ipc_endpoint_new(&ep)!=COG_OK) return NULL;
    cog_result_t rc=cog_ipc_endpoint_connect(ep,p->path,NULL);
    if(rc!=COG_OK&&rc!=COG_ERROR_AGAIN) { (void)cog_ipc_endpoint_free(ep);return NULL; }
    unsigned char data[64];cog_ipc_message_t msg={.data=data,.capacity=sizeof(data)};
    uint64_t end=fx_now()+4000;
    do {
        (void)ipc_step(ep);rc=cog_ipc_endpoint_recv(ep,&msg);
        if(rc==COG_OK) {
            if(msg.len<sizeof(p->received)) { memcpy(p->received,data,msg.len);p->received[msg.len]='\0';p->ok=1; }
            break;
        }
        if(rc!=COG_ERROR_AGAIN) break;
        fx_pause();
    } while(fx_now()<end);
    (void)cog_ipc_endpoint_free(ep);(void)atomic_fetch_add(&peers_done,1U);return NULL;
}

static void scenario_broadcast_cancel(void)
{
    char path[128];
    cog_ipc_endpoint_t *listener = NULL;
    cog_ipc_endpoint_t *s1 = NULL;
    cog_ipc_endpoint_t *s2 = NULL;
    cog_peer_id_t pid1 = COG_PEER_ID_INVALID;
    cog_peer_id_t pid2 = COG_PEER_ID_INVALID;
    cog_ipc_message_t out;
    peer_ctx_t pa;
    peer_ctx_t pb;
    pthread_t ta, tb;

    /* --- Broadcast reaches every connected peer --- */
    CHECK(make_socket_path(path, sizeof(path)) == 0);
    CHECK(cog_ipc_endpoint_new(&listener) == COG_OK);
    CHECK(cog_ipc_endpoint_listen(listener, path) == COG_OK);

    /* Two peers connect to the listener; the main thread accepts their
     * connections, then broadcasts to all of them. */
    (void)memset(&pa, 0, sizeof(pa));
    (void)snprintf(pa.path, sizeof(pa.path), "%s", path);
    (void)memset(&pb, 0, sizeof(pb));
    (void)snprintf(pb.path, sizeof(pb.path), "%s", path);
    atomic_store(&peers_done,0U);
    CHECK(pthread_create(&ta, NULL, peer_thread, &pa) == 0);
    CHECK(pthread_create(&tb, NULL, peer_thread, &pb) == 0);
    (void)nanosleep(&(struct timespec){0, 50000000L}, NULL); /* 50 ms */

    uint64_t end=fx_now()+4000;cog_result_t rc=COG_ERROR_AGAIN;
    while(rc==COG_ERROR_AGAIN&&fx_now()<end) { rc=cog_ipc_endpoint_accept(listener,&s1,&pid1);fx_pause(); }
    CHECK(rc==COG_OK);rc=COG_ERROR_AGAIN;
    while(rc==COG_ERROR_AGAIN&&fx_now()<end) { rc=cog_ipc_endpoint_accept(listener,&s2,&pid2);fx_pause(); }
    CHECK(rc==COG_OK);
    for(unsigned i=0;i<20;++i) { (void)ipc_step(s1);(void)ipc_step(s2);fx_pause(); }

    CHECK(cog_ipc_message_new(&out, 64U) == COG_OK);
    (void)memcpy(out.data, "cancel", 6U);
    out.len = 6U;
    CHECK(cog_ipc_broadcast(listener, &out) == COG_OK);
    (void)cog_ipc_message_free(&out);

    end=fx_now()+4000;
    while(atomic_load(&peers_done)!=2U&&fx_now()<end) { (void)ipc_step(s1);(void)ipc_step(s2);fx_pause(); }
    CHECK(atomic_load(&peers_done)==2U);
    (void)pthread_join(ta, NULL);
    (void)pthread_join(tb, NULL);
    CHECK(pa.ok);
    CHECK(pb.ok);
    CHECK(strcmp(pa.received, "cancel") == 0);
    CHECK(strcmp(pb.received, "cancel") == 0);

    CHECK(cog_ipc_endpoint_free(s1) == COG_OK);
    CHECK(cog_ipc_endpoint_free(s2) == COG_OK);
    CHECK(cog_ipc_endpoint_free(listener) == COG_OK);
    (void)unlink(path);

    /* --- Agent cancel semantics: is_cancelled + late-result rejection --- */
    {
        cog_cli_options_t opts;
        cog_cli_app_t *app = NULL;
        cog_core_t *core;
        cog_agent_config_t cfg;
        cog_agent_id_t id = 0U;
        cog_agent_status_t st;
        bool cancelled = false;
        char rbuf[16];
        size_t rlen = 0U;

        (void)memset(&opts, 0, sizeof(opts));
        opts.model_id = "mock-model";
        opts.endpoint_name = "mock";
        opts.interactive = false;
        opts.backend = COG_CLI_BACKEND_MOCK;
    opts.explicit_options |= COG_CLI_OPT_BACKEND | COG_CLI_OPT_MODEL | COG_CLI_OPT_ENDPOINT;
    CHECK(cog_cli_app_new(&app, &opts) == COG_OK);
        if (app != NULL) {
            core = cog_cli_app_core(app);
            CHECK(core != NULL);
            (void)memset(&cfg, 0, sizeof(cfg));
            cfg.task = "fan-out winner";
            cfg.force_local = true;
            cfg.group_id = 7U;
            CHECK(cog_agent_spawn(core, &cfg, &id) == COG_OK);
            CHECK(scenario_agent_step(core, id, COG_AGENT_STATUS_RUNNING, COG_OK, "", 0U) == COG_OK);

            /* Broadcast cancel semantics on the agent: cancel marks it
             * admitted while RUNNING and is_cancelled reports true. */
            CHECK(cog_agent_cancel(core, id) == COG_OK);
            CHECK(cog_agent_status(core, id, &st) == COG_OK);
            CHECK_EQ_INT((long long)st, (long long)COG_AGENT_STATUS_RUNNING);
            CHECK(cog_agent_is_cancelled(core, id, &cancelled) == COG_OK);
            CHECK(cancelled);

            /* A late result from a cancelled fan-out agent is rejected. */
            CHECK(scenario_agent_step(core, id, COG_AGENT_STATUS_COMPLETED, COG_OK, "late", 4U)
                  == COG_ERROR_INVALID_STATE);
            CHECK(cog_agent_result(core, id, rbuf, sizeof(rbuf), &rlen)
                  == COG_ERROR_INVALID_STATE);

            CHECK(scenario_agent_step(core, id, COG_AGENT_STATUS_CANCELLED, COG_ERROR_ABORTED, "", 0U) == COG_OK);
            CHECK(cog_agent_result(core, id, rbuf, sizeof(rbuf), &rlen) == COG_OK);
            cog_cli_app_free(app);
        }
    }
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    const char *aux_bin = COG_AUX_CORE_BIN;

    if (aux_bin == NULL && argc > 1) {
        aux_bin = argv[1];
    }
    if (aux_bin == NULL) {
        (void)fprintf(stderr,
                      "test_10_fan_out_fan_in: COG_AUX_CORE_BIN not defined "
                      "and no argv[1] fixture path given\n");
        return 2;
    }

    (void)printf("Scenario 10: fan-out/fan-in across aux cores\n");
    scenario_placement_across_cores();
    scenario_fan_out_fan_in();
    scenario_broadcast_cancel();

    if (failures != 0) {
        (void)fprintf(stderr, "test_10_fan_out_fan_in: %d failure(s)\n",
                      failures);
        return 1;
    }
    (void)printf("test_10_fan_out_fan_in: all checks passed\n");
    return 0;
}

/** @file test_empty_session.c @brief Durable empty construction and guarded restart. */
#define _POSIX_C_SOURCE 200809L
#include "app_internal.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x); return 1; } } while (0)
typedef enum fault_mode { NONE, SAVE_BEFORE, SAVE_AFTER, CAS_BEFORE, CAS_AFTER, CAS_RIVAL, ACQUIRE_FAIL, EPOCH_MAX } fault_mode_t;
static fault_mode_t fault;
static cog_result_t injected;
static size_t saves, exchanges, acquisitions;
static cog_store_owner_t rival;
cog_result_t __real_cog_store_checkpoint_save(cog_store_t *,const cog_session_t *,const cog_store_owner_guard_t *,cog_store_checkpoint_t *);
cog_result_t __wrap_cog_store_checkpoint_save(cog_store_t *,const cog_session_t *,const cog_store_owner_guard_t *,cog_store_checkpoint_t *);
cog_result_t __real_cog_store_owner_compare_exchange(cog_store_t *,const char *,cog_store_owner_t,cog_store_owner_t,cog_store_checkpoint_t);
cog_result_t __wrap_cog_store_owner_compare_exchange(cog_store_t *,const char *,cog_store_owner_t,cog_store_owner_t,cog_store_checkpoint_t);
cog_result_t __real_cog_store_owner_acquire(cog_store_t *,const char *,cog_store_owner_t,cog_store_owner_guard_t **);
cog_result_t __wrap_cog_store_owner_acquire(cog_store_t *,const char *,cog_store_owner_t,cog_store_owner_guard_t **);
cog_result_t __real_cog_store_owner_read(cog_store_t *,const char *,cog_store_owner_t *,cog_store_checkpoint_t *);
cog_result_t __wrap_cog_store_owner_read(cog_store_t *,const char *,cog_store_owner_t *,cog_store_checkpoint_t *);
cog_result_t __wrap_cog_store_checkpoint_save(cog_store_t *store,const cog_session_t *session,
    const cog_store_owner_guard_t *guard,cog_store_checkpoint_t *checkpoint)
{
    saves++;
    if (fault==SAVE_BEFORE) return injected;
    cog_result_t result=__real_cog_store_checkpoint_save(store,session,guard,checkpoint);
    return result==COG_OK && fault==SAVE_AFTER ? injected : result;
}
cog_result_t __wrap_cog_store_owner_compare_exchange(cog_store_t *store,const char *id,
    cog_store_owner_t expected,cog_store_owner_t next,cog_store_checkpoint_t checkpoint)
{
    exchanges++;
    if (fault==CAS_BEFORE) return injected;
    if (fault==CAS_RIVAL) {
        rival=next;rival.instance_id^=UINT64_C(0x8000000000000000);
        if (rival.instance_id==0U) rival.instance_id=1U;
        cog_result_t result=__real_cog_store_owner_compare_exchange(store,id,expected,rival,checkpoint);
        if (result!=COG_OK) return result;
    }
    cog_result_t result=__real_cog_store_owner_compare_exchange(store,id,expected,next,checkpoint);
    return result==COG_OK && fault==CAS_AFTER ? injected : result;
}
cog_result_t __wrap_cog_store_owner_acquire(cog_store_t *store,const char *id,
    cog_store_owner_t owner,cog_store_owner_guard_t **guard)
{
    acquisitions++;
    if (fault==ACQUIRE_FAIL) { *guard=NULL;return COG_ERROR_NOMEM; }
    return __real_cog_store_owner_acquire(store,id,owner,guard);
}
cog_result_t __wrap_cog_store_owner_read(cog_store_t *store,const char *id,
    cog_store_owner_t *owner,cog_store_checkpoint_t *checkpoint)
{
    cog_result_t result=__real_cog_store_owner_read(store,id,owner,checkpoint);
    if (result==COG_OK && fault==EPOCH_MAX && owner->epoch!=0U) owner->epoch=UINT64_MAX;
    return result;
}
static void reset(void) { fault=NONE;injected=COG_OK;saves=0U;exchanges=0U;acquisitions=0U; }
static cog_cli_options_t options(const char *base,const char *id,cog_cli_session_action_t action)
{
    cog_cli_options_t value={.backend=COG_CLI_BACKEND_MOCK,.model_id="current/model",
        .config_path="/dev/null",.store_mode=COG_CLI_STORE_DURABLE,.store_dir=base,.session_action=action,
        .explicit_options=COG_CLI_OPT_BACKEND|COG_CLI_OPT_MODEL|COG_CLI_OPT_STORE_MODE|COG_CLI_OPT_STORE_DIR};
    if (action==COG_CLI_SESSION_NEW) value.session_id=id; else value.source_session_id=id;
    if (action==COG_CLI_SESSION_FORK) value.fork_node_id=1U;
    if (action==COG_CLI_SESSION_RECOVER) value.previous_processes_fenced=true;
    return value;
}
static int store_new(const char *base,cog_store_t **store)
{ CHECK(cog_store_new(store,&(cog_store_config_t){.base_dir=base,.require_owner=true})==COG_OK);return 0; }
static int state(const char *base,const char *id,cog_store_owner_t *owner,cog_store_checkpoint_t *checkpoint)
{
    cog_store_t *store=NULL;CHECK(store_new(base,&store)==0);
    CHECK(cog_store_owner_read(store,id,owner,checkpoint)==COG_OK);cog_store_free(store);return 0;
}
static int seed(const char *base,const char *id)
{
    cog_store_t *store=NULL;CHECK(store_new(base,&store)==0);
    CHECK(cog_store_owner_compare_exchange(store,id,(cog_store_owner_t){0},
        (cog_store_owner_t){.instance_id=101U,.epoch=1U},(cog_store_checkpoint_t){0})==COG_OK);
    cog_store_free(store);return 0;
}
static int empty(cog_cli_app_t *app,const char *id)
{
    cog_session_snapshot_t view;CHECK(cog_session_snapshot_view(app->session,&view)==COG_OK);
    CHECK(strcmp(view.session_id,id)==0 && view.node_count==0U && view.active_node_id==0U &&
        view.node_id_high_water==0U && view.audit_high_water==0U && view.ledger_count==0U && !view.inspection_only);
    cog_mock_state_t provider;CHECK(cog_mock_provider_state(app->provider,&provider)==COG_OK && provider.send_count==0U);
    return 0;
}
static int fresh(const char *base)
{
    reset();cog_cli_options_t value=options(base,"idle",COG_CLI_SESSION_NEW);cog_cli_app_t *app=NULL;
    CHECK(cog_cli_app_new(&app,&value)==COG_OK);CHECK(empty(app,"idle")==0 && saves==1U);
    cog_store_owner_t owner,after;cog_store_checkpoint_t checkpoint,later;
    CHECK(state(base,"idle",&owner,&checkpoint)==0 && owner.epoch==1U && checkpoint.generation!=0U);
    value=options(base,"idle",COG_CLI_SESSION_RESUME);cog_cli_app_t *other=NULL;
    CHECK(cog_cli_app_new(&other,&value)==COG_ERROR_AGAIN && other==NULL);
    cog_cli_app_free(app);CHECK(cog_cli_app_new(&app,&value)==COG_OK && empty(app,"idle")==0);
    CHECK(state(base,"idle",&after,&later)==0 && after.epoch==2U && after.instance_id!=owner.instance_id &&
        checkpoint.generation==later.generation);cog_cli_app_free(app);
    value=options(base,"idle",COG_CLI_SESSION_NEW);CHECK(cog_cli_app_new(&app,&value)==COG_ERROR_INVALID_STATE && app==NULL);
    value=options(base,"absent",COG_CLI_SESSION_RESUME);reset();
    CHECK(cog_cli_app_new(&app,&value)==COG_ERROR_NOT_FOUND && app==NULL && exchanges==0U && saves==0U);
    CHECK(state(base,"absent",&after,&later)==0 && after.epoch==0U && later.generation==0U);
    return 0;
}
static int interrupted(const char *base)
{
    int ready[2],release[2];CHECK(pipe(ready)==0 && pipe(release)==0);
    pid_t child=fork();CHECK(child>=0);
    if (child==0) {
        (void)close(ready[0]);(void)close(release[1]);
        cog_store_t *store=NULL;cog_store_owner_guard_t *guard=NULL;char byte='r';
        if (seed(base,"interrupted")!=0 || store_new(base,&store)!=0 ||
            cog_store_owner_acquire(store,"interrupted",(cog_store_owner_t){.instance_id=101U,.epoch=1U},&guard)!=COG_OK ||
            write(ready[1],&byte,1U)!=1 || read(release[0],&byte,1U)!=1) _exit(2);
        _exit(0); /* Real OS owner death closes the held independent lock. */
    }
    CHECK(close(ready[1])==0 && close(release[0])==0);char byte;
    CHECK(read(ready[0],&byte,1U)==1);
    cog_cli_options_t value=options(base,"interrupted",COG_CLI_SESSION_RESUME);cog_cli_app_t *app=NULL;
    reset();CHECK(cog_cli_app_new(&app,&value)==COG_ERROR_AGAIN && app==NULL && saves==0U);
    cog_store_owner_t owner;cog_store_checkpoint_t checkpoint;
    CHECK(state(base,"interrupted",&owner,&checkpoint)==0 && owner.epoch==1U && checkpoint.generation==0U);
    CHECK(write(release[1],"x",1U)==1);int status=0;CHECK(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);
    CHECK(close(ready[0])==0 && close(release[1])==0);
    reset();CHECK(cog_cli_app_new(&app,&value)==COG_OK && empty(app,"interrupted")==0 && saves==1U);
    CHECK(state(base,"interrupted",&owner,&checkpoint)==0 && owner.epoch==2U && owner.instance_id!=101U && checkpoint.generation!=0U);
    cog_cli_app_free(app);return 0;
}
static int save_failures(const char *base)
{
    for (unsigned i=0U;i<8U;i++) {
        char id[32];CHECK(snprintf(id,sizeof(id),"save-%u",i)>0);
        bool repair=i>=4U;unsigned variant=i%4U;reset();if (repair) CHECK(seed(base,id)==0);
        cog_cli_options_t value=options(base,id,repair?COG_CLI_SESSION_RESUME:COG_CLI_SESSION_NEW);cog_cli_app_t *app=NULL;
        reset();fault=variant<2U?SAVE_BEFORE:SAVE_AFTER;injected=variant%2U==0U?COG_ERROR_IO:COG_ERROR_INDETERMINATE;
        CHECK(cog_cli_app_new(&app,&value)==injected && app==NULL && saves==1U);
        reset();cog_store_owner_t owner;cog_store_checkpoint_t checkpoint;
        CHECK(state(base,id,&owner,&checkpoint)==0 && owner.epoch==(repair?2U:1U) && (checkpoint.generation!=0U)==(variant>=2U));
        value=options(base,id,COG_CLI_SESSION_NEW);
        CHECK(cog_cli_app_new(&app,&value)==COG_ERROR_INVALID_STATE && app==NULL);
        value=options(base,id,COG_CLI_SESSION_RESUME);
        CHECK(cog_cli_app_new(&app,&value)==COG_OK && empty(app,id)==0);
        CHECK(state(base,id,&owner,&checkpoint)==0 && owner.epoch==(repair?3U:2U) && checkpoint.generation!=0U);
        cog_cli_app_free(app);
    }
    return 0;
}
static int admission_failures(const char *base)
{
    static const fault_mode_t cases[]={CAS_BEFORE,CAS_AFTER,CAS_RIVAL,ACQUIRE_FAIL,EPOCH_MAX};
    for (size_t i=0U;i<sizeof(cases)/sizeof(cases[0]);i++) {
        char id[32];CHECK(snprintf(id,sizeof(id),"admit-%zu",i)>0);reset();CHECK(seed(base,id)==0);
        reset();fault=cases[i];injected=COG_ERROR_INDETERMINATE;
        cog_result_t expected=fault==CAS_RIVAL?COG_ERROR_INVALID_STATE:fault==ACQUIRE_FAIL?COG_ERROR_NOMEM:
            fault==EPOCH_MAX?COG_ERROR_LIMIT:injected;
        cog_cli_options_t value=options(base,id,COG_CLI_SESSION_RESUME);cog_cli_app_t *app=NULL;
        CHECK(cog_cli_app_new(&app,&value)==expected && app==NULL && saves==0U);
        CHECK(exchanges==(cases[i]==EPOCH_MAX?0U:1U) && acquisitions==(cases[i]==ACQUIRE_FAIL?1U:0U));
        reset();cog_store_owner_t owner;cog_store_checkpoint_t checkpoint;
        CHECK(state(base,id,&owner,&checkpoint)==0 && checkpoint.generation==0U);
        uint64_t epoch=cases[i]==CAS_BEFORE || cases[i]==EPOCH_MAX?1U:2U;
        CHECK(owner.epoch==epoch);if (cases[i]==CAS_RIVAL) CHECK(owner.instance_id==rival.instance_id);
        CHECK(cog_cli_app_new(&app,&value)==COG_OK && empty(app,id)==0);
        CHECK(state(base,id,&owner,&checkpoint)==0 && owner.epoch==epoch+1U && checkpoint.generation!=0U);
        cog_cli_app_free(app);
    }
    return 0;
}
static int selectors_and_remnants(const char *base)
{
    reset();CHECK(seed(base,"selectors")==0);cog_cli_app_t *app=NULL;
    for (int action=COG_CLI_SESSION_NEW;action<=COG_CLI_SESSION_RECOVER;action++) {
        if (action==COG_CLI_SESSION_LIST || action==COG_CLI_SESSION_RESUME) continue;
        cog_cli_options_t value=options(base,"selectors",(cog_cli_session_action_t)action);
        CHECK(cog_cli_app_new(&app,&value)==(action==COG_CLI_SESSION_NEW?COG_ERROR_INVALID_STATE:COG_ERROR_NOT_FOUND) && app==NULL);
    }
    static const char *const remnants[]={"meta.json","nodes.jsonl","index.bin","meta.json.tmp","nodes.jsonl.tmp","index.bin.tmp"};
    for (size_t i=0U;i<sizeof(remnants)/sizeof(remnants[0]);i++) {
        char id[32],path[1024];CHECK(snprintf(id,sizeof(id),"remnant-%zu",i)>0);CHECK(seed(base,id)==0);
        CHECK(snprintf(path,sizeof(path),"%s/%s/%s",base,id,remnants[i])>0);
        FILE *file=fopen(path,"wb");CHECK(file!=NULL && fputs("historical fixture bytes",file)>=0 && fclose(file)==0);
        reset();cog_cli_options_t value=options(base,id,COG_CLI_SESSION_RESUME);
        CHECK(cog_cli_app_new(&app,&value)!=COG_OK && app==NULL && saves==0U && exchanges==0U);
        cog_store_owner_t owner;cog_store_checkpoint_t checkpoint;
        CHECK(state(base,id,&owner,&checkpoint)==0 && owner.epoch==1U && checkpoint.generation==0U);
        file=fopen(path,"rb");char bytes[64]={0};CHECK(file!=NULL && fread(bytes,1U,sizeof(bytes)-1U,file)==24U && fclose(file)==0);
        CHECK(strcmp(bytes,"historical fixture bytes")==0);
    }
    return 0;
}
static int provenance(const char *base)
{
    reset();CHECK(seed(base,"provenance")==0);cog_store_t *store=NULL;CHECK(store_new(base,&store)==0);
    cog_store_owner_guard_t *guard=NULL;
    CHECK(cog_store_owner_acquire(store,"provenance",(cog_store_owner_t){.instance_id=101U,.epoch=1U},&guard)==COG_OK);
    const cog_message_t message={.role=COG_MESSAGE_ROLE_ASSISTANT,.content="prior",.content_len=5U};
    const cog_node_data_t node={.node_id=1U,.role=COG_NODE_ASSISTANT,.content="prior",.content_len=5U,
        .model="historical-model",.provider="historical-provider",.message=&message};
    const cog_session_snapshot_t snapshot={.session_id="provenance",.active_node_id=1U,
        .node_id_high_water=1U,.nodes=&node,.node_count=1U};
    cog_session_t *session=NULL;
    CHECK(cog_session_restore(&session,&(cog_session_config_t){.id="provenance"},&snapshot)==COG_OK);
    cog_store_checkpoint_t checkpoint;
    CHECK(cog_store_checkpoint_save(store,session,guard,&checkpoint)==COG_OK);
    CHECK(cog_store_owner_release(guard)==COG_OK);cog_session_free(session);cog_store_free(store);
    char path[1024];CHECK(snprintf(path,sizeof(path),"%s/current.cfg",base)>0);
    FILE *file=fopen(path,"wb");CHECK(file!=NULL && fputs("models.default=from-current-config\n",file)>=0 && fclose(file)==0);
    cog_cli_options_t value=options(base,"provenance",COG_CLI_SESSION_RESUME);
    value.model_id=NULL;value.explicit_options&=~(uint32_t)COG_CLI_OPT_MODEL;value.config_path=path;
    reset();cog_cli_app_t *app=NULL;CHECK(cog_cli_app_new(&app,&value)==COG_OK && saves==0U);
    CHECK(strcmp(app->opts.model_id,"from-current-config")==0);
    cog_session_snapshot_t loaded;CHECK(cog_session_snapshot_view(app->session,&loaded)==COG_OK && loaded.node_count==1U);
    CHECK(strcmp(loaded.nodes[0].model,"historical-model")==0 && strcmp(loaded.nodes[0].provider,"historical-provider")==0 &&
        strcmp(loaded.nodes[0].message->content,"prior")==0);
    cog_store_owner_t owner;cog_store_checkpoint_t after;
    CHECK(state(base,"provenance",&owner,&after)==0 && owner.epoch==2U && after.generation==checkpoint.generation);
    cog_cli_app_free(app);return 0;
}
static int inspect(const char *base,const char *id)
{
    cog_store_t *store=NULL;CHECK(store_new(base,&store)==0);
    cog_store_owner_t owner;cog_store_checkpoint_t checkpoint;
    CHECK(cog_store_owner_read(store,id,&owner,&checkpoint)==COG_OK && owner.epoch!=0U && checkpoint.generation!=0U);
    cog_session_t *session=NULL;cog_store_load_report_t report;
    CHECK(cog_store_checkpoint_load(store,id,checkpoint,COG_STORE_LOAD_REPLAY,&session,&report)==COG_OK);
    cog_session_snapshot_t snapshot;CHECK(cog_session_snapshot_view(session,&snapshot)==COG_OK &&
        strcmp(snapshot.session_id,id)==0 && snapshot.node_count==0U && snapshot.ledger_count==0U &&
        snapshot.active_node_id==0U && snapshot.node_id_high_water==0U && snapshot.audit_high_water==0U);
    printf("%" PRIu64 " %" PRIu64 "\n",owner.epoch,checkpoint.generation);
    cog_session_free(session);cog_store_free(store);return 0;
}
int main(int argc,char **argv)
{
    if (argc==4 && strcmp(argv[1],"inspect")==0) return inspect(argv[2],argv[3]);
    if (argc==4 && strcmp(argv[1],"reserve")==0) return seed(argv[2],argv[3]);
    CHECK(argc==1);
    char base[]="/tmp/cog-empty-session-XXXXXX";CHECK(mkdtemp(base)!=NULL);
    CHECK(fresh(base)==0);CHECK(interrupted(base)==0);CHECK(save_failures(base)==0);
    CHECK(admission_failures(base)==0);CHECK(selectors_and_remnants(base)==0);CHECK(provenance(base)==0);
    char command[1024];CHECK(snprintf(command,sizeof(command),"rm -rf -- '%s'",base)>0);CHECK(system(command)==0);
    return 0;
}

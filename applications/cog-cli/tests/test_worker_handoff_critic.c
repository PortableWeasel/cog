/** Independent bootstrap replay and disposal-failure contract regressions. */
#include "../src/worker_host.c"
#include <stdio.h>
#define REQUIRE(x) do { if (!(x)) { fprintf(stderr,"setup line %d: %s\n",__LINE__,#x); return 2; } } while (0)
static cog_result_t credential(void *arg,const char *reference,char *out,size_t capacity,size_t *required)
{
    (void)arg;(void)reference;*required=sizeof("synthetic-review-only");
    if(capacity<*required)return COG_ERROR_OVERFLOW;
    memcpy(out,"synthetic-review-only",*required);return COG_OK;
}
static int profile_replay(void)
{
    cog_cli_options_t options;char program[]="review";char *argv[]={program};int exit_code=0;
    REQUIRE(cog_cli_options_parse(&options,1,argv,&exit_code)==COG_OK);
    options.config_path="/dev/null";options.model_id="synthetic/model";
    options.credential_ref="host:review";options.credential_read=credential;
    options.workspace_root="/work";options.store_dir="/state";
    options.explicit_options=COG_CLI_OPT_MODEL|COG_CLI_OPT_CREDENTIAL|COG_CLI_OPT_WORKSPACE|COG_CLI_OPT_STORE_DIR;
    cog_cli_configuration_t *config=NULL;
    REQUIRE(cog_cli_configuration_new(&config,&options,1U)==COG_OK);
    cog_cli_worker_host_t host={0};cli_executor_slot_t slots[2]={0};host.executors=slots;
    host.startup_ready=true;host.fixed.worker=(cog_worker_id_t){11U,3U};host.runtime_incarnation=7U;
    host.net_template=(cog_net_config_t){.host_runtime_initialized=true,.dependency_memory=COG_NET_DEPENDENCY_MEMORY_DYNAMIC};
    cli_secret_exchange_t *exchange=&slots[1].profile;
    REQUIRE(secret_new(exchange,32768U)==COG_OK);
    exchange->source=config;exchange->probe=(cog_cli_bootstrap_probe_t){.session_id="root",.owner={1U,2U},.checkpoint={3U}};
    exchange->purpose=3U;exchange->generation=1U;exchange->acknowledged=true;
    slots[1].pending_profile=config;slots[1].phase=CLI_SLOT_PROFILE;strcpy(slots[1].session,"root");
    unsigned char body[32768],raw[32768];size_t length=0U,total=0U;
    body[0]=3U;memset(body+1U,0,4U);
    REQUIRE(cog_cli_configuration_bootstrap_encode(config,&exchange->probe,&host.net_template,body+5U,sizeof(body)-5U,&length)==COG_OK);
    cog_cli_control_message_t request={COG_CLI_CONTROL_BOOTSTRAP,COG_CLI_LANE_PROFILE,7U,7U,19U,0U,{11U,3U},9U,0U,0U,"root",body,length+5U};
    secret_bind(exchange,&request);
    REQUIRE(cog_cli_control_encode(&request,raw,sizeof(raw),&total)==COG_OK);host.receive_secret=raw;
    REQUIRE(bootstrap_duplicate(&host,exchange,&request,total)==COG_OK);
    exchange->pending=false;
    cog_result_t actual=bootstrap(&host,&request,total);
    printf("purpose3 exact duplicate: actual=%d expected=%d\n",actual,COG_OK);
    REQUIRE(slots[1].pending_profile==config);
    body[length+4U]^=1U;
    REQUIRE(cog_cli_control_encode(&request,raw,sizeof(raw),&total)==COG_OK);
    REQUIRE(bootstrap(&host,&request,total)==COG_ERROR_PROTOCOL);
    REQUIRE(slots[1].pending_profile==config && exchange->source==config);
    secret_free(exchange);cog_cli_configuration_free(config);
    return actual==COG_OK?0:1;
}
static int abort_case(bool previous_failure)
{
    cog_cli_worker_host_t host={0};cli_executor_slot_t slot={0};
    cli_control_lane_t in[CLI_HOST_LANES]={0},out[CLI_HOST_LANES]={0};
    unsigned char pending[512],original[512],final[512],raw[512],body[192],decode[512];
    host.fixed.worker=(cog_worker_id_t){11U,3U};host.runtime_incarnation=7U;host.receive_secret=raw;
    slot.inbound=in;slot.outbound=out;slot.phase=CLI_SLOT_PROFILE;slot.local_done=true;
    slot.owner_peer=9U;strcpy(slot.session,"root");
    slot.profile.purpose=3U;slot.profile.probe=(cog_cli_bootstrap_probe_t){.session_id="root",.owner={1U,2U},.checkpoint={3U}};
    cli_control_lane_t *lane=&in[COG_CLI_LANE_HANDOFF-1U];
    lane->pending=(cli_host_bytes_t){pending,sizeof(pending),0U};
    lane->final_original=(cli_host_bytes_t){original,sizeof(original),0U};
    lane->final_reply=(cli_host_bytes_t){final,sizeof(final),0U};
    out[COG_CLI_LANE_HANDOFF-1U].next_wire=1U;
    cog_handoff_message_t inner={COG_HANDOFF_ABORT,"root",8U,9U,{1U,2U},{2U,4U},{3U}};
    size_t length,total;
    REQUIRE(cog_orchestrator_handoff_encode(&inner,body,sizeof(body),&length)==COG_OK);
    cog_cli_control_message_t request={COG_CLI_CONTROL_HANDOFF,COG_CLI_LANE_HANDOFF,7U,7U,20U,0U,{11U,3U},9U,0U,0U,"root",body,length};
    REQUIRE(cog_cli_control_encode(&request,raw,sizeof(raw),&total)==COG_OK);
    if(previous_failure) {
        slot.primary=COG_ERROR_IO;inner.kind=COG_HANDOFF_FAILED;
        unsigned char failed[192];size_t bytes;
        REQUIRE(cog_orchestrator_handoff_encode(&inner,failed,sizeof(failed),&bytes)==COG_OK);
        cog_cli_control_message_t response=reply_envelope(&host,&request,COG_CLI_CONTROL_HANDOFF,10U,failed,bytes);
        REQUIRE(cog_cli_control_encode(&response,final,sizeof(final),&lane->final_reply.length)==COG_OK);
        lane->final_valid=true;lane->final_answered_wire=19U;lane->final_reply_wire=10U;
    }
    if(!previous_failure) {
        cog_cli_options_t options;char program[]="review";char *argv[]={program};int code=0;
        REQUIRE(cog_cli_options_parse(&options,1,argv,&code)==COG_OK);
        options.config_path="/dev/null";options.model_id="synthetic/model";
        options.credential_ref="host:review";options.credential_read=credential;
        options.workspace_root="/work";options.store_dir="/state";
        options.explicit_options=COG_CLI_OPT_MODEL|COG_CLI_OPT_CREDENTIAL|COG_CLI_OPT_WORKSPACE|COG_CLI_OPT_STORE_DIR;
        REQUIRE(cog_cli_configuration_new(&slot.pending_profile,&options,1U)==COG_OK);
        slot.profile.source=slot.pending_profile;
        inner.checkpoint.generation=4U;
        REQUIRE(cog_orchestrator_handoff_encode(&inner,body,sizeof(body),&length)==COG_OK);
        REQUIRE(handoff_receive(&host,&slot,&request,total)==COG_ERROR_INVALID_STATE);
        REQUIRE(slot.pending_profile!=NULL && !lane->final_valid);
        inner.checkpoint.generation=3U;
        REQUIRE(cog_orchestrator_handoff_encode(&inner,body,sizeof(body),&length)==COG_OK);
    }
    if(!previous_failure) {
        cli_control_lane_t *policy=&out[COG_CLI_LANE_POLICY-1U];
        policy->pending_valid=true;policy->pending_wire=42U;
        REQUIRE(handoff_receive(&host,&slot,&request,total)==COG_ERROR_AGAIN);
        REQUIRE(slot.pending_profile!=NULL && !lane->final_valid);
        unsigned char answer[21]={0};answer[4]=2U;
        cog_cli_control_message_t accepted=request;accepted.kind=COG_CLI_CONTROL_COMMAND_RESULT;
        accepted.lane=COG_CLI_LANE_POLICY;accepted.reply_to=42U;accepted.payload=answer;accepted.payload_len=sizeof(answer);
        REQUIRE(policy_result(&slot,&accepted)==COG_OK && policy->retired_through==42U);
    }
    cog_result_t actual=handoff_receive(&host,&slot,&request,total);
    if(!previous_failure) REQUIRE(slot.pending_profile==NULL && slot.profile.source==NULL);

    cog_handoff_kind_t kind=0;
    if(actual==COG_OK) {
        cog_cli_control_message_t response;char session[128];size_t needed;
        REQUIRE(cog_cli_control_decode(final,lane->final_reply.length,&response,decode,sizeof(decode),&needed)==COG_OK);
        REQUIRE(cog_orchestrator_handoff_decode(response.payload,response.payload_len,&inner,session,sizeof(session),&needed)==COG_OK);
        kind=inner.kind;
    }
    printf("%s: actual rc=%d kind=%d expected kind=%d\n",previous_failure?"consuming failure then fresh ABORT":"bootstrap-only ABORT",actual,(int)kind,previous_failure?COG_HANDOFF_FAILED:COG_HANDOFF_ABORT);
    if(previous_failure) {
        REQUIRE(slot.primary==COG_ERROR_IO);
        bool duplicate=false;
        REQUIRE(lane_duplicate(lane,request.wire_id,raw,total,&duplicate)==COG_OK && duplicate);
        request.wire_id=21U;
        REQUIRE(cog_cli_control_encode(&request,raw,sizeof(raw),&total)==COG_OK);
        REQUIRE(handoff_receive(&host,&slot,&request,total)==COG_OK);
        cog_cli_control_message_t response;char session[128];size_t needed;
        REQUIRE(cog_cli_control_decode(final,lane->final_reply.length,&response,decode,sizeof(decode),&needed)==COG_OK);
        REQUIRE(cog_orchestrator_handoff_decode(response.payload,response.payload_len,&inner,session,sizeof(session),&needed)==COG_OK);
        REQUIRE(inner.kind==COG_HANDOFF_FAILED && slot.primary==COG_ERROR_IO && response.reply_to==21U);
        host.startup_ready=true;host.executors=&slot;
        /* Purpose3 addresses reserved physical slot1. */
        cli_executor_slot_t slots[2]={0};slots[1]=slot;host.executors=slots;
        unsigned char profile[17]={3U};
        cog_cli_control_message_t fresh=request;fresh.kind=COG_CLI_CONTROL_BOOTSTRAP;
        fresh.lane=COG_CLI_LANE_PROFILE;fresh.wire_id=22U;fresh.payload=profile;fresh.payload_len=sizeof(profile);
        REQUIRE(bootstrap(&host,&fresh,0U)==COG_ERROR_INVALID_STATE);
        REQUIRE(slots[1].primary==COG_ERROR_IO && slots[1].pending_profile==NULL);
    }
    return actual==COG_OK && kind==(previous_failure?COG_HANDOFF_FAILED:COG_HANDOFF_ABORT)?0:1;
}
int main(void)
{
    int failures=profile_replay();failures+=abort_case(false);failures+=abort_case(true);
    return failures==0?0:1;
}

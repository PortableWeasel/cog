/** Actual consuming reload plus nonsecret stale receipt lifetime. */
#include "../src/worker_host.c"
#include <assert.h>
#include <stdio.h>
int main(void)
{
    cog_cli_options_t options;char program[]="reload";char mock[]="--mock",no_store[]="--no-store";char *argv[]={program,mock,no_store};int code=0;
    assert(cog_cli_options_parse(&options,3,argv,&code)==COG_OK);options.config_path="/dev/null";
    cog_cli_configuration_t *configuration=NULL;
    assert(cog_cli_configuration_new(&configuration,&options,1U)==COG_OK);
    cog_cli_execution_admission_t admission={.kind=COG_CLI_EXECUTION_ROOT,.configuration=configuration,.configuration_generation=1U};
    cog_cli_app_t *app=NULL;assert(cog_cli_execution_new(&app,&admission)==COG_OK);
    cog_cli_worker_host_t host={0};cli_executor_slot_t slot={0};
    host.executors=&slot;host.executor_capacity=1U;host.startup_ready=true;
    host.fixed.worker=(cog_worker_id_t){11U,3U};host.runtime_incarnation=7U;
    slot.app=app;slot.owner_peer=9U;strcpy(slot.session,"root");
    cli_secret_exchange_t *exchange=&slot.profile;assert(secret_new(exchange,4096U)==COG_OK);
    cog_cli_control_message_t request={COG_CLI_CONTROL_BOOTSTRAP,COG_CLI_LANE_PROFILE,
        7U,7U,19U,0U,{11U,3U},9U,0U,0U,"root",NULL,0U};
    secret_bind(exchange,&request);exchange->purpose=4U;exchange->reload_attempts_used=1U;exchange->generation=2U;
    exchange->probe=(cog_cli_bootstrap_probe_t){.session_id="root",.owner={1U,2U},.checkpoint={3U}};
    assert(cog_cli_configuration_new(&configuration,&options,2U)==COG_OK);exchange->source=configuration;
    memset(exchange->frame,0xa5,exchange->frame_capacity);memset(exchange->scratch,0xa5,exchange->scratch_capacity);
    app->shutdown=true; /* Actual documented consuming invalid-state path. */
    cog_result_t outcome=reload_profile(&slot,exchange,&configuration);
    assert(outcome==COG_ERROR_INVALID_STATE && configuration==NULL && exchange->source==NULL);
    assert(exchange->retired_through==19U && exchange->probe.checkpoint.generation==0U);
    for(size_t i=0U;i<exchange->frame_capacity;++i) assert(exchange->frame[i]==0U);
    for(size_t i=0U;i<exchange->scratch_capacity;++i) assert(exchange->scratch[i]==0U);
    assert(bootstrap_reply(&host,exchange,&request,outcome,1U)==COG_OK);
    unsigned char receipt[512];assert(exchange->body_length<sizeof(receipt));size_t length=exchange->body_length;
    memcpy(receipt,exchange->body,length);uint64_t response_wire=exchange->replied_wire;
    exchange->pending=false;
    /* Even changed erased bytes at retired W have the same uniform stale result. */
    unsigned char changed=0xffU;request.payload=&changed;request.payload_len=1U;
    assert(bootstrap(&host,&request,0U)==COG_OK && exchange->pending && !host.shutdown);
    assert(exchange->replied_wire==response_wire && exchange->body_length==length && memcmp(receipt,exchange->body,length)==0);
    exchange->pending=false;request.wire_id=18U;
    assert(bootstrap(&host,&request,0U)==COG_OK && !exchange->pending);
    request.wire_id=20U;assert(bootstrap(&host,&request,0U)==COG_ERROR_PROTOCOL);
    assert(exchange->wire==19U && exchange->replied_wire==response_wire && memcmp(receipt,exchange->body,length)==0);
    /* A protocol-admitted successor consumes R1; its own consuming failure
       replaces R1 with R2, never aliases either consumed generation. */
    assert(cog_cli_configuration_new(&configuration,&options,3U)==COG_OK);
    secret_bind(exchange,&request);exchange->generation=3U;exchange->reload_attempts_used=2U;exchange->source=configuration;
    outcome=reload_profile(&slot,exchange,&configuration);assert(outcome==COG_ERROR_INVALID_STATE && configuration==NULL);
    assert(bootstrap_reply(&host,exchange,&request,outcome,1U)==COG_OK);
    assert(exchange->wire==20U && exchange->retired_through==20U && exchange->replied_wire==response_wire+1U);
    exchange->pending=false;request.wire_id=19U;
    assert(bootstrap(&host,&request,0U)==COG_OK && !exchange->pending);
    request.wire_id=20U;assert(bootstrap(&host,&request,0U)==COG_OK && exchange->pending);
    assert(cog_cli_execution_close(&app,COG_OK)==COG_OK && app==NULL);
    secret_free(exchange);puts("worker consumed reload receipt lifetime PASS");return 0;
}

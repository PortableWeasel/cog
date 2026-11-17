#include "../src/resource_exchange.h"
#include "../src/instruction_hash.h"
#include <assert.h>
#include <string.h>
static const unsigned char capsule[]={'C','G','I','M',1,0,0,0,1,0,0,0,42};
static cog_cli_control_message_t envelope(cog_cli_control_kind_t kind,uint64_t wire,uint64_t reply) {
    return (cog_cli_control_message_t){.kind=kind,.lane=COG_CLI_LANE_RESOURCE,
        .runtime_incarnation=1,.sender_incarnation=2,.wire_id=wire,.reply_to=reply,
        .worker={2,1},.owner_peer=3,.agent_id=4,.execution_epoch=1,.session_id="child"};
}
static void transfer(cli_resource_exchange_t *to,const cli_resource_exchange_t *from,size_t index) {
    unsigned char decoded[4096];size_t required=0U;
    cog_cli_control_message_t m;
    const cli_resource_frame_t *f=&from->frames[index];
    assert(cog_cli_control_decode(f->bytes,f->length,&m,decoded,sizeof(decoded),&required)==COG_OK);
    assert(cli_resource_exchange_receive(to,&m,f->bytes,f->length)==COG_OK);
    assert(cli_resource_exchange_receive(to,&m,f->bytes,f->length)==COG_ERROR_AGAIN);
    unsigned char changed[4096];assert(f->length<=sizeof(changed));
    memcpy(changed,f->bytes,f->length);changed[f->length-1U]^=1U;
    assert(cli_resource_exchange_receive(to,&m,changed,f->length)==COG_ERROR_PROTOCOL);
}
static void transaction(bool changed,bool failed) {
    cli_resource_exchange_t *worker=NULL,*host=NULL;
    assert(cli_resource_exchange_new(&worker,4096,1024)==COG_OK);
    assert(cli_resource_exchange_new(&host,4096,1024)==COG_OK);
    cog_cli_instruction_identity_t old={.catalog_generation=1,.snapshot_id=1};
    nt_hash(capsule,sizeof(capsule),old.sha256);
    cog_cli_instruction_resource_t request={.kind=COG_CLI_INSTRUCTION_RESOURCE_REQUEST,
        .context=old,.operation=COG_CLI_INSTRUCTION_RESOURCE_SCOPE,.root_id="root",
        .skill_name="",.path="",.assistant_node_id=3,.call_id="call",
        .request_checkpoint_generation=9};
    cog_cli_control_message_t m=envelope(COG_CLI_CONTROL_RESOURCE_REQUEST,1,0);
    assert(cli_resource_exchange_build(worker,&m,&request)==COG_OK);transfer(host,worker,0);
    cog_cli_instruction_resource_t result={.kind=COG_CLI_INSTRUCTION_RESOURCE_RESULT,.context=old,
        .candidate={old,capsule,sizeof(capsule)}};
    if(changed)result.candidate.identity.snapshot_id=2;
    if(failed) { result.result=COG_ERROR_NOT_FOUND;memset(&result.candidate,0,sizeof(result.candidate)); }
    m=envelope(COG_CLI_CONTROL_RESOURCE_RESULT,1,1);
    assert(cli_resource_exchange_build(host,&m,&result)==COG_OK);transfer(worker,host,1);
    cog_cli_instruction_resource_t commit={.kind=COG_CLI_INSTRUCTION_RESOURCE_COMMIT,.context=old,
        .candidate={.identity=result.candidate.identity},
        .outcome=failed?COG_CLI_INSTRUCTION_COMMIT_REFUSED:
            changed?COG_CLI_INSTRUCTION_COMMIT_SAVED:COG_CLI_INSTRUCTION_COMMIT_UNCHANGED,
        .checkpoint_generation=changed?10U:0U};
    m=envelope(COG_CLI_CONTROL_RESOURCE_COMMIT,2,99);
    assert(cli_resource_exchange_build(worker,&m,&commit)==COG_ERROR_PROTOCOL);
    m.reply_to=1;
    assert(cli_resource_exchange_build(worker,&m,&commit)==COG_OK);transfer(host,worker,2);
    cog_cli_instruction_resource_t ready=commit;ready.kind=COG_CLI_INSTRUCTION_RESOURCE_READY;
    m=envelope(COG_CLI_CONTROL_RESOURCE_READY,2,2);
    ready.candidate.identity.snapshot_id++;
    assert(cli_resource_exchange_build(host,&m,&ready)==COG_ERROR_PROTOCOL);
    ready.candidate.identity.snapshot_id--;
    assert(cli_resource_exchange_build(host,&m,&ready)==COG_OK);transfer(worker,host,3);
    cog_cli_instruction_resource_t consumed={.kind=COG_CLI_INSTRUCTION_RESOURCE_CONSUMED,.context=old};
    m=envelope(COG_CLI_CONTROL_RESOURCE_CONSUMED,3,2);
    assert(cli_resource_exchange_build(worker,&m,&consumed)==COG_OK);transfer(host,worker,4);
    cli_resource_exchange_reset(host);assert(!host->active && host->retired_request==1);
    for(size_t i=0U;i<host->result_capacity;i++)assert(host->result_storage[i]==0);
    m=envelope(COG_CLI_CONTROL_RESOURCE_REQUEST,1,0);
    assert(cli_resource_exchange_build(host,&m,&request)==COG_ERROR_AGAIN);
    cli_resource_exchange_free(&worker);cli_resource_exchange_free(&host);
    assert(worker==NULL && host==NULL);
}
int main(void) {transaction(false,false);transaction(true,false);transaction(false,true);return 0;}

/** @file test_11_ui_reconnect.c
 * @brief Actual coordinator/socket/client replay, ASK and retained command join.
 * Old scenario-local queue/buffer assertions remain in components/.
 */
#define _POSIX_C_SOURCE 200809L
#include "session_scenario.h"
#include <inttypes.h>
#include <errno.h>
#include <stdlib.h>
#define CHECK(x) do { if(!(x)){fprintf(stderr,"reconnect line %d: %s (rc=%d)\n",__LINE__,#x,(int)primary);if(primary==COG_OK)primary=COG_ERROR_PROTOCOL;goto finish;} }while(0)

/* Observe actual successful client transport copies. This wrapper introduces
   no frame or result; it proves that reconnect resends the identical command. */
typedef struct command_probe {
    cog_ipc_endpoint_t *endpoint;
    unsigned char original[65536];size_t length,sends;
    uint64_t wire;bool conflict;
} command_probe_t;
static command_probe_t *active_probe;
cog_result_t __real_cog_ipc_endpoint_send(cog_ipc_endpoint_t *endpoint,const cog_ipc_message_t *message);
cog_result_t __wrap_cog_ipc_endpoint_send(cog_ipc_endpoint_t *endpoint,const cog_ipc_message_t *message);
cog_result_t __wrap_cog_ipc_endpoint_send(cog_ipc_endpoint_t *endpoint,const cog_ipc_message_t *message)
{
    cog_result_t rc=__real_cog_ipc_endpoint_send(endpoint,message);
    command_probe_t *p=active_probe;
    if(rc!=COG_OK || p==NULL || endpoint!=p->endpoint)return rc;
    unsigned char storage[65536];cog_cli_control_message_t frame;size_t required=0U;
    if(cog_cli_control_decode(message->data,message->len,&frame,storage,sizeof(storage),&required)!=COG_OK || frame.kind!=COG_CLI_CONTROL_UI_COMMAND)return rc;
    if(p->sends==0U){
        if(message->len>sizeof(p->original)){p->conflict=true;return rc;}
        memcpy(p->original,message->data,message->len);p->length=message->len;p->wire=frame.wire_id;
    }else if(message->len!=p->length || memcmp(message->data,p->original,p->length)!=0 || frame.wire_id!=p->wire)p->conflict=true;
    ++p->sends;return rc;
}
static bool same_view(const cog_cli_projection_view_t *a,const cog_cli_projection_view_t *b)
{
    if(strcmp(a->display.session_id,b->display.session_id)!=0 ||
       a->display.generation!=b->display.generation || a->display.through_sequence!=b->display.through_sequence ||
       a->display.item_count!=b->display.item_count || a->policy_count!=b->policy_count ||
       a->agent_count!=b->agent_count || a->active_configuration!=b->active_configuration ||
       a->published_configuration!=b->published_configuration)return false;
    for(size_t i=0U;i<a->display.item_count;++i){
        const cog_cli_display_item_t *x=&a->display.items[i],*y=&b->display.items[i];
        if(x->node_id!=y->node_id || x->role!=y->role || x->status!=y->status ||
           x->text_len!=y->text_len || memcmp(x->text,y->text,x->text_len)!=0)return false;
    }
    for(size_t i=0U;i<a->policy_count;++i){
        const cog_cli_policy_view_t *x=&a->policies[i],*y=&b->policies[i];
        if(strcmp(x->session_id,y->session_id)!=0 || x->assistant_node!=y->assistant_node ||
           x->call_ordinal!=y->call_ordinal || x->nonce!=y->nonce ||
           x->configuration_generation!=y->configuration_generation || x->deadline_ms!=y->deadline_ms ||
           strcmp(x->tool_name,y->tool_name)!=0)return false;
    }
    return a->agent_count==0U; /* This scenario admits no child attempts. */
}
static cli_client_record_t *record(cog_cli_host_runtime_t *host,uint64_t lease)
{
    for(size_t i=0U;i<host->bounds.clients;++i)
        if(host->clients[i].used && host->clients[i].logical_lease==lease)return &host->clients[i];
    return NULL;
}
/* The client is deliberately stopped with an empty decoded endpoint. Read and
   discard only genuine framed bytes until its correlated final is observed;
   shutdown alone does not discard queued socket bytes on Linux. */
static cog_result_t lose_final(headless_fixture_t *f,session_client_t *c,
    uint64_t request,uint64_t *result_wire)
{
    cog_ipc_endpoint_t *ep=c->client->endpoint;
    if(ep->rx_count!=0U || ep->prefix_used!=0U || ep->rx_partial!=0U)return COG_ERROR_INVALID_STATE;
    unsigned char bytes[65536],decoded[65536];size_t used=0U,parsed=0U;
    uint64_t deadline=headless_fixture_clock_ms()+2000U;bool found=false;
    while(!found && headless_fixture_clock_ms()<deadline){
        cog_result_t rc=session_tick(f,NULL,NULL);if(rc!=COG_OK)return rc;
        ssize_t n=recv(ep->fd,bytes+used,sizeof(bytes)-used,MSG_DONTWAIT);
        if(n<0){if(errno==EAGAIN || errno==EWOULDBLOCK || errno==EINTR)continue;return COG_ERROR_IO;}
        if(n==0)return COG_ERROR_IO;
        used+=(size_t)n;
        while(used-parsed>=4U){
            const unsigned char *p=bytes+parsed;
            size_t length=((size_t)p[0]<<24U)|((size_t)p[1]<<16U)|((size_t)p[2]<<8U)|(size_t)p[3];
            if(length==0U || length>sizeof(bytes)-4U)return COG_ERROR_LIMIT;
            if(used-parsed<length+4U)break;
            cog_cli_control_message_t message;size_t required=0U;
            rc=cog_cli_control_decode(p+4U,length,&message,decoded,sizeof(decoded),&required);
            if(rc!=COG_OK)return rc;
            if(message.kind==COG_CLI_CONTROL_COMMAND_RESULT && message.reply_to==request){
                if(message.runtime_incarnation!=c->client->runtime || strcmp(message.session_id,c->client->session)!=0 ||
                   message.payload_len!=21U)return COG_ERROR_PROTOCOL;
                bool cached=false;
                for(size_t i=0U;i<f->host->bounds.command_records;++i){
                    const cli_command_record_t *r=&f->host->commands[i];
                    if(r->phase==CLI_COMMAND_FINAL && r->lease==c->client->lease && r->request_wire==request &&
                       r->reply.length==length && memcmp(r->reply.data,p+4U,length)==0)cached=true;
                }
                if(!cached)return COG_ERROR_PROTOCOL;
                *result_wire=message.wire_id;found=true;
            }
            parsed+=length+4U;
        }
        if(used==sizeof(bytes) && !found)return COG_ERROR_LIMIT;
    }
    if(!found)return COG_ERROR_TIMEOUT;
    cog_result_t rc=session_disconnect(c);if(rc!=COG_OK)return rc;
    /* With SHUT_RDWR, drain the bounded remainder before client service. The
       endpoint has consumed no bytes from this exchange and will observe EOF. */
    size_t discarded=0U;
    for(;;){ssize_t n=recv(ep->fd,bytes,sizeof(bytes),MSG_DONTWAIT);
        if(n==0)break;
        if(n<0){if(errno==EINTR)continue;if(errno==EAGAIN || errno==EWOULDBLOCK)break;return COG_ERROR_IO;}
        discarded+=(size_t)n;if(discarded>1024U*1024U)return COG_ERROR_LIMIT;
    }
    return COG_OK;
}
int main(int argc,char **argv)
{
    headless_fixture_t f={0};cog_result_t primary=COG_OK;bool complete=false;
    session_client_t controller={.input={-1,-1}},watch={.input={-1,-1}},replacement={.input={-1,-1}};
    unsigned char *original=NULL;size_t original_length=0U;uint64_t original_wire=0U,lost_result_wire=0U;
    char path[4096],effect[4096];uint64_t deadline=0U;
    command_probe_t probe={0};
    CHECK(argc==8 && strcmp(argv[7],"reconnect")==0);
    const char *paths[6]={argv[1],argv[2],argv[3],argv[4],argv[5],argv[6]};
    CHECK(headless_fixture_init(&f,paths,0U,1U)==COG_OK);
    f.options.tool_policy=COG_CLI_TOOL_POLICY_ASK;
    f.config.replay_records=2U;f.config.controller_reconnect_ms=1000U;f.config.client_receipt_ms=2000U;
    primary=headless_fixture_open(&f);CHECK(primary==COG_OK);
    CHECK(snprintf(path,sizeof(path),"%s/controller.ndjson",argv[4])>0);
    CHECK(session_client_open(&controller,f.ui_path,path,false)==COG_OK);
    deadline=headless_fixture_clock_ms()+3000U;
    while(!session_online(&f,&controller) && headless_fixture_clock_ms()<deadline){primary=session_tick(&f,&controller,NULL);CHECK(primary==COG_OK);}
    CHECK(session_online(&f,&controller));
    uint64_t lease=controller.client->lease,generation=controller.client->replay_generation;
    char session[128];CHECK(snprintf(session,sizeof(session),"%s",controller.client->session)>0);
    uint64_t before=controller.client->rendered_sequence;
    /* Five explicit client inputs are admitted while the first real TLS request
       waits for our offline marker. Four completions then the ASK publication
       overrun the real two-record ring while this same client is disconnected. */
    for(unsigned int input=1U;input<=5U;++input){
        if(input==5U)CHECK(snprintf(path,sizeof(path),"policy\n")>0);
        else CHECK(snprintf(path,sizeof(path),"stream %u\n",input)>0);
        CHECK(session_write(&controller,path)==COG_OK);
        deadline=headless_fixture_clock_ms()+2000U;
        while((controller.client->last_input_serial<input || controller.client->requests[0].phase!=CLI_CLIENT_REQUEST_ACKED || !session_online(&f,&controller)) && headless_fixture_clock_ms()<deadline){primary=session_tick(&f,&controller,NULL);CHECK(primary==COG_OK);}
        CHECK(controller.client->last_input_serial==input && controller.client->requests[0].phase==CLI_CLIENT_REQUEST_ACKED && controller.client->final_input_result==COG_OK && session_online(&f,&controller));
    }
    CHECK(session_disconnect(&controller)==COG_OK);
    CHECK(snprintf(path,sizeof(path),"%s/offline",argv[4])>0);
    FILE *marker=fopen(path,"w");CHECK(marker!=NULL);CHECK(fclose(marker)==0);
    cog_cli_projection_view_t current={0};deadline=headless_fixture_clock_ms()+5000U;
    do{primary=session_tick(&f,NULL,NULL);CHECK(primary==COG_OK);
       CHECK(cog_cli_projection_snapshot(f.host->projection,&current)==COG_OK);
    }while(current.policy_count!=1U && headless_fixture_clock_ms()<deadline);
    CHECK(current.policy_count==1U && current.display.through_sequence>before+2U);
    CHECK(snprintf(effect,sizeof(effect),"%s/effect-policy",argv[4])>0);CHECK(access(effect,F_OK)!=0);
    cog_cli_policy_view_t prompt=current.policies[0];
    uint64_t prompt_node=prompt.assistant_node,prompt_nonce=prompt.nonce;
    uint16_t prompt_ordinal=prompt.call_ordinal;
    deadline=headless_fixture_clock_ms()+3000U;
    while((!session_online(&f,&controller) || controller.client->gap_last==0U) && headless_fixture_clock_ms()<deadline){primary=session_tick(&f,&controller,NULL);CHECK(primary==COG_OK);}
    CHECK(session_online(&f,&controller) && controller.client->gap_last>before);
    CHECK(controller.client->lease==lease && controller.client->replay_generation==generation && strcmp(session,controller.client->session)==0);
    CHECK(cog_cli_projection_snapshot(f.host->projection,&current)==COG_OK);
    CHECK(same_view(&current,&controller.client->banks[controller.client->current].view));
    CHECK(current.policy_count==1U && current.policies[0].assistant_node==prompt_node && current.policies[0].nonce==prompt_nonce && current.policies[0].call_ordinal==prompt_ordinal);
    CHECK(access(effect,F_OK)!=0); /* Reconnect/render is never implicit ALLOW. */
    /* A separate actual watch client consumes state but cannot mutate it. */
    CHECK(snprintf(path,sizeof(path),"%s/watch.ndjson",argv[4])>0);
    CHECK(session_client_open(&watch,f.ui_path,path,true)==COG_OK);
    deadline=headless_fixture_clock_ms()+3000U;
    while(!session_online(&f,&watch) && headless_fixture_clock_ms()<deadline){primary=session_tick(&f,&controller,&watch);CHECK(primary==COG_OK);}
    CHECK(session_online(&f,&watch));
    uint64_t watch_wire=watch.client->requests[0].wire;
    CHECK(session_write(&watch,"watch-mutation\n/shutdown\n")==COG_OK);
    deadline=headless_fixture_clock_ms()+1000U;
    while(watch.client->last_input_serial<2U && headless_fixture_clock_ms()<deadline){primary=session_tick(&f,&controller,&watch);CHECK(primary==COG_OK);}
    CHECK(watch.client->last_input_serial==2U && watch.client->final_input_result==COG_ERROR_DENIED && watch.client->requests[0].wire==watch_wire);
    CHECK(!f.host->shutdown && access(effect,F_OK)!=0);
    primary=session_client_close(&watch,primary);CHECK(primary==COG_OK);
    CHECK(snprintf(path,sizeof(path),"/allow %" PRIu64 "\n",prompt_nonce)>0);
    probe.endpoint=controller.client->endpoint;active_probe=&probe;
    CHECK(session_write(&controller,path)==COG_OK);
    /* Send explicit decision, then stop reading before the host's final reply. */
    deadline=headless_fixture_clock_ms()+1000U;
    while((controller.client->requests[0].phase!=CLI_CLIENT_REQUEST_PENDING || !controller.client->requests[0].sent || controller.client->endpoint->tx_count!=0U) && headless_fixture_clock_ms()<deadline){
        cog_cli_host_runtime_step_t step={0};primary=cog_cli_host_client_step(controller.client,&step);CHECK(primary==COG_OK || primary==COG_ERROR_AGAIN);primary=COG_OK;
    }
    CHECK(controller.client->requests[0].phase==CLI_CLIENT_REQUEST_PENDING);
    original_length=controller.client->requests[0].frame_length;original_wire=controller.client->requests[0].wire;
    original=malloc(original_length);CHECK(original!=NULL);memcpy(original,controller.client->requests[0].frame,original_length);
    bool final=false;deadline=headless_fixture_clock_ms()+3000U;
    while(!final && headless_fixture_clock_ms()<deadline){primary=session_tick(&f,NULL,NULL);CHECK(primary==COG_OK);
        for(size_t i=0U;i<f.host->bounds.command_records;++i){const cli_command_record_t *r=&f.host->commands[i];
            if(r->lease==lease && r->request_wire==original_wire && r->phase==CLI_COMMAND_FINAL){CHECK(r->result==COG_OK);final=true;}}
    }
    CHECK(final && controller.client->requests[0].phase==CLI_CLIENT_REQUEST_PENDING && probe.sends==1U && probe.wire==original_wire && !probe.conflict);
    primary=lose_final(&f,&controller,original_wire,&lost_result_wire);CHECK(primary==COG_OK && lost_result_wire!=0U);
    deadline=headless_fixture_clock_ms()+3000U;
    do{primary=session_tick(&f,NULL,NULL);CHECK(primary==COG_OK);CHECK(cog_cli_projection_snapshot(f.host->projection,&current)==COG_OK);
    }while((current.policy_count!=0U || access(effect,F_OK)!=0 || f.host->executors[f.host->primary_slot].app->active) && headless_fixture_clock_ms()<deadline);
    CHECK(current.policy_count==0U && access(effect,F_OK)==0);
    deadline=headless_fixture_clock_ms()+3000U;
    while((!session_online(&f,&controller) || controller.client->requests[0].phase!=CLI_CLIENT_REQUEST_ACKED) && headless_fixture_clock_ms()<deadline){primary=session_tick(&f,&controller,NULL);CHECK(primary==COG_OK);}
    CHECK(session_online(&f,&controller) && controller.client->requests[0].phase==CLI_CLIENT_REQUEST_ACKED);
    CHECK(controller.client->requests[0].result_wire==lost_result_wire && controller.client->requests[0].wire==original_wire && controller.client->requests[0].frame_length==original_length && memcmp(original,controller.client->requests[0].frame,original_length)==0);
    CHECK(controller.client->lease==lease && session_contains(&controller,"POLICY_DONE") && probe.sends==2U && !probe.conflict);
    active_probe=NULL;
    CHECK(cog_cli_projection_snapshot(f.host->projection,&current)==COG_OK);
    CHECK(same_view(&current,&controller.client->banks[controller.client->current].view));
    /* Expire actual disconnected lease, assign a fresh controller, then let the
       old retained client attempt its known-lease reconnect with explicit input. */
    CHECK(session_disconnect(&controller)==COG_OK);
    deadline=headless_fixture_clock_ms()+3000U;
    while(record(f.host,lease)!=NULL && headless_fixture_clock_ms()<deadline){primary=session_tick(&f,NULL,NULL);CHECK(primary==COG_OK);}
    CHECK(record(f.host,lease)==NULL);
    CHECK(snprintf(path,sizeof(path),"%s/new-controller.ndjson",argv[4])>0);
    CHECK(session_client_open(&replacement,f.ui_path,path,false)==COG_OK);
    deadline=headless_fixture_clock_ms()+3000U;
    while(!session_online(&f,&replacement) && headless_fixture_clock_ms()<deadline){primary=session_tick(&f,&replacement,NULL);CHECK(primary==COG_OK);}
    CHECK(session_online(&f,&replacement) && replacement.client->lease!=lease);
    CHECK(record(f.host,replacement.client->lease)!=NULL && record(f.host,replacement.client->lease)->controller);
    CHECK(session_write(&controller,"expired-mutation\n")==COG_OK);
    cog_result_t rejected=COG_OK;deadline=headless_fixture_clock_ms()+3000U;
    while(rejected==COG_OK && headless_fixture_clock_ms()<deadline){rejected=session_tick(&f,&controller,&replacement);}
    CHECK(rejected==COG_ERROR_DENIED || rejected==COG_ERROR_INVALID_STATE);
    CHECK(!f.host->shutdown && f.host->primary==COG_OK);
    CHECK(cog_cli_projection_snapshot(f.host->projection,&current)==COG_OK && current.policy_count==0U);
    /* The client's own refusal is expected; retain success for cleanup. */
    (void)cog_cli_host_client_close(&controller.client,rejected);
finish:
    active_probe=NULL;
    free(original);
    primary=session_client_close(&controller,primary);
    primary=session_client_close(&watch,primary);
    primary=session_client_close(&replacement,primary);
    primary=headless_fixture_close(&f,primary,&complete);
    if(primary!=COG_OK || !complete){fprintf(stderr,"reconnect outcome=%d cleanup=%d\n",(int)primary,(int)complete);return 1;}
    puts("actual reconnect/GAP/ASK/retained command/lease/watch: PASS");return 0;
}

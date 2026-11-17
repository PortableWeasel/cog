#include "resource_exchange.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static void wipe(void *data,size_t n) {
    volatile unsigned char *p=data;
    while(n--!=0U) *p++=0U;
}
static size_t minimum(size_t a,size_t b) { return a<b?a:b; }
size_t cli_resource_frame_index(cog_cli_control_kind_t kind) {
    switch(kind) {
    case COG_CLI_CONTROL_RESOURCE_REQUEST:return 0U;
    case COG_CLI_CONTROL_RESOURCE_RESULT:return 1U;
    case COG_CLI_CONTROL_RESOURCE_COMMIT:return 2U;
    case COG_CLI_CONTROL_RESOURCE_READY:return 3U;
    case COG_CLI_CONTROL_RESOURCE_CONSUMED:return 4U;
    default:return CLI_RESOURCE_FRAME_COUNT;
    }
}
static bool identity(const cog_cli_instruction_identity_t *a,
    const cog_cli_instruction_identity_t *b) {
    return a->catalog_generation==b->catalog_generation && a->snapshot_id==b->snapshot_id &&
        memcmp(a->sha256,b->sha256,sizeof(a->sha256))==0;
}
cog_result_t cli_resource_exchange_new(cli_resource_exchange_t **out,
    size_t frame_bytes,size_t instruction_bytes) {
    if(out==NULL)return COG_ERROR_NULL_PTR;
    *out=NULL;
    if(frame_bytes<512U || instruction_bytes==0U || instruction_bytes>SIZE_MAX-512U)
        return COG_ERROR_INVALID_ARG;
    cli_resource_exchange_t *x=calloc(1U,sizeof(*x));
    if(x==NULL)return COG_ERROR_NOMEM;
    x->frames[0].capacity=minimum(frame_bytes,3U*65536U+1024U);
    x->frames[1].capacity=minimum(frame_bytes,instruction_bytes+512U);
    for(size_t i=2U;i<CLI_RESOURCE_FRAME_COUNT;i++)x->frames[i].capacity=512U;
    x->request_capacity=x->frames[0].capacity;
    x->result_capacity=x->frames[1].capacity;
    x->scratch_capacity=x->request_capacity>x->result_capacity?x->request_capacity:x->result_capacity;
    for(size_t i=0U;i<CLI_RESOURCE_FRAME_COUNT;i++) {
        x->frames[i].bytes=calloc(x->frames[i].capacity,1U);
        if(x->frames[i].bytes==NULL)goto fail;
    }
    x->request_storage=calloc(x->request_capacity,1U);
    x->result_storage=calloc(x->result_capacity,1U);
    x->scratch=calloc(x->scratch_capacity,1U);
    if(x->request_storage==NULL || x->result_storage==NULL || x->scratch==NULL)goto fail;
    *out=x;return COG_OK;
fail:
    cli_resource_exchange_free(&x);return COG_ERROR_NOMEM;
}
void cli_resource_exchange_reset(cli_resource_exchange_t *x) {
    if(x==NULL)return;
    if(x->frames[0].wire>x->retired_request)x->retired_request=x->frames[0].wire;
    for(size_t i=0U;i<CLI_RESOURCE_FRAME_COUNT;i++) {
        cli_resource_frame_t *f=&x->frames[i];
        if(f->wire>x->retired_wire[i])x->retired_wire[i]=f->wire;
        if(f->bytes!=NULL)wipe(f->bytes,f->capacity);
        f->length=0U;f->wire=0U;f->reply_to=0U;
    }
    if(x->request_storage!=NULL)wipe(x->request_storage,x->request_capacity);
    if(x->result_storage!=NULL)wipe(x->result_storage,x->result_capacity);
    if(x->scratch!=NULL)wipe(x->scratch,x->scratch_capacity);
    memset(&x->request,0,sizeof(x->request));memset(&x->result,0,sizeof(x->result));
    memset(&x->commit,0,sizeof(x->commit));memset(&x->ready,0,sizeof(x->ready));
    x->deadline_ms=0U;x->copied_wire=0U;x->next_send_ms=0U;x->send_index=0U;
    x->active=false;x->frozen=false;x->send_pending=false;
}
void cli_resource_exchange_free(cli_resource_exchange_t **owner) {
    if(owner==NULL || *owner==NULL)return;
    cli_resource_exchange_t *x=*owner;
    cli_resource_exchange_reset(x);
    for(size_t i=0U;i<CLI_RESOURCE_FRAME_COUNT;i++)free(x->frames[i].bytes);
    free(x->request_storage);free(x->result_storage);free(x->scratch);
    wipe(x,sizeof(*x));free(x);*owner=NULL;
}
static cog_result_t validate_chain(const cli_resource_exchange_t *x,size_t i,
    const cog_cli_control_message_t *m,const cog_cli_instruction_resource_t *body) {
    static const cog_cli_instruction_resource_kind_t kinds[CLI_RESOURCE_FRAME_COUNT]={
        COG_CLI_INSTRUCTION_RESOURCE_REQUEST,COG_CLI_INSTRUCTION_RESOURCE_RESULT,
        COG_CLI_INSTRUCTION_RESOURCE_COMMIT,COG_CLI_INSTRUCTION_RESOURCE_READY,
        COG_CLI_INSTRUCTION_RESOURCE_CONSUMED};
    if(m->lane!=COG_CLI_LANE_RESOURCE || body->kind!=kinds[i] || m->wire_id==0U)
        return COG_ERROR_PROTOCOL;
    if(i==0U) {
        if(m->reply_to!=0U || x->active)return COG_ERROR_PROTOCOL;
        return m->wire_id<=x->retired_request?COG_ERROR_AGAIN:COG_OK;
    }
    if(!x->active || x->frames[i-1U].length==0U ||
       m->reply_to!=x->frames[i-1U].wire || !identity(&body->context,&x->request.context))
        return COG_ERROR_PROTOCOL;
    if(i==2U) {
        const cog_cli_instruction_identity_t *expected=&x->result.candidate.identity;
        if(!identity(&body->candidate.identity,expected))return COG_ERROR_PROTOCOL;
        if(x->result.result!=COG_OK && body->outcome!=COG_CLI_INSTRUCTION_COMMIT_REFUSED)
            return COG_ERROR_PROTOCOL;
        if(body->outcome==COG_CLI_INSTRUCTION_COMMIT_UNCHANGED &&
            !identity(expected,&x->request.context))return COG_ERROR_PROTOCOL;
        if((body->outcome==COG_CLI_INSTRUCTION_COMMIT_SAVED ||
            body->outcome==COG_CLI_INSTRUCTION_COMMIT_UNCERTAIN) &&
            identity(expected,&x->request.context))return COG_ERROR_PROTOCOL;
    }
    if(i==3U && (!identity(&body->candidate.identity,&x->commit.candidate.identity) ||
        body->outcome!=x->commit.outcome ||
        body->checkpoint_generation!=x->commit.checkpoint_generation))return COG_ERROR_PROTOCOL;
    return COG_OK;
}
cog_result_t cli_resource_exchange_receive(cli_resource_exchange_t *x,
    const cog_cli_control_message_t *m,const void *frame,size_t length) {
    if(x==NULL || m==NULL || frame==NULL)return COG_ERROR_NULL_PTR;
    size_t i=cli_resource_frame_index(m->kind),required=0U;
    if(i==CLI_RESOURCE_FRAME_COUNT)return COG_ERROR_PROTOCOL;
    cli_resource_frame_t *f=&x->frames[i];
    if(f->length!=0U && f->wire==m->wire_id)return f->length==length &&
        memcmp(f->bytes,frame,length)==0?COG_ERROR_AGAIN:COG_ERROR_PROTOCOL;
    if(m->wire_id<=x->retired_wire[i])return COG_ERROR_AGAIN;
    if(f->length!=0U)return COG_ERROR_PROTOCOL;
    if(length>f->capacity)return COG_ERROR_LIMIT;
    /* Predecessor and old identity validation is repeated after typed decoding.
       REQUEST/RESULT arenas are not borrowed until their record is accepted. */
    unsigned char *arena=i==0U?x->request_storage:i==1U?x->result_storage:x->scratch;
    size_t capacity=i==0U?x->request_capacity:i==1U?x->result_capacity:x->scratch_capacity;
    cog_cli_instruction_resource_t body;
    cog_result_t rc=cog_cli_instruction_resource_decode(m->payload,m->payload_len,
        &body,arena,capacity,&required);
    if(rc!=COG_OK)return rc;
    rc=validate_chain(x,i,m,&body);
    if(rc!=COG_OK)return rc;
    memcpy(f->bytes,frame,length);f->length=length;f->wire=m->wire_id;f->reply_to=m->reply_to;
    if(i==0U){x->request=body;x->active=true;}
    else if(i==1U)x->result=body;
    else if(i==2U)x->commit=body;
    else if(i==3U)x->ready=body;
    return COG_OK;
}
cog_result_t cli_resource_exchange_build(cli_resource_exchange_t *x,
    const cog_cli_control_message_t *envelope,const cog_cli_instruction_resource_t *body) {
    if(x==NULL || envelope==NULL || body==NULL)return COG_ERROR_NULL_PTR;
    size_t i=cli_resource_frame_index(envelope->kind),required=0U;
    if(i==CLI_RESOURCE_FRAME_COUNT)return COG_ERROR_INVALID_ARG;
    cog_result_t rc=validate_chain(x,i,envelope,body);
    if(rc!=COG_OK)return rc;
    if(x->frames[i].length!=0U)return COG_ERROR_INVALID_STATE;
    rc=cog_cli_instruction_resource_encode(body,x->scratch,x->scratch_capacity,&required);
    if(rc!=COG_OK)return rc;
    cog_cli_control_message_t m=*envelope;
    m.payload=x->scratch;m.payload_len=required;
    cli_resource_frame_t *f=&x->frames[i];
    size_t length=0U;
    rc=cog_cli_control_encode(&m,f->bytes,f->capacity,&length);
    if(rc!=COG_OK)return rc;
    /* Copy inner decode before publishing length; exact full frame already owns
       bytes. receive's memcpy source==destination is explicitly avoided here. */
    unsigned char *arena=i==0U?x->request_storage:i==1U?x->result_storage:NULL;
    size_t capacity=i==0U?x->request_capacity:i==1U?x->result_capacity:0U;
    cog_cli_instruction_resource_t retained=*body;
    if(i<2U)rc=cog_cli_instruction_resource_decode(m.payload,m.payload_len,&retained,
        arena,capacity,&required);
    if(rc!=COG_OK){wipe(f->bytes,f->capacity);return rc;}
    f->length=length;f->wire=m.wire_id;f->reply_to=m.reply_to;
    if(i==0U){x->request=retained;x->active=true;}
    else if(i==1U)x->result=retained;
    else if(i==2U)x->commit=retained;
    else if(i==3U)x->ready=retained;
    return COG_OK;
}

void cli_resource_exchange_rebind(cli_resource_exchange_t *x) {
    if(x==NULL)return;
    cli_resource_exchange_reset(x);
    x->retired_request=0U;
    memset(x->retired_wire,0,sizeof(x->retired_wire));
}

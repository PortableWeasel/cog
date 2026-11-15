#include "cog_cli/cli.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint32_t rng = UINT32_C(0x7803b91a);
static uint32_t random_word(void)
{ rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
static void candidate(const unsigned char *wire, size_t length)
{
    cog_cli_display_snapshot_t out, saved;
    cog_cli_display_item_t items[32], saved_items[32];
    char text[1024], saved_text[1024];
    unsigned char encoded[2048];
    size_t ni = 111, nt = 222, required;
    cog_result_t rc;
    memset(&out, 0xa5, sizeof(out)); memcpy(&saved, &out, sizeof(out));
    memset(items, 0x5a, sizeof(items)); memcpy(saved_items, items, sizeof(items));
    memset(text, 0x35, sizeof(text)); memcpy(saved_text, text, sizeof(text));
    rc = cog_cli_display_snapshot_decode(wire,length,&out,items,32,text,sizeof(text),&ni,&nt);
    if(rc==COG_OK) {
        assert(cog_cli_display_snapshot_encode(&out,encoded,sizeof(encoded),&required)==COG_OK);
        assert(required==length && memcmp(encoded,wire,length)==0);
        assert(ni==out.item_count);
        size_t expected=strlen(out.session_id)+1;
        for(size_t i=0;i<out.item_count;i++)expected+=out.items[i].text_len+1;
        assert(nt==expected);
        /* Either destination independently too small preserves all outputs. */
        memcpy(&saved,&out,sizeof(out));memcpy(saved_items,items,sizeof(items));memcpy(saved_text,text,sizeof(text));
        if(ni!=0) {
            assert(cog_cli_display_snapshot_decode(wire,length,&out,items,ni-1,text,sizeof(text),&ni,&nt)==COG_ERROR_OVERFLOW);
            assert(memcmp(&saved,&out,sizeof(out))==0 && memcmp(saved_items,items,sizeof(items))==0 && memcmp(saved_text,text,sizeof(text))==0);
        }
        assert(cog_cli_display_snapshot_decode(wire,length,&out,items,32,text,nt-1,&ni,&nt)==COG_ERROR_OVERFLOW);
        assert(memcmp(&saved,&out,sizeof(out))==0 && memcmp(saved_items,items,sizeof(items))==0 && memcmp(saved_text,text,sizeof(text))==0);
    } else {
        assert(rc==COG_ERROR_PROTOCOL || rc==COG_ERROR_UNSUPPORTED);
        assert(ni==111 && nt==222);
        assert(memcmp(&saved,&out,sizeof(out))==0 && memcmp(saved_items,items,sizeof(items))==0 && memcmp(saved_text,text,sizeof(text))==0);
    }
}
int main(void)
{
    static const unsigned char golden[]={
        'C','G','D','1',1,'s', 1,2,3,4,5,6,7,8, 0,0,0,0,0,0,0,9,
        0,0,0,1, 0,0,0,0,0,0,0,7, 2,1, 0,0,0,2, 0xc3,0xa9};
    cog_cli_display_item_t item={7,COG_MESSAGE_ROLE_ASSISTANT,COG_MESSAGE_INCOMPLETE,"\xc3\xa9",2};
    cog_cli_display_snapshot_t snapshot={"s",UINT64_C(0x0102030405060708),9,&item,1};
    unsigned char wire[2048], altered[2048], before[2048];
    size_t n=0, required;
    assert(cog_cli_display_snapshot_encode(&snapshot,wire,sizeof(wire),&n)==COG_OK);
    assert(n==sizeof(golden)&&memcmp(wire,golden,n)==0);
    for(size_t form=0;form<3;form++) {
        if(form==1){snapshot.item_count=0;snapshot.items=NULL;}
        if(form==2){snapshot.items=&item;snapshot.item_count=1;item.text="";item.text_len=0;item.status=COG_MESSAGE_CANCELLED;item.role=COG_MESSAGE_ROLE_TOOL;}
        assert(cog_cli_display_snapshot_encode(&snapshot,wire,sizeof(wire),&n)==COG_OK);
        candidate(wire,n);
        for(size_t i=0;i<n;i++) {
            candidate(wire,i);
            for(unsigned bit=0;bit<8;bit++) {
                memcpy(altered,wire,n);altered[i]^=(unsigned char)(1u<<bit);candidate(altered,n);
            }
        }
        memcpy(altered,wire,n);altered[n]=0;candidate(altered,n+1);
        for(size_t i=0;i<10000;i++) {
            size_t length=(size_t)(random_word()%256u);
            memcpy(altered,wire,n);
            if(length>n)memset(altered+n,0,length-n);
            for(unsigned j=0;j<4;j++)altered[random_word()%256u]=(unsigned char)random_word();
            candidate(altered,length);
        }
    }
    /* Scalar role/status and embedded UTF-8 requirements, independent of decode. */
    snapshot.items=&item;snapshot.item_count=1;item.text="ok";item.text_len=2;
    for(int role=-1;role<=5;role++)for(int status=-1;status<=6;status++) {
        item.role=(cog_message_role_t)role;item.status=(cog_message_status_t)status;
        int valid=role>=COG_MESSAGE_ROLE_SYSTEM&&role<=COG_MESSAGE_ROLE_TOOL&&status>=COG_MESSAGE_COMPLETE&&status<=COG_MESSAGE_FAILED;
        memset(wire,0x5a,sizeof(wire));memcpy(before,wire,sizeof(wire));required=333;
        cog_result_t rc=cog_cli_display_snapshot_encode(&snapshot,wire,sizeof(wire),&required);
        assert(rc==(valid?COG_OK:COG_ERROR_INVALID_ARG));
        if(!valid)assert(required==333&&memcmp(wire,before,sizeof(wire))==0);
    }
    puts("CGD1 independent golden, scalar, mutation and transactional checks passed");return 0;
}

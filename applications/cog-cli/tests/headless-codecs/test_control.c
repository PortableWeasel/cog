/** @file test_control.c @brief CGL1/command golden, malformed, owned-copy and transactional tests. */
#include "control.h"
#include <stdbool.h>
#include "check.h"

static void envelope_bad(const void *p, size_t n, cog_result_t error)
{
    cog_cli_control_message_t m; unsigned char storage[512]; size_t need=999U;
    test_fill(&m,sizeof(m)); test_fill(storage,sizeof(storage));
    CHECK(cog_cli_control_decode(p,n,&m,storage,0,&need)==error);
    test_unchanged(&m,sizeof(m)); test_unchanged(storage,sizeof(storage)); CHECK(need==999U);
}
static void command_bad(const void *p, size_t n)
{
    cog_cli_command_t c; unsigned char storage[512]; size_t need=999U;
    test_fill(&c,sizeof(c)); test_fill(storage,sizeof(storage));
    CHECK(cog_cli_command_decode(p,n,&c,storage,0,&need)==COG_ERROR_PROTOCOL);
    test_unchanged(&c,sizeof(c)); test_unchanged(storage,sizeof(storage)); CHECK(need==999U);
}
int main(int argc, char **argv)
{
    (void)test_negative_control(argc,argv);
    static const unsigned char golden[86] = {[0]='C',[1]='G',[2]='L',[3]='1',[4]=2,[5]=1,
        [13]=1,[21]=2,[29]=3,[78]=1,[79]='r',[83]=2,[84]=0,[85]=255};
    static const unsigned char payload[] = {0,255};
    cog_cli_control_message_t m={.kind=COG_CLI_CONTROL_UI_COMMAND,.lane=COG_CLI_LANE_COMMAND,
        .runtime_incarnation=1,.sender_incarnation=2,.wire_id=3,.session_id="r",.payload=payload,.payload_len=2};
    unsigned char bytes[512], storage[512]; size_t need=999U;
    test_fill(bytes,sizeof(bytes));
    CHECK(cog_cli_control_encode(&m,bytes,85,&need)==COG_ERROR_OVERFLOW); CHECK(need==86U); test_unchanged(bytes,sizeof(bytes));
    OK(cog_cli_control_encode(&m,bytes,sizeof(bytes),&need)); CHECK(need==86U && memcmp(bytes,golden,86U)==0);
    test_unchanged(bytes+need,sizeof(bytes)-need);
    cog_cli_control_message_t out;
    OK(cog_cli_control_decode(bytes,need,&out,storage,sizeof(storage),&need)); CHECK(need==4U);
    memset(bytes,0,86U); CHECK(strcmp(out.session_id,"r")==0 && memcmp(out.payload,payload,2U)==0);
    test_fill(&out,sizeof(out)); test_fill(storage,sizeof(storage));
    CHECK(cog_cli_control_decode(golden,86,&out,storage,3,&need)==COG_ERROR_OVERFLOW); CHECK(need==4U);
    test_unchanged(&out,sizeof(out)); test_unchanged(storage,sizeof(storage));
    for (size_t n=0;n<86U;++n) envelope_bad(golden,n,COG_ERROR_PROTOCOL);
    memcpy(bytes,golden,86U); envelope_bad(bytes,87,COG_ERROR_PROTOCOL);
    bytes[0]='X'; envelope_bad(bytes,86,COG_ERROR_PROTOCOL); bytes[0]='C'; bytes[3]='2'; envelope_bad(bytes,4,COG_ERROR_UNSUPPORTED);
    memcpy(bytes,golden,86U); bytes[4]=0; envelope_bad(bytes,86,COG_ERROR_PROTOCOL);
    bytes[4]=30; envelope_bad(bytes,86,COG_ERROR_PROTOCOL); bytes[4]=2; bytes[5]=11; envelope_bad(bytes,86,COG_ERROR_PROTOCOL);
    memcpy(bytes,golden,86U); bytes[13]=0; envelope_bad(bytes,86,COG_ERROR_PROTOCOL);
    memcpy(bytes,golden,86U); bytes[45]=1; envelope_bad(bytes,86,COG_ERROR_PROTOCOL);
    memcpy(bytes,golden,86U); bytes[69]=1; envelope_bad(bytes,86,COG_ERROR_PROTOCOL);
    memcpy(bytes,golden,86U); bytes[79]='/'; envelope_bad(bytes,86,COG_ERROR_PROTOCOL);
    memcpy(bytes,golden,86U); bytes[79]=0; envelope_bad(bytes,86,COG_ERROR_PROTOCOL);
    memcpy(bytes,golden,86U); bytes[80]=255; envelope_bad(bytes,86,COG_ERROR_PROTOCOL);
    for (int kind=1;kind<=29;++kind) for (int lane=1;lane<=10;++lane) {
        m.kind=(cog_cli_control_kind_t)kind; m.lane=(cog_cli_control_lane_t)lane;
        OK(cog_cli_control_encode(&m,bytes,sizeof(bytes),&need));
        OK(cog_cli_control_decode(bytes,need,&out,storage,sizeof(storage),&need)); CHECK(out.kind==m.kind && out.lane==m.lane);
    }
    m.kind=COG_CLI_CONTROL_UI_HELLO; m.runtime_incarnation=0; m.session_id=""; m.payload=NULL; m.payload_len=0;
    OK(cog_cli_control_encode(&m,bytes,sizeof(bytes),&need)); CHECK(need==83U);
    OK(cog_cli_control_decode(bytes,need,&out,storage,sizeof(storage),&need)); CHECK(need==1U && out.session_id[0]=='\0');
    m.kind=COG_CLI_CONTROL_CHILD_UPDATE; m.runtime_incarnation=1; m.session_id="child";
    m.worker=(cog_worker_id_t){7,8}; m.owner_peer=9; m.agent_id=UINT64_MAX; m.execution_epoch=UINT64_MAX;
    OK(cog_cli_control_encode(&m,bytes,sizeof(bytes),&need));
    OK(cog_cli_control_decode(bytes,need,&out,storage,sizeof(storage),&need)); CHECK(out.agent_id==UINT64_MAX);
    static const char *const bad[]={"\xc0\x80","\x80","\xe2\x82","\xed\xa0\x80","\xf4\x90\x80\x80"};
    for(size_t i=0;i<sizeof(bad)/sizeof(bad[0]);++i) {
        m.session_id=bad[i]; need=999U; CHECK(cog_cli_control_encode(&m,NULL,0,&need)==COG_ERROR_INVALID_ARG); CHECK(need==999U);
    }
    m.session_id="\xe2\x98\x83"; OK(cog_cli_control_encode(&m,bytes,sizeof(bytes),&need));
    char max_id[129]; memset(max_id,'a',127U); max_id[127]='\0'; m.session_id=max_id;
    OK(cog_cli_control_encode(&m,bytes,sizeof(bytes),&need));
    OK(cog_cli_control_decode(bytes,need,&out,storage,sizeof(storage),&need)); CHECK(strlen(out.session_id)==127U);
    max_id[127]='a'; max_id[128]='\0'; CHECK(cog_cli_control_encode(&m,NULL,0,&need)==COG_ERROR_INVALID_ARG);
    m.session_id="child";
    m.payload_len=(size_t)UINT32_MAX+1U; m.payload="x"; CHECK(cog_cli_control_encode(&m,NULL,0,&need)==COG_ERROR_INVALID_ARG);
    m.payload_len=0; m.payload=NULL;
    CHECK(cog_cli_control_encode(&m,&m,sizeof(m),&need)==COG_ERROR_INVALID_ARG);
    memcpy(bytes,golden,86U); CHECK(cog_cli_control_decode(bytes,86,&out,bytes,sizeof(bytes),&need)==COG_ERROR_PROTOCOL);
    CHECK(cog_cli_control_encode(NULL,NULL,0,&need)==COG_ERROR_NULL_PTR);

    for(int op=1;op<=9;++op) {
        cog_cli_command_t c={.kind=(cog_cli_command_kind_t)op,.observed_generation=7,.observed_sequence=8};
        unsigned char expected[512]={0}; size_t length=17U;
        expected[0]=(unsigned char)op; expected[8]=7; expected[16]=8;
        switch(op) {
        case 1: c.text="hi"; c.text_len=2; expected[20]=2; memcpy(expected+21,"hi",2); length=23; break;
        case 2: c.node_id=9; c.text="hi"; c.text_len=2; expected[24]=9; expected[28]=2; memcpy(expected+29,"hi",2); length=31; break;
        case 3: c.target_session_id="r"; c.node_id=9; c.call_ordinal=15; c.nonce=10; c.decision=2;
            expected[20]=1; expected[21]='r'; expected[29]=9; expected[31]=15; expected[39]=10; expected[40]=2; length=41; break;
        case 4: c.agent_id=UINT64_MAX; memset(expected+17,255,8); length=25; break;
        case 5: c.reload_field=3; c.text="ref"; c.text_len=3; expected[17]=3; expected[21]=3; memcpy(expected+22,"ref",3); length=25; break;
        case 6: c.target=(cog_worker_id_t){9,10}; expected[24]=9; expected[32]=10; length=33; break;
        case 9: c.node_id=9; expected[24]=9; length=25; break;
        default: break;
        }
        test_fill(bytes,sizeof(bytes));
        CHECK(cog_cli_command_encode(&c,bytes,length-1U,&need)==COG_ERROR_OVERFLOW); CHECK(need==length); test_unchanged(bytes,sizeof(bytes));
        OK(cog_cli_command_encode(&c,bytes,sizeof(bytes),&need)); CHECK(need==length && memcmp(bytes,expected,length)==0);
        cog_cli_command_t d; OK(cog_cli_command_decode(expected,length,&d,storage,sizeof(storage),&need)); CHECK(d.kind==c.kind);
        if(c.text!=NULL) CHECK(d.text_len==c.text_len && memcmp(d.text,c.text,c.text_len)==0);
        for(size_t n=0;n<length;++n) command_bad(expected,n);
        expected[length]=0; command_bad(expected,length+1U);
        c.observed_sequence=0; need=999U; CHECK(cog_cli_command_encode(&c,NULL,0,&need)==COG_ERROR_INVALID_ARG); CHECK(need==999U);
        c.observed_sequence=8; c.decision=99; CHECK(cog_cli_command_encode(&c,NULL,0,&need)==COG_ERROR_INVALID_ARG);
    }
    cog_cli_command_t c={.kind=COG_CLI_COMMAND_SUBMIT,.observed_generation=1,.observed_sequence=1,.text="\xe2\x98\x83",.text_len=3};
    OK(cog_cli_command_encode(&c,bytes,sizeof(bytes),&need));
    size_t length=need; cog_cli_command_t decoded; test_fill(&decoded,sizeof(decoded)); test_fill(storage,sizeof(storage));
    CHECK(cog_cli_command_decode(bytes,length,&decoded,storage,3U,&need)==COG_ERROR_OVERFLOW); CHECK(need==4U);
    test_unchanged(&decoded,sizeof(decoded)); test_unchanged(storage,sizeof(storage));
    CHECK(cog_cli_command_decode(bytes,length,&decoded,bytes,sizeof(bytes),&need)==COG_ERROR_PROTOCOL);
    bytes[21]=0; command_bad(bytes,length);
    bytes[21]=0xed; bytes[22]=0xa0; bytes[23]=0x80; command_bad(bytes,length);
    c.text_len=(size_t)UINT32_MAX+1U; CHECK(cog_cli_command_encode(&c,NULL,0,&need)==COG_ERROR_INVALID_ARG);
    c.text_len=3; CHECK(cog_cli_command_encode(&c,&c,sizeof(c),&need)==COG_ERROR_INVALID_ARG);
    return 0;
}

/** @file test_child_context.c @brief Literal JSON, escaping and exact uint64 provenance tests. */
#include "child_profile.h"
#include <stdbool.h>
#include "check.h"
int main(int argc, char **argv)
{
    (void)test_negative_control(argc, argv);
    uint64_t members[] = {2, UINT64_MAX};
    cog_cli_child_context_t c = {.parent_session_id="root", .parent_call_id="id\n\"\\@",
        .parent_node=1, .runtime_incarnation=3, .group_id=4, .parent_call_ordinal=15,
        .agent_id=2, .execution_epoch=5, .configuration_generation=6, .members=members,.member_count=2};
    static const char expected[] = "{\"cog_child_context\":1,\"parent_session\":\"root\",\"parent_node\":\"1\",\"parent_call\":\"id\\u000a\\\"\\\\@\",\"parent_ordinal\":15,\"runtime\":\"3\",\"group\":\"4\",\"agent\":\"2\",\"attempt\":\"5\",\"configuration\":\"6\",\"members\":[\"2\",\"18446744073709551615\"]}";
    char output[4097]; size_t need=999U;
    test_fill(output,sizeof(output));
    CHECK(cog_cli_child_context_encode(&c,NULL,0,&need)==COG_ERROR_OVERFLOW); CHECK(need==sizeof(expected));
    CHECK(cog_cli_child_context_encode(&c,output,need-1U,&need)==COG_ERROR_OVERFLOW); test_unchanged(output,sizeof(output));
    OK(cog_cli_child_context_encode(&c,output,sizeof(output),&need));
    CHECK(need==sizeof(expected)); CHECK(memcmp(output,expected,need)==0); test_unchanged(output+need,sizeof(output)-need);
    char sid[128], call[256]; memset(sid,1,127U); sid[127]='\0'; memset(call,1,255U); call[255]='\0';
    uint64_t all[16]; for (size_t i=0;i<16U;++i) all[i]=UINT64_MAX-15U+i;
    c.parent_session_id=sid; c.parent_call_id=call; c.members=all; c.member_count=16;
    c.agent_id=all[0]; c.parent_node=UINT64_MAX; c.runtime_incarnation=UINT64_MAX;
    c.group_id=UINT64_MAX; c.execution_epoch=UINT64_MAX; c.configuration_generation=UINT64_MAX;
    OK(cog_cli_child_context_encode(&c,output,sizeof(output),&need)); CHECK(need<=4096U);
    CHECK(strstr(output,"18446744073709551615")!=NULL); CHECK(strstr(output,"\\u0001")!=NULL);
    static const char *const bad[] = {"\xc0\x80","\x80","\xe2\x82","\xed\xa0\x80","\xf4\x90\x80\x80"};
    for (size_t i=0;i<sizeof(bad)/sizeof(bad[0]);++i) {
        c.parent_call_id=bad[i]; need=999U; test_fill(output,sizeof(output));
        CHECK(cog_cli_child_context_encode(&c,output,sizeof(output),&need)==COG_ERROR_INVALID_ARG);
        CHECK(need==999U); test_unchanged(output,sizeof(output));
    }
    c.parent_call_id="\xe2\x98\x83"; OK(cog_cli_child_context_encode(&c,output,sizeof(output),&need));
    all[1]=all[0]; CHECK(cog_cli_child_context_encode(&c,NULL,0,&need)==COG_ERROR_INVALID_ARG); all[1]=all[0]+1U;
    c.agent_id=1; CHECK(cog_cli_child_context_encode(&c,NULL,0,&need)==COG_ERROR_INVALID_ARG); c.agent_id=all[0];
    c.parent_call_ordinal=16; CHECK(cog_cli_child_context_encode(&c,NULL,0,&need)==COG_ERROR_INVALID_ARG); c.parent_call_ordinal=0;
    c.member_count=17; CHECK(cog_cli_child_context_encode(&c,NULL,0,&need)==COG_ERROR_INVALID_ARG); c.member_count=16;
    CHECK(cog_cli_child_context_encode(&c,(char *)(void *)&c,sizeof(c),&need)==COG_ERROR_INVALID_ARG);
    CHECK(cog_cli_child_context_encode(NULL,NULL,0,&need)==COG_ERROR_NULL_PTR);
    return 0;
}

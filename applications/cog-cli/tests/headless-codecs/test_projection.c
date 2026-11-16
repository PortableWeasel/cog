/** @file test_projection.c @brief Exact CGV1 nested transactionality and bounds. */
#include "projection.h"
#include <stdbool.h>
#include "check.h"

typedef struct fixture_offsets { size_t agent, status, result, policy_count, policy_config, policy_ordinal, policy_name; } fixture_offsets_t;
static void number(unsigned char *p,size_t *n,uint64_t value,size_t width)
{ test_put(p+*n,value,width); *n+=width; }
static void literal(unsigned char *p,size_t *n,const char *s,size_t length)
{ memcpy(p+*n,s,length); *n+=length; }
static void string(unsigned char *p,size_t *n,const char *s,size_t length)
{ number(p,n,length,4); literal(p,n,s,length); }
static size_t golden(unsigned char *p,fixture_offsets_t *o)
{
    size_t n=0;
    literal(p,&n,"CGV1",4); number(p,&n,45,4);
    literal(p,&n,"CGD1",4); number(p,&n,4,1); literal(p,&n,"root",4);
    number(p,&n,7,8); number(p,&n,9,8); number(p,&n,1,4);
    number(p,&n,1,8); number(p,&n,1,1); number(p,&n,0,1); string(p,&n,"hi",2);
    CHECK(n==53U); number(p,&n,2,8); number(p,&n,3,8); number(p,&n,1,2);
    o->agent=n; number(p,&n,UINT64_MAX,8); number(p,&n,4,8); number(p,&n,5,8); number(p,&n,2,8);
    string(p,&n,"c",1); o->status=n; number(p,&n,3,1); number(p,&n,UINT32_MAX-15U,4);
    string(p,&n,"!",1); o->result=n; string(p,&n,"r\n",2);
    o->policy_count=n; number(p,&n,1,2); string(p,&n,"root",4); number(p,&n,1,8);
    o->policy_ordinal=n; number(p,&n,15,2); number(p,&n,6,8);
    o->policy_config=n; number(p,&n,2,8); number(p,&n,100,8); o->policy_name=n; string(p,&n,"read",4);
    return n;
}
static void bad(const void *bytes,size_t length,cog_result_t error)
{
    cog_cli_projection_view_t v; cog_cli_display_item_t items[2]; cog_cli_agent_view_t agents[2]; cog_cli_policy_view_t policies[1]; char text[256];
    size_t ni=999,na=999,np=999,nt=999;
    test_fill(&v,sizeof(v)); test_fill(items,sizeof(items)); test_fill(agents,sizeof(agents)); test_fill(policies,sizeof(policies)); test_fill(text,sizeof(text));
    CHECK(cog_cli_projection_decode(bytes,length,&v,items,0,agents,0,policies,0,text,0,&ni,&na,&np,&nt)==error);
    CHECK(ni==999U && na==999U && np==999U && nt==999U);
    test_unchanged(&v,sizeof(v)); test_unchanged(items,sizeof(items)); test_unchanged(agents,sizeof(agents)); test_unchanged(policies,sizeof(policies)); test_unchanged(text,sizeof(text));
}
int main(int argc,char **argv)
{
    (void)test_negative_control(argc,argv);
    unsigned char expected[4096],bytes[4096]; fixture_offsets_t o;
    size_t length=golden(expected,&o),need=999;
    cog_cli_display_item_t item={1,COG_MESSAGE_ROLE_USER,COG_MESSAGE_COMPLETE,"hi",2};
    cog_cli_agent_view_t agent={UINT64_MAX,4,5,2,"c",COG_AGENT_STATUS_FAILED,COG_ERROR_DENIED,"!","r\n"};
    cog_cli_policy_view_t policy={"root",1,6,2,100,15,"read"};
    cog_cli_projection_view_t source={.display={"root",7,9,&item,1},.active_configuration=2,.published_configuration=3,
        .agents=&agent,.agent_count=1,.policies=&policy,.policy_count=1};
    test_fill(bytes,sizeof(bytes));
    CHECK(cog_cli_projection_encode(&source,bytes,length-1U,&need)==COG_ERROR_OVERFLOW); CHECK(need==length); test_unchanged(bytes,sizeof(bytes));
    OK(cog_cli_projection_encode(&source,bytes,sizeof(bytes),&need)); CHECK(need==length && memcmp(bytes,expected,length)==0); test_unchanged(bytes+need,sizeof(bytes)-need);
    cog_cli_projection_view_t v; cog_cli_display_item_t items[2]; cog_cli_agent_view_t agents[16]; cog_cli_policy_view_t policies[1]; char text[4096];
    size_t ni=999,na=999,np=999,nt=999;
    test_fill(&v,sizeof(v)); test_fill(items,sizeof(items)); test_fill(agents,sizeof(agents)); test_fill(policies,sizeof(policies)); test_fill(text,sizeof(text));
    CHECK(cog_cli_projection_decode(expected,length,&v,items,0,agents,0,policies,0,text,0,&ni,&na,&np,&nt)==COG_ERROR_OVERFLOW);
    CHECK(ni==1U && na==1U && np==1U && nt==25U); test_unchanged(&v,sizeof(v)); test_unchanged(items,sizeof(items)); test_unchanged(agents,sizeof(agents)); test_unchanged(policies,sizeof(policies)); test_unchanged(text,sizeof(text));
    OK(cog_cli_projection_decode(expected,length,&v,items,2,agents,16,policies,1,text,sizeof(text),&ni,&na,&np,&nt));
    CHECK(v.display.generation==7U && v.display.through_sequence==9U && v.active_configuration==2U && v.published_configuration==3U);
    CHECK(v.agents==agents && v.policies==policies && v.display.items==items);
    CHECK(strcmp(v.display.session_id,"root")==0 && strcmp(items[0].text,"hi")==0 && agents[0].agent_id==UINT64_MAX);
    CHECK(agents[0].outcome==COG_ERROR_DENIED && strcmp(agents[0].result_text,"r\n")==0 && policies[0].call_ordinal==15U);
    memset(expected,0,length); CHECK(strcmp(agents[0].session_id,"c")==0 && strcmp(policies[0].tool_name,"read")==0);
    length=golden(expected,&o);
    for(size_t n=0;n<length;++n) bad(expected,n,COG_ERROR_PROTOCOL);
    memcpy(bytes,expected,length); bytes[length]=0; bad(bytes,length+1U,COG_ERROR_PROTOCOL);
    bytes[0]='X'; bad(bytes,length,COG_ERROR_PROTOCOL); bytes[0]='C'; bytes[3]='2'; bad(bytes,4,COG_ERROR_UNSUPPORTED);
    memcpy(bytes,expected,length); bytes[8]='X'; bad(bytes,length,COG_ERROR_PROTOCOL);
    memcpy(bytes,expected,length); memset(bytes+25,0,8); bad(bytes,length,COG_ERROR_PROTOCOL);
    memcpy(bytes,expected,length); memset(bytes+o.agent,0,8); bad(bytes,length,COG_ERROR_PROTOCOL);
    memcpy(bytes,expected,length); bytes[o.status]=0; bad(bytes,length,COG_ERROR_PROTOCOL);
    memcpy(bytes,expected,length); bytes[o.status]=6; bad(bytes,length,COG_ERROR_PROTOCOL);
    memcpy(bytes,expected,length); memset(bytes+o.status+1U,0,4); bad(bytes,length,COG_ERROR_PROTOCOL);
    memcpy(bytes,expected,length); bytes[o.result+4U]=0; bad(bytes,length,COG_ERROR_PROTOCOL);
    memcpy(bytes,expected,length); bytes[o.policy_count+1U]=2; bad(bytes,length,COG_ERROR_PROTOCOL);
    memcpy(bytes,expected,length); bytes[o.policy_ordinal+1U]=16; bad(bytes,length,COG_ERROR_PROTOCOL);
    memcpy(bytes,expected,length); bytes[o.policy_config+7U]=3; bad(bytes,length,COG_ERROR_PROTOCOL);
    memcpy(bytes,expected,length); bytes[length-1U]=0x80; bad(bytes,length,COG_ERROR_PROTOCOL);
    memcpy(bytes,expected,length); bytes[o.policy_name+3U]=0; bad(bytes,length,COG_ERROR_PROTOCOL);
    /* A bad later policy must not install a good nested CGD1 first, even with ample arenas. */
    test_fill(&v,sizeof(v)); test_fill(items,sizeof(items)); test_fill(agents,sizeof(agents)); test_fill(policies,sizeof(policies)); test_fill(text,sizeof(text)); ni=na=np=nt=999;
    CHECK(cog_cli_projection_decode(bytes,length,&v,items,2,agents,16,policies,1,text,sizeof(text),&ni,&na,&np,&nt)==COG_ERROR_PROTOCOL);
    test_unchanged(&v,sizeof(v)); test_unchanged(items,sizeof(items)); test_unchanged(agents,sizeof(agents)); test_unchanged(policies,sizeof(policies)); test_unchanged(text,sizeof(text)); CHECK(nt==999U);
    /* Whole meaningful agent predicates and ordered IDs, including UINT64_MAX. */
    source.policy_count=0; source.policies=NULL;
    for(int status=0;status<=5;++status) for(int result=-19;result<=1;++result) {
        agent.status=(cog_agent_status_t)status; agent.outcome=(cog_result_t)result; agent.result_text=status<2?"":"r";
        bool valid=(status<=2 && result==0)||(status>=3 && result<0 && result>=-18);
        CHECK(cog_cli_projection_encode(&source,bytes,sizeof(bytes),&need)==(valid?COG_OK:COG_ERROR_INVALID_ARG));
        if(valid) OK(cog_cli_projection_decode(bytes,need,&v,items,2,agents,16,policies,1,text,sizeof(text),&ni,&na,&np,&nt));
    }
    /* Each independent shortage reports all counts and preserves all destinations. */
    agent=(cog_cli_agent_view_t){UINT64_MAX,4,5,2,"c",COG_AGENT_STATUS_FAILED,COG_ERROR_DENIED,"!","r\n"};
    source.policies=&policy; source.policy_count=1;
    for(size_t short_at=0;short_at<4U;++short_at) {
        test_fill(&v,sizeof(v)); test_fill(items,sizeof(items)); test_fill(agents,sizeof(agents)); test_fill(policies,sizeof(policies)); test_fill(text,sizeof(text));
        CHECK(cog_cli_projection_decode(expected,length,&v,items,short_at==0U?0U:2U,agents,short_at==1U?0U:16U,
            policies,short_at==2U?0U:1U,text,short_at==3U?24U:sizeof(text),&ni,&na,&np,&nt)==COG_ERROR_OVERFLOW);
        CHECK(ni==1U && na==1U && np==1U && nt==25U);
        test_unchanged(&v,sizeof(v)); test_unchanged(items,sizeof(items)); test_unchanged(agents,sizeof(agents)); test_unchanged(policies,sizeof(policies)); test_unchanged(text,sizeof(text));
    }
    /* Late invalid source preserves both required and the entire output region. */
    policy.configuration_generation=3; need=999U; test_fill(bytes,sizeof(bytes));
    CHECK(cog_cli_projection_encode(&source,bytes,sizeof(bytes),&need)==COG_ERROR_INVALID_ARG); CHECK(need==999U); test_unchanged(bytes,sizeof(bytes));
    policy.configuration_generation=2;
    char max_name[257]; memset(max_name,'x',255U); max_name[255]='\0'; policy.tool_name=max_name;
    OK(cog_cli_projection_encode(&source,bytes,sizeof(bytes),&need));
    max_name[255]='x'; max_name[256]='\0'; CHECK(cog_cli_projection_encode(&source,NULL,0,&need)==COG_ERROR_INVALID_ARG);
    policy.tool_name="read"; source.policies=NULL; source.policy_count=0;
    cog_cli_agent_view_t many[16];
    for(size_t i=0;i<16U;++i) many[i]=(cog_cli_agent_view_t){i+1U,1,1,2,"c",COG_AGENT_STATUS_COMPLETED,COG_OK,"",""};
    source.agents=many; source.agent_count=16;
    OK(cog_cli_projection_encode(&source,bytes,sizeof(bytes),&need));
    OK(cog_cli_projection_decode(bytes,need,&v,items,2,agents,16,policies,1,text,sizeof(text),&ni,&na,&np,&nt)); CHECK(na==16U);
    many[1].agent_id=1; CHECK(cog_cli_projection_encode(&source,NULL,0,&need)==COG_ERROR_INVALID_ARG); many[1].agent_id=2;
    source.agent_count=17; CHECK(cog_cli_projection_encode(&source,NULL,0,&need)==COG_ERROR_INVALID_ARG); source.agent_count=16;
    source.display.through_sequence=0; CHECK(cog_cli_projection_encode(&source,NULL,0,&need)==COG_ERROR_INVALID_ARG);
    CHECK(cog_cli_display_snapshot_encode(&source.display,NULL,0,&need)==COG_ERROR_OVERFLOW); source.display.through_sequence=9;
    source.agent_count=0; source.agents=NULL; source.display.item_count=0; source.display.items=NULL;
    OK(cog_cli_projection_encode(&source,bytes,sizeof(bytes),&need));
    OK(cog_cli_projection_decode(bytes,need,&v,NULL,0,NULL,0,NULL,0,text,sizeof(text),&ni,&na,&np,&nt)); CHECK(ni==0U && na==0U && np==0U && nt==5U);
    CHECK(cog_cli_projection_encode(&source,&source,sizeof(source),&need)==COG_ERROR_INVALID_ARG);
    CHECK(cog_cli_projection_decode(expected,length,&v,items,2,agents,16,policies,1,(char *)(void *)expected,sizeof(expected),&ni,&na,&np,&nt)==COG_ERROR_PROTOCOL);
    CHECK(cog_cli_projection_decode(NULL,0,&v,NULL,0,NULL,0,NULL,0,NULL,0,&ni,&na,&np,&nt)==COG_ERROR_NULL_PTR);
    return 0;
}

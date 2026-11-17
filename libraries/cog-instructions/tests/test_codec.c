/** @file test_codec.c @brief Canonical CGIM golden bytes and adversarial decode. */
#include <cog/instruction_codec.h>
#include <cog/tool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#define CHECK(x) do { if (!(x)) { (void)fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x); abort(); } } while(0)
static volatile bool no_alloc;
static volatile size_t calls, fail_at;
void *__real_malloc(size_t n);
void *__real_calloc(size_t n,size_t z);
void *__real_realloc(void *p,size_t n);
void *__wrap_malloc(size_t n);
void *__wrap_calloc(size_t n,size_t z);
void *__wrap_realloc(void *p,size_t n);
static bool fail(void) { ++calls; return no_alloc || (fail_at!=0U && calls==fail_at); }
void *__wrap_malloc(size_t n) { return fail()?NULL:__real_malloc(n); }
void *__wrap_calloc(size_t n,size_t z) { return fail()?NULL:__real_calloc(n,z); }
void *__wrap_realloc(void *p,size_t n) { return fail()?NULL:__real_realloc(p,n); }
static cog_instruction_limits_t limits(void)
{
    const cog_instruction_limits_t l={16U,16U,2048U,8192U,4096U,131072U,64U,8U,8U,8U};return l;
}
static cog_instruction_snapshot_t *minimal(const cog_instruction_limits_t *l)
{
    cog_instruction_catalog_t *catalog=NULL;
    CHECK(cog_instruction_catalog_new(&catalog,l,1U,NULL,0U,NULL)==COG_OK);
    const cog_instruction_selection_t selection={.snapshot_id=2U,.workspace_root_id="r",.scope="",
        .model_id="m",.template_id="t",.template_revision=1U,.max_context_bytes=64U,.max_output_tokens=2U,.max_tool_rounds=3U};
    cog_instruction_snapshot_t *s=NULL;
    CHECK(cog_instruction_snapshot_new(&s,catalog,l,&selection,NULL,0U,NULL)==COG_OK);
    CHECK(cog_instruction_catalog_free(catalog)==COG_OK);return s;
}
/* Independent fixed vector, constructed directly from the documented field
 * sequence; neither production serializer nor native structs generate it. */
static const unsigned char golden_bytes[]={
    0x43U,0x47U,0x49U,0x4dU,0x01U,0x00U,0x00U,0x00U,0x01U,0x00U,0x00U,0x00U,
    0x01U,0x00U,0x00U,0x00U,0x00U,0x00U,0x00U,0x00U,0x02U,0x00U,0x00U,0x00U,
    0x00U,0x00U,0x00U,0x00U,0x01U,0x00U,0x00U,0x00U,0x72U,0x00U,0x00U,0x00U,
    0x00U,0xffU,0xffU,0xffU,0xffU,0x01U,0x00U,0x00U,0x00U,0x6dU,0x01U,0x00U,
    0x00U,0x00U,0x74U,0x01U,0x00U,0x00U,0x00U,0x40U,0x00U,0x00U,0x00U,0x00U,
    0x00U,0x00U,0x00U,0x02U,0x00U,0x00U,0x00U,0x03U,0x00U,0x00U,0x00U,0x00U,
    0x00U,0x00U,0x00U,0x00U,0x00U,0x00U,0x00U,0x00U,0x00U,0x00U,0x00U,0x00U,
    0x00U,0x00U,0x00U,0x00U,0x00U,0x00U,0x00U
};
static const unsigned char golden_sha[32]={
    0x10U,0x81U,0x7eU,0x6fU,0x9fU,0x91U,0xbbU,0xcaU,0xe1U,0x13U,0x08U,0x0fU,
    0x22U,0x9cU,0x02U,0x1aU,0xbdU,0x1fU,0x41U,0xb7U,0x33U,0x44U,0x4bU,0x22U,
    0x82U,0x55U,0x63U,0xd3U,0x0cU,0xfcU,0x7fU,0x0bU
};
static void golden(void)
{
    const cog_instruction_limits_t l=limits();cog_instruction_snapshot_t *s=minimal(&l),*decoded=NULL;
    cog_instruction_manifest_t m;CHECK(cog_instruction_snapshot_view(s,&m)==COG_OK);
    unsigned char output[1024];memset(output,0xa5,sizeof(output));size_t needed=999U;
    no_alloc=true;const size_t before=calls;
    CHECK(cog_instruction_manifest_encode(&m,NULL,0U,&needed)==COG_ERROR_OVERFLOW && needed==sizeof(golden_bytes));
    CHECK(cog_instruction_manifest_encode(&m,output,needed-1U,&needed)==COG_ERROR_OVERFLOW);
    for(size_t i=0U;i<sizeof(output);++i)CHECK(output[i]==0xa5U);
    CHECK(cog_instruction_manifest_encode(&m,output,sizeof(output),&needed)==COG_OK && needed==sizeof(golden_bytes));
    CHECK(memcmp(output,golden_bytes,needed)==0);
    unsigned char digest[32];CHECK(cog_instruction_snapshot_fingerprint(s,digest)==COG_OK);
    CHECK(memcmp(digest,golden_sha,sizeof(digest))==0 && calls==before);no_alloc=false;
    CHECK(cog_instruction_snapshot_decode(&decoded,&l,golden_bytes,sizeof(golden_bytes),COG_INSTRUCTION_INSPECT,NULL,0U)==COG_OK);
    cog_prompt_bundle_t bundle;CHECK(cog_instruction_prompt_view(decoded,&bundle)==COG_ERROR_INVALID_STATE);
    cog_instruction_snapshot_free(decoded);decoded=NULL;
    for(size_t n=0U;n<sizeof(golden_bytes);++n){
        CHECK(cog_instruction_snapshot_decode(&decoded,&l,golden_bytes,n,COG_INSTRUCTION_INSPECT,NULL,0U)!=COG_OK && decoded==NULL);
    }
    unsigned char invalid[sizeof(golden_bytes)+1U];memcpy(invalid,golden_bytes,sizeof(golden_bytes));invalid[sizeof(golden_bytes)]=0U;
    CHECK(cog_instruction_snapshot_decode(&decoded,&l,invalid,sizeof(invalid),COG_INSTRUCTION_INSPECT,NULL,0U)==COG_ERROR_PROTOCOL);
    invalid[4]=2U;CHECK(cog_instruction_snapshot_decode(&decoded,&l,invalid,sizeof(golden_bytes),COG_INSTRUCTION_INSPECT,NULL,0U)==COG_ERROR_UNSUPPORTED);
    memcpy(invalid,golden_bytes,sizeof(golden_bytes));
    memset(invalid+sizeof(golden_bytes)-20U,0xff,4U);
    const size_t alloc_before=calls;
    CHECK(cog_instruction_snapshot_decode(&decoded,&l,invalid,sizeof(golden_bytes),COG_INSTRUCTION_INSPECT,NULL,0U)==COG_ERROR_LIMIT);
    CHECK(calls==alloc_before);
    memset(output,0xa5,sizeof(output));m.version=9U;needed=999U;
    CHECK(cog_instruction_manifest_encode(&m,output,sizeof(output),&needed)==COG_ERROR_UNSUPPORTED && needed==999U);
    for(size_t i=0U;i<sizeof(output);++i)CHECK(output[i]==0xa5U);
    cog_instruction_snapshot_free(s);
}
static const cog_tool_t read_tool={.name="read",.description="read \"quoted\" text\nUTF-8: é",.input_schema="{\"type\":\"object\"}"};
static const cog_prompt_tool_binding_t binding={&read_tool,COG_PROMPT_TOOL_ASK};
static cog_instruction_snapshot_t *rich(const cog_instruction_limits_t *l)
{
    static const char host[]="host instructions";
    static const char role[]="---\ncog-agent: 1\nname: worker\ndescription: Worker\ntools: read\nskills: build\n---\nRole body\n";
    static const char skill[]="---\nname: build\ndescription: Build carefully\nallowed-tools: read\n---\nSkill body\n";
    const cog_instruction_source_t sources[]={
        {COG_INSTRUCTION_HOST,"work","host.txt","",NULL,0U,host,sizeof(host)-1U},
        {COG_INSTRUCTION_AGENT,"work","agents/worker.md","",NULL,1U,role,sizeof(role)-1U},
        {COG_INSTRUCTION_SKILL,"work","skills/build/SKILL.md","",NULL,1U,skill,sizeof(skill)-1U}};
    cog_instruction_catalog_t *catalog=NULL;CHECK(cog_instruction_catalog_new(&catalog,l,5U,sources,3U,NULL)==COG_OK);
    const cog_instruction_selection_t selection={.snapshot_id=9U,.workspace_root_id="work",.scope="src",.agent_name="worker",
        .model_id="model",.template_id="template",.template_revision=7U,.max_context_bytes=65536U,.max_output_tokens=128U,.max_tool_rounds=8U,
        .bindings=&binding,.binding_count=1U};
    static const char workspace[]="root rules",resource[]="resource contents";
    const cog_instruction_source_t additions[]={
        {COG_INSTRUCTION_AGENTS,"work","AGENTS.md","",NULL,0U,workspace,sizeof(workspace)-1U},
        {COG_INSTRUCTION_RESOURCE,"work","skills/build/info.txt","","build",0U,resource,sizeof(resource)-1U}};
    cog_instruction_snapshot_t *s=NULL;CHECK(cog_instruction_snapshot_new(&s,catalog,l,&selection,additions,2U,NULL)==COG_OK);
    cog_instruction_catalog_free(catalog);return s;
}
static void rich_roundtrip(void)
{
    const cog_instruction_limits_t l=limits();cog_instruction_snapshot_t *s=rich(&l),*decoded=NULL;
    cog_instruction_manifest_t m;CHECK(cog_instruction_snapshot_view(s,&m)==COG_OK);
    unsigned char bytes[32768],copy[32768],digest[32],restored_digest[32];size_t n=0U,n2=0U;
    no_alloc=true;const size_t before=calls;
    CHECK(cog_instruction_manifest_encode(&m,bytes,sizeof(bytes),&n)==COG_OK);
    CHECK(cog_instruction_snapshot_fingerprint(s,digest)==COG_OK && calls==before);no_alloc=false;
    CHECK(cog_instruction_snapshot_decode(&decoded,&l,bytes,n,COG_INSTRUCTION_EXECUTE,&binding,1U)==COG_OK);
    CHECK(cog_instruction_snapshot_fingerprint(decoded,restored_digest)==COG_OK && memcmp(digest,restored_digest,32U)==0);
    cog_prompt_bundle_t prompt;CHECK(cog_instruction_prompt_view(decoded,&prompt)==COG_OK);
    CHECK(prompt.tools[0].descriptor==&read_tool && prompt.tools[0].policy==COG_PROMPT_TOOL_ASK);
    cog_instruction_manifest_t restored;CHECK(cog_instruction_snapshot_view(decoded,&restored)==COG_OK);
    CHECK(cog_instruction_manifest_encode(&restored,copy,sizeof(copy),&n2)==COG_OK && n==n2 && memcmp(bytes,copy,n)==0);
    cog_instruction_snapshot_free(decoded);decoded=NULL;
    for(size_t k=0U;k<n;++k){
        CHECK(cog_instruction_snapshot_decode(&decoded,&l,bytes,k,COG_INSTRUCTION_INSPECT,NULL,0U)!=COG_OK && decoded==NULL);
    }
    /* Valid UTF-8 framing and valid source hashes cannot excuse forged resolved
     * fragments: encode the altered view, then demand semantic restore refusal. */
    cog_prompt_fragment_t fragments[64];CHECK(m.fragment_count<64U);
    memcpy(fragments,m.fragments,m.fragment_count*sizeof(*fragments));
    fragments[0].text="injected";fragments[0].text_bytes=8U;
    cog_instruction_manifest_t changed=m;changed.fragments=fragments;
    CHECK(cog_instruction_manifest_encode(&changed,copy,sizeof(copy),&n2)==COG_OK);
    CHECK(cog_instruction_snapshot_decode(&decoded,&l,copy,n2,COG_INSTRUCTION_INSPECT,NULL,0U)==COG_ERROR_PROTOCOL && decoded==NULL);
    const cog_prompt_tool_binding_t denied={&read_tool,COG_PROMPT_TOOL_DENY};
    CHECK(cog_instruction_snapshot_decode(&decoded,&l,bytes,n,COG_INSTRUCTION_EXECUTE,&denied,1U)==COG_ERROR_DENIED);
    cog_tool_t drift=read_tool;drift.description="changed";const cog_prompt_tool_binding_t different={&drift,COG_PROMPT_TOOL_ASK};
    CHECK(cog_instruction_snapshot_decode(&decoded,&l,bytes,n,COG_INSTRUCTION_EXECUTE,&different,1U)==COG_ERROR_UNSUPPORTED);
    cog_instruction_limits_t small=l;small.max_sources=1U;const size_t alloc_before=calls;
    CHECK(cog_instruction_snapshot_decode(&decoded,&small,bytes,n,COG_INSTRUCTION_INSPECT,NULL,0U)==COG_ERROR_LIMIT && calls==alloc_before);
    small=l;small.max_source_bytes=1U;
    CHECK(cog_instruction_snapshot_decode(&decoded,&small,bytes,n,COG_INSTRUCTION_INSPECT,NULL,0U)==COG_ERROR_LIMIT && calls==alloc_before);
    /* Every constructor allocation site must fail cleanly with NULL ownership. */
    calls=0U;CHECK(cog_instruction_snapshot_decode(&decoded,&l,bytes,n,COG_INSTRUCTION_EXECUTE,&binding,1U)==COG_OK);
    const size_t allocation_count=calls;cog_instruction_snapshot_free(decoded);decoded=NULL;
    for(size_t k=1U;k<=allocation_count;++k){
        calls=0U;fail_at=k;
        CHECK(cog_instruction_snapshot_decode(&decoded,&l,bytes,n,COG_INSTRUCTION_EXECUTE,&binding,1U)!=COG_OK && decoded==NULL);
    }
    fail_at=0U;
    /* Mutating each encoded byte exercises lengths, enum ranges, UTF-8, hashes
     * and semantic records together. Accepted independent values must still
     * survive canonical encoding, never retain decoder arena pointers. */
    for(size_t k=0U;k<n;++k){
        const unsigned char old=bytes[k];bytes[k]^=0xffU;
        const cog_result_t rc=cog_instruction_snapshot_decode(&decoded,&l,bytes,n,COG_INSTRUCTION_INSPECT,NULL,0U);
        if(rc==COG_OK){
            CHECK(cog_instruction_snapshot_view(decoded,&restored)==COG_OK);
            CHECK(cog_instruction_manifest_encode(&restored,copy,sizeof(copy),&n2)==COG_OK);
            cog_instruction_snapshot_free(decoded);decoded=NULL;
        }else CHECK(decoded==NULL);
        bytes[k]=old;
    }
    CHECK(cog_instruction_snapshot_decode(&decoded,&l,bytes,n,COG_INSTRUCTION_EXECUTE,&binding,1U)==COG_OK);
    cog_instruction_snapshot_free(s);memset(bytes,0,sizeof(bytes));
    CHECK(cog_instruction_snapshot_fingerprint(decoded,restored_digest)==COG_OK && memcmp(digest,restored_digest,32U)==0);
    cog_instruction_snapshot_free(decoded);
}
int main(void)
{
    golden();rich_roundtrip();(void)puts("instruction codec contracts passed");return 0;
}

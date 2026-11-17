/** @file test_provider_prompt.c @brief Prompt ownership, strict codec and failure oracles. */
#include <cog/openrouter.h>
#include <cog/provider_prompt.h>
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static bool forbid_alloc;
static size_t forbidden_allocations;
void *__wrap_malloc(size_t n);
void *__wrap_calloc(size_t n, size_t size);
void *__wrap_realloc(void *p, size_t n);
void *__real_malloc(size_t n);
void *__real_calloc(size_t n, size_t size);
void *__real_realloc(void *p, size_t n);
void *__wrap_malloc(size_t n) { if (forbid_alloc) { ++forbidden_allocations; return NULL; } return __real_malloc(n); }
void *__wrap_calloc(size_t n, size_t size) { if (forbid_alloc) { ++forbidden_allocations; return NULL; } return __real_calloc(n, size); }
void *__wrap_realloc(void *p, size_t n) { if (forbid_alloc) { ++forbidden_allocations; return NULL; } return __real_realloc(p, n); }

static cog_provider_prompt_limits_t limits(void)
{
    return (cog_provider_prompt_limits_t){.max_messages=8U,.max_fragments=8U,
        .max_tools=8U,.max_json_depth=32U,.max_context_bytes=8192U,
        .max_request_bytes=32768U,.scratch_bytes=131072U};
}
static cog_provider_prompt_input_t input(void)
{
    return (cog_provider_prompt_input_t){
        .prompt={.catalog_generation=1U,.snapshot_id=1U,.model_id="model",
            .template_id=COG_OPENROUTER_PROMPT_TEMPLATE_ID,.template_revision=1U,
            .max_context_bytes=8192U,.max_output_tokens=64U,.max_tool_rounds=2U},
        .request={.model_id="model",.max_tokens=32U,.stream=true,.configuration_generation=3U}};
}
static cog_result_t compile(cog_provider_prompt_compiler_t *c,
    const cog_provider_prompt_input_t *in, cog_provider_request_t *out)
{
    forbid_alloc=true;
    cog_result_t rc=cog_provider_prompt_compile(c,&cog_openrouter_provider_vtable,in,out);
    forbid_alloc=false;
    assert(forbidden_allocations==0U);
    return rc;
}
static void retained_and_strict(void)
{
    cog_provider_prompt_compiler_t *c=NULL;
    cog_provider_prompt_limits_t lim=limits();
    assert(cog_provider_prompt_compiler_new(&c,&cog_openrouter_prompt_template,&lim)==COG_OK);
    char text[]="user text";
    const unsigned char opaque[]={0,1,255};
    const cog_message_tool_call_t call={.id="call-1",.name="example",.arguments="{\"x\":\"y\"}",.arguments_len=9U};
    const cog_message_opaque_t hidden={.namespace_id="private",.format="bytes",.data=opaque,.size=sizeof(opaque)};
    cog_message_t history[2]={{.role=COG_MESSAGE_ROLE_USER,.content=text,.content_len=9U},
        {.role=COG_MESSAGE_ROLE_ASSISTANT,.tool_calls=&call,.tool_call_count=1U,
         .opaque=&hidden,.opaque_count=1U,.usage={.tokens_known=true,.input_tokens=4U,.output_tokens=5U,.cost=NAN}}};
    const cog_prompt_fragment_t fragment={.origin=COG_PROMPT_HOST,.source_id="host:base",.scope="",.text="rules",.text_bytes=5U};
    static const cog_tool_t tools[]={
        {.name="example",.description="quote \" and newline\n雪",.input_schema="{\"type\":\"object\",\"properties\":{\"x\":{\"type\":\"string\"}}}"},
        {.name="hidden",.description="not advertised",.input_schema=NULL}};
    cog_prompt_tool_binding_t bindings[]={{&tools[0],COG_PROMPT_TOOL_ASK},{&tools[1],COG_PROMPT_TOOL_DENY}};
    cog_provider_prompt_input_t in=input();
    in.request.messages=history;in.request.message_count=2U;
    in.prompt.fragments=&fragment;in.prompt.fragment_count=1U;
    in.prompt.tools=bindings;in.prompt.tool_count=2U;
    cog_provider_request_t out={0};
    assert(compile(c,&in,&out)==COG_OK);
    assert(out.message_count==3U);
    assert(strcmp(out.messages[0].content,"[COG host source=\"host:base\" scope=\"\"]\nrules\n[/COG]\n")==0);
    assert(out.messages[1].content!=text && strcmp(out.messages[1].content,text)==0);
    assert(out.messages[2].tool_calls!=&call && strcmp(out.messages[2].tool_calls[0].id,"call-1")==0);
    assert(out.messages[2].opaque[0].data!=opaque && memcmp(out.messages[2].opaque[0].data,opaque,3U)==0);
    assert(out.messages[2].usage.input_tokens==4U && out.messages[2].usage.output_tokens==5U);
    assert(strstr(out.tools_json,"hidden")==NULL);
    assert(strstr(out.tools_json,"quote \\\" and newline\\u000a雪")!=NULL);
    text[0]='X';
    assert(strcmp(out.messages[1].content,"user text")==0);
    unsigned char saved[sizeof(out)];memcpy(saved,&out,sizeof(out));
    const char *old_system=out.messages[0].content;
    in.request.temperature=-1.0f;
    assert(compile(c,&in,&out)==COG_ERROR_INVALID_ARG && memcmp(saved,&out,sizeof(out))==0);
    in.request.temperature=3.0f;
    assert(compile(c,&in,&out)==COG_ERROR_INVALID_ARG && memcmp(saved,&out,sizeof(out))==0);
    in.request.temperature=0.0f;
    in.prompt.template_revision=2U;
    assert(compile(c,&in,&out)==COG_ERROR_UNSUPPORTED);
    assert(memcmp(saved,&out,sizeof(out))==0 && strcmp(old_system,"[COG host source=\"host:base\" scope=\"\"]\nrules\n[/COG]\n")==0);
    in.prompt.template_revision=1U;
    in.prompt.max_context_bytes=1U;
    assert(compile(c,&in,&out)==COG_ERROR_LIMIT && memcmp(saved,&out,sizeof(out))==0);
    in.prompt.max_context_bytes=8192U;
    bindings[1].descriptor=&tools[0];
    assert(compile(c,&in,&out)==COG_ERROR_INVALID_ARG && memcmp(saved,&out,sizeof(out))==0);
    bindings[1].descriptor=&tools[1];
    static const cog_tool_t duplicate={.name="bad",.input_schema="{\"x\":1,\"\\u0078\":2}"};
    bindings[0].descriptor=&duplicate;
    assert(compile(c,&in,&out)==COG_ERROR_PROTOCOL);
    static const cog_tool_t nonobject={.name="bad",.input_schema="[]"};
    bindings[0].descriptor=&nonobject;
    assert(compile(c,&in,&out)==COG_ERROR_UNSUPPORTED);
    static const cog_tool_t malformed={.name="bad",.input_schema="{\"x\":}"};
    bindings[0].descriptor=&malformed;
    assert(compile(c,&in,&out)==COG_ERROR_PROTOCOL);
    bindings[0].descriptor=&tools[0];
    assert(compile(c,&in,&out)==COG_OK && out.messages[1].content[0]=='X');
    assert(cog_provider_prompt_compiler_free(c)==COG_OK);
}
static cog_result_t outside(const cog_provider_prompt_input_t *in,
    const cog_provider_prompt_limits_t *lim,void *scratch,size_t sc,void *storage,size_t cap,cog_provider_request_t *out)
{
    (void)lim;(void)scratch;(void)sc;(void)storage;(void)cap;
    *out=in->request;return COG_OK; /* Deliberately violates owned-range contract. */
}
static cog_result_t changed(const cog_provider_prompt_input_t *in,
    const cog_provider_prompt_limits_t *lim,void *scratch,size_t sc,void *storage,size_t cap,cog_provider_request_t *out)
{
    (void)lim;(void)scratch;(void)sc;
    assert(cap>6U);memcpy(storage,"model",6U);
    *out=in->request;out->model_id=storage;out->max_tokens=1U;return COG_OK;
}
static void invalid_renderers(void)
{
    cog_provider_prompt_limits_t lim=limits();
    const cog_provider_prompt_template_t formats[]={
        {.id="bad",.revision=1U,.provider=&cog_openrouter_provider_vtable,.render=outside},
        {.id="bad",.revision=1U,.provider=&cog_openrouter_provider_vtable,.render=changed}};
    for(size_t i=0U;i<2U;++i){
        cog_provider_prompt_compiler_t *c=NULL;
        assert(cog_provider_prompt_compiler_new(&c,&formats[i],&lim)==COG_OK);
        cog_provider_prompt_input_t in=input();in.prompt.template_id="bad";
        cog_provider_request_t out={.max_tokens=99U};
        assert(compile(c,&in,&out)==COG_ERROR_PROTOCOL && out.max_tokens==99U);
        assert(cog_provider_prompt_compiler_free(c)==COG_OK);
    }
}
static void limits_and_empty(void)
{
    cog_provider_prompt_limits_t lim=limits();
    cog_provider_prompt_compiler_t *c=NULL;
    assert(cog_provider_prompt_compiler_new(NULL,&cog_openrouter_prompt_template,&lim)==COG_ERROR_NULL_PTR);
    assert(cog_provider_prompt_compiler_new(&c,NULL,&lim)==COG_ERROR_NULL_PTR && c==NULL);
    lim.max_request_bytes=SIZE_MAX;
    assert(cog_provider_prompt_compiler_new(&c,&cog_openrouter_prompt_template,&lim)==COG_ERROR_LIMIT);
    lim=limits();lim.max_messages=1U;
    assert(cog_provider_prompt_compiler_new(&c,&cog_openrouter_prompt_template,&lim)==COG_OK);
    cog_provider_prompt_input_t in=input();cog_provider_request_t out={0};
    assert(compile(c,&in,&out)==COG_OK && out.message_count==0U && out.tools_json==NULL);
    cog_message_t m={.role=COG_MESSAGE_ROLE_USER,.content="hi",.content_len=2U};
    cog_prompt_fragment_t f={.origin=COG_PROMPT_HOST,.source_id="h",.scope="",.text="x",.text_bytes=1U};
    in.request.messages=&m;in.request.message_count=1U;in.prompt.fragments=&f;in.prompt.fragment_count=1U;
    assert(compile(c,&in,&out)==COG_ERROR_LIMIT);
    assert(cog_provider_prompt_compiler_free(c)==COG_OK);
    assert(cog_provider_prompt_compiler_free(NULL)==COG_OK);
}
int main(void)
{ retained_and_strict();invalid_renderers();limits_and_empty();return 0; }

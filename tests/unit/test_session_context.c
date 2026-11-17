/** @file test_session_context.c
 * @brief Context ownership, branch identity, hostile restore and v3 durability.
 */
#define _POSIX_C_SOURCE 200809L
#include <cog/session.h>
#include <cog/session_store.h>
#include "session_internal.h"
#include "session_store_codec.h"
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#define CHECK(x) do { if (!(x)) { (void)fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x); abort(); } } while (0)

static volatile bool forbid_alloc;
static volatile size_t alloc_calls, fail_allocation;
void *__real_malloc(size_t n);
void *__real_calloc(size_t n, size_t size);
void *__real_realloc(void *p, size_t n);
char *__real_strdup(const char *s);
void *__wrap_malloc(size_t n);
void *__wrap_calloc(size_t n, size_t size);
void *__wrap_realloc(void *p, size_t n);
char *__wrap_strdup(const char *s);
static bool allocation_fails(void)
{
    ++alloc_calls;
    return forbid_alloc || (fail_allocation != 0U && alloc_calls == fail_allocation);
}
void *__wrap_malloc(size_t n) { return allocation_fails() ? NULL : __real_malloc(n); }
void *__wrap_calloc(size_t n, size_t z) { return allocation_fails() ? NULL : __real_calloc(n,z); }
void *__wrap_realloc(void *p, size_t n) { return allocation_fails() ? NULL : __real_realloc(p,n); }
char *__wrap_strdup(const char *s) { return allocation_fails() ? NULL : __real_strdup(s); }

static cog_session_config_t config(const char *id)
{
    const cog_session_config_t c = {.id=id,.max_nodes=16U,.max_payload_bytes=65536U,
        .max_tool_calls_per_message=2U,.max_contexts=8U,.max_context_bindings=16U,.max_context_bytes=2048U};
    return c;
}
static void add_context(cog_session_t *s, uint64_t id, const char *text)
{
    const cog_session_context_t c = {id,"cog.instructions",1U,(const unsigned char *)text,strlen(text)};
    CHECK(cog_session_context_add(s,&c)==COG_OK);
}
static void publish(cog_session_t *s, uint64_t id, uint64_t context)
{
    cog_session_snapshot_t v;
    CHECK(cog_session_snapshot_view(s,&v)==COG_OK);
    const cog_session_context_binding_t b = {id,v.selected_context_binding_id,v.active_node_id,context};
    CHECK(cog_session_context_publish(s,&b)==COG_OK);
}
static cog_session_node_t *append(cog_session_t *s, const char *text)
{
    cog_session_node_t *n=NULL;
    const cog_message_t m={.role=COG_MESSAGE_ROLE_USER,.content=text,.content_len=strlen(text)};
    CHECK(cog_session_append(s,&m,&n)==COG_OK);
    return n;
}
static void constructors(void)
{
    cog_session_t *s=NULL;
    cog_session_config_t c=config("alloc");
    c.max_context_bytes=0U;
    CHECK(cog_session_new(&s,&c)==COG_ERROR_INVALID_ARG && s==NULL);
    c=config("alloc"); c.max_contexts=SIZE_MAX;
    CHECK(cog_session_new(&s,&c)==COG_ERROR_LIMIT && s==NULL);
    c=config("alloc"); c.max_context_bytes=SIZE_MAX;
    CHECK(cog_session_new(&s,&c)==COG_ERROR_LIMIT && s==NULL);
    c=config("alloc");
    alloc_calls=0U;
    CHECK(cog_session_new(&s,&c)==COG_OK);
    const size_t allocations=alloc_calls;
    cog_session_free(s);
    for(size_t i=1U;i<=allocations;++i){
        alloc_calls=0U;fail_allocation=i;s=NULL;
        CHECK(cog_session_new(&s,&c)==COG_ERROR_NOMEM && s==NULL);
    }
    fail_allocation=0U;
}
static void branches(void)
{
    cog_session_t *s=NULL,*restored=NULL;
    const cog_session_config_t c=config("branches");
    CHECK(cog_session_new(&s,&c)==COG_OK);
    char name[]="cog.instructions";unsigned char payload[]={1U,0U,255U};
    const cog_session_context_t input={1U,name,1U,payload,sizeof(payload)};
    forbid_alloc=true;const size_t before=alloc_calls;
    CHECK(cog_session_context_add(s,&input)==COG_OK);
    name[0]='x';payload[0]=9U;
    cog_session_context_t view;
    CHECK(cog_session_context_view(s,1U,&view)==COG_OK);
    CHECK(strcmp(view.namespace_id,"cog.instructions")==0 && view.data[0]==1U);
    CHECK(cog_session_context_add(s,&input)==COG_ERROR_INVALID_ARG);
    publish(s,1U,1U);
    cog_session_node_t *root=append(s,"root");
    add_context(s,4U,"next");publish(s,3U,4U);publish(s,5U,1U);
    cog_session_snapshot_t v;
    CHECK(cog_session_snapshot_view(s,&v)==COG_OK && v.selected_context_binding_id==5U);
    CHECK(cog_session_snapshot_validate(&v)==COG_OK);
    CHECK(cog_session_context_select(s,root,3U)==COG_OK);
    cog_session_node_t *left=append(s,"left");
    CHECK(cog_node_data(left)->context_binding_id==3U);
    add_context(s,8U,"left pending");publish(s,7U,8U);
    const cog_message_t m={.role=COG_MESSAGE_ROLE_USER,.content="right",.content_len=5U};
    cog_session_node_t *right=NULL;
    CHECK(cog_session_add_message(s,root,&m,&right)==COG_OK);
    CHECK(cog_node_data(right)->context_binding_id==1U);
    CHECK(cog_session_context_select(s,right,7U)==COG_ERROR_INVALID_ARG);
    CHECK(cog_session_context_select(s,left,7U)==COG_OK);
    CHECK(cog_session_context_select(s,left,1U)==COG_ERROR_INVALID_ARG);
    CHECK(cog_session_fork(s,left,NULL)==COG_OK);
    CHECK(cog_session_snapshot_view(s,&v)==COG_OK && v.selected_context_binding_id==3U);
    CHECK(cog_session_context_select(s,left,7U)==COG_OK);
    CHECK(cog_session_reset(s)==COG_ERROR_INVALID_STATE);
    CHECK(cog_session_snapshot_view(s,&v)==COG_OK);
    CHECK(cog_session_snapshot_validate(&v)==COG_OK);
    CHECK(alloc_calls==before);forbid_alloc=false;
    CHECK(cog_session_restore(&restored,&c,&v)==COG_OK);
    cog_session_free(s);s=NULL;
    CHECK(cog_session_snapshot_view(restored,&v)==COG_OK && v.context_high_water==8U && v.context_binding_high_water==7U);
    CHECK(v.selected_context_binding_id==7U && v.context_count==3U && v.context_binding_count==4U);
    CHECK(v.contexts[0].data[0]==1U && v.nodes[1].context_binding_id==3U && v.nodes[2].context_binding_id==1U);
    cog_session_snapshot_t bad=v;
    bad.selected_context_binding_id=5U;
    CHECK(cog_session_snapshot_validate(&bad)==COG_ERROR_PROTOCOL);
    bad=v;bad.context_high_water=0U;
    CHECK(cog_session_snapshot_validate(&bad)==COG_ERROR_PROTOCOL);
    cog_session_context_binding_t bs[16];memcpy(bs,v.context_bindings,v.context_binding_count*sizeof(*bs));
    bad=v;bad.context_bindings=bs;bs[1].previous_binding_id=bs[1].binding_id;
    CHECK(cog_session_snapshot_validate(&bad)==COG_ERROR_PROTOCOL);
    cog_session_free(restored);
}
static void discard_and_capacity(void)
{
    cog_session_t *s=NULL; cog_session_config_t c=config("discard");
    c.max_contexts=1U;c.max_context_bytes=18U;c.max_context_bindings=1U;
    CHECK(cog_session_new(&s,&c)==COG_OK);
    add_context(s,1U,"a");publish(s,1U,1U);
    const cog_session_context_t extra={2U,"x",1U,NULL,0U};
    CHECK(cog_session_context_add(s,&extra)==COG_ERROR_LIMIT);
    cog_session_node_t *root=append(s,"root");
    const cog_session_context_binding_t b={2U,1U,cog_node_data(root)->node_id,1U};
    CHECK(cog_session_context_publish(s,&b)==COG_ERROR_LIMIT);
    CHECK(cog_session_discard_node(s,root)==COG_OK);
    cog_session_snapshot_t v;CHECK(cog_session_snapshot_view(s,&v)==COG_OK);
    CHECK(v.selected_context_binding_id==0U && v.context_high_water==1U && v.context_binding_high_water==1U);
    CHECK(cog_session_context_select(s,NULL,1U)==COG_OK);
    cog_session_free(s);
    c=config("anchor");CHECK(cog_session_new(&s,&c)==COG_OK);
    add_context(s,1U,"a");root=append(s,"root");publish(s,1U,1U);
    CHECK(cog_session_discard_node(s,root)==COG_ERROR_INVALID_STATE);
    cog_session_free(s);
}
static void tools_and_codec(void)
{
    cog_session_t *s=NULL,*restored=NULL;const cog_session_config_t c=config("codec-context");
    CHECK(cog_session_new(&s,&c)==COG_OK);add_context(s,1U,"first");add_context(s,2U,"second");publish(s,1U,1U);
    const cog_message_tool_call_t call={"call","edit","{}",2U};
    const cog_message_t assistant={.role=COG_MESSAGE_ROLE_ASSISTANT,.tool_calls=&call,.tool_call_count=1U};
    cog_session_node_t *a=NULL;CHECK(cog_session_append(s,&assistant,&a)==COG_OK);
    const cog_session_context_binding_t b={2U,1U,cog_node_data(a)->node_id,2U};
    CHECK(cog_session_context_publish(s,&b)==COG_ERROR_INVALID_STATE);
    CHECK(cog_session_context_select(s,a,1U)==COG_ERROR_INVALID_STATE);
    const cog_session_context_t extra={3U,"x",1U,NULL,0U};
    CHECK(cog_session_context_add(s,&extra)==COG_ERROR_INVALID_STATE);
    CHECK(cog_session_tool_transition(s,a,"call",COG_TOOL_EXECUTION_PROPOSED,COG_TOOL_EXECUTION_STARTED)==COG_OK);
    cog_session_snapshot_t v;CHECK(cog_session_snapshot_view(s,&v)==COG_OK);
    cog_session_context_binding_t bs[2]={v.context_bindings[0],b};
    cog_session_snapshot_t bad=v;bad.context_bindings=bs;bad.context_binding_count=2U;
    bad.context_binding_high_water=2U;bad.selected_context_binding_id=2U;
    CHECK(cog_session_snapshot_validate(&bad)==COG_ERROR_PROTOCOL);
    /* A final ledger without its correlated result remains invalid, including
     * when an attacker also supplies a structurally valid later idle binding. */
    cog_tool_ledger_record_t missing_result=v.ledger[0];
    cog_tool_audit_record_t denial={1U,COG_TOOL_EXECUTION_PROPOSED,COG_TOOL_EXECUTION_DENIED,false,NULL,0U};
    missing_result.state=COG_TOOL_EXECUTION_DENIED;missing_result.audit=&denial;
    bad.ledger=&missing_result;
    CHECK(cog_session_snapshot_validate(&bad)==COG_ERROR_PROTOCOL);
    const cog_tool_audit_record_t success_audit[2]={
        {1U,COG_TOOL_EXECUTION_PROPOSED,COG_TOOL_EXECUTION_STARTED,false,NULL,0U},
        {2U,COG_TOOL_EXECUTION_STARTED,COG_TOOL_EXECUTION_SUCCEEDED,false,NULL,0U}};
    missing_result.state=COG_TOOL_EXECUTION_SUCCEEDED;missing_result.audit=success_audit;missing_result.audit_count=2U;
    bad.audit_high_water=2U;
    CHECK(cog_session_snapshot_validate(&bad)==COG_ERROR_PROTOCOL);
    unsigned char encoded[32768],copy[32768],scratch[32768];size_t size=0U,copy_size=0U;
    cog_store_codec_workspace_t w={scratch,sizeof(scratch),0U};
    forbid_alloc=true;const size_t before=alloc_calls;
    CHECK(cog_store_codec_encode(&v,encoded,sizeof(encoded),&size)==COG_OK);
    CHECK(memcmp(encoded,"COGSNAP3",8U)==0);
    /* Last u64 count plus (node ID,binding ID), all little endian. */
    const unsigned char tail[24]={1U,0U,0U,0U,0U,0U,0U,0U,1U,0U,0U,0U,0U,0U,0U,0U,1U,0U,0U,0U,0U,0U,0U,0U};
    CHECK(size>=sizeof(tail) && memcmp(encoded+size-sizeof(tail),tail,sizeof(tail))==0);
    cog_session_snapshot_t decoded={0};
    CHECK(cog_store_codec_decode(encoded,size,&w,&decoded)==COG_OK);
    CHECK(cog_session_snapshot_validate(&decoded)==COG_OK);
    CHECK(decoded.ledger[0].state==COG_TOOL_EXECUTION_STARTED && decoded.nodes[0].context_binding_id==1U);
    CHECK(cog_store_codec_encode(&decoded,copy,sizeof(copy),&copy_size)==COG_OK);
    CHECK(size==copy_size && memcmp(encoded,copy,size)==0);
    CHECK(alloc_calls==before);forbid_alloc=false;
    CHECK(cog_session_restore(&restored,&c,&decoded)==COG_OK);
    cog_session_snapshot_t rv;CHECK(cog_session_snapshot_view(restored,&rv)==COG_OK);
    CHECK(rv.audit_high_water==v.audit_high_water+1U && rv.ledger[0].state==COG_TOOL_EXECUTION_INDETERMINATE);
    CHECK(rv.nodes[0].context_binding_id==1U && rv.selected_context_binding_id==1U);
    const cog_message_t result={.role=COG_MESSAGE_ROLE_TOOL,.tool_call_id="call",.content="verified",.content_len=8U};
    cog_session_node_t *r=NULL;
    CHECK(cog_session_reconcile_tool(restored,cog_session_root(restored),"call",COG_TOOL_EXECUTION_ACKNOWLEDGED,&result,&r)==COG_OK);
    CHECK(cog_node_data(r)->context_binding_id==1U);publish(restored,2U,2U);
    CHECK(cog_session_snapshot_view(restored,&rv)==COG_OK && cog_session_snapshot_validate(&rv)==COG_OK);
    cog_session_free(restored);
    for(size_t i=0U;i<size;++i){
        w.used=0U;decoded.context_high_water=99U;
        CHECK(cog_store_codec_decode(encoded,i,&w,&decoded)!=COG_OK && decoded.context_high_water==99U);
    }
    encoded[size]=0U;w.used=0U;
    CHECK(cog_store_codec_decode(encoded,size+1U,&w,&decoded)==COG_ERROR_PROTOCOL);
    encoded[7]='9';w.used=0U;CHECK(cog_store_codec_decode(encoded,size,&w,&decoded)==COG_ERROR_UNSUPPORTED);encoded[7]='3';
    w.capacity=1U;w.used=0U;CHECK(cog_store_codec_decode(encoded,size,&w,&decoded)==COG_ERROR_LIMIT);
    w.capacity=sizeof(scratch);w.used=0U;
    CHECK(cog_store_codec_decode(encoded,size,&w,&decoded)==COG_OK);
    cog_session_config_t no_context=c;no_context.max_contexts=0U;no_context.max_context_bindings=0U;no_context.max_context_bytes=0U;
    restored=NULL;CHECK(cog_session_restore(&restored,&no_context,&decoded)==COG_ERROR_LIMIT && restored==NULL);
    cog_session_free(s);
}
static size_t callbacks;
static cog_result_t saved(void *context,const cog_session_t *s)
{ (void)context;(void)s;++callbacks;return COG_OK; }
static cog_result_t appended(void *context,const cog_session_t *s,const cog_session_node_t *n)
{ (void)context;(void)s;(void)n;++callbacks;return COG_OK; }
static void old_backend(void)
{
    cog_store_vtable_t old=cog_store_null_backend;old.save=saved;old.append=appended;
    cog_store_config_t cfg={.backend=&old,.required_capabilities=COG_STORE_CAP_CONTEXT_V3};
    cog_store_t *store=NULL;CHECK(cog_store_new(&store,&cfg)==COG_ERROR_UNSUPPORTED && store==NULL && callbacks==0U);
    cfg.required_capabilities=0U;old.capabilities=UINT64_C(2);
    CHECK(cog_store_new(&store,&cfg)==COG_ERROR_UNSUPPORTED && store==NULL);
    old.capabilities=0U;CHECK(cog_store_new(&store,&cfg)==COG_OK);
    cog_session_t *s=NULL;const cog_session_config_t c=config("backend");CHECK(cog_session_new(&s,&c)==COG_OK);
    cog_session_node_t *n=append(s,"old");CHECK(cog_store_save(store,s)==COG_OK && callbacks==1U);
    add_context(s,1U,"context");
    CHECK(cog_store_save(store,s)==COG_ERROR_UNSUPPORTED && callbacks==1U);
    CHECK(cog_store_append(store,s,n)==COG_ERROR_UNSUPPORTED && callbacks==1U);
    cog_session_free(s);cog_store_free(store);
}
static void remove_tree(const char *path)
{
    DIR *d=opendir(path);CHECK(d!=NULL);struct dirent *entry;
    while((entry=readdir(d))!=NULL){
        if(strcmp(entry->d_name,".")==0 || strcmp(entry->d_name,"..")==0)continue;
        char name[1024];int count=snprintf(name,sizeof(name),"%s/%s",path,entry->d_name);CHECK(count>0 && (size_t)count<sizeof(name));
        struct stat st;CHECK(lstat(name,&st)==0);
        if(S_ISDIR(st.st_mode))remove_tree(name);else CHECK(unlink(name)==0);
    }
    CHECK(closedir(d)==0);CHECK(rmdir(path)==0);
}
static void persistence(void)
{
    char path[]="context-store-XXXXXX";CHECK(mkdtemp(path)!=NULL);
    const cog_store_config_t cfg={.base_dir=path,.max_record_bytes=262144U,.required_capabilities=COG_STORE_CAP_CONTEXT_V3};
    cog_store_t *store=NULL;CHECK(cog_store_new(&store,&cfg)==COG_OK);
    cog_session_t *s=NULL,*loaded=NULL;const cog_session_config_t c=config("durable");
    CHECK(cog_session_new(&s,&c)==COG_OK);
    CHECK(cog_store_save(store,s)==COG_OK);
    cog_store_load_report_t report;
    CHECK(cog_store_load_ex(store,"durable",COG_STORE_LOAD_INSPECT,&loaded,&report)==COG_OK);
    CHECK(report.source_version==2U && !report.contexts_present);cog_session_free(loaded);
    add_context(s,1U,"old");add_context(s,2U,"new");publish(s,1U,1U);
    const cog_message_tool_call_t call={"call","edit","{}",2U};
    const cog_message_t assistant={.role=COG_MESSAGE_ROLE_ASSISTANT,.tool_calls=&call,.tool_call_count=1U};
    cog_session_node_t *a=NULL;CHECK(cog_session_append(s,&assistant,&a)==COG_OK);
    CHECK(cog_session_tool_transition(s,a,"call",COG_TOOL_EXECUTION_PROPOSED,COG_TOOL_EXECUTION_STARTED)==COG_OK);
    CHECK(cog_store_save(store,s)==COG_OK);
    CHECK(cog_store_load_ex(store,"durable",COG_STORE_LOAD_INSPECT,&loaded,&report)==COG_OK);
    CHECK(report.source_version==3U && report.contexts_present && report.source_audit_high_water==1U);
    cog_session_snapshot_t snapshot;CHECK(cog_session_snapshot_view(loaded,&snapshot)==COG_OK);
    CHECK(snapshot.audit_high_water==2U && snapshot.ledger[0].state==COG_TOOL_EXECUTION_INDETERMINATE);
    CHECK(snapshot.selected_context_binding_id==1U && memcmp(snapshot.contexts[0].data,"old",3U)==0);
    cog_session_free(loaded);
    CHECK(cog_session_tool_transition(s,a,"call",COG_TOOL_EXECUTION_STARTED,COG_TOOL_EXECUTION_SUCCEEDED)==COG_OK);
    const cog_message_t result={.role=COG_MESSAGE_ROLE_TOOL,.tool_call_id="call",.content="ok",.content_len=2U};
    CHECK(cog_session_append(s,&result,NULL)==COG_OK);publish(s,2U,2U);
    CHECK(cog_store_save(store,s)==COG_OK);
    CHECK(cog_store_load_ex(store,"durable",COG_STORE_LOAD_REPLAY,&loaded,&report)==COG_OK);
    CHECK(cog_session_snapshot_view(loaded,&snapshot)==COG_OK);
    CHECK(snapshot.selected_context_binding_id==2U && snapshot.nodes[0].context_binding_id==1U && snapshot.nodes[1].context_binding_id==1U);
    CHECK(snapshot.ledger[0].state==COG_TOOL_EXECUTION_SUCCEEDED && snapshot.context_count==2U);
    /* A replacement constructor cannot change already committed context bytes. */
    cog_session_context_t altered[2]={snapshot.contexts[0],snapshot.contexts[1]};
    altered[0].data=(const unsigned char *)"bad";
    cog_session_snapshot_t modified=snapshot;modified.contexts=altered;
    cog_session_t *replacement=NULL;CHECK(cog_session_restore(&replacement,&c,&modified)==COG_OK);
    CHECK(cog_store_save(store,replacement)==COG_ERROR_INVALID_STATE);
    cog_session_free(replacement);cog_session_free(loaded);cog_session_free(s);
    const cog_session_config_t branch_cfg=config("rebind");
    CHECK(cog_session_new(&s,&branch_cfg)==COG_OK);
    add_context(s,1U,"first");add_context(s,2U,"second");publish(s,1U,1U);publish(s,2U,2U);
    CHECK(cog_session_context_select(s,NULL,1U)==COG_OK);(void)append(s,"immutable");
    CHECK(cog_store_save(store,s)==COG_OK);
    CHECK(cog_session_snapshot_view(s,&snapshot)==COG_OK);
    cog_node_data_t rebound=snapshot.nodes[0];rebound.context_binding_id=2U;
    modified=snapshot;modified.nodes=&rebound;modified.selected_context_binding_id=2U;
    CHECK(cog_session_snapshot_validate(&modified)==COG_OK);
    CHECK(cog_session_restore(&replacement,&branch_cfg,&modified)==COG_OK);
    CHECK(cog_store_save(store,replacement)==COG_ERROR_INVALID_STATE);
    cog_session_free(replacement);cog_session_free(s);cog_store_free(store);remove_tree(path);
}
int main(void)
{
    constructors();branches();discard_and_capacity();tools_and_codec();old_backend();persistence();
    (void)puts("session context contracts passed");return 0;
}

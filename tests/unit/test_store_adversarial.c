/** @file test_store_adversarial.c @brief Durable owner, monotonic ledger and recovery boundaries. */
#define _POSIX_C_SOURCE 200809L
#include <cog/session_store.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#define CHECK(c) do { if (!(c)) { (void)fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#c); exit(1); } } while (0)
static void cleanup(const char *path)
{
    DIR *d=opendir(path);CHECK(d!=NULL);struct dirent *de;
    while((de=readdir(d))!=NULL){
        if(strcmp(de->d_name,".")==0 || strcmp(de->d_name,"..")==0)continue;
        char p[1024];CHECK(snprintf(p,sizeof(p),"%s/%s",path,de->d_name)>0);
        struct stat st;CHECK(lstat(p,&st)==0);if(S_ISDIR(st.st_mode))cleanup(p);else CHECK(unlink(p)==0);
    }
    CHECK(closedir(d)==0);CHECK(rmdir(path)==0);
}
static cog_store_t *store(const char *base,bool owned)
{ cog_store_t *s=NULL;CHECK(cog_store_new(&s,&(cog_store_config_t){.base_dir=base,.require_owner=owned,.max_record_bytes=1048576U})==COG_OK);return s; }
static cog_session_t *session(const char *id)
{ cog_session_t *s=NULL;CHECK(cog_session_new(&s,&(cog_session_config_t){.id=id,.max_nodes=16U,.max_payload_bytes=65536U})==COG_OK);return s; }
static cog_session_node_t *proposal(cog_session_t *s)
{
    const cog_message_tool_call_t call={.id="call-1",.name="edit",.arguments="{\"path\":\"x\"}",.arguments_len=12U};
    const unsigned char bytes[]={0U,42U,255U};
    const cog_message_opaque_t opaque={.namespace_id="example",.format="v1",.data=bytes,.size=sizeof(bytes)};
    const cog_message_t m={.role=COG_MESSAGE_ROLE_ASSISTANT,.tool_calls=&call,.tool_call_count=1U,.opaque=&opaque,.opaque_count=1U};
    cog_session_node_t *n=NULL;CHECK(cog_session_append(s,&m,&n)==COG_OK);return n;
}
static void append_user(cog_session_t *s)
{ const cog_message_t m={.role=COG_MESSAGE_ROLE_USER,.content="later",.content_len=5U};CHECK(cog_session_append(s,&m,NULL)==COG_OK); }
static void ownership(const char *base)
{
    cog_store_t *a=store(base,true),*b=store(base,true),*unowned=store(base,false);
    cog_session_t *s=session("owned");cog_store_owner_t owner={99U,99U};cog_store_checkpoint_t cp={99U};
    CHECK(cog_store_owner_read(a,"owned",&owner,&cp)==COG_OK && owner.epoch==0U && cp.generation==0U);
    cog_store_owner_t one={1U,101U},two={2U,202U};
    CHECK(cog_store_owner_compare_exchange(a,"owned",owner,one,cp)==COG_OK);
    CHECK(cog_store_owner_compare_exchange(a,"owned",owner,one,cp)==COG_OK); /* durable retry */
    CHECK(cog_store_save(a,s)==COG_ERROR_INVALID_STATE);
    cog_store_owner_guard_t *g=NULL,*other=NULL;
    CHECK(cog_store_owner_acquire(a,"owned",one,&g)==COG_OK);
    CHECK(cog_store_owner_acquire(b,"owned",one,&other)==COG_ERROR_AGAIN && other==NULL);
    CHECK(cog_store_owner_compare_exchange(b,"owned",one,two,cp)==COG_ERROR_AGAIN);
    CHECK(cog_store_delete(unowned,"owned")==COG_ERROR_UNSUPPORTED);
    CHECK(cog_store_owner_guard_borrow(g,"wrong")==COG_ERROR_INVALID_STATE);
    CHECK(cog_store_owner_guard_borrow(g,"owned")==COG_OK);
    CHECK(cog_store_owner_release(g)==COG_ERROR_INVALID_STATE);
    CHECK(cog_store_checkpoint_save(a,s,g,&cp)==COG_ERROR_INVALID_STATE);
    CHECK(cog_store_owner_guard_return(g)==COG_OK);
    CHECK(cog_store_owner_guard_return(g)==COG_ERROR_INVALID_STATE);
    CHECK(cog_store_checkpoint_save(a,s,g,&cp)==COG_OK && cp.generation==1U);
    cog_store_checkpoint_t first=cp;
    append_user(s);CHECK(cog_store_checkpoint_save(a,s,g,&cp)==COG_OK && cp.generation==2U);
    cog_session_t *old=NULL;cog_store_load_report_t r;
    CHECK(cog_store_checkpoint_load(b,"owned",first,COG_STORE_LOAD_REPLAY,&old,&r)==COG_OK);
    CHECK(cog_session_is_empty(old) && r.checkpoint_generation==first.generation);cog_session_free(old);
    CHECK(cog_store_checkpoint_load(b,"owned",(cog_store_checkpoint_t){900U},COG_STORE_LOAD_REPLAY,&old,&r)==COG_ERROR_NOT_FOUND);
    CHECK(cog_store_save(unowned,s)==COG_ERROR_AGAIN);
    CHECK(cog_store_owner_release(g)==COG_OK);
    CHECK(cog_store_save(unowned,s)==COG_ERROR_INVALID_STATE);
    CHECK(cog_store_owner_compare_exchange(b,"owned",one,two,first)==COG_ERROR_INVALID_STATE);
    CHECK(cog_store_owner_compare_exchange(b,"owned",one,two,cp)==COG_OK);
    CHECK(cog_store_owner_acquire(a,"owned",one,&g)==COG_ERROR_INVALID_STATE && g==NULL);
    CHECK(cog_store_owner_acquire(b,"owned",two,&g)==COG_OK);CHECK(cog_store_owner_release(g)==COG_OK);
    CHECK(cog_store_delete(a,"owned")==COG_ERROR_UNSUPPORTED);
    cog_session_free(s);cog_store_free(a);cog_store_free(b);cog_store_free(unowned);
}
static void recovery(const char *base)
{
    cog_store_t *st=store(base,true);cog_session_t *s=session("effect");cog_session_node_t *assistant=proposal(s);
    cog_store_owner_t one={1U,111U},two={2U,222U},three={3U,333U};cog_store_checkpoint_t cp={0};
    CHECK(cog_store_owner_compare_exchange(st,"effect",(cog_store_owner_t){0},one,cp)==COG_OK);
    cog_store_owner_guard_t *g=NULL;CHECK(cog_store_owner_acquire(st,"effect",one,&g)==COG_OK);
    CHECK(cog_store_save(st,s)==COG_OK);
    CHECK(cog_store_owner_release(g)==COG_OK);
    CHECK(cog_store_owner_acquire(st,"effect",one,&g)==COG_OK); /* coherent PROPOSED resumes */
    CHECK(cog_session_tool_transition(s,assistant,"call-1",COG_TOOL_EXECUTION_PROPOSED,COG_TOOL_EXECUTION_STARTED)==COG_OK);
    CHECK(cog_store_save(st,s)==COG_OK);
    CHECK(cog_store_owner_guard_borrow(g,"effect")==COG_OK);CHECK(cog_store_owner_guard_return(g)==COG_OK);
    cog_session_t *empty=session("effect");CHECK(cog_store_save(st,empty)==COG_ERROR_INVALID_STATE);cog_session_free(empty);
    CHECK(cog_store_owner_read(st,"effect",&one,&cp)==COG_OK);
    CHECK(cog_store_owner_release(g)==COG_OK);cog_session_free(s);
    CHECK(cog_store_owner_compare_exchange(st,"effect",one,two,cp)==COG_OK);
    CHECK(cog_store_owner_acquire(st,"effect",two,&g)==COG_ERROR_INDETERMINATE && g==NULL);
    CHECK(cog_store_owner_acquire_recovery(st,"effect",two,&g)==COG_OK);
    CHECK(cog_store_owner_guard_borrow(g,"effect")==COG_ERROR_INVALID_STATE);
    cog_store_load_report_t report;
    CHECK(cog_store_load_ex(st,"effect",COG_STORE_LOAD_REPLAY,&s,&report)==COG_OK && report.indeterminate_tools==1U);
    assistant=cog_session_root(s);
    cog_tool_execution_state_t state;CHECK(cog_session_tool_state(s,assistant,"call-1",&state)==COG_OK && state==COG_TOOL_EXECUTION_INDETERMINATE);
    CHECK(cog_store_append(st,s,assistant)==COG_ERROR_INVALID_STATE);
    CHECK(cog_store_checkpoint_save(st,s,g,&cp)==COG_ERROR_INVALID_STATE);
    CHECK(cog_store_save(st,s)==COG_OK); /* conservative normalization only */
    const cog_message_t result={.role=COG_MESSAGE_ROLE_TOOL,.tool_call_id="call-1",.content="effects remain unknown",.content_len=22U};
    CHECK(cog_session_reconcile_tool(s,assistant,"call-1",COG_TOOL_EXECUTION_ACKNOWLEDGED,&result,NULL)==COG_OK);
    CHECK(cog_store_save(st,s)==COG_OK);
    CHECK(cog_store_owner_guard_borrow(g,"effect")==COG_ERROR_INVALID_STATE);
    cog_session_t *bad=NULL;cog_session_snapshot_t snap;CHECK(cog_session_snapshot_view(s,&snap)==COG_OK);
    CHECK(cog_session_restore(&bad,NULL,&snap)==COG_OK);append_user(bad);
    CHECK(cog_store_save(st,bad)==COG_ERROR_INVALID_STATE);cog_session_free(bad);
    CHECK(cog_store_owner_read(st,"effect",&two,&cp)==COG_OK);
    CHECK(cog_store_owner_release(g)==COG_OK);
    CHECK(cog_store_owner_compare_exchange(st,"effect",two,three,cp)==COG_OK);
    CHECK(cog_store_owner_acquire(st,"effect",three,&g)==COG_OK);
    /* A final result/audit cannot be replaced by another valid replacement object. */
    CHECK(cog_session_snapshot_view(s,&snap)==COG_OK);
    cog_node_data_t nodes[2];CHECK(snap.node_count==2U);memcpy(nodes,snap.nodes,sizeof(nodes));
    cog_message_t changed=*nodes[1].message;changed.content="invented";changed.content_len=8U;
    nodes[1].message=&changed;nodes[1].content=changed.content;nodes[1].content_len=changed.content_len;nodes[1].tool_result=changed.content;
    snap.nodes=nodes;
    CHECK(cog_session_restore(&bad,NULL,&snap)==COG_OK);
    CHECK(cog_store_save(st,bad)==COG_ERROR_INVALID_STATE);cog_session_free(bad);
    append_user(s);CHECK(cog_store_save(st,s)==COG_OK);
    CHECK(cog_store_owner_release(g)==COG_OK);cog_session_free(s);cog_store_free(st);
}
static void proposed_denial(const char *base)
{
    cog_store_t *st=store(base,true);cog_session_t *s=session("denied");
    cog_session_node_t *assistant=proposal(s);cog_store_owner_t owner={1U,987U};
    CHECK(cog_store_owner_compare_exchange(st,"denied",(cog_store_owner_t){0},owner,(cog_store_checkpoint_t){0})==COG_OK);
    cog_store_owner_guard_t *g=NULL;CHECK(cog_store_owner_acquire(st,"denied",owner,&g)==COG_OK);
    CHECK(cog_store_save(st,s)==COG_OK);CHECK(cog_store_owner_release(g)==COG_OK);
    CHECK(cog_store_owner_acquire(st,"denied",owner,&g)==COG_OK);
    CHECK(cog_session_tool_transition(s,assistant,"call-1",COG_TOOL_EXECUTION_PROPOSED,COG_TOOL_EXECUTION_DENIED)==COG_OK);
    const cog_message_t result={.role=COG_MESSAGE_ROLE_TOOL,.tool_call_id="call-1",.content="policy denied; zero effects",.content_len=27U};
    cog_session_node_t *n=NULL;CHECK(cog_session_append(s,&result,&n)==COG_OK);
    CHECK(cog_store_append(st,s,n)==COG_ERROR_INVALID_STATE); /* existing ledger update uses save */
    CHECK(cog_store_save(st,s)==COG_OK);CHECK(cog_store_owner_release(g)==COG_OK);
    cog_session_free(s);CHECK(cog_store_load(st,"denied",&s)==COG_OK);
    cog_tool_execution_state_t state;CHECK(cog_session_tool_state(s,cog_session_root(s),"call-1",&state)==COG_OK && state==COG_TOOL_EXECUTION_DENIED);
    cog_session_free(s);cog_store_free(st);
}
static void corruption(const char *base)
{
    cog_store_t *st=store(base,false);cog_session_t *s=session("corrupt"),*out=NULL;
    CHECK(cog_store_save(st,s)==COG_OK);append_user(s);CHECK(cog_store_save(st,s)==COG_OK);
    char p[1024];CHECK(snprintf(p,sizeof(p),"%s/corrupt/generation-%020u.bin",base,2U)>0);
    int fd=open(p,O_WRONLY);CHECK(fd>=0);unsigned char x=0U;CHECK(pwrite(fd,&x,1U,60)==1);CHECK(close(fd)==0);
    cog_store_load_report_t r={.source_version=999U};
    CHECK(cog_store_load_ex(st,"corrupt",COG_STORE_LOAD_REPLAY,&out,&r)==COG_ERROR_IO && out==NULL && r.source_version==999U);
    cog_store_owner_t owner={99U,88U};cog_store_checkpoint_t cp={77U};
    CHECK(cog_store_owner_read(st,"corrupt",&owner,&cp)==COG_ERROR_IO && owner.epoch==99U && cp.generation==77U);
    CHECK(cog_store_save(st,s)==COG_ERROR_IO);
    CHECK(cog_store_checkpoint_load(st,"corrupt",(cog_store_checkpoint_t){1U},COG_STORE_LOAD_REPLAY,&out,&r)==COG_OK);
    CHECK(cog_session_is_empty(out));cog_session_free(out);cog_session_free(s);cog_store_free(st);
}
int main(void)
{
    char base[]="/tmp/cog-store-adversarial-XXXXXX";CHECK(mkdtemp(base)!=NULL);
    ownership(base);recovery(base);proposed_denial(base);corruption(base);cleanup(base);(void)puts("test_store_adversarial: ok");return 0;
}

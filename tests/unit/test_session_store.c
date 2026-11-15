/** @file test_session_store.c @brief Public v2 store and legacy migration contracts. */
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
static void remove_tree(const char *path)
{
    DIR *d = opendir(path);
    if (d != NULL) {
        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            if (strcmp(de->d_name,".") == 0 || strcmp(de->d_name,"..") == 0) continue;
            char p[1024]; CHECK(snprintf(p,sizeof(p),"%s/%s",path,de->d_name) > 0);
            struct stat st; CHECK(lstat(p,&st) == 0);
            if (S_ISDIR(st.st_mode)) remove_tree(p); else CHECK(unlink(p) == 0);
        }
        CHECK(closedir(d) == 0); CHECK(rmdir(path) == 0);
    }
}
static void write_text(const char *path, const char *text)
{
    FILE *f = fopen(path,"wb"); CHECK(f != NULL);
    size_t n = strlen(text); CHECK(fwrite(text,1U,n,f) == n); CHECK(fclose(f) == 0);
}
static cog_session_t *new_session(const char *id)
{
    cog_session_t *s = NULL;
    CHECK(cog_session_new(&s,&(cog_session_config_t){.id=id,.max_nodes=32U,.max_payload_bytes=65536U}) == COG_OK);
    return s;
}
static cog_session_node_t *add(cog_session_t *s, const char *text)
{
    cog_session_node_t *n = NULL;
    cog_message_t m = {.role=COG_MESSAGE_ROLE_USER,.content=text,.content_len=strlen(text)};
    CHECK(cog_session_append(s,&m,&n) == COG_OK); return n;
}
static void basics(const char *base)
{
    cog_store_t *st = NULL;
    cog_store_config_t c = {.base_dir=base,.auto_save=true};
    CHECK(cog_store_new(NULL,&c) == COG_ERROR_NULL_PTR);
    CHECK(cog_store_new(&st,&c) == COG_OK);
    CHECK(cog_store_auto_save(st)); CHECK(!cog_store_requires_owner(st));
    CHECK(!cog_store_exists(st,"bad/id"));
    cog_session_t *s = new_session("z"), *loaded = NULL;
    CHECK(cog_store_save(st,NULL) == COG_ERROR_NULL_PTR);
    CHECK(cog_store_save(st,s) == COG_OK);
    CHECK(cog_store_load(st,"z",&loaded) == COG_OK); CHECK(cog_session_is_empty(loaded));
    cog_session_free(loaded);
    cog_session_node_t *first = add(s,"hello"), *second;
    CHECK(cog_store_append(st,s,first) == COG_OK);
    CHECK(cog_session_reset(s) == COG_ERROR_INVALID_STATE);
    CHECK(cog_session_discard_node(s,first) == COG_ERROR_INVALID_STATE);
    CHECK(cog_store_append(st,s,first) == COG_ERROR_INVALID_STATE);
    second = add(s,"second"); CHECK(cog_store_append(st,s,second) == COG_OK);
    cog_store_load_report_t report = {0};
    CHECK(cog_store_load_ex(st,"z",COG_STORE_LOAD_REPLAY,&loaded,&report) == COG_OK);
    CHECK(report.source_version == 2U && report.checkpoint_generation == 3U && report.replay_safe);
    cog_session_snapshot_t v;
    CHECK(cog_session_snapshot_view(loaded,&v) == COG_OK);
    CHECK(v.node_count == 2U && v.node_id_high_water == 2U && strcmp(v.nodes[0].content,"hello") == 0);
    CHECK(cog_store_save(st,loaded) == COG_OK);
    cog_session_free(loaded); loaded = NULL;
    cog_session_t *replacement = new_session("z");
    CHECK(cog_store_save(st,replacement) == COG_ERROR_INVALID_STATE); cog_session_free(replacement);
    cog_session_t *foreign = new_session("foreign");
    CHECK(cog_store_append(st,s,add(foreign,"bad")) == COG_ERROR_INVALID_STATE); cog_session_free(foreign);
    CHECK(cog_store_delete(st,"z") == COG_ERROR_UNSUPPORTED);
    cog_session_t *a = new_session("a"); CHECK(cog_store_save(st,a) == COG_OK); cog_session_free(a);
    char **ids = NULL; size_t count = 0U;
    CHECK(cog_store_list(st,&ids,&count) == COG_OK && count == 2U);
    CHECK(strcmp(ids[0],"a") == 0 && strcmp(ids[1],"z") == 0);
    for (size_t i=0U;i<count;++i) free(ids[i]);
    free(ids);
    CHECK(cog_store_load(st,"absent",&loaded) == COG_ERROR_NOT_FOUND && loaded == NULL);
    const char *const bad[] = {"", ".", "..", "a/b", "a\\b", "bad\001", ".cog-domain.lock"};
    for (size_t i=0U;i<sizeof(bad)/sizeof(bad[0]);++i) {
        CHECK(cog_store_load(st,bad[i],&loaded) == COG_ERROR_INVALID_ARG);
        CHECK(cog_store_delete(st,bad[i]) == COG_ERROR_INVALID_ARG);
    }
    char long_id[130]; memset(long_id,'x',sizeof(long_id)-1U); long_id[sizeof(long_id)-1U]='\0';
    CHECK(cog_store_load(st,long_id,&loaded) == COG_ERROR_OVERFLOW);
    CHECK(cog_store_load_ex(st,"z",(cog_store_load_mode_t)3,&loaded,&report) == COG_ERROR_INVALID_ARG);
    cog_store_free(st); cog_session_free(s);
}
static void limits(const char *base)
{
    cog_store_t *st = NULL;
    cog_store_config_t c = {.base_dir=base,.max_sessions=1U,.max_record_bytes=8192U};
    CHECK(cog_store_new(&st,&c) == COG_OK);
    cog_session_t *s = new_session("one"), *t = new_session("two");
    CHECK(cog_store_save(st,s) == COG_OK); CHECK(cog_store_save(st,t) == COG_ERROR_LIMIT);
    CHECK(cog_store_exists(st,"one") && !cog_store_exists(st,"two"));
    cog_store_free(st); c.max_record_bytes=80U; CHECK(cog_store_new(&st,&c) == COG_OK);
    CHECK(cog_store_save(st,s) == COG_ERROR_LIMIT); cog_store_free(st);
    c.compress=true; CHECK(cog_store_new(&st,&c) == COG_ERROR_UNSUPPORTED && st == NULL);
    c.compress=false; c.backend=&cog_store_null_backend;
    CHECK(cog_store_new(&st,&c) == COG_OK);
    CHECK(cog_store_save(st,s) == COG_ERROR_UNSUPPORTED);
    CHECK(cog_store_append(st,s,add(s,"text")) == COG_ERROR_UNSUPPORTED);
    cog_session_t *out=NULL; CHECK(cog_store_load(st,"one",&out) == COG_ERROR_NOT_FOUND && out == NULL);
    cog_store_free(st); c.require_owner=true;
    CHECK(cog_store_new(&st,&c) == COG_ERROR_UNSUPPORTED && st == NULL);
    cog_session_free(s); cog_session_free(t);
}
static void structured(const char *base)
{
    cog_store_t *st=NULL; CHECK(cog_store_new(&st,&(cog_store_config_t){.base_dir=base}) == COG_OK);
    cog_session_t *s=new_session("structured"),*out=NULL;
    unsigned char bytes[]={0U,255U,42U};
    cog_message_opaque_t opaque={.namespace_id="example",.format="v1",.data=bytes,.size=sizeof(bytes)};
    cog_message_t message={.role=COG_MESSAGE_ROLE_ASSISTANT,.content="answer",.content_len=6U,
        .opaque=&opaque,.opaque_count=1U,.usage={.tokens_known=true,.input_tokens=4U,.output_tokens=2U,
        .reasoning_tokens_known=true,.reasoning_tokens=1U,.cost_known=true,.cost=0.125,.currency="USD"}};
    CHECK(cog_session_append(s,&message,NULL) == COG_OK);
    CHECK(cog_store_save(st,s) == COG_OK && cog_store_load(st,"structured",&out) == COG_OK);
    cog_session_snapshot_t v; CHECK(cog_session_snapshot_view(out,&v) == COG_OK);
    const cog_message_t *m=v.nodes[0].message;
    CHECK(m!=NULL && m->opaque_count==1U && m->opaque[0].size==sizeof(bytes));
    CHECK(memcmp(m->opaque[0].data,bytes,sizeof(bytes))==0 && m->usage.cost_known && m->usage.cost==0.125);
    cog_session_free(s); cog_session_free(out); cog_store_free(st);
}
static void legacy(const char *base)
{
    char dir[1024],meta[1100],nodes[1100],manifest[1100];
    CHECK(snprintf(dir,sizeof(dir),"%s/legacy",base)>0); CHECK(mkdir(dir,0700)==0);
    CHECK(snprintf(meta,sizeof(meta),"%s/meta.json",dir)>0); CHECK(snprintf(nodes,sizeof(nodes),"%s/nodes.jsonl",dir)>0);
    CHECK(snprintf(manifest,sizeof(manifest),"%s/manifest.bin",dir)>0);
    write_text(meta,"{\"version\":1,\"id\":\"bGVnYWN5\",\"parent_id\":null,\"max_nodes\":8,\"node_count\":0,\"next_node_id\":1,\"active_node_id\":0}");
    write_text(nodes,"");
    cog_store_t *st=NULL; CHECK(cog_store_new(&st,&(cog_store_config_t){.base_dir=base})==COG_OK);
    cog_session_t *s=NULL; cog_store_load_report_t r={0};
    CHECK(cog_store_load_ex(st,"legacy",COG_STORE_LOAD_REPLAY,&s,&r)==COG_OK);
    CHECK(r.source_version==1U && r.migrated_in_memory && access(manifest,F_OK)!=0);
    CHECK(cog_store_append(st,s,add(s,"explicit migration"))==COG_ERROR_UNSUPPORTED);
    CHECK(cog_store_save(st,s)==COG_OK); CHECK(access(manifest,F_OK)==0);
    CHECK(access(meta,F_OK)==0 && access(nodes,F_OK)==0);
    cog_session_free(s); cog_store_free(st);
    /* A distinct unowned legacy identity remains deletable. */
    CHECK(snprintf(dir,sizeof(dir),"%s/deletable",base)>0); CHECK(mkdir(dir,0700)==0);
    CHECK(snprintf(meta,sizeof(meta),"%s/meta.json",dir)>0); write_text(meta,"legacy");
    CHECK(cog_store_new(&st,&(cog_store_config_t){.base_dir=base})==COG_OK);
    CHECK(cog_store_delete(st,"deletable")==COG_OK); CHECK(cog_store_delete(st,"deletable")==COG_ERROR_NOT_FOUND);
    cog_store_free(st);
}
/* Public custom callback contexts are exact, including a stateless NULL context. */
typedef struct custom { const cog_session_t *source; unsigned calls; } custom_t;
static cog_result_t custom_save(void *ctx,const cog_session_t *s)
{ custom_t *c=ctx; CHECK(c!=NULL && c->source==s); ++c->calls; return COG_OK; }
static cog_result_t custom_load(void *ctx,const char *id,cog_session_t **out)
{
    custom_t *c=ctx; CHECK(c!=NULL && strcmp(id,cog_session_id(c->source))==0); ++c->calls;
    cog_session_snapshot_t v; CHECK(cog_session_snapshot_view(c->source,&v)==COG_OK);
    return cog_session_restore(out,NULL,&v);
}
static cog_result_t custom_load_ex(void *ctx,const char *id,cog_store_load_mode_t mode,cog_session_t **out,cog_store_load_report_t *r)
{ CHECK(mode==COG_STORE_LOAD_REPLAY); cog_result_t rc=custom_load(ctx,id,out); if(rc==COG_OK)*r=(cog_store_load_report_t){.source_version=2U,.replay_safe=true}; return rc; }
static cog_result_t custom_append(void *ctx,const cog_session_t *s,const cog_session_node_t *n)
{ CHECK(n!=NULL); return custom_save(ctx,s); }
static cog_result_t custom_delete(void *ctx,const char *id)
{ custom_t *c=ctx; CHECK(c!=NULL && id!=NULL); ++c->calls; return COG_ERROR_UNSUPPORTED; }
static cog_result_t custom_list(void *ctx,char ***ids,size_t *n)
{ custom_t *c=ctx; CHECK(c!=NULL); ++c->calls; *ids=NULL; *n=0U; return COG_OK; }
static bool custom_exists(void *ctx,const char *id)
{ custom_t *c=ctx; CHECK(c!=NULL && id!=NULL); ++c->calls; return true; }
static const cog_store_vtable_t custom_vtable={.name="custom",.save=custom_save,.load=custom_load,.append=custom_append,
    .delete_=custom_delete,.list=custom_list,.exists=custom_exists,.load_ex=custom_load_ex};
/* Saving must protect a live slot beyond an earlier discarded draft hole. */
static void sparse_committed(const char *base, bool custom)
{
    cog_session_t *s=new_session(custom ? "sparse-custom" : "sparse-file");
    cog_store_t *st=NULL;custom_t context={.source=s};
    cog_store_config_t cfg={.base_dir=base,.backend=custom ? &custom_vtable : NULL,.backend_context=&context};
    CHECK(cog_store_new(&st,&cfg)==COG_OK);
    cog_session_node_t *root=add(s,"root"),*first=add(s,"discarded");
    CHECK(cog_session_fork(s,root,NULL)==COG_OK);
    const cog_message_t m={.role=COG_MESSAGE_ROLE_USER,.content="retained",.content_len=8U};
    cog_session_node_t *second=NULL;CHECK(cog_session_add_message(s,root,&m,&second)==COG_OK);
    CHECK(cog_session_discard_node(s,first)==COG_OK);
    CHECK(cog_store_save(st,s)==COG_OK);
    CHECK(cog_session_discard_node(s,second)==COG_ERROR_INVALID_STATE);
    CHECK(cog_session_reset(s)==COG_ERROR_INVALID_STATE);
    cog_session_snapshot_t v;CHECK(cog_session_snapshot_view(s,&v)==COG_OK && v.node_count==2U);
    CHECK(strcmp(v.nodes[1].content,"retained")==0);
    cog_store_free(st);cog_session_free(s);
}
static void custom_backend(void)
{
    cog_session_t *s=new_session("custom"),*out=NULL; custom_t c={.source=s}; cog_store_t *st=NULL;
    CHECK(cog_store_new(&st,&(cog_store_config_t){.backend=&custom_vtable,.backend_context=&c})==COG_OK);
    CHECK(cog_store_save(st,s)==COG_OK); CHECK(cog_store_append(st,s,add(s,"message"))==COG_OK);
    CHECK(cog_store_load(st,"custom",&out)==COG_OK && out!=NULL); CHECK(cog_store_exists(st,"custom"));
    CHECK(cog_store_delete(st,"custom")==COG_ERROR_UNSUPPORTED);
    char **ids=NULL;size_t n=0U;CHECK(cog_store_list(st,&ids,&n)==COG_OK && ids==NULL && n==0U);
    CHECK(c.calls==6U); CHECK(cog_session_reset(s)==COG_ERROR_INVALID_STATE);
    CHECK(cog_session_discard_node(s,cog_session_active(s))==COG_ERROR_INVALID_STATE); cog_store_free(st); cog_session_free(s); cog_session_free(out);
    cog_store_vtable_t bad=custom_vtable;bad.load_ex=NULL;
    CHECK(cog_store_new(&st,&(cog_store_config_t){.backend=&bad})==COG_ERROR_UNSUPPORTED && st==NULL);
    bad.save=NULL; CHECK(cog_store_new(&st,&(cog_store_config_t){.backend=&bad})==COG_ERROR_INVALID_ARG && st==NULL);
}
int main(void)
{
    char a[]="/tmp/cog-store-v2-XXXXXX",b[]="/tmp/cog-store-limit-XXXXXX",c[]="/tmp/cog-store-structured-XXXXXX";
    CHECK(mkdtemp(a)!=NULL && mkdtemp(b)!=NULL && mkdtemp(c)!=NULL);
    basics(a);limits(b);structured(c);legacy(a);custom_backend();sparse_committed(a,false);sparse_committed(a,true);
    remove_tree(a);remove_tree(b);remove_tree(c);(void)puts("test_session_store: ok");return 0;
}

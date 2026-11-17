/** @file test_configuration_clone.c @brief Exact copied ownership and rollback. */
#include "configuration.h"
#include "options_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"line %d: %s\n",__LINE__,#x); return 1; } } while (0)
void *__real_calloc(size_t count, size_t size);
void __real_free(void *p);
void *__wrap_calloc(size_t count, size_t size);
void __wrap_free(void *p);
static bool fail_next, watch_next, uncleared;
static void *watched;
static size_t watched_size, reads;
void *__wrap_calloc(size_t count, size_t size)
{
    if (fail_next) { fail_next = false; return NULL; }
    void *p = __real_calloc(count,size);
    if (watch_next && p != NULL) { watched=p; watched_size=count*size; watch_next=false; }
    return p;
}
void __wrap_free(void *p)
{
    if (p != NULL && p == watched) {
        const unsigned char *bytes=p;
        for (size_t i=0U;i<watched_size;++i) if (bytes[i]!=0U) uncleared=true;
        watched=NULL;
    }
    __real_free(p);
}
static cog_result_t resolve(void *arg,const char *ref,char *out,size_t cap,size_t *required)
{
    (void)arg; (void)ref; ++reads;
    static const char secret[]="synthetic-clone-secret";
    *required=sizeof(secret);
    if (cap<sizeof(secret)) return COG_ERROR_LIMIT;
    memcpy(out,secret,sizeof(secret)); return COG_OK;
}
int main(void)
{
    cog_cli_options_t o; cog_cli_options_defaults(&o);
    o.backend=COG_CLI_BACKEND_REAL; o.model_id="fixture/model";
    o.credential_ref="fixture:clone"; o.credential_read=resolve;
    o.store_mode=COG_CLI_STORE_DURABLE; o.store_dir="/fixture/store";
    o.workspace_root="/fixture/workspace"; o.prompt="initial task"; o.queue_text="second task";
    o.session_id="clone-session";
    o.explicit_options=COG_CLI_OPT_BACKEND|COG_CLI_OPT_MODEL|COG_CLI_OPT_CREDENTIAL|
        COG_CLI_OPT_STORE_MODE|COG_CLI_OPT_STORE_DIR|COG_CLI_OPT_WORKSPACE;
    cog_cli_configuration_t *source=NULL,*mirror=NULL,*copy=NULL;
    CHECK(cog_cli_configuration_new(&source,&o,37U)==COG_OK && reads==1U);
    fail_next=true;
    CHECK(cog_cli_configuration_clone_new(&mirror,source,NULL)==COG_ERROR_NOMEM && mirror==NULL);
    CHECK(cog_cli_configuration_clone_new(&mirror,source,NULL)==COG_OK);
    const cog_cli_options_t *a=cog_cli_configuration_options(source),*b=cog_cli_configuration_options(mirror);
    CHECK(a->prompt!=b->prompt && strcmp(a->prompt,b->prompt)==0);
    CHECK(a->queue_text!=b->queue_text && strcmp(a->queue_text,b->queue_text)==0);
    CHECK(a->model_id!=b->model_id && strcmp(a->model_id,b->model_id)==0);
    CHECK(b->net==NULL && b->credential_read==NULL && b->credential_arg==NULL);
    uint32_t mask=0U;
    CHECK(cog_cli_configuration_tool_mask(mirror,&mask)==COG_OK && mask==15U);
    static const cog_provider_vtable_t table={0};
    CHECK(cog_cli_configuration_bind(mirror,&table,&table,NULL,NULL)==COG_ERROR_NULL_PTR);
    CHECK(cog_cli_configuration_finish_host(mirror,NULL,"/fixture/workspace","/fixture/store")==COG_ERROR_INVALID_STATE);
    cog_cli_configuration_free(source); source=NULL;
    CHECK(strcmp(b->prompt,"initial task")==0 && strcmp(b->session_id,"clone-session")==0);
    /* Borrow token only; no provider construction or transport operation. */
    unsigned char token;
    cog_net_ctx_t *net=(cog_net_ctx_t *)(void *)&token;
    watch_next=true;
    CHECK(cog_cli_configuration_clone_new(&copy,mirror,net)==COG_OK);
    cog_provider_limits_t limits={.request_bytes=1U,.parser_bytes=1U,.writer_bytes=1U,
        .max_json_depth=1U,.max_frame_bytes=1U,.retained_delta_bytes=1U,.retained_delta_count=1U,
        .max_text_bytes=1U,.max_tool_calls=1U,.max_tool_id_bytes=1U,.max_tool_name_bytes=1U,
        .max_tool_arguments_bytes=1U,.max_opaque_bytes=1U};
    CHECK(cog_cli_configuration_bind(copy,&table,&table,NULL,&limits)==COG_OK);
    const cog_endpoint_t *endpoint=cog_cli_configuration_endpoint(copy);
    CHECK(endpoint!=NULL && endpoint->generation==37U);
    CHECK(strcmp(endpoint->api_key,"synthetic-clone-secret")==0);
    CHECK(cog_cli_configuration_options(copy)->net==net && reads==1U);
    cog_cli_configuration_free(mirror); mirror=NULL;
    CHECK(strcmp(endpoint->api_key,"synthetic-clone-secret")==0);
    cog_cli_configuration_free(copy); copy=NULL;
    CHECK(watched==NULL && !uncleared);
    static const cog_mock_script_t script={0};
    cog_cli_options_defaults(&o); o.backend=COG_CLI_BACKEND_MOCK; o.script=&script;
    o.prompt="mock task"; o.queue_text="mock queue"; o.explicit_options=COG_CLI_OPT_BACKEND;
    CHECK(cog_cli_configuration_new(&source,&o,8U)==COG_OK);
    CHECK(cog_cli_configuration_clone_new(&copy,source,net)==COG_ERROR_INVALID_ARG && copy==NULL);
    CHECK(cog_cli_configuration_bind(source,&table,&table,&script,NULL)==COG_OK);
    CHECK(cog_cli_configuration_clone_new(&copy,source,NULL)==COG_OK);
    cog_cli_configuration_free(source);
    CHECK(cog_cli_configuration_bind(copy,&table,&table,&script,NULL)==COG_OK);
    CHECK(cog_cli_configuration_endpoint(copy)->generation==8U);
    CHECK(strcmp(cog_cli_configuration_options(copy)->queue_text,"mock queue")==0);
    CHECK(cog_cli_configuration_options(copy)->script==&script);
    cog_cli_configuration_free(copy);
    puts("configuration clone: PASS"); return 0;
}

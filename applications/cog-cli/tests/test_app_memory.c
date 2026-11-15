/** @file test_app_memory.c @brief Constructor OOM rollback and reserved text run. */
#include "cog_cli/cli.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
/* Observe linker-wrapper effects across builtin/libc leaf assumptions. */
static volatile bool inject;
static volatile size_t allocation, fail_at;
void *__real_malloc(size_t size);
void *__real_calloc(size_t count,size_t size);
void *__wrap_malloc(size_t size);
void *__wrap_calloc(size_t count,size_t size);
void *__wrap_malloc(size_t size)
{if(inject && ++allocation==fail_at)return NULL;return __real_malloc(size);}
void *__wrap_calloc(size_t count,size_t size)
{if(inject && ++allocation==fail_at)return NULL;return __real_calloc(count,size);}
#define REQUIRE(x) do {if(!(x)){fprintf(stderr,"%s:%d: %s (%zu)\n",__FILE__,__LINE__,#x,fail_at);return 1;}}while(0)
/* Positive control: optimized direct allocator calls must reach wrappers,
 * update the real oracle, and honor its fault injection before use. */
static void allocation_oracle_control(void)
{
    inject = true;
    size_t before = allocation;
    unsigned char *bytes = malloc(8U);
    assert(bytes != NULL && allocation == before + 1U);
    bytes[0] = 0x5aU;
    free(bytes);
    before = allocation;
    bytes = calloc(8U, 1U);
    assert(bytes != NULL && allocation == before + 1U);
    assert(bytes[0] == 0U && bytes[7] == 0U);
    free(bytes);
    before = allocation;
    bytes = malloc(8U);
    free(bytes);
    assert(allocation == before + 1U); /* Detect optimized-away allocator pairs. */
    before = allocation;
    fail_at = allocation + 1U;
    bytes = malloc(8U);
    assert(bytes == NULL && allocation == before + 1U);
    fail_at = 0U;
    allocation = 0U;
    inject = false;
}

int main(void)
{
    allocation_oracle_control();
    cog_cli_options_t opts={.backend=COG_CLI_BACKEND_MOCK,.explicit_options=COG_CLI_OPT_BACKEND,.prompt="hello"};
    bool complete=false;
    for(size_t i=1;i<512;i++) {
        cog_cli_app_t *app=NULL;allocation=0;fail_at=i;inject=true;
        cog_result_t rc=cog_cli_app_new(&app,&opts);inject=false;
        if(rc==COG_OK) {cog_cli_app_free(app);complete=true;break;}
        REQUIRE(rc==COG_ERROR_NOMEM && app==NULL);
    }
    REQUIRE(complete);
    cog_cli_app_t *app=NULL;REQUIRE(cog_cli_app_new(&app,&opts)==COG_OK);
    allocation=0;fail_at=1;inject=true;
    cog_result_t rc=cog_cli_app_run(app);inject=false;
    REQUIRE(rc==COG_OK && allocation==0U);
    cog_cli_app_free(app);
    /* Reload's explicit constructors may allocate; publication's later service,
       activation, notification and next model turn use only reserved storage. */
    REQUIRE(cog_cli_app_new(&app,&opts)==COG_OK);
    uint64_t generation=0U;
    REQUIRE(cog_cli_app_reload(app,&(cog_cli_reload_options_t){.model_id="replacement"},&generation)==COG_OK);
    allocation=0;fail_at=1U;inject=true;
    rc=cog_cli_app_run(app);inject=false;
    REQUIRE(rc==COG_OK && allocation==0U && generation==2U);
    cog_cli_app_free(app);return 0;
}

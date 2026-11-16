/** Actual file guard consuming-close error must not leave a dangling app owner. */
#define _POSIX_C_SOURCE 200809L
#include "app_internal.h"
#include "execution_internal.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
static bool inject,inside_release,triggered;
int __real_close(int fd);
int __wrap_close(int fd);
cog_result_t __real_cog_store_owner_release(cog_store_owner_guard_t *guard);
cog_result_t __wrap_cog_store_owner_release(cog_store_owner_guard_t *guard);
int __wrap_close(int fd)
{
    int rc=__real_close(fd);
    if(inject && inside_release && rc==0) { inject=false;triggered=true;errno=EIO;return -1; }
    return rc;
}
cog_result_t __wrap_cog_store_owner_release(cog_store_owner_guard_t *guard)
{
    inside_release=true;
    cog_result_t rc=__real_cog_store_owner_release(guard);
    inside_release=false;return rc;
}
int main(int argc,char **argv)
{
    assert(argc==3);
    char program[]="close",mock[]="--mock";char *args[]={program,mock};int status=0;
    cog_cli_options_t options;assert(cog_cli_options_parse(&options,2,args,&status)==COG_OK);
    options.config_path="/dev/null";options.store_mode=COG_CLI_STORE_DURABLE;
    options.store_dir=argv[1];options.workspace_root=argv[2];
    options.explicit_options|=COG_CLI_OPT_STORE_MODE|COG_CLI_OPT_STORE_DIR|COG_CLI_OPT_WORKSPACE;
    cog_cli_configuration_t *configuration=NULL;
    assert(cog_cli_configuration_new(&configuration,&options,1U)==COG_OK);
    cog_cli_execution_admission_t admission={.kind=COG_CLI_EXECUTION_ROOT,
        .configuration=configuration,.configuration_generation=1U};
    cog_cli_app_t *app=NULL;assert(cog_cli_execution_new(&app,&admission)==COG_OK);
    assert(app->guard!=NULL && app->workspace!=NULL && app->core!=NULL);
    inject=true;
    cog_result_t rc=cog_cli_execution_close(&app,COG_OK);
    assert(triggered && rc==COG_ERROR_IO);
    bool consumed=app==NULL;
    printf("consuming file guard close IO: app_consumed=%d expected=1\n",(int)consumed);
    if(app!=NULL) {
        /* Diagnostic-only safe teardown: source has already destroyed these
           resources but retained their addresses. Never dereference them. */
        assert(app->guard!=NULL && app->workspace!=NULL && app->core==NULL && app->configuration==NULL);
        app->guard=NULL;app->workspace=NULL;
        (void)cog_cli_execution_close(&app,COG_OK);
        assert(app==NULL);
    }
    return consumed?0:1;
}

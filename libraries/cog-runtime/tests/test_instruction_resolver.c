#define _POSIX_C_SOURCE 200809L
#include "../src/execution.h"
#include "../src/runtime_internal.h"
#include "../src/instruction_transfer.h"
#include <assert.h>
#include <cog/workspace.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

static cog_instruction_snapshot_t *resolve(cog_runtime_t *r,
    const cog_instruction_snapshot_t *base, unsigned operation,
    const char *skill, const char *path, uint64_t id, cog_result_t expected) {
    runtime_instruction_job_t *job = NULL, *occupied = NULL;
    assert(cli_runtime_resolver_new(&job,r,base,operation,"root",skill,path,id)==COG_OK);
    assert(cli_runtime_resolver_new(&occupied,r,base,operation,"root",skill,path,id)==COG_ERROR_AGAIN);
    bool done=false;
    cog_result_t rc=COG_ERROR_AGAIN;
    for(size_t i=0U;i<10000U && !done;i++) {
        rc=cli_runtime_resolver_step(job,&done);
        assert(rc==COG_ERROR_AGAIN || done);
    }
    assert(done && rc==expected);
    cog_instruction_snapshot_t *candidate=NULL;
    assert(cli_runtime_resolver_take(job,&candidate)==expected);
    assert(cli_runtime_resolver_free(&job)==COG_OK && job==NULL);
    assert(r->resolver_job==NULL);
    return candidate;
}
static void write_file(const char *directory,const char *name,const char *text) {
    char path[4096];
    int n=snprintf(path,sizeof(path),"%s/%s",directory,name);
    assert(n>0 && (size_t)n<sizeof(path));
    FILE *file=fopen(path,"wb"); assert(file!=NULL);
    assert(fwrite(text,1U,strlen(text),file)==strlen(text));
    assert(fclose(file)==0);
}
int main(void) {
    char directory[]="resolver-XXXXXX";
    assert(mkdtemp(directory)!=NULL);
    char cwd[4096],absolute[4096]; assert(getcwd(cwd,sizeof(cwd))!=NULL);
    int n=snprintf(absolute,sizeof(absolute),"%s/%s",cwd,directory);
    assert(n>0 && (size_t)n<sizeof(absolute));
    cog_runtime_t r={0};
    cog_prompt_tool_binding_t archived={&cog_workspace_tools.tools[0],COG_PROMPT_TOOL_ASK};
    cog_prompt_tool_binding_t host[]={
        {&cog_workspace_tools.tools[0],COG_PROMPT_TOOL_ALLOW},
        {&cog_workspace_tools.tools[2],COG_PROMPT_TOOL_ALLOW}};
    r.bindings=host;r.config.tools.binding_count=2U;
    r.config.instruction_limits=(cog_instruction_limits_t){32,16,4096,8192,4096,131072,64,16,16,16};
    cog_instruction_source_root_t root={"root",absolute};
    cog_instruction_source_config_t cfg={&root,1U,{1,4096,8192,32768,16,16,1,1}};
    assert(cog_instruction_source_reader_new(&r.reader,&cfg,NULL)==COG_OK);
    static const char skill_text[]="---\nname: check\ndescription: Check things\n---\nSkill body\n";
    cog_instruction_source_t sources[]={
        {COG_INSTRUCTION_HOST,"root","host.md","",NULL,0,"HOST",4},
        {COG_INSTRUCTION_SKILL,"root","check/SKILL.md","",NULL,0,skill_text,sizeof(skill_text)-1U}};
    cog_instruction_catalog_t *catalog=NULL;
    assert(cog_instruction_catalog_new(&catalog,&r.config.instruction_limits,1U,sources,2U,NULL)==COG_OK);
    cog_instruction_selection_t sel={.snapshot_id=1U,.workspace_root_id="root",.scope="",
        .model_id="test",.template_id="test",.template_revision=1U,
        .max_context_bytes=65536U,.max_output_tokens=128U,.max_tool_rounds=4U,
        .bindings=&archived,.binding_count=1U};
    cog_instruction_snapshot_t *base=NULL;
    assert(cog_instruction_snapshot_new(&base,catalog,&r.config.instruction_limits,&sel,NULL,0U,NULL)==COG_OK);
    /* Empty AGENTS chain must be a completed no-change, including repeated reads. */
    assert(resolve(&r,base,COG_CLI_INSTRUCTION_RESOURCE_SCOPE,"","",2U,COG_OK)==NULL);
    assert(resolve(&r,base,COG_CLI_INSTRUCTION_RESOURCE_SCOPE,"","",3U,COG_OK)==NULL);
    write_file(directory,"AGENTS.md","ROOT RULE");
    cog_instruction_snapshot_t *scoped=resolve(&r,base,COG_CLI_INSTRUCTION_RESOURCE_SCOPE,"","",7U,COG_OK);
    assert(scoped!=NULL);
    cog_instruction_manifest_t m;
    assert(cog_instruction_snapshot_view(scoped,&m)==COG_OK && m.snapshot_id==7U && m.source_count==3U);
    assert(resolve(&r,scoped,COG_CLI_INSTRUCTION_RESOURCE_SCOPE,"","",8U,COG_OK)==NULL);
    assert(resolve(&r,scoped,COG_CLI_INSTRUCTION_RESOURCE_LOAD,"check","data.txt",8U,COG_ERROR_DENIED)==NULL);
    cog_instruction_snapshot_t *active=resolve(&r,scoped,COG_CLI_INSTRUCTION_RESOURCE_ACTIVATE,"check","",9U,COG_OK);
    assert(active!=NULL);
    assert(resolve(&r,active,COG_CLI_INSTRUCTION_RESOURCE_ACTIVATE,"check","",10U,COG_OK)==NULL);
    char package[4096];
    n=snprintf(package,sizeof(package),"%s/check",directory);
    assert(n>0 && (size_t)n<sizeof(package) && mkdir(package,0700)==0);
    write_file(package,"data.txt","RESOURCE");
    cog_instruction_snapshot_t *loaded=resolve(&r,active,COG_CLI_INSTRUCTION_RESOURCE_LOAD,"check","data.txt",11U,COG_OK);
    assert(loaded!=NULL);
    assert(resolve(&r,loaded,COG_CLI_INSTRUCTION_RESOURCE_LOAD,"check","data.txt",12U,COG_OK)==NULL);
    assert(cog_instruction_snapshot_view(loaded,&m)==COG_OK && m.source_count==4U && m.snapshot_id==11U);
    assert(m.tool_count==1U && strcmp(m.tools[0].name,"read_file")==0 &&
        m.tools[0].policy==COG_PROMPT_TOOL_ASK);
    /* A local child's borrowed pinned reader serializes with a native resolver.
       Cancelling a waiting child must not cancel the sibling's active scan. */
    cog_runtime_t child={0};child.config=r.config;child.bindings=host;
    child.source_owner=&r;
    runtime_instruction_job_t *native=NULL,*local=NULL;
    assert(cli_runtime_resolver_new(&native,&r,loaded,COG_CLI_INSTRUCTION_RESOURCE_SCOPE,
        "root","","",13U)==COG_OK);
    bool done=false;
    assert(cli_runtime_resolver_step(native,&done)==COG_ERROR_AGAIN && !done);
    assert(r.source_lease==native);
    assert(cli_runtime_resolver_new(&local,&child,loaded,COG_CLI_INSTRUCTION_RESOURCE_LOAD,
        "root","check","data.txt",13U)==COG_OK);
    assert(cli_runtime_resolver_step(local,&done)==COG_ERROR_AGAIN && !done);
    assert(r.source_lease==native);
    child.shutdown=true;
    assert(cli_runtime_resolver_step(local,&done)==COG_ERROR_ABORTED && done);
    assert(cli_runtime_resolver_free(&local)==COG_OK && r.source_lease==native);
    child.shutdown=false;done=false;
    for(size_t i=0U;i<10000U && !done;i++) {
        cog_result_t step=cli_runtime_resolver_step(native,&done);
        assert(step==COG_ERROR_AGAIN || step==COG_OK);
    }
    assert(done && r.source_lease==NULL);
    assert(cli_runtime_resolver_free(&native)==COG_OK);
    assert(resolve(&child,loaded,COG_CLI_INSTRUCTION_RESOURCE_LOAD,"check","data.txt",14U,COG_OK)==NULL);
    assert(child.reader==NULL && r.source_lease==NULL);
    cog_instruction_snapshot_free(loaded);cog_instruction_snapshot_free(active);
    cog_instruction_snapshot_free(scoped);cog_instruction_snapshot_free(base);
    cog_instruction_catalog_free(catalog);
    assert(cog_instruction_source_reader_free(r.reader)==COG_OK);
    char path[4096];
    n=snprintf(path,sizeof(path),"%s/AGENTS.md",directory);assert(n>0 && (size_t)n<sizeof(path));assert(unlink(path)==0);
    n=snprintf(path,sizeof(path),"%s/data.txt",package);assert(n>0 && (size_t)n<sizeof(path));assert(unlink(path)==0);
    assert(rmdir(package)==0);
    assert(rmdir(directory)==0);
    return 0;
}

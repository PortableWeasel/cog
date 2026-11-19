/** @file runtime_jobs.c @brief Owner-thread background execution and status queries. */
#include "composition_internal.h"
#include "runtime_internal.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct status_query {
    cog_runtime_t *runtime;
    cog_tool_job_id_t id;
    size_t capacity;
    cog_tool_output_t result;
    bool done;
    char text[];
} status_query_t;

static cog_result_t status_new(cog_core_t *core, const char *arguments, size_t bytes,
    const cog_tool_async_config_t *config, void **out)
{
    (void)core;
    *out = NULL;
    char storage[64];
    const char *token;
    cog_result_t rc = cli_runtime_json_field(arguments, bytes, "job_id", storage, sizeof(storage), &token);
    if (rc != COG_OK || token == NULL || strlen(token) != 20U || strncmp(token, "job-", 4U) != 0)
        return COG_ERROR_INVALID_ARG;
    uint64_t id = 0U;
    for (size_t i = 4U; i < 20U; i++) {
        const char *digit = strchr("0123456789abcdef", token[i]);
        if (digit == NULL) return COG_ERROR_INVALID_ARG;
        unsigned value = token[i] <= '9' ? (unsigned)(token[i]-'0') : (unsigned)(token[i]-'a')+10U;
        id = (id << 4U) | value;
    }
    if (id == 0U || config->user == NULL) return COG_ERROR_INVALID_ARG;
    if (config->max_output_bytes >= SIZE_MAX-sizeof(status_query_t)) return COG_ERROR_LIMIT;
    status_query_t *q = calloc(1U, sizeof(*q)+config->max_output_bytes+1U);
    if (q == NULL) return COG_ERROR_NOMEM;
    q->id = id;
    q->runtime = config->user;
    q->capacity = config->max_output_bytes+1U;
    *out = q;
    return COG_OK;
}

static cog_result_t format_status(status_query_t *q, const cog_tool_job_record_t *job)
{
    const cog_tool_output_t *o = job->outcome;
    const char *state = job->state == COG_TOOL_JOB_ACKNOWLEDGED ? "unknown_outcome" :
        o == NULL ? "running" : o->status == COG_OK ? "completed" :
        o->status == COG_ERROR_ABORTED ? "cancelled" : "failed";
    int n = snprintf(q->text,q->capacity,"{\"job_id\":\"job-%016" PRIx64 "\",\"state\":\"%s\"",q->id,state);
    if (n < 0 || (size_t)n >= q->capacity) return COG_ERROR_LIMIT;
    size_t used=(size_t)n;
    if (o != NULL) {
        if (o->exit_status_known) {
            n=snprintf(q->text+used,q->capacity-used,",\"exit_status\":%d",o->exit_status);
            if (n<0 || (size_t)n>=q->capacity-used) return COG_ERROR_LIMIT;
            used+=(size_t)n;
        }
        n=snprintf(q->text+used,q->capacity-used,",\"status\":%d,\"truncated\":%s,\"side_effects_possible\":%s,\"text\":\"",
            (int)o->status,o->truncated?"true":"false",o->side_effects_possible?"true":"false");
        if (n<0 || (size_t)n>=q->capacity-used) return COG_ERROR_LIMIT;
        used+=(size_t)n;
        for (size_t i=0U;i<o->output_len;i++) {
            unsigned char c=(unsigned char)o->output[i];
            if (q->capacity-used<7U) return COG_ERROR_LIMIT;
            if (c<32U) { (void)snprintf(q->text+used,7U,"\\u%04x",(unsigned)c); used+=6U; }
            else {
                if (c=='"' || c=='\\') q->text[used++]='\\';
                q->text[used++]=(char)c;
            }
        }
        if (q->capacity-used<2U) return COG_ERROR_LIMIT;
        q->text[used++]='"';
    }
    if (q->capacity-used<2U) return COG_ERROR_LIMIT;
    q->text[used++]='}';
    q->text[used]='\0';
    q->result.output_len=used;
    return COG_OK;
}

static cog_result_t status_start(void *handle, uint64_t now)
{
    (void)now;
    status_query_t *q=handle;
    cog_tool_job_record_t record;
    q->result.status=cog_runtime_job(q->runtime,q->id,&record);
    if (q->result.status==COG_OK) q->result.status=format_status(q,&record);
    q->result.output=q->result.status==COG_OK?q->text:"";
    q->done=true;
    return COG_OK;
}
static cog_result_t status_poll(void *handle,uint64_t now,bool *done,uint64_t *deadline)
{
    (void)now;
    *done=((status_query_t *)handle)->done;
    *deadline=UINT64_MAX;
    return COG_OK;
}
static cog_result_t status_result(const void *handle,cog_tool_output_t *out)
{
    const status_query_t *q=handle;
    if (!q->done) return COG_ERROR_AGAIN;
    *out=q->result;
    return COG_OK;
}
static cog_result_t status_cancel(void *handle,uint64_t now)
{
    (void)now;
    status_query_t *q=handle;
    if (!q->done) { q->result.status=COG_ERROR_ABORTED; q->result.output=""; q->done=true; }
    return COG_OK;
}
static cog_result_t status_free(void *handle) { free(handle); return COG_OK; }
static const cog_tool_async_vtable_t status_ops={
    status_new,status_start,status_poll,status_cancel,status_result,status_free
};
const cog_tool_t cog_runtime_background_status_tool={
    .name="background_status", .description="Read the current state and terminal output of a background job.",
    .input_schema="{\"type\":\"object\",\"properties\":{\"job_id\":{\"type\":\"string\",\"minLength\":20,\"maxLength\":20}},\"required\":[\"job_id\"],\"additionalProperties\":false}",
    .async=&status_ops
};
const cog_tool_table_t *cli_runtime_job_table(const cog_tool_t *tool)
{
    static const cog_tool_table_t start={&cog_workspace_start_background_tool,1U};
    static const cog_tool_table_t status={&cog_runtime_background_status_tool,1U};
    if(tool==start.tools)return &start;
    if(tool==status.tools)return &status;
    return NULL;
}
cog_result_t cog_runtime_job(const cog_runtime_t *r,cog_tool_job_id_t id,cog_tool_job_record_t *record)
{
    if(r==NULL || record==NULL)return COG_ERROR_NULL_PTR;
    if(id==0U)return COG_ERROR_INVALID_ARG;
    if(r->offline || !r->begun || r->foreground_done)return COG_ERROR_INVALID_STATE;
    if(r->job_save_failed || r->app->recovery)return COG_ERROR_INDETERMINATE;
    const cog_tool_table_t *table=NULL;
    if(cli_tools_binding(r->app,"background_status",&table,NULL)!=COG_OK ||
        table!=cli_runtime_job_table(&cog_runtime_background_status_tool))return COG_ERROR_NOT_FOUND;
    cog_tool_job_record_t job;
    cog_result_t rc=cog_session_tool_job(r->app->session,id,&job);
    if(rc!=COG_OK)return rc;
    cog_session_snapshot_t snapshot;
    rc=cog_session_snapshot_view(r->app->session,&snapshot);
    if(rc!=COG_OK)return rc;
    for(cog_session_node_t *node=cog_session_active(r->app->session);node!=NULL;node=cog_node_parent(node)){
        const cog_node_data_t *data=cog_node_data(node);
        if(data->node_id==job.receipt_node_id && data->context_binding_id==snapshot.selected_context_binding_id){
            *record=job;
            return COG_OK;
        }
    }
    return COG_ERROR_NOT_FOUND;
}
cog_result_t cog_runtime_job_cancel(cog_runtime_t *r,cog_tool_job_id_t id)
{
    cog_tool_job_record_t job;
    cog_result_t rc=cog_runtime_job(r,id,&job);
    if(rc!=COG_OK || r->background_execution==NULL || r->background_id!=id)return rc;
    uint64_t now;
    rc=cog_cli_clock(&now);
    return rc==COG_OK?cog_toolkit_execution_cancel(r->background_execution,now):rc;
}
cog_result_t cog_runtime_job_reconcile(cog_runtime_t *r,cog_tool_job_id_t id,
    const cog_tool_output_t *outcome,const char *evidence,size_t bytes)
{
    if(r==NULL || evidence==NULL)return COG_ERROR_NULL_PTR;
    if(r->config.mode!=COG_RUNTIME_RECOVER || !r->config.previous_processes_fenced ||
       r->app->guard==NULL || r->app->recovery_write_failed)return COG_ERROR_INVALID_STATE;
    if(r->view.serial!=r->acknowledged)return COG_ERROR_AGAIN;
    cog_result_t rc=cog_session_tool_job_reconcile(r->app->session,id,outcome,evidence,bytes);
    if(rc!=COG_OK)return rc;
    rc=cog_cli_save(r->app);
    if(rc==COG_OK)r->app->display_sequence++;
    else r->app->recovery_write_failed=true;
    return rc;
}
cog_result_t cli_runtime_job_service(cog_runtime_t *r,uint64_t *deadline)
{
    *deadline=UINT64_MAX;
    cog_toolkit_execution_t *execution=r->background_execution;
    if(execution==NULL)return COG_OK;
    uint64_t now;
    cog_result_t rc=cog_cli_clock(&now);
    if(rc!=COG_OK)return rc;
    if(r->shutdown || r->app->shutdown || r->primary!=COG_OK || r->app->recovery){
        rc=cog_toolkit_execution_cancel(execution,now);
        if(rc!=COG_OK)return rc;
    }
    bool done=false;
    rc=cog_toolkit_execution_poll(execution,now,&done,deadline);
    if(rc!=COG_OK || !done)return rc;
    cog_toolkit_result_t result={0};
    size_t required=0U;
    rc=cog_toolkit_execution_result(execution,&result,r->background_output,
        r->config.session.max_tool_job_output_bytes+1U,&required);
    if(rc==COG_OK && !r->job_save_failed && !r->app->recovery){
        cog_tool_output_t out={.status=result.status,.output=r->background_output,.output_len=result.output_len,
            .truncated=result.truncated,.side_effects_possible=result.side_effects_possible,
            .exit_status=result.exit_status,.exit_status_known=result.exit_status_known};
        rc=out.status==COG_ERROR_INDETERMINATE?
            cog_session_tool_job_mark_uncertain(r->app->session,r->background_id):
            cog_session_tool_job_finish(r->app->session,r->background_id,&out,NULL);
        if(rc==COG_OK)rc=cog_cli_save(r->app);
        if(rc==COG_OK && out.status==COG_ERROR_INDETERMINATE)rc=out.status;
    }
    cog_result_t released=cog_toolkit_execution_free(execution);
    if(released==COG_OK){r->background_execution=NULL;r->background_id=0U;}
    if(rc!=COG_OK){r->job_save_failed=true;r->app->recovery=true;}
    return rc==COG_OK?released:rc;
}

/** @file app_tools.c @brief Durable sequential admission of complete tool calls. */
#include "app_internal.h"
#include "composition_internal.h"
#include "runtime_internal.h"
#include "child_profile.h"
#include <stdio.h>
#include <string.h>

static cog_prompt_tool_policy_t runtime_policy(const cog_cli_app_t *app,
    const char *name)
{
    cog_runtime_t *r=app->runtime_owner;
    if(r!=NULL && r->instructions!=NULL){
        cog_instruction_manifest_t manifest;
        if(cog_instruction_snapshot_view(r->instructions,&manifest)!=COG_OK)
            return COG_PROMPT_TOOL_DENY;
        for(size_t i=0U;i<manifest.tool_count;i++)
            if(strcmp(manifest.tools[i].name,name)==0)return manifest.tools[i].policy;
        return COG_PROMPT_TOOL_DENY;
    }
    for(size_t i=0U;i<app->runtime_config.tools.binding_count;i++){
        const cog_prompt_tool_binding_t *b=&app->runtime_config.tools.bindings[i];
        if(strcmp(b->descriptor->name,name)==0)return b->policy;
    }
    return COG_PROMPT_TOOL_DENY;
}

cog_result_t cli_tools_binding(const cog_cli_app_t *app, const char *name,
    const cog_tool_table_t **table, void **user)
{
    if (app == NULL || name == NULL || table == NULL) return COG_ERROR_NULL_PTR;
    if(app->runtime_resolved){
        const cog_runtime_tool_config_t *configured=&app->runtime_config.tools;
        for(size_t i=0U;i<configured->binding_count;i++){
            const cog_prompt_tool_binding_t *b=&configured->bindings[i];
            if(strcmp(b->descriptor->name,name)!=0)continue;
            if(runtime_policy(app,name)==COG_PROMPT_TOOL_DENY)return COG_ERROR_NOT_FOUND;
            for(size_t j=0U;j<cog_runtime_delegation_tools.count;j++) {
                if(b->descriptor!=&cog_runtime_delegation_tools.tools[j])continue;
                bool child=app->headless!=NULL && (app->headless->kind==COG_CLI_EXECUTION_LOCAL_CHILD || app->headless->kind==COG_CLI_EXECUTION_REMOTE_CHILD);
                if((j==0U && child) || (j!=0U && !child))return COG_ERROR_NOT_FOUND;
                if(app->headless==NULL || app->headless->delegation==NULL)return COG_ERROR_INVALID_STATE;
                if(user!=NULL)*user=app->headless->delegation;
                *table=&cog_cli_delegation_tools;return COG_OK;
            }
            const cog_tool_table_t *job_table=cli_runtime_job_table(b->descriptor);
            if(job_table!=NULL){
                if(app->runtime_owner==NULL || app->runtime_config.background_job_timeout_ms==0U ||
                   (app->headless!=NULL && app->headless->kind!=COG_CLI_EXECUTION_ROOT))
                    return COG_ERROR_NOT_FOUND;
                if(user!=NULL)*user=b->descriptor==&cog_runtime_background_status_tool ?
                    (void *)app->runtime_owner : configured->user;
                *table=job_table;
                return COG_OK;
            }
            const cog_tool_table_t *builtin=cli_runtime_instruction_table(b->descriptor);
            if(user!=NULL) {
                *user=builtin!=NULL ? (void *)app->runtime_owner : configured->user;
                if(builtin==NULL && configured->table==&cog_workspace_tools && configured->user==NULL)
                    *user=app->workspace;
            }
            *table=builtin!=NULL ? builtin : configured->table;
            const cog_tool_t *resolved=NULL;
            if(*table==NULL || cog_tool_table_lookup(*table,name,&resolved)!=COG_OK ||
               resolved!=b->descriptor)return COG_ERROR_INVALID_STATE;
            return COG_OK;
        }
        return COG_ERROR_NOT_FOUND;
    }
    static const cog_tool_table_t *const tables[] = {&cog_workspace_tools, &cog_cli_delegation_tools};
    const cli_execution_state_t *h = app->headless;
    for (size_t t = 0U; t < 2U; ++t) for (size_t i = 0U; i < tables[t]->count; ++i) {
        if (strcmp(tables[t]->tools[i].name, name) != 0) continue;
        bool enabled = t == 0U;
        if (h != NULL && t == 0U) {
            uint32_t bit = strcmp(name, "read_file") == 0 ? COG_CLI_CHILD_READ :
                strcmp(name, "search") == 0 ? COG_CLI_CHILD_SEARCH :
                strcmp(name, "edit_file") == 0 ? COG_CLI_CHILD_EDIT : COG_CLI_CHILD_COMMAND;
            enabled = (h->tool_mask & bit) != 0U;
        } else if (h != NULL) {
            bool is_child = h->kind == COG_CLI_EXECUTION_LOCAL_CHILD || h->kind == COG_CLI_EXECUTION_REMOTE_CHILD;
            enabled = is_child ? strcmp(name, "delegate") != 0 :
                strcmp(name, "delegate") == 0 && app->opts.backend == COG_CLI_BACKEND_REAL &&
                app->opts.store_mode == COG_CLI_STORE_DURABLE && app->opts.workspace_root != NULL;
        }
        if (!enabled) return COG_ERROR_NOT_FOUND;
        if (user != NULL) {
            *user = t == 0U ? (void *)app->workspace : h == NULL ? NULL : (void *)h->delegation;
            if (*user == NULL) return COG_ERROR_INVALID_STATE;
        }
        *table = tables[t]; return COG_OK;
    }
    return COG_ERROR_NOT_FOUND;
}
static bool json_append(cog_cli_app_t *app,size_t *used,const char *text,size_t bytes) {
    if(*used>=app->tools_json_capacity || bytes>=app->tools_json_capacity-*used)return false;
    memcpy(app->tools_json+*used,text,bytes);*used+=bytes;app->tools_json[*used]='\0';return true;
}
static bool json_quote(cog_cli_app_t *app,size_t *used,const char *text) {
    static const char hex[]="0123456789abcdef";
    if(text==NULL || !json_append(app,used,"\"",1U))return false;
    for(const unsigned char *p=(const unsigned char *)text;*p!=0U;p++) {
        char escaped[6];size_t bytes=1U;escaped[0]=(char)*p;
        if(*p=='"' || *p=='\\'){escaped[0]='\\';escaped[1]=(char)*p;bytes=2U;}
        else if(*p<32U){memcpy(escaped,"\\u00",4U);escaped[4]=hex[*p>>4U];escaped[5]=hex[*p&15U];bytes=6U;}
        if(!json_append(app,used,escaped,bytes))return false;
    }
    return json_append(app,used,"\"",1U);
}
static cog_result_t runtime_tools_json(cog_cli_app_t *app) {
    size_t used=0U,count=0U;
    if(!json_append(app,&used,"[",1U))return COG_ERROR_LIMIT;
    for(size_t i=0U;i<app->runtime_config.tools.binding_count;i++) {
        const cog_prompt_tool_binding_t *b=&app->runtime_config.tools.bindings[i];
        if(b->policy==COG_PROMPT_TOOL_DENY)continue;
        bool child=app->headless!=NULL && (app->headless->kind==COG_CLI_EXECUTION_LOCAL_CHILD || app->headless->kind==COG_CLI_EXECUTION_REMOTE_CHILD);
        bool role_ok=true;
        for(size_t j=0U;j<cog_runtime_delegation_tools.count;j++)
            if(b->descriptor==&cog_runtime_delegation_tools.tools[j] && ((j==0U && child)||(j!=0U && !child)))role_ok=false;
        if(!role_ok)continue;
        const cog_tool_t *d=b->descriptor;
        if(d->input_schema==NULL)return COG_ERROR_UNSUPPORTED;
        if((count++!=0U && !json_append(app,&used,",",1U)) ||
            !json_append(app,&used,"{\"type\":\"function\",\"function\":{\"name\":",38U) ||
            !json_quote(app,&used,d->name))return COG_ERROR_LIMIT;
        if(d->description!=NULL && (!json_append(app,&used,",\"description\":",15U) ||
            !json_quote(app,&used,d->description)))return COG_ERROR_LIMIT;
        if(!json_append(app,&used,",\"parameters\":",14U) ||
            !json_append(app,&used,d->input_schema,strlen(d->input_schema)) ||
            !json_append(app,&used,"}}",2U))return COG_ERROR_LIMIT;
    }
    if(!json_append(app,&used,"]",1U))return COG_ERROR_LIMIT;
    app->tools_json_len=used;return COG_OK;
}
cog_result_t cog_cli_tools_json(cog_cli_app_t *app)
{
    static const cog_tool_table_t *const tables[] = {&cog_workspace_tools, &cog_cli_delegation_tools};
    size_t used = 0U, count = 0U;
    app->tools_json[used++] = '[';
    if(app->runtime_resolved)return runtime_tools_json(app);
    for (size_t t = 0U; t < 2U; ++t) for (size_t i = 0U; i < tables[t]->count; ++i) {
        const cog_tool_t *tool = &tables[t]->tools[i]; const cog_tool_table_t *selected = NULL;
        cog_result_t rc = cli_tools_binding(app, tool->name, &selected, NULL);
        if (rc == COG_ERROR_NOT_FOUND) continue;
        if (rc != COG_OK) return rc;
        int n = snprintf(app->tools_json + used, CLI_SCHEMA_BYTES - used,
            "%s{\"type\":\"function\",\"function\":{\"name\":\"%s\",\"parameters\":%s}}",
            count++ == 0U ? "" : ",", tool->name, tool->input_schema);
        if (n < 0 || (size_t)n >= CLI_SCHEMA_BYTES - used) return COG_ERROR_LIMIT;
        used += (size_t)n;
    }
    if (used + 2U > CLI_SCHEMA_BYTES) return COG_ERROR_LIMIT;
    app->tools_json[used++] = ']'; app->tools_json[used] = '\0'; app->tools_json_len = used;
    return COG_OK;
}
static cog_result_t notify_saved(cog_cli_app_t *app)
{
    if (app->headless == NULL || app->headless->delegation == NULL || app->execution == NULL) return COG_OK;
    const char *name = app->call_views[app->tool_index].name;
    const cog_tool_table_t *table = NULL;
    cog_result_t rc = cli_tools_binding(app, name, &table, NULL);
    if (rc != COG_OK || table != &cog_cli_delegation_tools) return COG_OK;
    cog_store_owner_t owner; cog_store_checkpoint_t token;
    rc = cog_store_owner_read(app->store, cog_session_id(app->session), &owner, &token);
    if (rc != COG_OK) return rc;
    if (owner.epoch != app->headless->expected_owner.epoch ||
        owner.instance_id != app->headless->expected_owner.instance_id) return COG_ERROR_INVALID_STATE;
    return cli_delegation_saved(app->headless->delegation,
        cog_node_data(app->assistant)->node_id, (uint16_t)app->tool_index, token);
}
static cog_result_t record_result(cog_cli_app_t *app, cog_result_t status,
    const char *output, size_t length, bool truncated, bool effects,
    bool exit_known, int exit_status, cog_tool_execution_state_t expected)
{
    if (app->result_node_added) {
        cog_result_t notified = notify_saved(app);
        if (notified != COG_OK) return notified;
        app->tool_index++; app->result_node_added = false;
        app->policy_prompt = false; app->policy_decision = COG_TOOLKIT_POLICY_UNDECIDED;
        return COG_OK;
    }
    cog_message_tool_call_t *call = &app->call_views[app->tool_index];
    cog_cli_observation_tool_end(app, status, effects, truncated);
    /* Persist execution metadata beside text. A failed/truncated command never
       masquerades as a successful complete result or permission to replay. */
    int n = snprintf(app->result_storage, 384U,
        "status=%s; truncated=%s; side_effects_possible=%s; exit_status=%s%d\n",
        cog_cli_result_name(status), truncated ? "true" : "false", effects ? "true" : "false",
        exit_known ? "" : "unknown:", exit_status);
    if (status==COG_OK && app->runtime_config.background_job_timeout_ms!=0U &&
        strcmp(call->name,"background_status")==0)n=0;
    if (n < 0 || (size_t)n >= 384U || length > app->result_capacity - (size_t)n - 1U)
        return COG_ERROR_LIMIT;
    memmove(app->result_storage + (size_t)n, output, length);
    app->result_storage[(size_t)n + length] = '\0';
    app->pending_result_len = (size_t)n + length;
    cog_message_t message = {.role = COG_MESSAGE_ROLE_TOOL, .tool_call_id = call->id,
        .name = call->name, .content = app->result_storage, .content_len = (size_t)n + length};
    cog_tool_execution_state_t final = expected == COG_TOOL_EXECUTION_PROPOSED ? COG_TOOL_EXECUTION_DENIED :
        status == COG_OK ? COG_TOOL_EXECUTION_SUCCEEDED :
        status == COG_ERROR_ABORTED ? COG_TOOL_EXECUTION_CANCELLED :
        status == COG_ERROR_INDETERMINATE ? COG_TOOL_EXECUTION_INDETERMINATE : COG_TOOL_EXECUTION_FAILED;
    cog_result_t rc;
    if (expected == COG_TOOL_EXECUTION_PROPOSED) {
        rc = cog_session_tool_transition(app->session, app->assistant, call->id, expected, final);
        if (rc != COG_OK) return rc;
    }
    rc = cog_session_append(app->session, &message, NULL);
    if (rc != COG_OK) return rc;
    app->result_node_added = true;
    if (expected != COG_TOOL_EXECUTION_PROPOSED) {
        rc = cog_session_tool_transition(app->session, app->assistant, call->id, expected, final);
        if (rc != COG_OK) return rc;
    }
    rc=app->runtime_owner!=NULL && app->runtime_owner->control_change!=NULL ? cli_runtime_control_saved(app) : cog_cli_save(app);
    if (rc != COG_OK) { app->recovery = true; return rc; }
    if (app->display_sequence == UINT64_MAX) return COG_ERROR_LIMIT;
    app->display_sequence++;
    if (final == COG_TOOL_EXECUTION_INDETERMINATE) { app->recovery = true; return COG_ERROR_INDETERMINATE; }
    rc = notify_saved(app);
    if (rc != COG_OK) return rc;
    app->tool_index++;
    app->result_node_added = false;
    app->policy_prompt = false;
    app->policy_decision = COG_TOOLKIT_POLICY_UNDECIDED;
    return COG_OK;
}
static cog_result_t finish_launch(cog_cli_app_t *app,cog_result_t disposition,cog_state_t *next)
{
    cog_runtime_t *r=app->runtime_owner;
    const cog_message_tool_call_t *call=&app->call_views[app->tool_index];
    cog_result_t rc;
    if(disposition==COG_OK){
        r->background_execution=app->execution;
        app->execution=NULL;
        app->effects=true;
        rc=cog_session_tool_job_accept(app->session,r->background_id);
    }else{
        const char *why=cog_cli_result_name(disposition);
        cog_message_t failure={.role=COG_MESSAGE_ROLE_TOOL,.name=call->name,.tool_call_id=call->id,
            .content=why,.content_len=strlen(why)};
        cog_tool_output_t outcome={.status=disposition,.output=""};
        rc=cog_session_tool_job_finish(app->session,r->background_id,&outcome,&failure);
        cog_result_t released=cog_toolkit_execution_free(app->execution);
        if(released==COG_OK)app->execution=NULL;
        if(rc==COG_OK)rc=released;
        r->background_id=0U;
    }
    if(rc==COG_OK)rc=cog_cli_save(app);
    if(rc!=COG_OK){
        app->recovery=true;
        r->job_save_failed=true;
        return rc;
    }
    cog_cli_observation_tool_end(app,disposition,disposition==COG_OK,false);
    app->display_sequence++;
    app->tool_index++;
    app->policy_prompt=false;
    app->policy_decision=COG_TOOLKIT_POLICY_UNDECIDED;
    return cli_execution_barrier(app,CLI_AFTER_TOOL_SAVE,CLI_POLICY,next);
}

cog_result_t cog_cli_tools_step(cog_cli_app_t *app, cog_state_t *next)
{
    *next = CLI_POLICY;
    if(app->runtime_owner!=NULL && app->runtime_owner->scope_replaced){app->runtime_owner->scope_replaced=false;return cli_execution_barrier(app,CLI_AFTER_TOOL_SAVE,CLI_POLICY,next);}
    if (app->terminal_result_pending) return app->tool_result_error;
    if (app->execution != NULL && app->tool_done) {
        cog_toolkit_result_t result = {0};
        size_t required = 0U;
        /* Capture beyond the result prefix; record_result uses memmove. */
        char *capture = app->result_storage + 384U;
        cog_result_t rc = cog_toolkit_execution_result(app->execution, &result, capture,
            app->result_capacity - 384U, &required);
        if (rc != COG_OK) return rc;
        rc = record_result(app, result.status, capture, result.output_len, result.truncated,
            result.side_effects_possible, result.exit_status_known, result.exit_status,
            COG_TOOL_EXECUTION_STARTED);
        if (rc == COG_ERROR_AGAIN) return rc;
        if (rc != COG_OK) {
            app->terminal_result_pending = true;
            app->tool_result_error = rc;
            app->recovery = true;
            return rc;
        }
        cog_result_t released = cog_toolkit_execution_free(app->execution);
        if (released == COG_OK) app->execution = NULL;
        app->tool_done = false;
        if (rc != COG_OK) return rc;
        if (released != COG_OK) return released;
        if (app->headless != NULL) return cli_execution_barrier(app, CLI_AFTER_TOOL_SAVE, CLI_POLICY, next);
    }
    while (app->tool_index < app->call_count && app->calls[app->tool_index].resumed_closed)
        app->tool_index++;
    if (app->tool_index == app->call_count) {
        if (app->cancelling || app->shutdown) {
            app->active = false; *next = CLI_IDLE; return COG_OK;
        }
        cog_result_t rc = cog_cli_begin_request(app, true);
        *next = CLI_MODEL;
        return rc;
    }
    cog_message_tool_call_t *call = &app->call_views[app->tool_index];
    const cog_tool_table_t *table = NULL; void *userdata = NULL;
    cog_result_t binding = cli_tools_binding(app, call->name, &table, &userdata);
    cog_cli_tool_policy_t effective_policy=app->opts.tool_policy;
    if(app->runtime_resolved){
        cog_prompt_tool_policy_t p=runtime_policy(app,call->name);
        effective_policy=p==COG_PROMPT_TOOL_ALLOW ? COG_CLI_TOOL_POLICY_ALLOW :
            p==COG_PROMPT_TOOL_DENY ? COG_CLI_TOOL_POLICY_DENY : COG_CLI_TOOL_POLICY_ASK;
    }
    bool denied = app->cancelling || app->shutdown || binding != COG_OK ||
        app->store == NULL || effective_policy == COG_CLI_TOOL_POLICY_DENY;
    if (app->policy_decision == COG_TOOLKIT_POLICY_DENY) denied = true;
    cog_runtime_t *runtime=app->runtime_owner;
    const cog_tool_t *descriptor=NULL;
    bool launch=binding==COG_OK && table==cli_runtime_job_table(&cog_workspace_start_background_tool);
    if(!denied && runtime!=NULL && runtime->validator!=NULL){
        cog_result_t valid=cog_tool_table_lookup(table,call->name,&descriptor);
        if(valid==COG_OK)valid=cog_toolkit_validate(runtime->validator,descriptor,call->arguments,call->arguments_len);
        if(valid!=COG_OK){
            const char *why=cog_cli_result_name(valid);
            cog_result_t saved=record_result(app,valid,why,strlen(why),false,false,false,0,COG_TOOL_EXECUTION_PROPOSED);
            return saved==COG_OK?cli_execution_barrier(app,CLI_AFTER_TOOL_SAVE,CLI_POLICY,next):saved;
        }
        if(runtime->background_id!=0U && !(launch && app->execution!=NULL) &&
           ((descriptor->flags & COG_TOOL_SIDE_EFFECTING)!=0U ||
            table==&cog_cli_delegation_tools || cli_runtime_instruction_table(descriptor)!=NULL))denied=true;
        if(launch && runtime->future_provider!=NULL)denied=true;
    }
    if (!denied && effective_policy == COG_CLI_TOOL_POLICY_ASK &&
        app->policy_decision != COG_TOOLKIT_POLICY_ALLOW) {
        if (app->headless != NULL && app->headless->kind != COG_CLI_EXECUTION_LOCAL_CHILD &&
            app->headless->kind != COG_CLI_EXECUTION_REMOTE_CHILD) {
            cli_execution_state_t *h = app->headless;
            if (!app->policy_prompt) {
                if (h->prompt_serial == UINT64_MAX || app->started_at > UINT64_MAX - app->opts.max_turn_ms)
                    return COG_ERROR_LIMIT;
                size_t sid = strlen(cog_session_id(app->session)), tool = strlen(call->name);
                if (sid >= sizeof(h->policy_session) || tool >= sizeof(h->policy_tool)) return COG_ERROR_LIMIT;
                memcpy(h->policy_session, cog_session_id(app->session), sid + 1U);
                memcpy(h->policy_tool, call->name, tool + 1U);
                ++h->prompt_serial; h->policy_pending = true; app->policy_prompt = true;
                h->policy = (cog_cli_policy_view_t){h->policy_session, cog_node_data(app->assistant)->node_id,
                    0U, app->configuration_generation, app->started_at + app->opts.max_turn_ms,
                    (uint16_t)app->tool_index, h->policy_tool};
                return cli_execution_barrier(app, CLI_AFTER_POLICY, CLI_POLICY, next);
            }
            return COG_ERROR_AGAIN;
        } else if (!app->opts.interactive) denied = true;
        else if (!app->policy_prompt) {
            /* Display the complete request to make an interactive approval
               meaningful. Never accept an arbitrary queued prompt as approval. */
            if (app->frontend == NULL || app->frontend->ask == NULL) return COG_ERROR_DENIED;
            cog_result_t asked=app->frontend->ask(app,call);
            if(asked!=COG_OK) return asked;
            app->policy_prompt = true;
            return COG_ERROR_AGAIN;
        } else return COG_ERROR_AGAIN;
    }
    if (denied && app->headless != NULL && app->headless->policy_pending) {
        cli_execution_state_t *h = app->headless;
        h->policy_pending = false; h->policy_retirement_pending = true;
        h->retired_prompt_serial = h->prompt_serial;
        return cli_execution_barrier(app, CLI_AFTER_CONTROL, CLI_POLICY, next);
    }
    if (denied) {
        static const char text[] = "Tool was not executed: policy denied or durable workspace unavailable.";
        cog_tool_execution_state_t state = COG_TOOL_EXECUTION_PROPOSED;
        cog_result_t rc = cog_session_tool_state(app->session, app->assistant, call->id, &state);
        if (rc != COG_OK) return rc;
        if(launch && state==COG_TOOL_EXECUTION_STARTED && runtime->background_id!=0U)
            return finish_launch(app,COG_ERROR_ABORTED,next);
        if (app->execution != NULL && !app->tool_execution_started) {
            rc = cog_toolkit_execution_free(app->execution);
            if (rc != COG_OK) return rc;
            app->execution = NULL;
        }
        rc = record_result(app, COG_ERROR_DENIED, text, sizeof(text) - 1U, false, false, false, 0, state);
        return rc == COG_OK ? cli_execution_barrier(app, CLI_AFTER_TOOL_SAVE, CLI_POLICY, next) : rc;
    }
    cog_result_t scoped=cli_runtime_scope(app,call);
    if(scoped==COG_ERROR_AGAIN)return scoped;
    if(scoped!=COG_OK) {
        const char *why=cog_cli_result_name(scoped);
        cog_result_t saved=record_result(app,scoped,why,strlen(why),false,false,false,0,COG_TOOL_EXECUTION_PROPOSED);
        return saved==COG_OK ? cli_execution_barrier(app,CLI_AFTER_TOOL_SAVE,CLI_POLICY,next) : saved;
    }
    if (app->execution == NULL) {
        cog_result_t observed = cog_cli_observation_check(app, COG_OK);
        if (observed != COG_OK) return observed;
        cog_tool_async_config_t config = {.max_output_bytes = app->opts.max_tool_output_bytes,
            .max_argument_bytes = CLI_ARGUMENT_BYTES, .max_schema_bytes = CLI_SCHEMA_BYTES,
            .max_json_depth = 64U, .codec_workspace_bytes = CLI_CODEC_BYTES,
            .timeout_ms = app->opts.max_tool_ms, .terminate_grace_ms = 100U,
            .user = userdata, .owner_guard = app->guard};
        if(launch){
            config.max_output_bytes=runtime->config.session.max_tool_job_output_bytes;
            config.timeout_ms=runtime->config.background_job_timeout_ms;
        }
        cog_result_t rc = cog_toolkit_execution_new(&app->execution, app->core, table,
            call->id, call->name, call->arguments, call->arguments_len, &config);
        if (rc != COG_OK) {
            cog_cli_observation_tool_end(app, rc, false, false);
            if (rc == COG_ERROR_NOMEM || rc == COG_ERROR_IO || rc == COG_ERROR_INDETERMINATE)
                return rc;
            const char *why = cog_cli_result_name(rc);
            return record_result(app, rc, why, strlen(why), false, false, false, 0,
                COG_TOOL_EXECUTION_PROPOSED);
        }
        /* This flag belongs to the newly constructed handle. A previous
           handle's observation identity may persist across publication. */
        app->tool_execution_started = false;
    }
    cog_tool_execution_state_t state;
    cog_result_t rc = cog_session_tool_state(app->session, app->assistant, call->id, &state);
    if (rc != COG_OK) return rc;
    if (state == COG_TOOL_EXECUTION_PROPOSED) {
        if (app->headless != NULL && table == &cog_cli_delegation_tools) {
            bool ready = false;
            rc = cog_cli_delegation_prepare(app->headless->delegation, &ready);
            if (rc == COG_ERROR_AGAIN || (rc == COG_OK && !ready)) { *next = CLI_PREPARE; return COG_ERROR_AGAIN; }
            if (rc != COG_OK) {
                cog_result_t released = cog_toolkit_execution_free(app->execution);
                if (released != COG_OK) return released;
                app->execution = NULL;
                const char *why = cog_cli_result_name(rc);
                rc = record_result(app, rc, why, strlen(why), false, false, false, 0, COG_TOOL_EXECUTION_PROPOSED);
                return rc == COG_OK ? cli_execution_barrier(app, CLI_AFTER_TOOL_SAVE, CLI_POLICY, next) : rc;
            }
        }
        rc = launch ? cog_session_tool_job_intent(app->session,app->assistant,call->id,&runtime->background_id) :
            cog_session_tool_transition(app->session, app->assistant, call->id,
                COG_TOOL_EXECUTION_PROPOSED, COG_TOOL_EXECUTION_STARTED);
        if(launch && rc==COG_ERROR_LIMIT){
            cog_result_t released=cog_toolkit_execution_free(app->execution);
            if(released!=COG_OK)return released;
            app->execution=NULL;
            const char *why=cog_cli_result_name(rc);
            rc=record_result(app,rc,why,strlen(why),false,false,false,0,COG_TOOL_EXECUTION_PROPOSED);
            return rc==COG_OK?cli_execution_barrier(app,CLI_AFTER_TOOL_SAVE,CLI_POLICY,next):rc;
        }
        if (rc == COG_OK) rc = cog_cli_save(app);
        if (rc != COG_OK) { app->recovery = true; return rc; }
        if (app->headless != NULL) return cli_execution_barrier(app, CLI_AFTER_CONTROL, CLI_PREPARE, next);
    } else if (state != COG_TOOL_EXECUTION_STARTED || app->tool_execution_started) return COG_ERROR_INVALID_STATE;
    rc = cog_cli_observation_tool_begin(app);
    if (rc == COG_OK) rc = cog_toolkit_execution_start(app->execution, COG_TOOLKIT_POLICY_ALLOW, app->now);
    if(launch)return finish_launch(app,rc,next);
    if (rc != COG_OK) {
        /* The dispatcher guarantees start failure has no effects. Preserve the
           durable intent and an explicit failed result rather than retrying. */
        cog_result_t recorded = record_result(app, rc, "Tool start failed; no automatic retry.",
            38U, false, false, false, 0, COG_TOOL_EXECUTION_STARTED);
        (void)cog_toolkit_execution_free(app->execution); app->execution = NULL;
        return cog_cli_observation_check(app, recorded);
    }
    app->tool_execution_started = true;
    app->effects = true;
    *next = CLI_TOOL;
    return COG_OK;
}
cog_result_t cog_cli_tools_cancel(cog_cli_app_t *app)
{
    if (app->execution == NULL) return COG_OK;
    return cog_toolkit_execution_cancel(app->execution, app->now);
}

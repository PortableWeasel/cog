/** @file runtime.c @brief Frontend-neutral facade over the existing executor.
 */
#include "execution_internal.h"
#include "composition_internal.h"
#include "runtime_internal.h"
#include <stdlib.h>
#include <string.h>

static cog_result_t clone_snapshot(const cog_instruction_snapshot_t *source,
                                   const cog_runtime_config_t *config,
                                   cog_instruction_snapshot_t **out) {
  cog_instruction_manifest_t manifest;
  cog_result_t rc = cog_instruction_snapshot_view(source, &manifest);
  return rc == COG_OK ? cog_instruction_snapshot_restore(
                            out, &config->instruction_limits, &manifest,
                            COG_INSTRUCTION_EXECUTE, config->tools.bindings,
                            config->tools.binding_count, NULL)
                      : rc;
}
static cog_result_t
install_context(cog_cli_app_t *app,
                const cog_instruction_snapshot_t *snapshot) {
  cog_instruction_manifest_t manifest;
  size_t bytes = 0U;
  cog_result_t rc = cog_instruction_snapshot_view(snapshot, &manifest);
  if (rc != COG_OK)
    return rc;
  rc = cog_instruction_manifest_encode(&manifest, NULL, 0U, &bytes);
  if (rc != COG_ERROR_OVERFLOW && rc != COG_OK)
    return rc;
  if (bytes > app->runtime_config.limits.max_instruction_bytes)
    return COG_ERROR_LIMIT;
  unsigned char *encoded = malloc(bytes == 0U ? 1U : bytes);
  if (encoded == NULL)
    return COG_ERROR_NOMEM;
  rc = cog_instruction_manifest_encode(&manifest, encoded, bytes, &bytes);
  cog_session_snapshot_t state;
  if (rc == COG_OK)
    rc = cog_session_snapshot_view(app->session, &state);
  if (rc == COG_OK && (state.context_high_water == UINT64_MAX ||
                       state.context_binding_high_water == UINT64_MAX))
    rc = COG_ERROR_LIMIT;
  if (rc == COG_OK) {
    cog_session_context_t context = {
        state.context_high_water + 1U, COG_INSTRUCTION_CONTEXT_NAMESPACE,
        COG_INSTRUCTION_CONTEXT_FORMAT, encoded, bytes};
    cog_session_context_binding_t binding = {
        state.context_binding_high_water + 1U,
        state.selected_context_binding_id, state.active_node_id,
        context.context_id};
    rc = cog_session_context_add(app->session, &context);
    if (rc == COG_OK)
      rc = cog_session_context_publish(app->session, &binding);
  }
  if (encoded != NULL) {
    volatile unsigned char *p = encoded;
    for (size_t i = 0U; i < bytes; i++)
      p[i] = 0U;
    free(encoded);
  }
  return rc;
}
cog_result_t cli_runtime_initial_preflight(cog_cli_app_t *app) {
  if(app==NULL)return COG_ERROR_NULL_PTR;
  cog_runtime_t *r=app->runtime_owner;
  if(r==NULL)return COG_OK;
  size_t count=0U,missing=0U;
  cog_result_t rc=COG_OK;
  if(app->session!=NULL) {
    if(r->config.instruction_mode==COG_RUNTIME_INSTRUCTIONS_ADOPT_LEGACY)
      rc=cog_session_copy_history(app->session,NULL,app->history,CLI_NODES,&count);
    else {
      for(cog_session_node_t *node=cog_session_active(app->session);node!=NULL;node=cog_node_parent(node))
        if(++count>CLI_NODES){rc=COG_ERROR_LIMIT;break;}
      if(rc==COG_OK) {
        size_t at=count;
        for(cog_session_node_t *node=cog_session_active(app->session);node!=NULL;node=cog_node_parent(node))
          app->history[--at]=*cog_node_data(node)->message;
        for(size_t i=0U;i<count;i++) {
          const cog_message_t *m=&app->history[i];
          if(m->role==COG_MESSAGE_ROLE_ASSISTANT)missing=m->tool_call_count;
          else if(m->role==COG_MESSAGE_ROLE_TOOL && missing!=0U)missing--;
        }
      }
    }
  }
  cog_provider_prompt_input_t input={0};
  if(rc==COG_OK && r->instructions!=NULL)rc=cog_instruction_prompt_view(r->instructions,&input.prompt);
  else if(rc==COG_OK)input.prompt=(cog_prompt_bundle_t){.catalog_generation=1U,.snapshot_id=1U,
      .model_id=r->config.provider.model_id,
      .template_id=r->config.provider.prompt_template==NULL ? NULL : r->config.provider.prompt_template->id,
      .template_revision=r->config.provider.prompt_template==NULL ? 0U : r->config.provider.prompt_template->revision,
      .max_context_bytes=r->config.limits.max_context_bytes,.max_output_tokens=r->config.limits.max_output_tokens,
      .max_tool_rounds=r->config.limits.max_tool_rounds,.tools=r->bindings,.tool_count=r->config.tools.binding_count};
  if(rc==COG_OK && (input.prompt.max_context_bytes>r->config.limits.max_context_bytes ||
      input.prompt.max_output_tokens>r->config.limits.max_output_tokens ||
      input.prompt.max_tool_rounds>r->config.limits.max_tool_rounds))rc=COG_ERROR_LIMIT;
  input.request=(cog_provider_request_t){.model_id=r->config.provider.model_id,
      .messages=app->history,.message_count=count,.max_tokens=input.prompt.max_output_tokens,
      .configuration_generation=r->config.provider.endpoint->generation,.stream=true};
  cog_provider_request_t compiled=input.request;
  if(rc==COG_OK && r->compiler!=NULL)rc=cog_provider_prompt_compile(r->compiler,r->config.provider.endpoint->vtable,&input,&compiled);
  else if(rc==COG_OK && app->tools_json_len>2U){compiled.tools_json=app->tools_json;compiled.tools_json_len=app->tools_json_len;}
  if(rc==COG_OK)rc=cog_provider_preflight(app->provider,&compiled,missing!=0U ? COG_PROVIDER_PREFLIGHT_PENDING_TOOLS : COG_PROVIDER_PREFLIGHT_COMPLETE);
  return rc;
}
cog_result_t cli_runtime_initial_context(cog_cli_app_t *app) {
  if (!app->runtime_resolved ||
      app->runtime_config.instruction_mode == COG_RUNTIME_INSTRUCTIONS_DISABLED)
    return COG_OK;
  if (app->runtime_config.mode != COG_RUNTIME_NEW)
    return COG_OK;
  return install_context(app, app->runtime_config.instructions);
}
static cog_result_t select_snapshot(cog_runtime_t *r, cog_instruction_snapshot_t *snapshot) {
  cog_instruction_manifest_t m;
  cog_result_t rc = cog_instruction_snapshot_view(snapshot, &m);
  if (rc != COG_OK) return rc;
  if (m.max_context_bytes > r->config.limits.max_context_bytes ||
      m.max_output_tokens > r->config.limits.max_output_tokens ||
      m.max_tool_rounds > r->config.limits.max_tool_rounds)
    return COG_ERROR_LIMIT;
  r->instructions = snapshot;
  r->app->opts.max_context_bytes = m.max_context_bytes;
  r->app->opts.max_output_tokens = m.max_output_tokens;
  r->app->opts.max_tool_rounds = m.max_tool_rounds;
  return COG_OK;
}
static cog_result_t current_snapshot(cog_runtime_t *r) {
  cog_session_snapshot_t state;
  cog_result_t rc = cog_session_snapshot_view(r->app->session, &state);
  if (rc != COG_OK)
    return rc;
  uint64_t selected_id = 0U;
  for (size_t i = 0U; i < state.context_binding_count; i++)
    if (state.context_bindings[i].binding_id ==
        state.selected_context_binding_id)
      selected_id = state.context_bindings[i].context_id;
  if (!r->snapshots_loaded) {
    cog_instruction_snapshot_t *initial = r->instructions;
    r->instructions = NULL;
    for (size_t i = 0U; i < state.context_count; i++) {
      const cog_session_context_t *c = &state.contexts[i];
      if (strcmp(c->namespace_id, COG_INSTRUCTION_CONTEXT_NAMESPACE) != 0)
        continue;
      if (c->format != COG_INSTRUCTION_CONTEXT_FORMAT) {
        rc = COG_ERROR_UNSUPPORTED;
        break;
      }
      if (r->snapshot_count == r->snapshot_capacity) {
        rc = COG_ERROR_LIMIT;
        break;
      }
      cog_instruction_snapshot_t *decoded = NULL;
      rc = cog_instruction_snapshot_decode(
          &decoded, &r->config.instruction_limits, c->data, c->data_bytes,
          r->offline ? COG_INSTRUCTION_INSPECT : COG_INSTRUCTION_EXECUTE,
          r->offline ? NULL : r->config.tools.bindings,
          r->offline ? 0U : r->config.tools.binding_count);
      if (rc != COG_OK)
        break;
      cog_instruction_manifest_t m;
      rc = cog_instruction_snapshot_view(decoded, &m);
      if (rc != COG_OK) {
        cog_instruction_snapshot_free(decoded);
        break;
      }
      if (m.snapshot_id > r->snapshot_high_water)
        r->snapshot_high_water = m.snapshot_id;
      r->snapshots[r->snapshot_count++] =
          (runtime_snapshot_cache_t){c->context_id, decoded};
    }
    cog_instruction_snapshot_free(initial);
    r->snapshots_loaded = true;
    if (rc != COG_OK)
      return rc;
  }
  for (size_t i = 0U; i < r->snapshot_count; i++)
    if (r->snapshots[i].context_id == selected_id) {
      return select_snapshot(r, r->snapshots[i].snapshot);
    }
  return COG_ERROR_UNSUPPORTED;
}
cog_result_t cli_runtime_cache_publish(cog_runtime_t *r,
                                       cog_instruction_snapshot_t **snapshot) {
  if (r->snapshot_count == r->snapshot_capacity)
    return COG_ERROR_LIMIT;
  cog_session_snapshot_t state;
  cog_result_t rc = cog_session_snapshot_view(r->app->session, &state);
  if (rc != COG_OK)
    return rc;
  r->snapshots[r->snapshot_count++] =
      (runtime_snapshot_cache_t){state.context_high_water, *snapshot};
  rc = select_snapshot(r, *snapshot);
  *snapshot = NULL;
  return rc;
}
static cog_result_t validate_config(const cog_runtime_config_t *c) {
  if (c == NULL || c->session_id == NULL)
    return COG_ERROR_NULL_PTR;
  if (c->mode < COG_RUNTIME_NEW || c->mode > COG_RUNTIME_INSPECT ||
      c->instruction_mode < COG_RUNTIME_INSTRUCTIONS_DISABLED ||
      c->instruction_mode > COG_RUNTIME_INSTRUCTIONS_ADOPT_LEGACY)
    return COG_ERROR_INVALID_ARG;
  if (c->limits.max_queued_inputs == 0U ||
      c->limits.max_queued_inputs > CLI_INPUTS ||
      c->session.max_nodes > CLI_NODES ||
      c->session.max_tool_calls_per_message > CLI_CALLS ||
      c->limits.max_input_bytes == 0U ||
      c->limits.max_input_bytes > c->limits.max_context_bytes ||
      c->limits.max_events == 0U || c->limits.max_dispatch_per_step == 0U ||
      c->limits.max_view_bytes == 0U || c->limits.max_context_bytes == 0U ||
      c->limits.max_tool_output_bytes == 0U ||
      c->limits.max_instruction_bytes == 0U ||
      c->limits.max_output_tokens == 0U || c->limits.max_tool_rounds == 0U ||
      c->limits.max_turn_ms == 0U || c->limits.max_tool_ms == 0U ||
      c->limits.max_retry_elapsed_ms == 0U)
    return COG_ERROR_LIMIT;
  if(c->workers!=NULL) {
    const cog_runtime_workers_t *w=c->workers;
    if(c->store.backend!=NULL && c->store.backend!=&cog_store_file_backend)return COG_ERROR_UNSUPPORTED;
    if(w->workers==0U || w->workers>8U || w->children==0U || w->children>16U ||
        w->request_budget==0U || w->shutdown_ms==0U || w->frame_bytes==0U ||
        (w->tool_mask&~15U)!=0U || w->executable==NULL || w->socket_path==NULL ||
        w->executable[0]!='/' || w->net_config==NULL)
      return COG_ERROR_INVALID_ARG;
    if(w->tool_mask!=0U && w->workspace_root==NULL)return COG_ERROR_INVALID_ARG;
    if(c->limits.max_context_bytes>SIZE_MAX/16U || w->instruction_bytes>SIZE_MAX/16U)
      return COG_ERROR_LIMIT;
    if((c->session.max_nodes!=0U && c->session.max_nodes!=CLI_NODES) ||
       (c->session.max_tool_calls_per_message!=0U && c->session.max_tool_calls_per_message!=CLI_CALLS) ||
       c->session.max_payload_bytes<c->limits.max_context_bytes*16U ||
       (c->instruction_mode!=COG_RUNTIME_INSTRUCTIONS_DISABLED &&
        (c->session.max_contexts<16U || c->session.max_context_bindings<16U ||
         c->session.max_context_bytes<w->instruction_bytes*16U)))return COG_ERROR_UNSUPPORTED;
    if((c->instruction_mode==COG_RUNTIME_INSTRUCTIONS_DISABLED)!=(w->instruction_bytes==0U))
      return COG_ERROR_INVALID_ARG;
    if(c->provider.endpoint==NULL || c->provider.endpoint->vtable!=&cog_openrouter_provider_vtable ||
        c->tools.user!=NULL)
      return COG_ERROR_UNSUPPORTED;
  }
  if (c->instruction_mode == COG_RUNTIME_INSTRUCTIONS_DISABLED) {
    if (c->instructions != NULL || c->sources != NULL || ((c->scopes != NULL ||
        c->scope_count != 0U) && c->background_job_timeout_ms == 0U) || c->workspace_root_id != NULL)
      return COG_ERROR_INVALID_ARG;
  } else {
    if (!c->store.require_owner || c->store.backend == &cog_store_null_backend)
      return COG_ERROR_UNSUPPORTED;
    if (c->store.backend != NULL &&
        (c->store.backend->capabilities & COG_STORE_CAP_CONTEXT_V3) == 0U)
      return COG_ERROR_UNSUPPORTED;
    if (c->instruction_mode == COG_RUNTIME_INSTRUCTIONS_ADOPT_LEGACY &&
        c->mode != COG_RUNTIME_RESUME)
      return COG_ERROR_INVALID_ARG;
    bool initial = c->mode == COG_RUNTIME_NEW ||
                   c->instruction_mode == COG_RUNTIME_INSTRUCTIONS_ADOPT_LEGACY;
    if (initial != (c->instructions != NULL))
      return COG_ERROR_INVALID_ARG;
  }
  if ((c->tools.binding_count != 0U && c->tools.bindings == NULL))
    return COG_ERROR_NULL_PTR;
  if(c->tools.table!=NULL && c->tools.table->count!=0U && c->tools.table->tools==NULL)
    return COG_ERROR_INVALID_ARG;
  bool start_bound=false,status_bound=false;
  for (size_t i = 0U; i < c->tools.binding_count; i++) {
    if(c->tools.bindings[i].policy<COG_PROMPT_TOOL_ALLOW ||
        c->tools.bindings[i].policy>COG_PROMPT_TOOL_DENY)return COG_ERROR_INVALID_ARG;
    const cog_tool_t *d = c->tools.bindings[i].descriptor;
    bool found = false;
    if (d == NULL || d->name == NULL || d->async == NULL)
      return COG_ERROR_INVALID_ARG;
    if (cli_runtime_job_table(d) != NULL) {
      found=true;
      if(c->background_job_timeout_ms==0U || c->workers!=NULL)return COG_ERROR_UNSUPPORTED;
      if(d==&cog_workspace_start_background_tool)start_bound=true;
      else status_bound=true;
    }
    if (cli_runtime_instruction_table(d) != NULL)
      found = true;
    for (size_t j = 0U; c->tools.table != NULL && j < c->tools.table->count;
         j++)
      if (d == &c->tools.table->tools[j])
        found = true;
    for(size_t j=0U;j<cog_runtime_delegation_tools.count;j++)
      if(d==&cog_runtime_delegation_tools.tools[j])found=true;
    if(c->workers!=NULL) {
      bool serializable=cli_runtime_instruction_table(d)!=NULL;
      for(size_t j=0U;j<cog_runtime_delegation_tools.count;j++)
        if(d==&cog_runtime_delegation_tools.tools[j])serializable=true;
      for(size_t j=0U;j<cog_workspace_tools.count;j++)
        if(d==&cog_workspace_tools.tools[j]) {
          serializable=true;
          if(c->tools.bindings[i].policy!=COG_PROMPT_TOOL_DENY && c->workers->workspace_root==NULL)
            return COG_ERROR_INVALID_ARG;
        }
      if(!serializable)return COG_ERROR_UNSUPPORTED;
    }
    if (!found)
      return COG_ERROR_INVALID_ARG;
    for (size_t j = 0U; j < i; j++)
      if (strcmp(d->name, c->tools.bindings[j].descriptor->name) == 0)
        return COG_ERROR_INVALID_ARG;
  }
  if(c->background_job_timeout_ms!=0U) {
    if(!c->store.require_owner || (c->store.backend!=NULL &&
       (c->store.backend->capabilities & COG_STORE_CAP_TOOL_JOBS_V4)==0U))
      return COG_ERROR_UNSUPPORTED;
    if(!start_bound || !status_bound || c->tools.user==NULL || c->session.max_tool_jobs==0U ||
       c->session.max_tool_job_output_bytes==0U || c->scopes==NULL)return COG_ERROR_INVALID_ARG;
    size_t cap=c->limits.max_tool_output_bytes;
    if(cap>c->limits.max_context_bytes)cap=c->limits.max_context_bytes;
    if(cap<256U || c->session.max_tool_job_output_bytes>(cap-256U)/6U)return COG_ERROR_LIMIT;
    bool scope=false;
    for(size_t i=0U;i<c->scope_count;i++){
      const cog_runtime_scope_binding_t *b=&c->scopes[i];
      if(b->descriptor==&cog_workspace_start_background_tool && b->path_key!=NULL &&
         strcmp(b->path_key,"cwd")==0 && !b->file_target && b->default_scope!=NULL &&
         b->default_scope[0]=='\0')scope=true;
    }
    if(!scope)return COG_ERROR_INVALID_ARG;
  }
  if(c->workers!=NULL && c->instruction_mode!=COG_RUNTIME_INSTRUCTIONS_DISABLED) {
    if(c->scope_count!=0U && c->scopes==NULL)return COG_ERROR_NULL_PTR;
    for(size_t i=0U;i<c->tools.binding_count;i++) {
      if(c->tools.bindings[i].policy==COG_PROMPT_TOOL_DENY)continue;
      for(size_t j=0U;j<cog_workspace_tools.count;j++) {
        if(c->tools.bindings[i].descriptor!=&cog_workspace_tools.tools[j])continue;
        const char *key=j==3U?"cwd":"path";bool file=j==0U || j==2U,found=false;
        for(size_t k=0U;k<c->scope_count;k++)if(c->scopes[k].descriptor==c->tools.bindings[i].descriptor) {
          const cog_runtime_scope_binding_t *scope=&c->scopes[k];found=true;
          if(scope->path_key==NULL || strcmp(scope->path_key,key)!=0 || scope->file_target!=file ||
             (file ? scope->default_scope!=NULL : scope->default_scope==NULL || scope->default_scope[0]!='\0'))
            return COG_ERROR_UNSUPPORTED;
        }
        if(!found)return COG_ERROR_UNSUPPORTED;
      }
    }
  }
  if (c->mode == COG_RUNTIME_RECOVER || c->mode == COG_RUNTIME_INSPECT)
    return COG_OK;
  if (c->provider.endpoint == NULL || c->provider.model_id == NULL)
    return COG_ERROR_NULL_PTR;
  if (c->provider.endpoint->generation == 0U)
    return COG_ERROR_INVALID_ARG;
  cog_result_t rc = cog_endpoint_model_lookup(c->provider.endpoint,
                                              c->provider.model_id, NULL);
  if (rc != COG_OK)
    return rc;
  if (c->instruction_mode != COG_RUNTIME_INSTRUCTIONS_DISABLED &&
      c->provider.prompt_template == NULL)
    return COG_ERROR_NULL_PTR;
  return COG_OK;
}
cog_result_t cli_runtime_new_admitted(cog_runtime_t **out,
    const cog_runtime_config_t *config, const cog_cli_execution_admission_t *override) {
  cog_cli_configuration_t *supplied=override==NULL ? NULL : override->configuration;
  if (out == NULL) { cog_cli_configuration_free(supplied); return COG_ERROR_NULL_PTR; }
  *out = NULL;
  cog_result_t rc = validate_config(config);
  if (rc != COG_OK) { cog_cli_configuration_free(supplied); return rc; }
  cog_runtime_t *r = calloc(1U, sizeof(*r));
  if (r == NULL) { cog_cli_configuration_free(supplied); return COG_ERROR_NOMEM; }
  r->config = *config;
  r->snapshot_capacity = config->session.max_contexts;
  if (r->snapshot_capacity != 0U) {
    r->snapshots = calloc(r->snapshot_capacity, sizeof(*r->snapshots));
    if (r->snapshots == NULL) {
      free(r);
      cog_cli_configuration_free(supplied);
      return COG_ERROR_NOMEM;
    }
  }
  r->offline = config->mode == COG_RUNTIME_INSPECT ||
               config->mode == COG_RUNTIME_RECOVER;
  r->items = calloc(CLI_NODES + 2U, sizeof(*r->items));
  r->json_storage = calloc(CLI_ARGUMENT_BYTES * 3U, 1U);
  r->policy_storage = calloc(CLI_ARGUMENT_BYTES + CLI_ID_BYTES + CLI_ID_BYTES, 1U);
  rc = cli_runtime_instruction_init(r);
  if (rc != COG_OK || r->json_storage == NULL || r->policy_storage == NULL) {
    if (rc == COG_OK)
      rc = COG_ERROR_NOMEM;
    goto fail;
  }
  if (r->items == NULL) {
    rc = COG_ERROR_NOMEM;
    goto fail;
  }
  if (config->tools.binding_count != 0U) {
    cog_tool_async_config_t validation={.max_argument_bytes=CLI_ARGUMENT_BYTES,.max_schema_bytes=CLI_SCHEMA_BYTES,
        .max_json_depth=64U,.codec_workspace_bytes=CLI_CODEC_BYTES};
    rc=cog_toolkit_validator_new(&r->validator,&validation);
    if(rc!=COG_OK)goto fail;
    for(size_t i=0U;i<config->tools.binding_count;i++){
      rc=cog_toolkit_validate_schema(r->validator,config->tools.bindings[i].descriptor);
      if(rc!=COG_OK)goto fail;
    }
    if(config->background_job_timeout_ms!=0U){
      r->background_output=malloc(config->session.max_tool_job_output_bytes+1U);
      if(r->background_output==NULL){rc=COG_ERROR_NOMEM;goto fail;}
    }
    r->bindings = calloc(config->tools.binding_count, sizeof(*r->bindings));
    if (r->bindings == NULL) {
      rc = COG_ERROR_NOMEM;
      goto fail;
    }
    memcpy(r->bindings, config->tools.bindings,
           config->tools.binding_count * sizeof(*r->bindings));
    r->config.tools.bindings = r->bindings;
  }
  if (config->scope_count != 0U || config->workspace_root_id != NULL) {
    if (config->scope_count != 0U && config->scopes == NULL) {
      rc = COG_ERROR_NULL_PTR;
      goto fail;
    }
    size_t total = config->workspace_root_id == NULL
                       ? 0U
                       : strlen(config->workspace_root_id) + 1U;
    if (total > config->limits.max_instruction_bytes) { rc = COG_ERROR_LIMIT; goto fail; }
    for (size_t i = 0U; i < config->scope_count; i++) {
      const char *strings[2] = {config->scopes[i].path_key,
                                config->scopes[i].default_scope};
      for (size_t j = 0U; j < 2U; j++)
        if (strings[j] != NULL) {
          size_t n = strlen(strings[j]) + 1U;
          if (n > config->limits.max_instruction_bytes - total) {
            rc = COG_ERROR_LIMIT;
            goto fail;
          }
          total += n;
        }
    }
    if (total > config->limits.max_instruction_bytes ||
        (config->instruction_mode!=COG_RUNTIME_INSTRUCTIONS_DISABLED && config->scope_count > config->instruction_limits.max_tools)) {
      rc = COG_ERROR_LIMIT;
      goto fail;
    }
    r->scope_strings = malloc(total == 0U ? 1U : total);
    r->scope_bindings =
        calloc(config->scope_count == 0U ? 1U : config->scope_count,
               sizeof(*r->scope_bindings));
    if (r->scope_strings == NULL || r->scope_bindings == NULL) {
      rc = COG_ERROR_NOMEM;
      goto fail;
    }
    size_t at = 0U;
    if (config->workspace_root_id != NULL) {
      size_t n = strlen(config->workspace_root_id) + 1U;
      memcpy(r->scope_strings, config->workspace_root_id, n);
      r->config.workspace_root_id = r->scope_strings;
      at = n;
    }
    for (size_t i = 0U; i < config->scope_count; i++) {
      r->scope_bindings[i] = config->scopes[i];
      const char **targets[2] = {&r->scope_bindings[i].path_key,
                                 &r->scope_bindings[i].default_scope};
      for (size_t j = 0U; j < 2U; j++)
        if (*targets[j] != NULL) {
          size_t n = strlen(*targets[j]) + 1U;
          memcpy(r->scope_strings + at, *targets[j], n);
          *targets[j] = r->scope_strings + at;
          at += n;
        }
    }
    r->config.scopes = r->scope_bindings;
  }
  if (config->instructions != NULL) {
    rc = clone_snapshot(config->instructions, &r->config, &r->instructions);
    if (rc != COG_OK)
      goto fail;
    r->config.instructions = r->instructions;
  }
  if (!r->offline && config->provider.prompt_template != NULL) {
    rc = cog_provider_prompt_compiler_new(&r->compiler,
                                          config->provider.prompt_template,
                                          &config->provider.compiler_limits);
    if (rc != COG_OK)
      goto fail;
  }
  if (config->sources != NULL) {
    rc = cog_instruction_source_reader_new(&r->reader, config->sources, NULL);
    if (rc != COG_OK)
      goto fail;
  }
  bool coordinated = override == NULL && config->workers != NULL;
  for(size_t i=0U;override==NULL && i<config->tools.binding_count;i++)
    for(size_t j=0U;j<cog_runtime_delegation_tools.count;j++)
      if(config->tools.bindings[i].descriptor==&cog_runtime_delegation_tools.tools[j])
        coordinated=true;
  if(coordinated) {
    r->host_root_owner=true;
    cog_cli_host_runtime_config_t host={.headless=true,.runtime_config=&r->config,.runtime_owner=r,.signal_fd=-1,.projection_bytes=config->limits.max_view_bytes};
    if(config->workers!=NULL) {
      r->workers=*config->workers;
      r->config.workers=&r->workers;
      host.workers=r->workers.workers;host.children=r->workers.children;
      host.worker_socket=r->workers.socket_path;host.executable=r->workers.executable;
      host.net_config=r->workers.net_config;host.child_requests=r->workers.request_budget;
      host.child_profile.tools=r->workers.tool_mask;host.shutdown_ms=r->workers.shutdown_ms;
    }
    rc=cog_cli_host_runtime_new(&r->host,&host);
    if(rc!=COG_OK)goto fail;
    r->host->instruction_owner=r;
    r->app=r->host->executors[0].app;
    if(config->workers!=NULL) {
      r->workers.socket_path=r->host->fixed.worker_socket;
      r->workers.executable=r->host->fixed.executable;
      r->workers.workspace_root=r->host->canonical_workspace;
    }
  } else {
    cog_cli_configuration_t *resolved=supplied;
    if(resolved==NULL) {
      rc=cli_configuration_runtime_new(&resolved,&r->config);
      if(rc!=COG_OK)goto fail;
    }
    cog_cli_execution_admission_t admission=override==NULL ?
        (cog_cli_execution_admission_t){.kind=COG_CLI_EXECUTION_ROOT} : *override;
    admission.configuration=resolved;
    admission.configuration_generation=config->provider.endpoint==NULL ? 1U : config->provider.endpoint->generation;
    admission.runtime_config=&r->config;admission.runtime_owner=r;
    supplied=NULL; /* execution constructor consumes configuration on failure too */
    rc=cog_cli_execution_new(&r->app,&admission);
    if(rc!=COG_OK)goto fail;
  }
  r->app->runtime_owner = r;
  r->config.provider.model_id = r->app->opts.model_id;
  r->config.store.base_dir = r->app->opts.store_dir;
  r->config.session_id = cog_session_id(r->app->session);
  r->app->runtime_config.session.max_nodes =
      config->session.max_nodes == 0U ? CLI_NODES : config->session.max_nodes;
  if (config->instruction_mode == COG_RUNTIME_INSTRUCTIONS_ADOPT_LEGACY) {
    cog_session_snapshot_t state;
    rc = cog_session_snapshot_view(r->app->session, &state);
    if (rc == COG_OK && state.context_count != 0U)
      rc = COG_ERROR_INVALID_STATE;
    if (rc == COG_OK)
      rc = cli_runtime_initial_preflight(r->app);
    if (rc == COG_OK)
      rc = install_context(r->app, r->instructions);
    if (rc == COG_OK)
      rc = cog_cli_save(r->app);
    if (rc != COG_OK)
      goto fail;
  }
  if (config->instruction_mode != COG_RUNTIME_INSTRUCTIONS_DISABLED) {
    rc = current_snapshot(r);
    if (rc == COG_OK && !r->offline) rc=cli_runtime_initial_preflight(r->app);
    if (rc != COG_OK)
      goto fail;
  }
  if(r->host!=NULL && r->config.workers!=NULL && r->instructions!=NULL) {
    rc=cli_host_pin_instructions(r->host,r->instructions);
    if(rc!=COG_OK)goto fail;
  }
  *out = r;
  return COG_OK;
fail:
  cog_cli_configuration_free(supplied);
  (void)cog_runtime_close(&r, rc);
  return rc;
}
cog_result_t cog_runtime_new(cog_runtime_t **out,const cog_runtime_config_t *config) {
  return cli_runtime_new_admitted(out,config,NULL);
}
/* Host root owns its facade externally; children own their private facade. */
cog_result_t cli_runtime_app_close(cog_cli_app_t **app,cog_result_t primary) {
  if(app==NULL)return COG_ERROR_NULL_PTR;
  if(*app==NULL)return primary;
  cog_runtime_t *owner=(*app)->runtime_owner;
  if(owner==NULL)return cog_cli_execution_close(app,primary);
  if(owner->host_root_owner) {
    cog_result_t rc=cog_cli_execution_close(app,primary);
    if(*app==NULL)owner->app=NULL;
    return rc;
  }
  cog_result_t rc=cog_runtime_close(&owner,primary);
  if(owner==NULL)*app=NULL;
  return rc;
}
cog_result_t cli_runtime_compile(cog_cli_app_t *app,
                                 cog_provider_request_t *request) {
  if (app->runtime_owner == NULL)
    return COG_OK;
  cog_runtime_t *r = app->runtime_owner;
  if (r->instructions != NULL) {
    cog_result_t selected = current_snapshot(r);
    if (selected != COG_OK) return selected;
  }
  if (r->compiler == NULL)
    return COG_OK;
  cog_provider_prompt_input_t input = {0};
  if (request->max_tokens > app->opts.max_output_tokens)
    request->max_tokens = app->opts.max_output_tokens;
  input.request = *request;
  input.request.tools_json = NULL;
  input.request.tools_json_len = 0U;
  cog_result_t rc;
  if (r->instructions != NULL)
    rc = cog_instruction_prompt_view(r->instructions, &input.prompt);
  else {
    input.prompt = (cog_prompt_bundle_t){
        .catalog_generation=1U,.snapshot_id=1U,
        .model_id = request->model_id,
        .template_id = r->config.provider.prompt_template->id,
        .template_revision = r->config.provider.prompt_template->revision,
        .max_context_bytes = r->config.limits.max_context_bytes,
        .max_output_tokens = r->config.limits.max_output_tokens,
        .max_tool_rounds = r->config.limits.max_tool_rounds,
        .tools = r->config.tools.bindings,
        .tool_count = r->config.tools.binding_count};
    rc = COG_OK;
  }
  if (rc == COG_OK)
    rc = cog_provider_prompt_compile(
        r->compiler, r->config.provider.endpoint->vtable, &input, request);
  return rc;
}
cog_result_t cog_runtime_begin(cog_runtime_t *r) {
  if (r == NULL)
    return COG_ERROR_NULL_PTR;
  if (r->begun || r->shutdown || r->offline)
    return COG_ERROR_INVALID_STATE;
  cog_result_t rc = r->host==NULL ? cog_cli_execution_begin(r->app) : cog_cli_host_runtime_begin(r->host);
  if (rc == COG_OK || (r->host!=NULL && r->host->begun))
    r->begun = true;
  if(rc!=COG_OK)r->primary=rc;
  return rc;
}
cog_result_t cog_runtime_step(cog_runtime_t *r, cog_runtime_step_t *step) {
  if (r == NULL || step == NULL)
    return COG_ERROR_NULL_PTR;
  if (!r->begun || r->in_call)
    return COG_ERROR_INVALID_STATE;
  uint64_t job_deadline;
  cog_result_t job_rc=cli_runtime_job_service(r,&job_deadline);
  if(job_rc!=COG_OK){
    if(r->primary==COG_OK)r->primary=job_rc;
    (void)cog_runtime_shutdown(r);
  }
  if(r->foreground_done){
    *step=(cog_runtime_step_t){.done=r->background_execution==NULL,.next_deadline_ms=job_deadline,
      .max_wait_ms=1U,.outcome=r->primary};
    return job_rc;
  }
  cog_result_t activated = cli_runtime_activate(r);
  if (activated != COG_OK && activated != COG_ERROR_AGAIN)
    return activated;
  cog_result_t progressed = cli_runtime_instruction_progress(r);
  if (progressed != COG_OK && progressed != COG_ERROR_AGAIN) {
    r->primary = progressed;
    (void)cog_cli_app_shutdown(r->app);
  }
  r->in_call = true;
  cog_core_step_info_t info = {0};
  cog_result_t rc;
  if(r->host!=NULL) {
    cog_cli_host_runtime_step_t h;
    rc=cog_cli_host_runtime_step(r->host,&h);
    info.done=h.done;info.dispatched=h.dispatched;
    info.next_deadline_ms=h.next_deadline_ms;info.max_wait_ms=h.max_wait_ms;
  } else rc=cog_cli_execution_step(r->app,&info);
  r->in_call = false;
  if (rc != COG_OK && rc != COG_ERROR_AGAIN && r->primary == COG_OK)
    r->primary = rc;
  r->foreground_done=info.done;
  if(r->background_execution!=NULL){
    info.done=false;
    if(info.max_wait_ms>1U)info.max_wait_ms=1U;
    if(job_deadline<info.next_deadline_ms)info.next_deadline_ms=job_deadline;
  }
  *step = (cog_runtime_step_t){info.done,
                               !r->app->active && !r->shutdown,
                               r->app->headless->publication_pending,
                               info.dispatched,
                               info.next_deadline_ms,
                               info.max_wait_ms,
                               r->primary};
  return job_rc!=COG_OK?job_rc:rc;
}
cog_result_t cog_runtime_submit(cog_runtime_t *r, const char *text,
                                size_t bytes, bool queue) {
  if (r == NULL || text == NULL)
    return COG_ERROR_NULL_PTR;
  if (!r->begun || r->shutdown || r->offline)
    return COG_ERROR_INVALID_STATE;
  if (bytes == 0U || bytes > r->config.limits.max_input_bytes)
    return COG_ERROR_LIMIT;
  if (r->app->headless->publication_pending || r->future_provider!=NULL || r->control_change!=NULL)
    return COG_ERROR_AGAIN;
  if(r->background_id!=0U && r->app->continuation_required && !queue)return COG_ERROR_AGAIN;
  size_t used = 0U;
  for (size_t i = 0U; i < CLI_INPUTS; i++)
    if (r->app->inputs[i].used)
      used++;
  if (used >= r->config.limits.max_queued_inputs)
    return COG_ERROR_OVERFLOW;
  return queue ? cog_cli_app_queue(r->app, text, bytes)
               : cog_cli_app_submit(r->app, text, bytes);
}
cog_result_t cog_runtime_cancel(cog_runtime_t *r) {
  if (r == NULL)
    return COG_ERROR_NULL_PTR;
  return !r->begun || r->offline ? COG_ERROR_INVALID_STATE
                                 : cog_cli_app_cancel(r->app);
}
cog_result_t cog_runtime_shutdown(cog_runtime_t *r) {
  if (r == NULL)
    return COG_ERROR_NULL_PTR;
  r->shutdown = true;
  if(r->host!=NULL)return cog_cli_host_runtime_request_shutdown(r->host);
  return r->begun ? cog_cli_app_shutdown(r->app) : COG_OK;
}
cog_result_t cog_runtime_view(const cog_runtime_t *owner,
                              cog_runtime_view_t *view) {
  if (owner == NULL || view == NULL)
    return COG_ERROR_NULL_PTR;
  cog_runtime_t *r = (cog_runtime_t *)owner;
  if (r->view.serial != 0U && r->view.serial != r->acknowledged) {
    *view = r->view;
    return COG_OK;
  }
  cog_cli_display_snapshot_t old;
  uint64_t serial = 0U;
  cog_result_t rc = r->offline
                        ? cog_cli_app_display_snapshot(r->app, &old)
                        : cog_cli_execution_copy_view(r->app, &serial, &old);
  if (rc != COG_OK)
    return rc;
  size_t bytes = 0U;
  for (size_t i = 0U; i < old.item_count; i++) {
    if (old.items[i].text_len > r->config.limits.max_view_bytes - bytes)
      return COG_ERROR_LIMIT;
    bytes += old.items[i].text_len;
    r->items[i] = (cog_runtime_view_item_t){
        old.items[i].node_id, old.items[i].role, old.items[i].status,
        old.items[i].text, old.items[i].text_len};
  }
  cog_instruction_manifest_t manifest = {0};
  if (r->instructions != NULL) {
    rc = cog_instruction_snapshot_view(r->instructions, &manifest);
    if (rc != COG_OK)
      return rc;
  }
  r->view = (cog_runtime_view_t){
      .serial = r->offline ? r->app->display_sequence + 1U : serial,
      .active_generation = r->app->configuration_generation,
      .published_generation = r->future_provider == NULL
                                  ? r->app->configuration_generation
                                  : r->future.endpoint->generation,
      .instruction_catalog_generation = manifest.catalog_generation,
      .instruction_snapshot_id = manifest.snapshot_id,
      .session_id = old.session_id,
      .items = r->items,
      .item_count = old.item_count,
      .outcome = r->primary,
      .continuation_required = r->app->continuation_required};
  if(r->offline){
    cog_session_snapshot_t state;
    rc=cog_session_snapshot_view(r->app->session,&state);
    if(rc!=COG_OK)return rc;
    r->view.offline_jobs=state.tool_jobs;
    r->view.offline_job_count=state.tool_job_count;
  }
  if (r->app->headless != NULL && r->app->headless->policy_pending) {
    const cog_message_tool_call_t *call =
        &r->app->call_views[r->app->tool_index];
    size_t id_bytes = strlen(call->id) + 1U;
    size_t name_bytes = strlen(call->name) + 1U;
    if (id_bytes > CLI_ID_BYTES || name_bytes > CLI_ID_BYTES ||
        call->arguments_len > CLI_ARGUMENT_BYTES) return COG_ERROR_LIMIT;
    memcpy(r->policy_storage, call->id, id_bytes);
    memcpy(r->policy_storage + CLI_ID_BYTES, call->name, name_bytes);
    memcpy(r->policy_storage + CLI_ID_BYTES + CLI_ID_BYTES,
           call->arguments, call->arguments_len);
    r->policy =
        (cog_runtime_policy_t){r->app->headless->prompt_serial,
                               cog_node_data(r->app->assistant)->node_id,
                               r->app->configuration_generation,
                               manifest.catalog_generation,
                               manifest.snapshot_id,
                               r->app->headless->policy.deadline_ms,
                               r->policy_storage,
                               r->policy_storage + CLI_ID_BYTES,
                               r->policy_storage + CLI_ID_BYTES + CLI_ID_BYTES,
                               call->arguments_len};
    r->view.policy = &r->policy;
  }
  *view = r->view;
  return COG_OK;
}
cog_result_t cog_runtime_ack(cog_runtime_t *r, uint64_t serial) {
  if (r == NULL)
    return COG_ERROR_NULL_PTR;
  if (serial == 0U)
    return COG_ERROR_INVALID_STATE;
  if (serial == r->acknowledged)
    return COG_OK;
  cog_result_t rc =
      r->offline ? (serial == r->view.serial ? COG_OK : COG_ERROR_INVALID_STATE)
                 : cog_cli_execution_commit_view(r->app, serial,
                                                 r->app->generation, serial);
  if (rc == COG_OK)
    r->acknowledged = serial;
  return rc;
}
cog_result_t cog_runtime_answer(cog_runtime_t *r,
                                const cog_runtime_policy_t *policy,
                                bool allow) {
  if (r == NULL || policy == NULL)
    return COG_ERROR_NULL_PTR;
  const cog_runtime_policy_t *p = &r->policy;
  if (!r->begun || r->view.policy == NULL ||
      r->acknowledged != r->view.serial || p->serial != policy->serial ||
      p->assistant_node_id != policy->assistant_node_id ||
      p->configuration_generation != policy->configuration_generation ||
      p->instruction_catalog_generation !=
          policy->instruction_catalog_generation ||
      p->instruction_snapshot_id != policy->instruction_snapshot_id ||
      p->deadline_ms != policy->deadline_ms || policy->call_id == NULL ||
      policy->tool_name == NULL || policy->arguments == NULL ||
      p->argument_bytes != policy->argument_bytes ||
      strcmp(p->call_id, policy->call_id) != 0 ||
      strcmp(p->tool_name, policy->tool_name) != 0 ||
      memcmp(p->arguments, policy->arguments, p->argument_bytes) != 0)
    return COG_ERROR_INVALID_STATE;
  cog_cli_command_t command = {.kind = COG_CLI_COMMAND_POLICY, .nonce=p->serial,
                               .observed_generation=r->app->generation,
                               .observed_sequence=r->acknowledged,
                               .node_id = p->assistant_node_id,
                               .call_ordinal = (uint16_t)r->app->tool_index,
                               .target_session_id =
                                   cog_session_id(r->app->session),
                               .decision = allow ? 2U : 1U};
  return cog_cli_execution_apply_command(
      r->app, &command, r->app->headless->expected_owner, p->serial);
}
cog_result_t cog_runtime_fork(cog_runtime_t *r, uint64_t node_id) {
  if (r == NULL)
    return COG_ERROR_NULL_PTR;
  if (r->background_id!=0U || (r->app->headless != NULL && r->app->headless->publication_pending) ||
      r->future_provider!=NULL || r->control_change!=NULL || r->instruction_job!=NULL)
    return COG_ERROR_AGAIN;
  cog_result_t rc = cog_cli_app_fork(r->app, node_id);
  if (rc == COG_OK && r->instructions != NULL)
    rc = current_snapshot(r);
  return rc;
}
cog_result_t cog_runtime_reconcile(cog_runtime_t *r, uint64_t node_id,
                                   const char *call_id,
                                   cog_tool_execution_state_t outcome,
                                   const char *evidence, size_t bytes) {
  if (r == NULL)
    return COG_ERROR_NULL_PTR;
  return cog_cli_app_reconcile(r->app, node_id, call_id, outcome, evidence,
                               bytes);
}
cog_result_t cog_runtime_close(cog_runtime_t **owner, cog_result_t primary) {
  if (owner == NULL)
    return COG_ERROR_NULL_PTR;
  cog_runtime_t *r = *owner;
  if (r == NULL)
    return primary;
  if(r->background_execution!=NULL)return COG_ERROR_INVALID_STATE;
  if(r->host!=NULL) {
    cog_result_t closed=cog_cli_host_runtime_close(&r->host,primary);
    if(r->host!=NULL)return closed;
    r->app=NULL;
    if(primary==COG_OK)primary=closed;
  }
  if (r->app != NULL) {
    cog_result_t rc = r->app->headless != NULL
                          ? cog_cli_execution_close(&r->app, primary)
                          : cli_app_dispose(&r->app, primary);
    if (r->app != NULL)
      return rc;
    if (primary == COG_OK)
      primary = rc;
  }
  cog_result_t future_cleanup = cog_provider_disconnect(r->future_provider);
  if (primary == COG_OK)
    primary = future_cleanup;
  cog_cli_configuration_free(r->future_configuration);
  cog_provider_prompt_compiler_free(r->future_compiler);
  cog_runtime_instruction_change_free(&r->future_instructions);
  for (size_t i = 0U; i < CLI_CALLS; i++)
    cog_instruction_snapshot_free(r->child_snapshots[i]);
  cog_result_t instruction_cleanup = cli_runtime_instruction_dispose(r);
  if (primary == COG_OK)
    primary = instruction_cleanup;
  free(r->json_storage);
  free(r->policy_storage);
  if (!r->snapshots_loaded)
    cog_instruction_snapshot_free(r->instructions);
  for (size_t i = 0U; i < r->snapshot_count; i++)
    cog_instruction_snapshot_free(r->snapshots[i].snapshot);
  free(r->snapshots);
  cog_provider_prompt_compiler_free(r->compiler);
  cog_result_t rc = cog_instruction_source_reader_free(r->reader);
  if (primary == COG_OK)
    primary = rc;
  cog_toolkit_validator_free(r->validator);
  free(r->background_output);
  free(r->scope_bindings);
  free(r->scope_strings);
  free(r->bindings);
  free(r->items);
  free(r);
  *owner = NULL;
  return primary;
}

static cog_result_t encode_owned(const cog_instruction_snapshot_t *snapshot,
                                 size_t limit, unsigned char **out,
                                 size_t *bytes) {
  cog_instruction_manifest_t m;
  cog_result_t rc = cog_instruction_snapshot_view(snapshot, &m);
  if (rc != COG_OK)
    return rc;
  size_t needed = 0U;
  rc = cog_instruction_manifest_encode(&m, NULL, 0U, &needed);
  if (rc != COG_OK && rc != COG_ERROR_OVERFLOW)
    return rc;
  if (needed > limit)
    return COG_ERROR_LIMIT;
  unsigned char *p = malloc(needed == 0U ? 1U : needed);
  if (p == NULL)
    return COG_ERROR_NOMEM;
  rc = cog_instruction_manifest_encode(&m, p, needed, &needed);
  if (rc != COG_OK) {
    free(p);
    return rc;
  }
  *out = p;
  *bytes = needed;
  return COG_OK;
}
static cog_result_t context_capacity(const cog_runtime_t *r,
    const cog_session_snapshot_t *state, size_t count) {
  size_t used = 0U;
  for(size_t i=0U;i<state->context_count;i++) {
    size_t name_bytes=strlen(state->contexts[i].namespace_id)+1U;
    if(name_bytes>SIZE_MAX-used || state->contexts[i].data_bytes>SIZE_MAX-used-name_bytes)
      return COG_ERROR_LIMIT;
    used+=name_bytes+state->contexts[i].data_bytes;
  }
  size_t name_bytes=sizeof(COG_INSTRUCTION_CONTEXT_NAMESPACE);
  if(state->context_count>=r->config.session.max_contexts ||
      state->context_binding_count>=r->config.session.max_context_bindings ||
      used>r->config.session.max_context_bytes ||
      name_bytes>r->config.session.max_context_bytes-used ||
      count>r->config.session.max_context_bytes-used-name_bytes)
    return COG_ERROR_LIMIT;
  return COG_OK;
}
static cog_result_t install_encoded(cog_runtime_t *r,
                                    const unsigned char *bytes, size_t count) {
  cog_session_snapshot_t state;
  cog_result_t rc = cog_session_snapshot_view(r->app->session, &state);
  if (rc != COG_OK)
    return rc;
  if (state.context_high_water == UINT64_MAX ||
      state.context_binding_high_water == UINT64_MAX)
    return COG_ERROR_LIMIT;
  rc = context_capacity(r, &state, count);
  if (rc != COG_OK) return rc;
  cog_session_context_t context = {
      state.context_high_water + 1U, COG_INSTRUCTION_CONTEXT_NAMESPACE,
      COG_INSTRUCTION_CONTEXT_FORMAT, bytes, count};
  cog_session_context_binding_t binding = {
      state.context_binding_high_water + 1U, state.selected_context_binding_id,
      state.active_node_id, context.context_id};
  rc = cog_session_context_add(r->app->session, &context);
  if (rc == COG_OK)
    rc = cog_session_context_publish(r->app->session, &binding);
  if (rc != COG_OK) {
    r->app->recovery = true;
    return rc;
  }
  rc = cog_cli_save(r->app);
  if (rc != COG_OK)
    r->app->recovery = true;
  return rc;
}
static cog_result_t snapshot_restriction(const cog_runtime_t *r,
    const cog_instruction_snapshot_t *candidate) {
  if(r->instructions==NULL)return COG_OK;
  cog_instruction_manifest_t old,next;
  cog_result_t rc=cog_instruction_snapshot_view(r->instructions,&old);
  if(rc==COG_OK)rc=cog_instruction_snapshot_view(candidate,&next);
  if(rc!=COG_OK)return rc;
  if(strcmp(old.workspace_root_id,next.workspace_root_id)!=0 ||
      next.max_context_bytes>old.max_context_bytes || next.max_output_tokens>old.max_output_tokens ||
      next.max_tool_rounds>old.max_tool_rounds)return COG_ERROR_DENIED;
  for(size_t i=0U;i<next.tool_count;i++) {
    bool allowed=false;
    for(size_t j=0U;j<old.tool_count;j++)
      if(strcmp(next.tools[i].name,old.tools[j].name)==0 && next.tools[i].policy>=old.tools[j].policy)allowed=true;
    if(!allowed)return COG_ERROR_DENIED;
  }
  for(size_t i=0U;i<next.source_count;i++) {
    bool allowed=false;
    for(size_t j=0U;j<old.source_count;j++)
      if(strcmp(next.sources[i].root_id,old.sources[j].root_id)==0)allowed=true;
    if(!allowed)return COG_ERROR_DENIED;
  }
  return COG_OK;
}
cog_result_t
cog_runtime_instruction_prepare(cog_runtime_instruction_change_t **out,
                                cog_runtime_t *r,
                                const cog_instruction_snapshot_t *snapshot) {
  if (out == NULL)
    return COG_ERROR_NULL_PTR;
  *out = NULL;
  if (r == NULL || snapshot == NULL)
    return COG_ERROR_NULL_PTR;
  if (r->offline || r->shutdown || r->app->recovery ||
      r->config.instruction_mode == COG_RUNTIME_INSTRUCTIONS_DISABLED)
    return COG_ERROR_INVALID_STATE;
  cog_instruction_manifest_t m;
  cog_result_t rc = cog_instruction_snapshot_view(snapshot, &m);
  if (rc != COG_OK)
    return rc;
  rc=snapshot_restriction(r,snapshot);
  if(rc!=COG_OK)return rc;
  if (m.snapshot_id <= r->snapshot_high_water)
    return COG_ERROR_INVALID_ARG;
  if (r->snapshot_count == r->snapshot_capacity ||
      m.max_context_bytes > r->config.limits.max_context_bytes ||
      m.max_output_tokens > r->config.limits.max_output_tokens ||
      m.max_tool_rounds > r->config.limits.max_tool_rounds)
    return COG_ERROR_LIMIT;
  if (strcmp(m.model_id, r->app->opts.model_id) != 0 ||
      strcmp(m.template_id, r->config.provider.prompt_template->id) != 0 ||
      m.template_revision != r->config.provider.prompt_template->revision)
    return COG_ERROR_UNSUPPORTED;
  cog_runtime_instruction_change_t *c = calloc(1U, sizeof(*c));
  if (c == NULL)
    return COG_ERROR_NOMEM;
  rc = clone_snapshot(snapshot, &r->config, &c->snapshot);
  if (rc == COG_OK)
    rc = encode_owned(c->snapshot, r->config.limits.max_instruction_bytes,
                      &c->encoded, &c->encoded_bytes);
  if (rc == COG_OK) {
    size_t count = 0U;
    bool pending=r->app->assistant!=NULL && r->app->call_count!=0U;
    if(pending) {
      for(cog_session_node_t *node=cog_session_active(r->app->session);node!=NULL;node=cog_node_parent(node))
        if(++count>CLI_NODES){rc=COG_ERROR_LIMIT;break;}
      if(rc==COG_OK) {
        size_t at=count;
        for(cog_session_node_t *node=cog_session_active(r->app->session);node!=NULL;node=cog_node_parent(node))
          r->app->history[--at]=*cog_node_data(node)->message;
      }
    } else rc=cog_session_copy_history(r->app->session,NULL,r->app->history,CLI_NODES,&count);
    cog_provider_prompt_input_t input = {0};
    input.request = (cog_provider_request_t){
        .model_id = r->app->opts.model_id, .messages = r->app->history,
        .message_count = count, .stream = true,
        .max_tokens = m.max_output_tokens,
        .configuration_generation = r->app->configuration_generation};
    if (rc == COG_OK) rc = cog_instruction_prompt_view(c->snapshot, &input.prompt);
    cog_provider_request_t compiled;
    if (rc == COG_OK) rc = cog_provider_prompt_compile(r->compiler,
        r->config.provider.endpoint->vtable, &input, &compiled);
    if (rc == COG_OK) rc = cog_provider_preflight(r->app->provider, &compiled,
        r->app->assistant != NULL && r->app->call_count != 0U
            ? COG_PROVIDER_PREFLIGHT_PENDING_TOOLS : COG_PROVIDER_PREFLIGHT_COMPLETE);
  }
  cog_session_snapshot_t state;
  if (rc == COG_OK)
    rc = cog_session_snapshot_view(r->app->session, &state);
  if (rc == COG_OK) rc = context_capacity(r, &state, c->encoded_bytes);
  if (rc != COG_OK) {
    cog_runtime_instruction_change_free(&c);
    return rc;
  }
  c->runtime = r;
  c->expected_node = state.active_node_id;
  c->expected_binding = state.selected_context_binding_id;
  c->expected_generation = r->app->configuration_generation;
  r->snapshot_high_water = m.snapshot_id;
  *out = c;
  return COG_OK;
}
cog_result_t
cog_runtime_instruction_publish(cog_runtime_t *r,
                                cog_runtime_instruction_change_t **owner) {
  if (r == NULL || owner == NULL || *owner == NULL)
    return COG_ERROR_NULL_PTR;
  cog_runtime_instruction_change_t *c = *owner;
  if (c->runtime != r || r->offline || r->app->recovery || r->shutdown)
    return COG_ERROR_INVALID_STATE;
  if (r->app->active || r->app->provider_active || r->app->execution != NULL ||
      r->app->headless->publication_pending || r->future_provider!=NULL)
    return COG_ERROR_AGAIN;
  cog_session_snapshot_t state;
  cog_result_t rc = cog_session_snapshot_view(r->app->session, &state);
  if (rc != COG_OK)
    return rc;
  if (state.active_node_id != c->expected_node ||
      state.selected_context_binding_id != c->expected_binding ||
      r->app->configuration_generation != c->expected_generation)
    return COG_ERROR_INVALID_STATE;
  rc = install_encoded(r, c->encoded, c->encoded_bytes);
  if (rc != COG_OK)
    return rc;
  rc = cli_runtime_cache_publish(r, &c->snapshot);
  if (rc != COG_OK) {
    r->app->recovery = true;
    return rc;
  }
  cog_state_t next;
  rc = cli_execution_barrier(r->app, CLI_AFTER_CONTROL, CLI_IDLE, &next);
  if (rc != COG_OK) {
    r->app->recovery = true;
    return rc;
  }
  return cog_runtime_instruction_change_free(owner);
}
cog_result_t
cog_runtime_instruction_change_free(cog_runtime_instruction_change_t **owner) {
  if (owner == NULL)
    return COG_ERROR_NULL_PTR;
  cog_runtime_instruction_change_t *c = *owner;
  if (c == NULL)
    return COG_OK;
  cog_instruction_snapshot_free(c->snapshot);
  if (c->encoded != NULL) {
    volatile unsigned char *p = c->encoded;
    for (size_t i = 0U; i < c->encoded_bytes; i++)
      p[i] = 0U;
    free(c->encoded);
  }
  free(c);
  *owner = NULL;
  return COG_OK;
}
cog_result_t
cog_runtime_reload(cog_runtime_t *r,
                   const cog_runtime_provider_config_t *provider,
                   const cog_instruction_snapshot_t *instructions) {
  if (r == NULL || provider == NULL || provider->endpoint == NULL ||
      provider->model_id == NULL)
    return COG_ERROR_NULL_PTR;
  if (r->offline || r->shutdown || r->app->recovery)
    return COG_ERROR_INVALID_STATE;
  if (r->background_id!=0U || r->future_provider != NULL)
    return COG_ERROR_AGAIN;
  if (provider->endpoint->generation <= r->app->configuration_generation)
    return COG_ERROR_INVALID_ARG;
  if (r->reload_attempts >= 64U)
    return COG_ERROR_LIMIT;
  r->reload_attempts++;
  cog_runtime_config_t candidate = r->config;
  candidate.provider = *provider;
  cog_instruction_snapshot_t *snapshot = NULL;
  cog_runtime_instruction_change_t *change = NULL;
  cog_cli_configuration_t *configuration = NULL;
  cog_provider_t *connected = NULL;
  cog_provider_prompt_compiler_t *compiler = NULL;
  cog_result_t rc = COG_OK;
  if (r->config.instruction_mode == COG_RUNTIME_INSTRUCTIONS_DISABLED &&
      instructions != NULL)
    return COG_ERROR_INVALID_ARG;
  if (r->instructions != NULL) {
    const cog_instruction_snapshot_t *source =
        instructions == NULL ? r->instructions : instructions;
    rc = clone_snapshot(source, &candidate, &snapshot);
    cog_instruction_manifest_t m;
    if (rc == COG_OK)
      rc = cog_instruction_snapshot_view(snapshot, &m);
    if (rc == COG_OK &&
        (strcmp(m.model_id, provider->model_id) != 0 ||
         provider->prompt_template == NULL ||
         strcmp(m.template_id, provider->prompt_template->id) != 0 ||
         m.template_revision != provider->prompt_template->revision))
      rc = COG_ERROR_UNSUPPORTED;
    if (rc == COG_OK && instructions != NULL) {
      if (m.snapshot_id <= r->snapshot_high_water)
        rc = COG_ERROR_INVALID_ARG;
      else {
        change = calloc(1U, sizeof(*change));
        if (change == NULL)
          rc = COG_ERROR_NOMEM;
        else {
          change->snapshot = snapshot;
          snapshot = NULL;
          change->runtime = r;
          rc = encode_owned(change->snapshot,
                            r->config.limits.max_instruction_bytes,
                            &change->encoded, &change->encoded_bytes);
        }
      }
    }
  }
  if(rc==COG_OK && (change!=NULL || snapshot!=NULL))
    rc=snapshot_restriction(r,change!=NULL ? change->snapshot : snapshot);
  if (rc == COG_OK)
    rc = cli_configuration_runtime_new(&configuration, &candidate);
  if (rc == COG_OK && provider->prompt_template != NULL)
    rc = cog_provider_prompt_compiler_new(&compiler, provider->prompt_template,
                                          &provider->compiler_limits);
  if (rc == COG_OK)
    rc = cog_provider_connect(provider->endpoint, &connected);
  if (rc == COG_OK) {
    size_t count = 0U;
    cog_session_node_t *selected=r->app->fork_parent!=NULL ? r->app->fork_parent : cog_session_active(r->app->session);
    for(cog_session_node_t *node=selected;node!=NULL;node=cog_node_parent(node))
      if(++count>CLI_NODES){rc=COG_ERROR_LIMIT;break;}
    size_t missing=0U;
    if(rc==COG_OK) {
      size_t at=count;
      for(cog_session_node_t *node=selected;node!=NULL;node=cog_node_parent(node))
        r->app->history[--at]=*cog_node_data(node)->message;
      for(size_t i=0U;i<count;i++) {
        const cog_message_t *message=&r->app->history[i];
        if(message->role==COG_MESSAGE_ROLE_ASSISTANT)missing=message->tool_call_count;
        else if(message->role==COG_MESSAGE_ROLE_TOOL && missing!=0U)missing--;
      }
    }
    cog_provider_request_t request = {
        .model_id = provider->model_id,
        .messages = r->app->history,
        .message_count = count,
        .stream = true,
        .max_tokens = r->config.limits.max_output_tokens,
        .configuration_generation = provider->endpoint->generation};
    if (rc == COG_OK && compiler != NULL) {
      cog_provider_prompt_input_t input = {0};
      input.request = request;
      const cog_instruction_snapshot_t *source =
          change != NULL ? change->snapshot : snapshot;
      if (source != NULL)
        rc = cog_instruction_prompt_view(source, &input.prompt);
      else
        input.prompt = (cog_prompt_bundle_t){
            .catalog_generation=1U,.snapshot_id=1U,
            .model_id = provider->model_id,
            .template_id = provider->prompt_template->id,
            .template_revision = provider->prompt_template->revision,
            .max_context_bytes = r->config.limits.max_context_bytes,
            .max_output_tokens = r->config.limits.max_output_tokens,
            .max_tool_rounds = r->config.limits.max_tool_rounds,
            .tools = r->bindings,
            .tool_count = r->config.tools.binding_count};
      if(rc==COG_OK && input.request.max_tokens>input.prompt.max_output_tokens)
        input.request.max_tokens=input.prompt.max_output_tokens;
      if (rc == COG_OK)
        rc = cog_provider_prompt_compile(compiler, provider->endpoint->vtable,
                                         &input, &request);
    }
    if (rc == COG_OK)
      rc = cog_provider_preflight(connected, &request,
                                  missing!=0U ? COG_PROVIDER_PREFLIGHT_PENDING_TOOLS : COG_PROVIDER_PREFLIGHT_COMPLETE);
  }
  if(rc==COG_OK && r->host!=NULL)
    rc=cli_host_prepare_reload(r->host,configuration,
        change!=NULL ? change->snapshot : snapshot,provider->endpoint->generation);
  cog_instruction_snapshot_free(snapshot);
  if (rc != COG_OK) {
    cog_result_t cleanup = cog_provider_disconnect(connected);
    cog_cli_configuration_free(configuration);
    cog_provider_prompt_compiler_free(compiler);
    cog_runtime_instruction_change_free(&change);
    if (cleanup != COG_OK) {
      r->app->recovery = true;
      r->primary = cleanup;
    }
    return rc;
  }
  if (change != NULL) {
    cog_instruction_manifest_t m;
    rc = cog_instruction_snapshot_view(change->snapshot, &m);
    if (rc != COG_OK)
      return rc;
    r->snapshot_high_water = m.snapshot_id;
  }
  r->future = *provider;
  r->future.model_id = cog_cli_configuration_options(configuration)->model_id;
  r->future_provider = connected;
  r->future_configuration = configuration;
  r->future_compiler = compiler;
  r->future_instructions = change;
  return COG_OK;
}
cog_result_t cli_runtime_activate(cog_runtime_t *r) {
  if (r->future_provider == NULL)
    return COG_OK;
  if (r->app->active || r->app->provider_active || r->app->execution != NULL ||
      r->app->headless->publication_pending || r->app->recovery)
    return COG_ERROR_AGAIN;
  if (r->future_instructions != NULL) {
    cog_result_t rc = install_encoded(r, r->future_instructions->encoded,
                                      r->future_instructions->encoded_bytes);
    if (rc != COG_OK)
      return rc;
  }
  cog_provider_t *old = r->app->provider;
  cog_cli_configuration_t *old_config = r->app->configuration;
  r->app->provider = r->future_provider;
  r->future_provider = NULL;
  r->app->configuration = r->future_configuration;
  r->future_configuration = NULL;
  r->app->opts = *cog_cli_configuration_options(r->app->configuration);
  r->app->configuration_generation = r->future.endpoint->generation;
  cog_core_set_provider(r->app->core, r->app->provider);
  r->config.provider = r->future;
  r->app->runtime_config.provider=r->future;
  if(r->host!=NULL) {
    r->host->active_configuration=r->future.endpoint->generation;
    r->host->executors[r->host->primary_slot].configuration_generation=r->future.endpoint->generation;
  }
  cog_provider_prompt_compiler_free(r->compiler);
  r->compiler = r->future_compiler;
  r->future_compiler = NULL;
  if (r->future_instructions != NULL) {
    cog_result_t cached =
        cli_runtime_cache_publish(r, &r->future_instructions->snapshot);
    if (cached != COG_OK) {
      r->app->recovery = true;
      return cached;
    }
    cog_runtime_instruction_change_free(&r->future_instructions);
  }
  if(r->instructions!=NULL) {
    cog_result_t selected=select_snapshot(r,r->instructions);
    if(selected!=COG_OK){r->app->recovery=true;return selected;}
  }
  cog_result_t rc = cog_provider_disconnect(old);
  cog_cli_configuration_free(old_config);
  if (rc != COG_OK) {
    r->app->recovery = true;
    r->primary = rc;
    return rc;
  }
  cog_state_t next;
  return cli_execution_barrier(r->app, CLI_AFTER_CONTROL, CLI_IDLE, &next);
}
cog_result_t cog_runtime_child_select(cog_runtime_t *r, uint64_t node_id,
                                      const char *call_id, size_t ordinal,
                                      const char *agent,
                                      const char *const *skills, size_t count) {
  if (r == NULL || call_id == NULL || (count != 0U && skills == NULL))
    return COG_ERROR_NULL_PTR;
  if (r->instructions == NULL || r->offline || r->app->assistant == NULL ||
      cog_node_data(r->app->assistant)->node_id != node_id ||
      ordinal >= CLI_CALLS)
    return COG_ERROR_INVALID_STATE;
  const cog_message_tool_call_t *call = NULL;
  for (size_t i = 0U; i < r->app->call_count; i++)
    if (strcmp(r->app->call_views[i].id, call_id) == 0)
      call = &r->app->call_views[i];
  if (call == NULL)
    return COG_ERROR_NOT_FOUND;
  if (strcmp(call->name, "delegate") != 0)
    return COG_ERROR_INVALID_STATE;
  bool original=false;
  for(size_t i=0U;i<r->config.tools.binding_count;i++)
    if(r->bindings[i].descriptor==&cog_runtime_delegation_tools.tools[0])original=true;
  if(!original)return COG_ERROR_INVALID_STATE;
  size_t task_count=0U;
  cog_result_t rc=cli_delegation_task_count(call->arguments,call->arguments_len,&task_count);
  if(rc!=COG_OK)return rc;
  if(ordinal>=task_count)return COG_ERROR_INVALID_ARG;
  cog_tool_execution_state_t state;
  rc = cog_session_tool_state(r->app->session, r->app->assistant,
                                           call_id, &state);
  if (rc != COG_OK)
    return rc;
  if (state != COG_TOOL_EXECUTION_PROPOSED)
    return COG_ERROR_INVALID_STATE;
  cog_instruction_manifest_t m;
  rc = cog_instruction_snapshot_view(r->instructions, &m);
  if (rc != COG_OK)
    return rc;
  if (r->child_node != node_id || strcmp(r->child_call,call_id)!=0) {
    for(size_t i=0U;i<CLI_CALLS;i++) {
      cog_instruction_snapshot_free(r->child_snapshots[i]);
      r->child_snapshots[i]=NULL;
    }
  }

  cog_prompt_bundle_t parent_prompt;
  rc=cog_instruction_prompt_view(r->instructions,&parent_prompt);
  if(rc!=COG_OK)return rc;
  bool enabled=false;
  for(size_t i=0U;i<parent_prompt.tool_count;i++)
    if(parent_prompt.tools[i].descriptor==&cog_runtime_delegation_tools.tools[0] &&
       parent_prompt.tools[i].policy!=COG_PROMPT_TOOL_DENY)enabled=true;
  if(!enabled)return COG_ERROR_DENIED;
  cog_instruction_source_t *sources = calloc(m.source_count, sizeof(*sources));
  if (sources == NULL)
    return COG_ERROR_NOMEM;
  size_t n = 0U;
  for (size_t i = 0U; i < m.source_count; i++)
    if (m.sources[i].kind == COG_INSTRUCTION_HOST ||
        m.sources[i].kind == COG_INSTRUCTION_AGENT ||
        m.sources[i].kind == COG_INSTRUCTION_SKILL)
      sources[n++] = m.sources[i];
  cog_instruction_catalog_t *catalog = NULL;
  rc = cog_instruction_catalog_new(&catalog, &r->config.instruction_limits,
                                   m.catalog_generation, sources, n, NULL);
  free(sources);
  size_t selected_count=m.active_skill_count;
  if(count>r->config.instruction_limits.max_active_skills ||
      selected_count>r->config.instruction_limits.max_active_skills-count) {
    cog_instruction_catalog_free(catalog);return COG_ERROR_LIMIT;
  }
  const char **selected_skills=calloc(selected_count+count+1U,sizeof(*selected_skills));
  if(selected_skills==NULL){cog_instruction_catalog_free(catalog);return COG_ERROR_NOMEM;}
  for(size_t i=0U;i<m.active_skill_count;i++)selected_skills[i]=m.active_skills[i];
  for(size_t i=0U;i<count;i++) {
    if(skills[i]==NULL){free(selected_skills);cog_instruction_catalog_free(catalog);return COG_ERROR_NULL_PTR;}
    for(size_t j=0U;j<i;j++)if(strcmp(skills[i],skills[j])==0){free(selected_skills);cog_instruction_catalog_free(catalog);return COG_ERROR_INVALID_ARG;}
    bool present=false;
    for(size_t j=0U;j<selected_count;j++)if(strcmp(skills[i],selected_skills[j])==0)present=true;
    if(!present)selected_skills[selected_count++]=skills[i];
  }
  cog_prompt_tool_binding_t *child_bindings=calloc(parent_prompt.tool_count+1U,sizeof(*child_bindings));
  if(child_bindings==NULL){free(selected_skills);cog_instruction_catalog_free(catalog);return COG_ERROR_NOMEM;}
  for(size_t i=0U;i<parent_prompt.tool_count;i++) {
    child_bindings[i]=parent_prompt.tools[i];
    const cog_tool_t *d=child_bindings[i].descriptor;
    if(d==&cog_runtime_delegation_tools.tools[0] || child_bindings[i].policy==COG_PROMPT_TOOL_ASK)
      child_bindings[i].policy=COG_PROMPT_TOOL_DENY;
    if(r->config.workers!=NULL)for(size_t j=0U;j<cog_workspace_tools.count;j++)
      if(d==&cog_workspace_tools.tools[j]) {
        uint32_t bit=strcmp(d->name,"read_file")==0 ? 1U : strcmp(d->name,"search")==0 ? 2U : strcmp(d->name,"edit_file")==0 ? 4U : 8U;
        if((r->config.workers->tool_mask&bit)==0U)child_bindings[i].policy=COG_PROMPT_TOOL_DENY;
      }
  }
  cog_instruction_selection_t selected = {
      .snapshot_id = 1U,
      .workspace_root_id = m.workspace_root_id,
      .scope = m.scope,
      .agent_name = agent == NULL ? m.agent_name : agent,
      .skill_names = selected_skills,
      .skill_count = selected_count,
      .model_id = m.model_id,
      .template_id = m.template_id,
      .template_revision = m.template_revision,
      .max_context_bytes = m.max_context_bytes,
      .max_output_tokens = m.max_output_tokens,
      .max_tool_rounds = m.max_tool_rounds,
      .bindings = child_bindings,
      .binding_count = parent_prompt.tool_count};
  sources = calloc(m.source_count, sizeof(*sources));
  if (sources == NULL) {
    free(child_bindings);
    free(selected_skills);
    cog_instruction_catalog_free(catalog);
    return COG_ERROR_NOMEM;
  }
  n = 0U;
  for (size_t i = 0U; i < m.source_count; i++)
    if (m.sources[i].kind == COG_INSTRUCTION_AGENTS ||
        m.sources[i].kind == COG_INSTRUCTION_RESOURCE)
      sources[n++] = m.sources[i];
  cog_instruction_snapshot_t *candidate=NULL;
  if (rc == COG_OK)
    rc = cog_instruction_snapshot_new(&candidate, catalog,
                                      &r->config.instruction_limits, &selected,
                                      sources, n, NULL);
  free(sources);
  free(selected_skills);
  free(child_bindings);
  cog_instruction_catalog_free(catalog);
  if(rc==COG_OK && r->child_snapshots[ordinal]!=NULL) {
    unsigned char prior[32],next[32];
    rc=cog_instruction_snapshot_fingerprint(r->child_snapshots[ordinal],prior);
    if(rc==COG_OK)rc=cog_instruction_snapshot_fingerprint(candidate,next);
    if(rc==COG_OK && memcmp(prior,next,sizeof(prior))!=0)rc=COG_ERROR_PROTOCOL;
    cog_instruction_snapshot_free(candidate);
    candidate=NULL;
  } else if(rc==COG_OK)r->child_snapshots[ordinal]=candidate;
  if (rc == COG_OK) {
    size_t len = strlen(call_id);
    if (len >= sizeof(r->child_call)) {
      cog_instruction_snapshot_free(r->child_snapshots[ordinal]);
      r->child_snapshots[ordinal] = NULL;
      return COG_ERROR_LIMIT;
    }
    memcpy(r->child_call, call_id, len + 1U);
    r->child_node = node_id;
  }
  return rc;
}

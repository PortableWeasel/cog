/** @file runtime_instructions.c @brief Local bounded instruction transitions.
 */
#include "codec_internal.h"
#include "execution_internal.h"
#include "runtime_internal.h"
#include "instruction_transfer.h"
#include <cog/runtime_instructions.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* Children borrow the coordinator's already-open roots; only the lease holder
 * may access reader state or its borrowed completed view. */
static cog_runtime_t *source_owner(cog_runtime_t *r) {
  while (r->source_owner != NULL) r = r->source_owner;
  return r;
}
typedef struct runtime_json {
  const unsigned char *input;
  size_t length, at;
  char *output;
  size_t capacity, used;
  bool valid;
} runtime_json_t;
static void json_space(runtime_json_t *p) {
  while (p->at < p->length &&
         (p->input[p->at] == ' ' || p->input[p->at] == '\n' ||
          p->input[p->at] == '\r' || p->input[p->at] == '\t'))
    ++p->at;
}
static bool json_token(runtime_json_t *p, unsigned char c) {
  json_space(p);
  if (p->at == p->length || p->input[p->at] != c) {
    p->valid = false;
    return false;
  }
  ++p->at;
  return true;
}
static bool json_emit(runtime_json_t *p, unsigned char c) {
  if (p->used >= p->capacity) {
    p->valid = false;
    return false;
  }
  p->output[p->used++] = (char)c;
  return true;
}
static unsigned hex_digit(unsigned char c) {
  if (c >= '0' && c <= '9')
    return (unsigned)(c - '0');
  if (c >= 'a' && c <= 'f')
    return (unsigned)(c - 'a') + 10U;
  if (c >= 'A' && c <= 'F')
    return (unsigned)(c - 'A') + 10U;
  return 16U;
}
static uint32_t json_hex(runtime_json_t *p) {
  uint32_t value = 0U;
  for (size_t i = 0U; i < 4U; ++i) {
    if (p->at == p->length) {
      p->valid = false;
      return 0U;
    }
    unsigned digit = hex_digit(p->input[p->at++]);
    if (digit == 16U) {
      p->valid = false;
      return 0U;
    }
    value = value * 16U + digit;
  }
  return value;
}
static const char *json_string(runtime_json_t *p, size_t *length) {
  if (!json_token(p, '"'))
    return NULL;
  size_t start = p->used;
  bool closed = false;
  while (p->valid && p->at < p->length) {
    unsigned char c = p->input[p->at++];
    if (c == '"') {
      closed = true;
      break;
    }
    if (c < 32U) {
      p->valid = false;
      break;
    }
    if (c != '\\') {
      (void)json_emit(p, c);
      continue;
    }
    if (p->at == p->length) {
      p->valid = false;
      break;
    }
    c = p->input[p->at++];
    if (c == '"' || c == '\\' || c == '/')
      (void)json_emit(p, c);
    else if (c == 'b')
      (void)json_emit(p, 8U);
    else if (c == 'f')
      (void)json_emit(p, 12U);
    else if (c == 'n')
      (void)json_emit(p, 10U);
    else if (c == 'r')
      (void)json_emit(p, 13U);
    else if (c == 't')
      (void)json_emit(p, 9U);
    else if (c == 'u') {
      uint32_t code = json_hex(p);
      if (code >= 0xd800U && code <= 0xdbffU) {
        if (p->at + 2U > p->length || p->input[p->at] != '\\' ||
            p->input[p->at + 1U] != 'u') {
          p->valid = false;
          break;
        }
        p->at += 2U;
        uint32_t low = json_hex(p);
        if (low < 0xdc00U || low > 0xdfffU) {
          p->valid = false;
          break;
        }
        code = 0x10000U + (code - 0xd800U) * 1024U + low - 0xdc00U;
      }
      if (code == 0U || (code >= 0xdc00U && code <= 0xdfffU)) {
        p->valid = false;
        break;
      }
      if (code < 0x80U)
        (void)json_emit(p, (unsigned char)code);
      else {
        unsigned width = code < 0x800U ? 2U : code < 0x10000U ? 3U : 4U;
        (void)json_emit(p, (unsigned char)((width == 2U   ? 0xc0U
                                            : width == 3U ? 0xe0U
                                                          : 0xf0U) |
                                           (code >> (6U * (width - 1U)))));
        for (unsigned i = width - 1U; i > 0U; --i)
          (void)json_emit(
              p, (unsigned char)(0x80U | ((code >> (6U * (i - 1U))) & 0x3fU)));
      }
    } else
      p->valid = false;
  }
  *length = p->used - start;
  if (!closed || !p->valid || !cli_wire_utf8(p->output + start, *length) ||
      !json_emit(p, 0U)) {
    p->valid = false;
    return NULL;
  }
  return p->output + start;
}
static bool key_is(const char *key, size_t length, const char *wanted) {
  return key != NULL && strlen(wanted) == length &&
         memcmp(key, wanted, length) == 0;
}

typedef enum job_kind { JOB_SCOPE, JOB_ACTIVATE, JOB_RESOURCE } job_kind_t;
struct runtime_instruction_job {
  cog_runtime_t *runtime;
  job_kind_t kind;
  bool started, reading, read_done, done, cancelled;
  cog_result_t outcome;
  char *strings;
  const char *skill, *path, *root;
  char output[128];
  size_t output_bytes;
  cog_instruction_snapshot_t *candidate;
  const cog_instruction_snapshot_t *base;
  uint64_t fresh_id;
  bool coordinator;
};
static bool skip_value(runtime_json_t *p, unsigned depth) {
  if (depth > 64U)
    return false;
  json_space(p);
  if (p->at == p->length)
    return false;
  if (p->input[p->at] == '"') {
    size_t n;
    return json_string(p, &n) != NULL;
  }
  if (p->input[p->at] == '{' || p->input[p->at] == '[') {
    unsigned char open = p->input[p->at++], close = open == '{' ? '}' : ']';
    json_space(p);
    if (p->at < p->length && p->input[p->at] == close) {
      p->at++;
      return true;
    }
    for (;;) {
      if (open == '{') {
        size_t n;
        if (json_string(p, &n) == NULL || !json_token(p, ':'))
          return false;
      }
      if (!skip_value(p, depth + 1U))
        return false;
      json_space(p);
      if (p->at < p->length && p->input[p->at] == close) {
        p->at++;
        return true;
      }
      if (!json_token(p, ','))
        return false;
    }
  }
  size_t begin = p->at;
  while (p->at < p->length &&
         strchr(",]} \r\n\t", (int)p->input[p->at]) == NULL)
    p->at++;
  return p->at > begin;
}
cog_result_t cli_runtime_json_field(const char *json, size_t bytes, const char *wanted,
                          char *storage, size_t capacity, const char **value) {
  runtime_json_t p = {
      (const unsigned char *)json, bytes, 0U, storage, capacity, 0U, true};
  *value = NULL;
  if (!json_token(&p, '{'))
    return COG_ERROR_INVALID_ARG;
  json_space(&p);
  if (p.at < p.length && p.input[p.at] == '}')
    return COG_OK;
  for (;;) {
    size_t n;
    const char *key = json_string(&p, &n);
    if (key == NULL || !json_token(&p, ':'))
      return COG_ERROR_INVALID_ARG;
    bool match = key_is(key, n, wanted);
    if (match) {
      if (*value != NULL)
        return COG_ERROR_PROTOCOL;
      *value = json_string(&p, &n);
      if (*value == NULL)
        return COG_ERROR_INVALID_ARG;
    } else if (!skip_value(&p, 0U))
      return COG_ERROR_INVALID_ARG;
    json_space(&p);
    if (p.at < p.length && p.input[p.at] == '}') {
      p.at++;
      break;
    }
    if (!json_token(&p, ','))
      return COG_ERROR_INVALID_ARG;
  }
  json_space(&p);
  return p.valid && p.at == p.length ? COG_OK : COG_ERROR_INVALID_ARG;
}
static bool path_valid(const char *path) {
  if (path == NULL || path[0] == '/')
    return false;
  const char *p = path;
  while (*p != '\0') {
    const char *slash = strchr(p, '/');
    size_t n = slash == NULL ? strlen(p) : (size_t)(slash - p);
    if (n == 0U || (n == 1U && p[0] == '.') ||
        (n == 2U && p[0] == '.' && p[1] == '.'))
      return false;
    if (slash == NULL)
      return true;
    p = slash + 1;
    if (*p == '\0')
      return false;
  }
  return true;
}
static void job_free(runtime_instruction_job_t *job) {
  if (job == NULL)
    return;
  cog_instruction_snapshot_free(job->candidate);
  if (job->strings != NULL) {
    volatile unsigned char *p = (unsigned char *)job->strings;
    for (size_t i = 0U; i < CLI_ARGUMENT_BYTES * 4U; i++)
      p[i] = 0U;
    free(job->strings);
  }
  free(job);
}
static cog_result_t control_new(cog_core_t *core, const char *request,
                                size_t bytes,
                                const cog_tool_async_config_t *config,
                                void **out, job_kind_t kind) {
  (void)core;
  if (out == NULL)
    return COG_ERROR_NULL_PTR;
  *out = NULL;
  if (config == NULL || config->user == NULL || request == NULL)
    return COG_ERROR_NULL_PTR;
  cog_runtime_t *r = config->user;
  if (r->instructions == NULL || r->instruction_job != NULL)
    return COG_ERROR_INVALID_STATE;
  runtime_instruction_job_t *j = calloc(1U, sizeof(*j));
  if (j == NULL)
    return COG_ERROR_NOMEM;
  j->strings = calloc(CLI_ARGUMENT_BYTES * 4U, 1U);
  if (j->strings == NULL) {
    free(j);
    return COG_ERROR_NOMEM;
  }
  j->runtime = r;
  j->kind = kind;
  const char *name = NULL;
  cog_result_t rc =
      cli_runtime_json_field(request, bytes, kind == JOB_ACTIVATE ? "name" : "skill", j->strings,
            CLI_ARGUMENT_BYTES, &name);
  if (rc == COG_OK && (name == NULL || *name == '\0'))
    rc = COG_ERROR_INVALID_ARG;
  j->skill = name;
  if (rc == COG_OK && kind == JOB_RESOURCE) {
    rc = cli_runtime_json_field(request, bytes, "path", j->strings + CLI_ARGUMENT_BYTES,
               CLI_ARGUMENT_BYTES, &j->path);
    if (rc == COG_OK && (!path_valid(j->path) || *j->path == '\0'))
      rc = COG_ERROR_DENIED;
  }
  if (rc != COG_OK) {
    job_free(j);
    return rc;
  }
  r->instruction_job = j;
  *out = j;
  return COG_OK;
}
static cog_result_t activate_new(cog_core_t *core, const char *request,
                                 size_t bytes,
                                 const cog_tool_async_config_t *config,
                                 void **out) {
  return control_new(core, request, bytes, config, out, JOB_ACTIVATE);
}
static cog_result_t resource_new(cog_core_t *core, const char *request,
                                 size_t bytes,
                                 const cog_tool_async_config_t *config,
                                 void **out) {
  return control_new(core, request, bytes, config, out, JOB_RESOURCE);
}
static cog_result_t control_start(void *handle, uint64_t now) {
  (void)now;
  runtime_instruction_job_t *j = handle;
  if (j == NULL)
    return COG_ERROR_NULL_PTR;
  if (j->started)
    return COG_ERROR_INVALID_STATE;
  j->started = true;
  return COG_OK;
}
static cog_result_t control_poll(void *handle, uint64_t now, bool *done,
                                 uint64_t *deadline) {
  (void)now;
  runtime_instruction_job_t *j = handle;
  if (j == NULL || done == NULL || deadline == NULL)
    return COG_ERROR_NULL_PTR;
  *done = j->done;
  *deadline = UINT64_MAX;
  return COG_OK;
}
static cog_result_t control_cancel(void *handle, uint64_t now) {
  (void)now;
  runtime_instruction_job_t *j = handle;
  if (j == NULL)
    return COG_ERROR_NULL_PTR;
  if (!j->done)
    j->cancelled = true;
  return COG_OK;
}
static cog_result_t control_result(const void *handle, cog_tool_output_t *out) {
  const runtime_instruction_job_t *j = handle;
  if (j == NULL || out == NULL)
    return COG_ERROR_NULL_PTR;
  if (!j->done)
    return COG_ERROR_AGAIN;
  *out = (cog_tool_output_t){
      .status = j->outcome, .output = j->output, .output_len = j->output_bytes};
  return COG_OK;
}
static cog_result_t control_destroy(void *handle) {
  runtime_instruction_job_t *j = handle;
  if (j == NULL)
    return COG_OK;
  if (j->started && !j->done)
    return COG_ERROR_INVALID_STATE;
  if (j->runtime->instruction_job == j)
    j->runtime->instruction_job = NULL;
  job_free(j);
  return COG_OK;
}
static const cog_tool_async_vtable_t activate_ops = {
    activate_new,   control_start,  control_poll,
    control_cancel, control_result, control_destroy};
static const cog_tool_async_vtable_t resource_ops = {
    resource_new,   control_start,  control_poll,
    control_cancel, control_result, control_destroy};
const cog_tool_t cog_runtime_activate_skill_tool = {
    "activate_skill",
    "Activate an eligible skill in the pinned catalog",
    "{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"}},"
    "\"required\":[\"name\"],\"additionalProperties\":false}",
    NULL,
    &activate_ops,
    COG_TOOL_SIDE_EFFECTING};
const cog_tool_t cog_runtime_read_skill_resource_tool = {
    "read_skill_resource",
    "Load one resource from an active skill",
    "{\"type\":\"object\",\"properties\":{\"skill\":{\"type\":\"string\"},"
    "\"path\":{\"type\":\"string\"}},\"required\":[\"skill\",\"path\"],"
    "\"additionalProperties\":false}",
    NULL,
    &resource_ops,
    COG_TOOL_SIDE_EFFECTING};
const cog_tool_table_t *
cli_runtime_instruction_table(const cog_tool_t *descriptor) {
  static const cog_tool_table_t activate = {&cog_runtime_activate_skill_tool,
                                            1U},
                                resource = {
                                    &cog_runtime_read_skill_resource_tool, 1U};
  return descriptor == &cog_runtime_activate_skill_tool        ? &activate
         : descriptor == &cog_runtime_read_skill_resource_tool ? &resource
                                                               : NULL;
}
static cog_result_t remote_saved(cog_runtime_t *r, cog_result_t status);
static cog_result_t deny_remaining(cog_runtime_t *r, size_t first) {
  cog_cli_app_t *a = r->app;
  for (size_t i = first; i < a->call_count; i++) {
    const cog_message_tool_call_t *call = &a->call_views[i];
    cog_tool_execution_state_t state;
    cog_result_t rc =
        cog_session_tool_state(a->session, a->assistant, call->id, &state);
    if (rc != COG_OK)
      return rc;
    if (state != COG_TOOL_EXECUTION_PROPOSED) {
      if (a->calls[i].resumed_closed)
        continue;
      return COG_ERROR_INVALID_STATE;
    }
    rc = cog_session_tool_transition(a->session, a->assistant, call->id, state,
                                     COG_TOOL_EXECUTION_DENIED);
    if (rc != COG_OK)
      return rc;
    static const char result[] = "Tool not executed: instruction context "
                                 "changed; submit a fresh proposal.";
    cog_message_t message = {.role = COG_MESSAGE_ROLE_TOOL,
                             .tool_call_id = call->id,
                             .name = call->name,
                             .content = result,
                             .content_len = sizeof(result) - 1U};
    rc = cog_session_append(a->session, &message, NULL);
    if (rc != COG_OK)
      return rc;
    a->calls[i].resumed_closed = true;
  }
  return COG_OK;
}
static cog_result_t rebuild(runtime_instruction_job_t *j) {
  cog_runtime_t *r = j->runtime;
  cog_instruction_manifest_t m;
  const cog_instruction_snapshot_t *base = j->base == NULL ? r->instructions : j->base;
  cog_result_t rc = cog_instruction_snapshot_view(base, &m);
  cog_prompt_bundle_t bundle;
  if (rc == COG_OK)
    rc = cog_instruction_prompt_view(base, &bundle);
  if (rc != COG_OK)
    return rc;
  const cog_instruction_source_item_t *items = NULL;
  size_t item_count = 0U;
  if (j->kind != JOB_ACTIVATE) {
    rc = cog_instruction_source_view(source_owner(r)->reader, &items, &item_count);
    if (rc != COG_OK)
      return rc;
  }
  if (m.source_count == SIZE_MAX || item_count > SIZE_MAX - m.source_count - 1U)
    return COG_ERROR_LIMIT;
  size_t capacity = m.source_count + item_count + 1U;
  cog_instruction_source_t *catalog_sources =
                               calloc(m.source_count == 0U ? 1U : m.source_count, sizeof(*catalog_sources)),
                           *additions = calloc(capacity, sizeof(*additions));
  const char **skills = calloc(m.active_skill_count + 1U, sizeof(*skills));
  if (catalog_sources == NULL || additions == NULL || skills == NULL) {
    free(catalog_sources);
    free(additions);
    free(skills);
    return COG_ERROR_NOMEM;
  }
  size_t catalogs = 0U, added = 0U, skill_count = m.active_skill_count;
  for (size_t i = 0U; i < skill_count; i++)
    skills[i] = m.active_skills[i];
  bool already = false;
  if (j->kind == JOB_ACTIVATE) {
    for (size_t i = 0U; i < skill_count; i++)
      if (strcmp(skills[i], j->skill) == 0)
        already = true;
    if (!already)
      skills[skill_count++] = j->skill;
  }
  for (size_t i = 0U; i < m.source_count; i++) {
    const cog_instruction_source_t *s = &m.sources[i];
    if (s->kind == COG_INSTRUCTION_HOST || s->kind == COG_INSTRUCTION_AGENT ||
        s->kind == COG_INSTRUCTION_SKILL)
      catalog_sources[catalogs++] = *s;
    else if (s->kind == COG_INSTRUCTION_AGENTS) {
      if (j->kind != JOB_SCOPE)
        additions[added++] = *s;
    } else if (!(j->kind == JOB_RESOURCE && strcmp(s->root_id, j->root) == 0 &&
                 strcmp(s->path, j->path) == 0))
      additions[added++] = *s;
  }
  size_t before_read = added;
  for (size_t i = 0U; i < item_count; i++)
    if (items[i].present)
      additions[added++] = (cog_instruction_source_t){
          .kind = j->kind == JOB_SCOPE ? COG_INSTRUCTION_AGENTS
                                       : COG_INSTRUCTION_RESOURCE,
          .root_id = items[i].root_id,
          .path = items[i].path,
          .scope = j->kind == JOB_SCOPE ? items[i].path : "",
          .owner_skill = j->kind == JOB_SCOPE ? NULL : j->skill,
          .text = items[i].text,
          .text_bytes = items[i].text_bytes};
  /* Scope labels are directories, not AGENTS.md filenames. */
  char **scope_copies =
      calloc(item_count == 0U ? 1U : item_count, sizeof(*scope_copies));
  if (scope_copies == NULL) {
    rc = COG_ERROR_NOMEM;
    goto end;
  }
  if (j->kind == JOB_SCOPE)
    for (size_t i = before_read; i < added; i++) {
      const char *path = additions[i].path;
      const char *slash = strrchr(path, '/');
      size_t len = slash == NULL ? 0U : (size_t)(slash - path);
      scope_copies[i - before_read] = malloc(len + 1U);
      if (scope_copies[i - before_read] == NULL) {
        rc = COG_ERROR_NOMEM;
        goto scopes_end;
      }
      memcpy(scope_copies[i - before_read], path, len);
      scope_copies[i - before_read][len] = '\0';
      additions[i].scope = scope_copies[i - before_read];
    }
  if (j->fresh_id == 0U && r->snapshot_high_water == UINT64_MAX) {
    rc = COG_ERROR_LIMIT;
    goto scopes_end;
  }
  cog_instruction_catalog_t *catalog = NULL;
  rc = cog_instruction_catalog_new(&catalog, &r->config.instruction_limits,
                                   m.catalog_generation, catalog_sources,
                                   catalogs, NULL);
  cog_instruction_selection_t selection = {
      .snapshot_id = j->fresh_id == 0U ? r->snapshot_high_water + 1U : j->fresh_id,
      .workspace_root_id = m.workspace_root_id,
      .scope = j->kind == JOB_SCOPE ? j->path : m.scope,
      .agent_name = m.agent_name,
      .skill_names = skills,
      .skill_count = skill_count,
      .model_id = m.model_id,
      .template_id = m.template_id,
      .template_revision = m.template_revision,
      .max_context_bytes = m.max_context_bytes,
      .max_output_tokens = m.max_output_tokens,
      .max_tool_rounds = m.max_tool_rounds,
      .bindings = bundle.tools,
      .binding_count = bundle.tool_count};
  if (rc == COG_OK)
    rc = cog_instruction_snapshot_new(&j->candidate, catalog,
                                      &r->config.instruction_limits, &selection,
                                      additions, added, NULL);
  cog_instruction_catalog_free(catalog);
  if (rc == COG_OK) {
    cog_instruction_manifest_t candidate;
    rc = cog_instruction_snapshot_view(j->candidate, &candidate);
    bool equal = rc == COG_OK && strcmp(candidate.scope, m.scope) == 0 &&
                 candidate.source_count == m.source_count &&
                 candidate.active_skill_count == m.active_skill_count;
    for (size_t i = 0U; equal && i < m.active_skill_count; i++)
      equal = strcmp(candidate.active_skills[i], m.active_skills[i]) == 0;
    for (size_t i = 0U; equal && i < m.source_count; i++)
      equal = strcmp(candidate.sources[i].root_id, m.sources[i].root_id) == 0 &&
              strcmp(candidate.sources[i].path, m.sources[i].path) == 0 &&
              candidate.sources[i].text_bytes == m.sources[i].text_bytes &&
              memcmp(candidate.sources[i].text, m.sources[i].text,
                     m.sources[i].text_bytes) == 0;
    if (equal) {
      cog_instruction_snapshot_free(j->candidate);
      j->candidate = NULL;
    }
  }
scopes_end:
  for (size_t i = 0U; i < item_count; i++)
    free(scope_copies[i]);
  free(scope_copies);
end:
  free(catalog_sources);
  free(additions);
  free(skills);
  return rc;
}
static cog_result_t start_read(runtime_instruction_job_t *j) {
  cog_runtime_t *r = j->runtime;
  if (source_owner(r)->reader == NULL)
    return COG_ERROR_DENIED;
  cog_instruction_source_request_t request = {
      .operation = j->kind == JOB_SCOPE ? COG_INSTRUCTION_SOURCE_SCOPE
                                        : COG_INSTRUCTION_SOURCE_READ,
      .root_id = j->root,
      .path = j->path};
  cog_result_t rc = cog_instruction_source_begin(source_owner(r)->reader, &request);
  if (rc == COG_OK)
    j->reading = true;
  return rc;
}
static cog_result_t resolve_step_owned(runtime_instruction_job_t *j) {
  cog_runtime_t *r = j->runtime;
  cog_result_t rc = COG_OK;
  if (j->cancelled || r->shutdown || (!j->coordinator && r->app->cancelling)) {
    if (j->reading && source_owner(r)->source_lease == j)
      (void)cog_instruction_source_cancel(source_owner(r)->reader);
    rc = COG_ERROR_ABORTED;
  }
  if (rc == COG_OK && j->kind == JOB_RESOURCE && !j->reading) {
    cog_instruction_manifest_t m;
    rc = cog_instruction_snapshot_view(j->base == NULL ? r->instructions : j->base, &m);
    bool active = false;
    if (rc == COG_OK)
      for (size_t i = 0U; i < m.active_skill_count; i++)
        if (strcmp(m.active_skills[i], j->skill) == 0)
          active = true;
    if (rc == COG_OK && !active)
      rc = COG_ERROR_DENIED;
    const cog_instruction_entry_t *entry = NULL;
    if (rc == COG_OK)
      for (size_t i = 0U; i < m.available_skill_count; i++)
        if (strcmp(m.available_skills[i].name, j->skill) == 0)
          entry = &m.available_skills[i];
    if (rc == COG_OK && entry == NULL)
      rc = COG_ERROR_NOT_FOUND;
    if (rc == COG_OK && j->root != NULL && strcmp(j->root, entry->root_id) != 0)
      rc = COG_ERROR_DENIED;
    if (rc == COG_OK) {
      const char *slash = strrchr(entry->path, '/');
      size_t prefix = slash == NULL ? 0U : (size_t)(slash - entry->path) + 1U;
      size_t length = strlen(j->path);
      if (prefix + length >= CLI_ARGUMENT_BYTES)
        rc = COG_ERROR_LIMIT;
      else {
        char *joined = j->strings + CLI_ARGUMENT_BYTES * 2U;
        memcpy(joined, entry->path, prefix);
        memcpy(joined + prefix, j->path, length + 1U);
        j->path = joined;
        j->root = entry->root_id;
      }
    }
  }
  if (rc == COG_OK && j->kind != JOB_ACTIVATE && !j->reading) {
    rc = start_read(j);
    if (rc == COG_OK)
      return COG_ERROR_AGAIN;
  }
  if (rc == COG_OK && j->kind != JOB_ACTIVATE && !j->read_done) {
    rc = cog_instruction_source_poll(source_owner(r)->reader, &j->read_done);
    if (rc == COG_ERROR_AGAIN)
      return COG_ERROR_AGAIN;
    if (rc == COG_OK && !j->read_done)
      return COG_ERROR_AGAIN;
  }
  if (rc == COG_OK)
    rc = rebuild(j);
  return rc;
}
static cog_result_t resolve_step(runtime_instruction_job_t *j) {
  cog_runtime_t *r = j->runtime, *owner = source_owner(r);
  bool cancelled = j->cancelled || r->shutdown ||
      (!j->coordinator && r->app->cancelling);
  if (owner->source_lease != NULL && owner->source_lease != j)
    return cancelled ? COG_ERROR_ABORTED : COG_ERROR_AGAIN;
  owner->source_lease = j;
  cog_result_t rc = resolve_step_owned(j);
  if (rc != COG_ERROR_AGAIN) owner->source_lease = NULL;
  return rc;
}
cog_result_t cli_runtime_instruction_progress(cog_runtime_t *r) {
  runtime_instruction_job_t *j = r->instruction_job;
  if (j == NULL || !j->started || j->done)
    return COG_OK;
  if (r->resolver_job != NULL || r->remote_instructions)
    return COG_OK;
  cog_result_t rc = resolve_step(j);
  if (rc == COG_ERROR_AGAIN)
    return COG_OK;
  if (rc == COG_OK && j->candidate != NULL && r->background_id != 0U && j->kind == JOB_SCOPE) {
    r->scope_refused = true;
    r->scope_refusal = COG_ERROR_DENIED;
    r->instruction_job = NULL;
    cog_instruction_snapshot_free(j->candidate);
    j->candidate = NULL;
    return COG_OK;
  }
  if (rc == COG_OK && j->candidate != NULL)
    rc = cog_runtime_instruction_prepare(&r->control_change, r, j->candidate);
  if (j->kind == JOB_SCOPE) {
    if (rc == COG_OK && r->control_change != NULL) {
      rc = deny_remaining(r, r->app->tool_index);
      if (rc == COG_OK)
        rc = cli_runtime_control_saved(r->app);
      if (rc == COG_OK) {
        r->scope_replaced = true;
        r->scope_allowed = false;
      }
    } else if (rc == COG_OK)
      r->scope_allowed = true;
    if (rc != COG_OK) {
      r->app->recovery = true;
      r->primary = rc;
    }
    r->instruction_job = NULL;
    cog_instruction_snapshot_free(j->candidate);
    j->candidate = NULL;
    return rc;
  }
  j->outcome = rc;
  j->done = true;
  int n = snprintf(j->output, sizeof(j->output),
                   "Instruction control status=%d", (int)rc);
  if (n < 0 || (size_t)n >= sizeof(j->output)) {
    j->outcome = COG_ERROR_LIMIT;
    j->output[0] = '\0';
    j->output_bytes = 0U;
  } else
    j->output_bytes = (size_t)n;
  return COG_OK;
}
static cog_result_t scope_target(cog_runtime_t *r,const cog_tool_t *descriptor,
    const cog_message_tool_call_t *call,char *target,size_t capacity) {
  bool found = false;
  for (size_t i = 0U; i < r->config.scope_count; i++)
    if (r->config.scopes[i].descriptor == descriptor) {
      const cog_runtime_scope_binding_t *binding = &r->config.scopes[i];
      const char *path = binding->default_scope;
      if (binding->path_key != NULL) {
        cog_result_t rc =
            cli_runtime_json_field(call->arguments, call->arguments_len, binding->path_key,
                  r->json_storage, CLI_ARGUMENT_BYTES * 3U, &path);
        if (rc != COG_OK)
          return rc;
        if (path == NULL)
          path = binding->default_scope;
      }
      if (!path_valid(path))
        return COG_ERROR_DENIED;
      size_t len = strlen(path);
      if (binding->file_target) {
        const char *slash = strrchr(path, '/');
        len = slash == NULL ? 0U : (size_t)(slash - path);
      }
      if (len >= capacity)
        return COG_ERROR_LIMIT;
      if (found && (strlen(target) != len || memcmp(target, path, len) != 0))
        return COG_ERROR_UNSUPPORTED;
      memcpy(target, path, len);
      target[len] = '\0';
      found = true;
    }
  return found ? COG_OK : COG_ERROR_DENIED;
}
cog_result_t cli_runtime_scope(cog_cli_app_t *app,
                               const cog_message_tool_call_t *call) {
  cog_runtime_t *r = app->runtime_owner;
  if (r == NULL || r->instructions == NULL)
    return COG_OK;
  const cog_tool_t *descriptor = NULL;
  for (size_t i = 0U; i < r->config.tools.binding_count; i++)
    if (strcmp(r->bindings[i].descriptor->name, call->name) == 0)
      descriptor = r->bindings[i].descriptor;
  if (descriptor == NULL)
    return COG_ERROR_DENIED;
  if (descriptor == &cog_runtime_background_status_tool || cli_runtime_instruction_table(descriptor) != NULL)
    return COG_OK;
  for (size_t i = 0U; i < cog_runtime_delegation_tools.count; i++)
    if (descriptor == &cog_runtime_delegation_tools.tools[i])
      return COG_OK;
  uint64_t node = cog_node_data(app->assistant)->node_id;
  if (r->scope_refused && r->scope_node == node && r->scope_ordinal == app->tool_index)
    return r->scope_refusal;
  if (r->scope_allowed && r->scope_node == node &&
      r->scope_ordinal == app->tool_index)
    return COG_OK;
  if (r->instruction_job != NULL || r->resolver_job != NULL)
    return COG_ERROR_AGAIN;
  char target[CLI_ARGUMENT_BYTES];
  cog_result_t scope_rc=scope_target(r,descriptor,call,target,sizeof(target));
  if(scope_rc!=COG_OK)return scope_rc;
  if(r->config.workspace_root_id==NULL || (source_owner(r)->reader==NULL && !r->remote_instructions))
    return COG_ERROR_DENIED;
  runtime_instruction_job_t *j = r->scope_job;
  char *strings = j->strings;
  memset(j, 0, sizeof(*j));
  j->strings = strings;
  strcpy(j->strings, target);
  j->runtime = r;
  j->kind = JOB_SCOPE;
  j->path = j->strings;
  j->root = r->config.workspace_root_id;
  j->started = true;
  r->instruction_job = j;
  r->scope_node = node;
  r->scope_ordinal = app->tool_index;
  r->scope_allowed = false;
  r->scope_refused = false;
  return COG_ERROR_AGAIN;
}
cog_result_t cli_runtime_control_saved(cog_cli_app_t *app) {
  cog_runtime_t *r = app->runtime_owner;
  if (r == NULL || r->control_change == NULL)
    return COG_OK;
  cog_result_t rc = deny_remaining(r, app->tool_index + 1U);
  if (rc != COG_OK)
    return remote_saved(r, rc);
  /* Current result and remaining denials are already in memory. The paired
     checkpoint hook installs the new binding before the ordinary save. */
  cog_session_snapshot_t state;
  rc = cog_session_snapshot_view(app->session, &state);
  if (rc != COG_OK)
    return remote_saved(r, rc);
  if (state.context_high_water == UINT64_MAX ||
      state.context_binding_high_water == UINT64_MAX)
    return remote_saved(r, COG_ERROR_LIMIT);
  cog_runtime_instruction_change_t *change = r->control_change;
  cog_session_context_t context = {
      state.context_high_water + 1U, COG_INSTRUCTION_CONTEXT_NAMESPACE,
      COG_INSTRUCTION_CONTEXT_FORMAT, change->encoded, change->encoded_bytes};
  cog_session_context_binding_t binding = {
      state.context_binding_high_water + 1U, state.selected_context_binding_id,
      state.active_node_id, context.context_id};
  rc = cog_session_context_add(app->session, &context);
  if (rc == COG_OK) {
    rc = cog_session_context_publish(app->session, &binding);
  }
  if (rc == COG_OK) {
    rc = cog_cli_save(app);
  }
  if (rc != COG_OK) {
    app->recovery = true;
    return remote_saved(r, rc);
  }
  rc = cli_runtime_cache_publish(r, &change->snapshot);
  if (rc != COG_OK) {
    app->recovery = true;
    return remote_saved(r, rc);
  }
  rc = cog_runtime_instruction_change_free(&r->control_change);
  return remote_saved(r, rc);
}
cog_result_t cli_runtime_instruction_dispose(cog_runtime_t *r) {
  if (r->resolver_job != NULL)
    return COG_ERROR_INVALID_STATE;
  if (r->instruction_job != NULL) {
    runtime_instruction_job_t *j = r->instruction_job;
    if (j->kind != JOB_SCOPE)
      return COG_ERROR_INVALID_STATE;
    if (j->reading && source_owner(r)->source_lease == j)
      (void)cog_instruction_source_cancel(source_owner(r)->reader);
    if (source_owner(r)->source_lease == j) source_owner(r)->source_lease = NULL;
    r->instruction_job = NULL;
  }
  job_free(r->scope_job);
  r->scope_job = NULL;
  return cog_runtime_instruction_change_free(&r->control_change);
}

cog_result_t cli_runtime_instruction_init(cog_runtime_t *r) {
  r->scope_job = calloc(1U, sizeof(*r->scope_job));
  if (r->scope_job == NULL)
    return COG_ERROR_NOMEM;
  r->scope_job->strings = calloc(CLI_ARGUMENT_BYTES * 4U, 1U);
  if (r->scope_job->strings == NULL)
    return COG_ERROR_NOMEM;
  return COG_OK;
}

cog_result_t cli_runtime_resolver_new(runtime_instruction_job_t **out,
    cog_runtime_t *r, const cog_instruction_snapshot_t *base,
    unsigned operation, const char *root, const char *skill, const char *path,
    uint64_t fresh_id) {
  if (out == NULL) return COG_ERROR_NULL_PTR;
  *out = NULL;
  if (r == NULL || base == NULL || root == NULL || skill == NULL || path == NULL)
    return COG_ERROR_NULL_PTR;
  if (r->resolver_job != NULL || r->instruction_job != NULL)
    return COG_ERROR_AGAIN;
  if (operation < COG_CLI_INSTRUCTION_RESOURCE_LOAD ||
      operation > COG_CLI_INSTRUCTION_RESOURCE_SCOPE || *root == '\0' ||
      strlen(root) >= CLI_ARGUMENT_BYTES || strlen(skill) >= CLI_ARGUMENT_BYTES ||
      strlen(path) >= CLI_ARGUMENT_BYTES || !path_valid(path))
    return COG_ERROR_INVALID_ARG;
  bool scope = operation == COG_CLI_INSTRUCTION_RESOURCE_SCOPE;
  bool activate = operation == COG_CLI_INSTRUCTION_RESOURCE_ACTIVATE;
  if ((scope && *skill != '\0') || (!scope && *skill == '\0') ||
      (activate && *path != '\0') || (!scope && !activate && *path == '\0'))
    return COG_ERROR_INVALID_ARG;
  cog_instruction_manifest_t m;
  cog_prompt_bundle_t bundle;
  cog_result_t rc = cog_instruction_snapshot_view(base, &m);
  if (rc == COG_OK) rc = cog_instruction_prompt_view(base, &bundle);
  if (rc != COG_OK) return rc;
  if (fresh_id <= m.snapshot_id) return COG_ERROR_INVALID_ARG;
  if (scope && strcmp(root, m.workspace_root_id) != 0)
    return COG_ERROR_DENIED;
  if (!scope) {
    const cog_instruction_entry_t *entry = NULL;
    for (size_t i = 0U; i < m.available_skill_count; i++)
      if (strcmp(m.available_skills[i].name, skill) == 0)
        entry = &m.available_skills[i];
    if (entry == NULL) return COG_ERROR_NOT_FOUND;
    if (strcmp(entry->root_id, root) != 0) return COG_ERROR_DENIED;
  }
  runtime_instruction_job_t *j = calloc(1U, sizeof(*j));
  if (j == NULL) return COG_ERROR_NOMEM;
  j->strings = calloc(CLI_ARGUMENT_BYTES * 4U, 1U);
  if (j->strings == NULL) { free(j); return COG_ERROR_NOMEM; }
  strcpy(j->strings, skill);
  strcpy(j->strings + CLI_ARGUMENT_BYTES, path);
  strcpy(j->strings + CLI_ARGUMENT_BYTES * 3U, root);
  j->skill = j->strings;
  j->path = j->strings + CLI_ARGUMENT_BYTES;
  j->root = j->strings + CLI_ARGUMENT_BYTES * 3U;
  j->runtime = r; j->base = base; j->fresh_id = fresh_id;
  j->coordinator = true; j->started = true;
  j->kind = scope ? JOB_SCOPE : activate ? JOB_ACTIVATE : JOB_RESOURCE;
  r->resolver_job = j;
  *out = j;
  return COG_OK;
}
cog_result_t cli_runtime_resolver_step(runtime_instruction_job_t *j, bool *done) {
  if (j == NULL || done == NULL) return COG_ERROR_NULL_PTR;
  if (!j->coordinator || j->runtime->resolver_job != j)
    return COG_ERROR_INVALID_STATE;
  if (!j->done) {
    cog_result_t rc = resolve_step(j);
    if (rc != COG_ERROR_AGAIN) { j->done = true; j->outcome = rc; }
  }
  *done = j->done;
  return j->done ? j->outcome : COG_ERROR_AGAIN;
}
cog_result_t cli_runtime_resolver_take(runtime_instruction_job_t *j,
    cog_instruction_snapshot_t **candidate) {
  if (j == NULL || candidate == NULL) return COG_ERROR_NULL_PTR;
  if (!j->coordinator || j->runtime->resolver_job != j || !j->done)
    return COG_ERROR_INVALID_STATE;
  if (j->outcome != COG_OK) return j->outcome;
  *candidate = j->candidate;
  j->candidate = NULL;
  return COG_OK;
}
cog_result_t cli_runtime_resolver_free(runtime_instruction_job_t **owner) {
  if (owner == NULL) return COG_ERROR_NULL_PTR;
  runtime_instruction_job_t *j = *owner;
  if (j == NULL) return COG_OK;
  if (!j->coordinator || j->runtime->resolver_job != j)
    return COG_ERROR_INVALID_STATE;
  cog_result_t rc = COG_OK;
  if (j->reading && !j->read_done && source_owner(j->runtime)->source_lease == j) {
    rc = cog_instruction_source_cancel(source_owner(j->runtime)->reader);
    if (rc != COG_OK && rc != COG_ERROR_IO) return rc;
  }
  if (source_owner(j->runtime)->source_lease == j)
    source_owner(j->runtime)->source_lease = NULL;
  j->runtime->resolver_job = NULL;
  job_free(j);
  *owner = NULL;
  return rc;
}

static bool remote_identity(const cog_cli_instruction_identity_t *a,
    const cog_cli_instruction_identity_t *b) {
  return a->catalog_generation==b->catalog_generation && a->snapshot_id==b->snapshot_id &&
      memcmp(a->sha256,b->sha256,32U)==0;
}
static cog_result_t remote_saved(cog_runtime_t *r,cog_result_t status) {
  if(!r->remote_active)return status;
  r->remote_finish_control=false;
  r->remote_outcome=COG_CLI_INSTRUCTION_COMMIT_UNCERTAIN;
  r->remote_checkpoint=0U;
  if(status==COG_OK) {
    cog_store_owner_t owner;
    cog_store_checkpoint_t checkpoint;
    status=cog_store_owner_read(r->app->store,cog_session_id(r->app->session),&owner,&checkpoint);
    if(status==COG_OK && (owner.epoch!=r->app->headless->expected_owner.epoch ||
        owner.instance_id!=r->app->headless->expected_owner.instance_id))status=COG_ERROR_DENIED;
    if(status==COG_OK && checkpoint.generation==0U)status=COG_ERROR_PROTOCOL;
    if(status==COG_OK) {
      r->remote_outcome=COG_CLI_INSTRUCTION_COMMIT_SAVED;
      r->remote_checkpoint=checkpoint.generation;
    }
  }
  r->remote_status=status;
  if(status!=COG_OK)r->app->recovery=true;
  return status;
}
bool cli_runtime_remote_hold(const cog_runtime_t *r) {
  return r!=NULL && r->remote_active && !r->remote_finish_control;
}
cog_result_t cli_runtime_remote_request(cog_runtime_t *r,uint64_t checkpoint,
    cog_cli_instruction_resource_t *request) {
  if(r==NULL || request==NULL)return COG_ERROR_NULL_PTR;
  runtime_instruction_job_t *j=r->instruction_job;
  if(!r->remote_instructions || r->remote_active || j==NULL || !j->started || j->done)
    return COG_ERROR_AGAIN;
  if(checkpoint==0U || r->app->assistant==NULL || r->app->tool_index>=r->app->call_count)
    return COG_ERROR_INVALID_STATE;
  cog_instruction_manifest_t m;
  cog_result_t rc=cog_instruction_snapshot_view(r->instructions,&m);
  if(rc!=COG_OK)return rc;
  const char *root=j->root;
  if(j->kind!=JOB_SCOPE) {
    const cog_instruction_entry_t *entry=NULL;
    for(size_t i=0U;i<m.available_skill_count;i++)
      if(strcmp(m.available_skills[i].name,j->skill)==0)entry=&m.available_skills[i];
    if(entry==NULL) {
      j->done=true;j->outcome=COG_ERROR_NOT_FOUND;j->output_bytes=0U;
      return COG_ERROR_AGAIN;
    }
    root=entry->root_id;
  }
  cog_cli_instruction_resource_t body={.kind=COG_CLI_INSTRUCTION_RESOURCE_REQUEST,
      .context={.catalog_generation=m.catalog_generation,.snapshot_id=m.snapshot_id},
      .operation=j->kind==JOB_SCOPE?COG_CLI_INSTRUCTION_RESOURCE_SCOPE:
          j->kind==JOB_ACTIVATE?COG_CLI_INSTRUCTION_RESOURCE_ACTIVATE:COG_CLI_INSTRUCTION_RESOURCE_LOAD,
      .root_id=root,.skill_name=j->kind==JOB_SCOPE?"":j->skill,
      .path=j->kind==JOB_ACTIVATE?"":j->path,
      .assistant_node_id=cog_node_data(r->app->assistant)->node_id,
      .call_id=r->app->call_views[r->app->tool_index].id,
      .call_ordinal=(uint16_t)r->app->tool_index,.request_checkpoint_generation=checkpoint};
  rc=cog_instruction_snapshot_fingerprint(r->instructions,body.context.sha256);
  if(rc!=COG_OK)return rc;
  r->remote_active=true;r->remote_finish_control=false;
  r->remote_outcome=COG_CLI_INSTRUCTION_COMMIT_NONE;r->remote_status=COG_OK;
  r->remote_checkpoint=0U;*request=body;return COG_OK;
}
static bool same_source(const cog_instruction_source_t *a,const cog_instruction_source_t *b) {
  return a->kind==b->kind && a->precedence==b->precedence &&
      strcmp(a->root_id,b->root_id)==0 && strcmp(a->path,b->path)==0 &&
      strcmp(a->scope,b->scope)==0 &&
      ((a->owner_skill==NULL && b->owner_skill==NULL) ||
       (a->owner_skill!=NULL && b->owner_skill!=NULL && strcmp(a->owner_skill,b->owner_skill)==0)) &&
      a->text_bytes==b->text_bytes && memcmp(a->text,b->text,a->text_bytes)==0;
}
static bool delta_mutable(const runtime_instruction_job_t *j,const cog_instruction_source_t *s,
    const cog_instruction_manifest_t *old) {
  if(j->kind==JOB_SCOPE)return s->kind==COG_INSTRUCTION_AGENTS;
  if(j->kind!=JOB_RESOURCE || s->kind!=COG_INSTRUCTION_RESOURCE ||
      s->owner_skill==NULL || strcmp(s->owner_skill,j->skill)!=0)return false;
  for(size_t i=0U;i<old->available_skill_count;i++) {
    const cog_instruction_entry_t *entry=&old->available_skills[i];
    if(strcmp(entry->name,j->skill)!=0 || strcmp(entry->root_id,s->root_id)!=0)continue;
    const char *slash=strrchr(entry->path,'/');
    size_t prefix=slash==NULL?0U:(size_t)(slash-entry->path)+1U;
    return strlen(s->path)==prefix+strlen(j->path) &&
        memcmp(s->path,entry->path,prefix)==0 && strcmp(s->path+prefix,j->path)==0;
  }
  return false;
}
static bool nullable_equal(const char *a,const char *b) {
  return a==NULL || b==NULL ? a==b : strcmp(a,b)==0;
}
static bool remote_delta(const runtime_instruction_job_t *j,
    const cog_instruction_manifest_t *a,const cog_instruction_manifest_t *b) {
  if(a->catalog_generation!=b->catalog_generation || strcmp(a->workspace_root_id,b->workspace_root_id)!=0 ||
      !nullable_equal(a->agent_name,b->agent_name) || strcmp(a->model_id,b->model_id)!=0 ||
      strcmp(a->template_id,b->template_id)!=0 || a->template_revision!=b->template_revision ||
      a->max_context_bytes!=b->max_context_bytes || a->max_output_tokens!=b->max_output_tokens ||
      a->max_tool_rounds!=b->max_tool_rounds || a->tool_count!=b->tool_count ||
      strcmp(b->scope,j->kind==JOB_SCOPE?j->path:a->scope)!=0)return false;
  if(j->kind==JOB_ACTIVATE) {
    bool active=false;
    for(size_t i=0U;i<b->active_skill_count;i++)if(strcmp(b->active_skills[i],j->skill)==0)active=true;
    if(!active)return false;
  }
  if(j->kind==JOB_RESOURCE) {
    bool present=false;
    for(size_t i=0U;i<b->source_count;i++)if(delta_mutable(j,&b->sources[i],a))present=true;
    if(!present)return false;
  }
  if(j->kind==JOB_SCOPE)for(size_t i=0U;i<b->source_count;i++) {
    const cog_instruction_source_t *source=&b->sources[i];
    if(source->kind!=COG_INSTRUCTION_AGENTS)continue;
    size_t n=strlen(source->scope),target=strlen(j->path);
    if(strcmp(source->root_id,a->workspace_root_id)!=0 || n>target ||
        memcmp(source->scope,j->path,n)!=0 ||
        (n!=0U && n<target && j->path[n]!='/'))return false;
  }
  for(size_t i=0U;i<a->tool_count;i++)if(strcmp(a->tools[i].name,b->tools[i].name)!=0 ||
      a->tools[i].policy!=b->tools[i].policy)return false;
  for(size_t side=0U;side<2U;side++) {
    const cog_instruction_manifest_t *from=side==0U?a:b,*to=side==0U?b:a;
    for(size_t i=0U;i<from->source_count;i++) {
      if(delta_mutable(j,&from->sources[i],a))continue;
      bool found=false;
      for(size_t k=0U;k<to->source_count;k++)if(same_source(&from->sources[i],&to->sources[k]))found=true;
      if(!found)return false;
    }
    for(size_t i=0U;i<from->active_skill_count;i++) {
      if(j->kind==JOB_ACTIVATE && side==1U && strcmp(from->active_skills[i],j->skill)==0)continue;
      bool found=false;
      for(size_t k=0U;k<to->active_skill_count;k++)if(strcmp(from->active_skills[i],to->active_skills[k])==0)found=true;
      if(!found)return false;
    }
  }
  return true;
}
static void remote_complete(runtime_instruction_job_t *j,cog_result_t status) {
  j->done=true;j->outcome=status;
  int n=snprintf(j->output,sizeof(j->output),"Instruction control status=%d",(int)status);
  j->output_bytes=n<0 || (size_t)n>=sizeof(j->output)?0U:(size_t)n;
}
cog_result_t cli_runtime_remote_result(cog_runtime_t *r,const cog_cli_instruction_resource_t *result) {
  if(r==NULL || result==NULL)return COG_ERROR_NULL_PTR;
  runtime_instruction_job_t *j=r->instruction_job;
  if(!r->remote_active || j==NULL || r->remote_outcome!=COG_CLI_INSTRUCTION_COMMIT_NONE ||
      result->kind!=COG_CLI_INSTRUCTION_RESOURCE_RESULT)return COG_ERROR_INVALID_STATE;
  cog_instruction_manifest_t old={0};
  cog_result_t rc=cog_instruction_snapshot_view(r->instructions,&old);
  cog_cli_instruction_identity_t identity={.catalog_generation=old.catalog_generation,.snapshot_id=old.snapshot_id};
  if(rc==COG_OK)rc=cog_instruction_snapshot_fingerprint(r->instructions,identity.sha256);
  if(rc==COG_OK && !remote_identity(&identity,&result->context))rc=COG_ERROR_PROTOCOL;
  if(rc!=COG_OK)return rc;
  r->remote_status=result->result;
  if(result->result!=COG_OK || j->cancelled || r->shutdown || r->app->cancelling) {
    if(result->result==COG_OK)r->remote_status=COG_ERROR_ABORTED;
    r->remote_outcome=COG_CLI_INSTRUCTION_COMMIT_REFUSED;return COG_OK;
  }
  cog_instruction_snapshot_t *candidate=NULL;
  rc=cog_instruction_snapshot_decode(&candidate,&r->config.instruction_limits,
      result->candidate.bytes,result->candidate.length,COG_INSTRUCTION_EXECUTE,
      r->bindings,r->config.tools.binding_count);
  cog_instruction_manifest_t m;
  if(rc==COG_OK)rc=cog_instruction_snapshot_view(candidate,&m);
  unsigned char digest[32];
  if(rc==COG_OK)rc=cog_instruction_snapshot_fingerprint(candidate,digest);
  if(rc==COG_OK && memcmp(digest,result->candidate.identity.sha256,32U)!=0)rc=COG_ERROR_PROTOCOL;
  if(rc==COG_OK && (m.catalog_generation!=result->candidate.identity.catalog_generation ||
      m.snapshot_id!=result->candidate.identity.snapshot_id || !remote_delta(j,&old,&m)))rc=COG_ERROR_PROTOCOL;
  if(rc==COG_OK && remote_identity(&identity,&result->candidate.identity)) {
    r->remote_outcome=COG_CLI_INSTRUCTION_COMMIT_UNCHANGED;
    cog_instruction_snapshot_free(candidate);return COG_OK;
  }
  if(rc==COG_OK)rc=cog_runtime_instruction_prepare(&r->control_change,r,candidate);
  cog_instruction_snapshot_free(candidate);
  if(rc!=COG_OK) {
    r->remote_status=rc;r->remote_outcome=COG_CLI_INSTRUCTION_COMMIT_REFUSED;return COG_OK;
  }
  if(j->kind==JOB_SCOPE) {
    rc=deny_remaining(r,r->app->tool_index);
    if(rc==COG_OK)rc=cli_runtime_control_saved(r->app);
    else (void)remote_saved(r,rc);
    if(rc==COG_OK)r->scope_replaced=true;
    return COG_OK;
  }
  r->remote_finish_control=true;
  remote_complete(j,COG_OK);return COG_OK;
}
cog_result_t cli_runtime_remote_release(cog_runtime_t *r,const cog_cli_instruction_resource_t *ready) {
  if(r==NULL || ready==NULL)return COG_ERROR_NULL_PTR;
  if(!r->remote_active || ready->kind!=COG_CLI_INSTRUCTION_RESOURCE_READY ||
      ready->outcome!=r->remote_outcome || ready->checkpoint_generation!=r->remote_checkpoint)
    return COG_ERROR_PROTOCOL;
  if(r->remote_outcome==COG_CLI_INSTRUCTION_COMMIT_UNCERTAIN)return COG_ERROR_INDETERMINATE;
  runtime_instruction_job_t *j=r->instruction_job;
  if(j!=NULL && j->kind==JOB_SCOPE) {
    r->scope_allowed=r->remote_outcome==COG_CLI_INSTRUCTION_COMMIT_UNCHANGED;
    r->scope_refused=r->remote_outcome==COG_CLI_INSTRUCTION_COMMIT_REFUSED;
    r->scope_refusal=r->remote_status;
    r->instruction_job=NULL;
  } else if(j!=NULL && !j->done)remote_complete(j,r->remote_status);
  r->remote_active=false;r->remote_finish_control=false;return COG_OK;
}

cog_result_t cli_runtime_resource_request_matches(cog_runtime_t *r,
    const cog_cli_instruction_resource_t *request,const cog_message_tool_call_t *call) {
  if(r==NULL || request==NULL || call==NULL)return COG_ERROR_NULL_PTR;
  const cog_tool_t *descriptor=NULL;
  for(size_t i=0U;i<r->config.tools.binding_count;i++)
    if(strcmp(r->bindings[i].descriptor->name,call->name)==0)descriptor=r->bindings[i].descriptor;
  if(descriptor==NULL)return COG_ERROR_DENIED;
  if(request->operation==COG_CLI_INSTRUCTION_RESOURCE_SCOPE) {
    char target[CLI_ARGUMENT_BYTES];
    cog_result_t rc=scope_target(r,descriptor,call,target,sizeof(target));
    return rc!=COG_OK?rc:strcmp(target,request->path)==0?COG_OK:COG_ERROR_PROTOCOL;
  }
  bool activate=request->operation==COG_CLI_INSTRUCTION_RESOURCE_ACTIVATE;
  if(descriptor!=(activate?&cog_runtime_activate_skill_tool:&cog_runtime_read_skill_resource_tool))
    return COG_ERROR_DENIED;
  const char *name=NULL;
  cog_result_t rc=cli_runtime_json_field(call->arguments,call->arguments_len,activate?"name":"skill",
      r->json_storage,CLI_ARGUMENT_BYTES*3U,&name);
  if(rc!=COG_OK)return rc;
  if(name==NULL || strcmp(name,request->skill_name)!=0)return COG_ERROR_PROTOCOL;
  if(!activate) {
    const char *path=NULL;
    rc=cli_runtime_json_field(call->arguments,call->arguments_len,"path",r->json_storage,CLI_ARGUMENT_BYTES*3U,&path);
    if(rc!=COG_OK)return rc;
    if(path==NULL || strcmp(path,request->path)!=0)return COG_ERROR_PROTOCOL;
  }
  return COG_OK;
}

#include "internal.h"
static const cog_instruction_entry_t *find_entry(const cog_instruction_catalog_t *c,
                                                 cog_instruction_kind_t kind, const char *name) {
    if (!name)
        return NULL;
    for (size_t i = 0; i < c->entry_count; ++i)
        if (c->entries[i].kind == kind && strcmp(c->entries[i].name, name) == 0)
            return &c->entries[i];
    return NULL;
}
static bool word(const char **list, const char **start, size_t *n) {
    if (*list == NULL)
        return false;
    while (**list == ' ')
        ++*list;
    if (**list == '\0')
        return false;
    *start = *list;
    while (**list && **list != ' ')
        ++*list;
    *n = (size_t)(*list - *start);
    return true;
}
static bool in_list(const char *list, const char *name) {
    const char *p;
    size_t n;
    while (word(&list, &p, &n))
        if (strlen(name) == n && memcmp(name, p, n) == 0)
            return true;
    return false;
}
static cog_result_t eligible(const char *list, const cog_prompt_tool_binding_t *bindings,
                             size_t count) {
    const char *p;
    size_t n;
    while (word(&list, &p, &n)) {
        for (size_t j = 0; j < n; ++j)
            if ((unsigned char)p[j] <= 32U || strchr("*?[]()", p[j]) != NULL)
                return COG_ERROR_UNSUPPORTED;
        bool found = false;
        for (size_t i = 0; i < count; ++i)
            if (strlen(bindings[i].descriptor->name) == n &&
                memcmp(bindings[i].descriptor->name, p, n) == 0 &&
                bindings[i].policy != COG_PROMPT_TOOL_DENY) {
                found = true;
                break;
            }
        if (!found)
            return COG_ERROR_DENIED;
    }
    return COG_OK;
}
static int bind_cmp(const void *a, const void *b) {
    return strcmp(((const cog_prompt_tool_binding_t *)a)->descriptor->name,
                  ((const cog_prompt_tool_binding_t *)b)->descriptor->name);
}
static int str_cmp(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}
static int src_cmp(const void *pa, const void *pb) {
    const cog_instruction_source_t *a = pa, *b = pb;
    if (a->kind != b->kind)
        return a->kind < b->kind ? -1 : 1;
    if (a->kind == COG_INSTRUCTION_HOST && a->precedence != b->precedence)
        return a->precedence < b->precedence ? -1 : 1;
    if (a->kind == COG_INSTRUCTION_AGENTS) {
        size_t da = 0, db = 0;
        (void)ci_path(a->scope, true, &da);
        (void)ci_path(b->scope, true, &db);
        if (da != db)
            return da < db ? -1 : 1;
    }
    if (a->kind == COG_INSTRUCTION_RESOURCE) {
        int k = strcmp(a->owner_skill, b->owner_skill);
        if (k)
            return k;
    }
    int k = strcmp(a->root_id, b->root_id);
    return k ? k : strcmp(a->path, b->path);
}
static cog_result_t copy_entry(ci_arena *a, const cog_instruction_limits_t *l,
                               const cog_instruction_entry_t *in, cog_instruction_entry_t *out) {
    *out = *in;
    const char *const input[] = {in->name,       in->description, in->root_id,    in->path,
                                 in->model_hint, in->tool_names,  in->skill_names};
    const char **output[] = {&out->name,       &out->description, &out->root_id,    &out->path,
                             &out->model_hint, &out->tool_names,  &out->skill_names};
    for (size_t i = 0; i < 7U; ++i) {
        cog_result_t rc = ci_string_copy(a, l, input[i], i >= 4U, output[i]);
        if (rc != COG_OK)
            return rc;
    }
    return COG_OK;
}
static cog_result_t fragment(cog_instruction_snapshot_t *s, const cog_instruction_limits_t *l,
                             cog_prompt_origin_t origin, const char *id, const char *scope,
                             const char *text, size_t n) {
    if (strlen(id) >= l->max_string_bytes || strlen(scope) >= l->max_string_bytes)
        return COG_ERROR_LIMIT;
    size_t k = s->manifest.fragment_count;
    if (k == l->max_fragments)
        return COG_ERROR_LIMIT;
    s->fragments[k] = (cog_prompt_fragment_t){origin, id, scope, text, n};
    s->manifest.fragment_count = k + 1U;
    return COG_OK;
}
static char *join(ci_arena *a, const char *x, const char *sep, const char *y, const char *tail) {
    size_t n = 0;
    const char *parts[] = {x, sep, y, tail};
    for (size_t i = 0; i < 4U; ++i)
        if (!ci_add(n, strlen(parts[i]), &n))
            return NULL;
    if (n == SIZE_MAX || n + 1U > a->capacity - a->used)
        return NULL;
    char *out = a->data + a->used;
    size_t at = 0;
    for (size_t i = 0; i < 4U; ++i) {
        size_t k = strlen(parts[i]);
        memcpy(out + at, parts[i], k);
        at += k;
    }
    out[at] = '\0';
    a->used += n + 1U;
    return out;
}
static cog_result_t source_fragment(cog_instruction_snapshot_t *s,
                                    const cog_instruction_limits_t *l,
                                    const cog_instruction_source_t *src, cog_prompt_origin_t origin,
                                    size_t offset) {
    char *id = join(&s->arena, src->root_id, ":", src->path, "");
    if (!id)
        return COG_ERROR_LIMIT;
    return fragment(s, l, origin, id, src->scope, src->text + offset, src->text_bytes - offset);
}
static const cog_instruction_source_t *source_for(const cog_instruction_snapshot_t *s,
                                                  const cog_instruction_entry_t *e) {
    for (size_t i = 0; i < s->manifest.source_count; ++i)
        if (ci_equal(e->root_id, s->sources[i].root_id) && ci_equal(e->path, s->sources[i].path))
            return &s->sources[i];
    return NULL;
}
static size_t body_for(const cog_instruction_catalog_t *c, const cog_instruction_entry_t *e) {
    for (size_t i = 0; i < c->source_count; ++i)
        if (ci_equal(e->root_id, c->sources[i].root_id) && ci_equal(e->path, c->sources[i].path))
            return c->body_offset[i];
    return 0;
}
static bool ancestor(const char *a, const char *b) {
    size_t n = strlen(a);
    return n == 0U || (strncmp(a, b, n) == 0 && (b[n] == '\0' || b[n] == '/'));
}
static cog_result_t active_add(cog_instruction_snapshot_t *s, const cog_instruction_catalog_t *c,
                               const cog_instruction_limits_t *l, const char *p, size_t n) {
    if (n == 0U || n > 64U)
        return COG_ERROR_PROTOCOL;
    char name[65];
    memcpy(name, p, n);
    name[n] = '\0';
    if (!ci_name(name))
        return COG_ERROR_PROTOCOL;
    for (size_t i = 0; i < s->manifest.active_skill_count; ++i)
        if (strcmp(s->active[i], name) == 0)
            return COG_OK;
    const cog_instruction_entry_t *e = find_entry(c, COG_INSTRUCTION_SKILL, name);
    if (!e)
        return COG_ERROR_NOT_FOUND;
    cog_result_t rc = eligible(e->tool_names, s->bindings, s->manifest.tool_count);
    if (rc != COG_OK)
        return rc;
    if (s->manifest.active_skill_count == l->max_active_skills)
        return COG_ERROR_LIMIT;
    const char *copy = ci_copy(&s->arena, p, n);
    if (!copy)
        return COG_ERROR_LIMIT;
    s->active[s->manifest.active_skill_count++] = copy;
    return COG_OK;
}
cog_result_t cog_instruction_snapshot_free(cog_instruction_snapshot_t *s) {
    if (s) {
        ci_wipe(s->arena.data, s->arena.capacity);
        free(s->arena.data);
        free(s->sources);
        free(s->hashes);
        free(s->skills);
        free(s->active);
        free(s->tools);
        free(s->bindings);
        free(s->fragments);
        free(s);
    }
    return COG_OK;
}
cog_result_t cog_instruction_snapshot_new(cog_instruction_snapshot_t **out,
                                          const cog_instruction_catalog_t *c,
                                          const cog_instruction_limits_t *l,
                                          const cog_instruction_selection_t *sel,
                                          const cog_instruction_source_t *additions, size_t count,
                                          cog_instruction_diagnostic_t *err) {
    ci_diag(err, COG_OK, NULL, 0, false);
    if (!out) {
        ci_diag(err, COG_ERROR_NULL_PTR, NULL, 0, false);
        return COG_ERROR_NULL_PTR;
    }
    *out = NULL;
    cog_result_t rc = COG_OK;
    const cog_instruction_source_t *bad = NULL;
    cog_instruction_snapshot_t *s = NULL;
    size_t total = 0;
    if (!c || !l || !sel || (count && !additions) || (sel->binding_count && !sel->bindings) ||
        (sel->skill_count && !sel->skill_names)) {
        rc = COG_ERROR_NULL_PTR;
        goto done;
    }
    if (!ci_limits(l) || !sel->snapshot_id || !ci_root(sel->workspace_root_id) ||
        !ci_path(sel->scope, true, NULL) || !sel->model_id || !*sel->model_id ||
        !sel->template_id || !*sel->template_id || !sel->template_revision ||
        !sel->max_context_bytes || !sel->max_output_tokens || !sel->max_tool_rounds) {
        rc = COG_ERROR_INVALID_ARG;
        goto done;
    }
    if (!ci_add(c->source_count, count, &total) || total > l->max_sources ||
        sel->binding_count > l->max_tools || sel->skill_count > l->max_active_skills ||
        total > SIZE_MAX / sizeof(cog_instruction_source_t) ||
        l->max_tools > SIZE_MAX / sizeof(cog_instruction_tool_record_t) ||
        l->max_fragments > SIZE_MAX / sizeof(cog_prompt_fragment_t) ||
        l->max_entries > SIZE_MAX / sizeof(cog_instruction_entry_t) ||
        l->max_active_skills > SIZE_MAX / sizeof(char *)) {
        rc = COG_ERROR_LIMIT;
        goto done;
    }
    const cog_instruction_entry_t *agent = NULL;
    if (sel->agent_name) {
        agent = find_entry(c, COG_INSTRUCTION_AGENT, sel->agent_name);
        if (!agent) {
            rc = COG_ERROR_NOT_FOUND;
            goto done;
        }
        if (agent->model_hint && !ci_equal(agent->model_hint, sel->model_id)) {
            rc = COG_ERROR_UNSUPPORTED;
            goto done;
        }
    }
    for (size_t i = 0; i < sel->binding_count; ++i) {
        const cog_prompt_tool_binding_t *b = &sel->bindings[i];
        if (!b->descriptor || !b->descriptor->name || !*b->descriptor->name ||
            b->policy < COG_PROMPT_TOOL_ALLOW || b->policy > COG_PROMPT_TOOL_DENY) {
            rc = COG_ERROR_INVALID_ARG;
            goto done;
        }
        for (const unsigned char *p = (const unsigned char *)b->descriptor->name; *p; ++p)
            if (*p <= 32U || *p == 127U) {
                rc = COG_ERROR_INVALID_ARG;
                goto done;
            }
        if (!ci_text(b->descriptor->name, strlen(b->descriptor->name), NULL)) {
            rc = COG_ERROR_PROTOCOL;
            goto done;
        }
        for (size_t j = 0; j < i; ++j)
            if (strcmp(sel->bindings[j].descriptor->name, b->descriptor->name) == 0) {
                rc = COG_ERROR_INVALID_ARG;
                goto done;
            }
    }
    if (agent) {
        rc = eligible(agent->tool_names, sel->bindings, sel->binding_count);
        if (rc != COG_OK)
            goto done;
    }
    s = calloc(1, sizeof(*s));
    if (!s) {
        rc = COG_ERROR_NOMEM;
        goto done;
    }
    s->arena.capacity = l->max_total_bytes;
    s->arena.data = malloc(l->max_total_bytes);
    s->sources = ci_array(total, sizeof(*s->sources));
    s->hashes = ci_array(total, sizeof(*s->hashes));
    s->skills = ci_array(l->max_entries, sizeof(*s->skills));
    s->active = ci_array(l->max_active_skills, sizeof(*s->active));
    s->tools = ci_array(l->max_tools, sizeof(*s->tools));
    s->bindings = ci_array(l->max_tools, sizeof(*s->bindings));
    s->fragments = ci_array(l->max_fragments, sizeof(*s->fragments));
    if (!s->arena.data || !s->sources || !s->hashes || !s->skills || !s->active || !s->tools ||
        !s->bindings || !s->fragments) {
        rc = COG_ERROR_NOMEM;
        goto done;
    }
    cog_instruction_manifest_t *m = &s->manifest;
    *m = (cog_instruction_manifest_t){.version = 1,
                                      .catalog_generation = c->generation,
                                      .snapshot_id = sel->snapshot_id,
                                      .template_revision = sel->template_revision,
                                      .max_context_bytes = sel->max_context_bytes,
                                      .max_output_tokens = sel->max_output_tokens,
                                      .max_tool_rounds = sel->max_tool_rounds,
                                      .sources = s->sources,
                                      .source_sha256 = (const unsigned char(*)[32])s->hashes,
                                      .available_skills = s->skills,
                                      .active_skills = s->active,
                                      .tools = s->tools,
                                      .fragments = s->fragments};
    const char *inputs[] = {sel->workspace_root_id, sel->scope, sel->agent_name, sel->model_id,
                            sel->template_id};
    const char **outputs[] = {&m->workspace_root_id, &m->scope, &m->agent_name, &m->model_id,
                              &m->template_id};
    for (size_t i = 0; i < 5U; ++i) {
        rc = ci_string_copy(&s->arena, l, inputs[i], i == 2U, outputs[i]);
        if (rc != COG_OK)
            goto done;
    }
    if (agent) {
        if (agent->context_byte_limit && agent->context_byte_limit < m->max_context_bytes)
            m->max_context_bytes = agent->context_byte_limit;
        if (agent->output_token_limit && agent->output_token_limit < m->max_output_tokens)
            m->max_output_tokens = agent->output_token_limit;
        if (agent->tool_round_limit && agent->tool_round_limit < m->max_tool_rounds)
            m->max_tool_rounds = agent->tool_round_limit;
    }
    for (size_t i = 0; i < sel->binding_count; ++i) {
        const cog_prompt_tool_binding_t *b = &sel->bindings[i];
        if (b->policy == COG_PROMPT_TOOL_DENY ||
            (agent && agent->tool_names && !in_list(agent->tool_names, b->descriptor->name)))
            continue;
        s->bindings[m->tool_count++] = *b;
    }
    qsort(s->bindings, m->tool_count, sizeof(*s->bindings), bind_cmp);
    for (size_t i = 0; i < m->tool_count; ++i) {
        const cog_tool_t *t = s->bindings[i].descriptor;
        s->tools[i].policy = s->bindings[i].policy;
        rc = ci_string_copy(&s->arena, l, t->name, false, &s->tools[i].name);
        if (rc == COG_OK)
            rc = ci_string_copy(&s->arena, l, t->description, true, &s->tools[i].description);
        if (rc == COG_OK)
            rc = ci_string_copy(&s->arena, l, t->input_schema, true, &s->tools[i].input_schema);
        if (rc != COG_OK)
            goto done;
    }
    if (agent) {
        const char *list = agent->skill_names, *p;
        size_t n;
        while (word(&list, &p, &n)) {
            rc = active_add(s, c, l, p, n);
            if (rc != COG_OK)
                goto done;
        }
    }
    for (size_t i = 0; i < sel->skill_count; ++i) {
        if (!sel->skill_names[i]) {
            rc = COG_ERROR_NULL_PTR;
            goto done;
        }
        for (size_t j = 0; j < i; ++j)
            if (strcmp(sel->skill_names[i], sel->skill_names[j]) == 0) {
                rc = COG_ERROR_INVALID_ARG;
                goto done;
            }
        rc = active_add(s, c, l, sel->skill_names[i], strlen(sel->skill_names[i]));
        if (rc != COG_OK)
            goto done;
    }
    qsort(s->active, m->active_skill_count, sizeof(*s->active), str_cmp);
    size_t resources = 0;
    for (size_t i = 0; i < total; ++i) {
        const cog_instruction_source_t *in =
            i < c->source_count ? &c->sources[i] : &additions[i - c->source_count];
        bad = in;
        if (i >= c->source_count && in->kind != COG_INSTRUCTION_AGENTS &&
            in->kind != COG_INSTRUCTION_RESOURCE) {
            rc = COG_ERROR_INVALID_ARG;
            goto done;
        }
        rc = ci_source_copy(&s->arena, l, in, &s->sources[i]);
        if (rc != COG_OK)
            goto done;
        for (size_t j = 0; j < i; ++j)
            if (ci_equal(in->root_id, s->sources[j].root_id) &&
                ci_equal(in->path, s->sources[j].path)) {
                rc = COG_ERROR_INVALID_ARG;
                goto done;
            }
        if (in->kind == COG_INSTRUCTION_AGENTS) {
            size_t k = strlen(in->scope);
            if (!ci_equal(in->root_id, sel->workspace_root_id) ||
                !ancestor(in->scope, sel->scope) || strlen(in->path) != k + (k ? 1U : 0U) + 9U ||
                (k && memcmp(in->path, in->scope, k) != 0) || (k && in->path[k] != '/') ||
                strcmp(in->path + k + (k ? 1U : 0U), "AGENTS.md") != 0) {
                rc = COG_ERROR_PROTOCOL;
                goto done;
            }
        }
        if (in->kind == COG_INSTRUCTION_RESOURCE) {
            if (++resources > l->max_resources) {
                rc = COG_ERROR_LIMIT;
                goto done;
            }
            bool active = false;
            for (size_t j = 0; j < m->active_skill_count; ++j)
                if (ci_equal(in->owner_skill, s->active[j]))
                    active = true;
            const cog_instruction_entry_t *e =
                find_entry(c, COG_INSTRUCTION_SKILL, in->owner_skill);
            if (!active || !e) {
                rc = COG_ERROR_NOT_FOUND;
                goto done;
            }
            size_t prefix = (size_t)(strrchr(e->path, '/') - e->path) + 1U;
            if (!ci_equal(e->root_id, in->root_id) || strncmp(e->path, in->path, prefix) != 0 ||
                strlen(in->path) <= prefix) {
                rc = COG_ERROR_PROTOCOL;
                goto done;
            }
        }
        ++m->source_count;
    }
    qsort(s->sources, total, sizeof(*s->sources), src_cmp);
    for (size_t i = 0; i < total; ++i)
        ci_hash(s->sources[i].text, s->sources[i].text_bytes, s->hashes[i]);
    for (size_t i = 0; i < c->entry_count; ++i) {
        const cog_instruction_entry_t *e = &c->entries[i];
        if (e->kind != COG_INSTRUCTION_SKILL ||
            eligible(e->tool_names, s->bindings, m->tool_count) != COG_OK)
            continue;
        if (m->available_skill_count == l->max_entries) {
            rc = COG_ERROR_LIMIT;
            goto done;
        }
        rc = copy_entry(&s->arena, l, e, &s->skills[m->available_skill_count++]);
        if (rc != COG_OK)
            goto done;
    }
    for (size_t i = 0; i < total; ++i) {
        const cog_instruction_source_t *src = &s->sources[i];
        if (src->kind == COG_INSTRUCTION_HOST || src->kind == COG_INSTRUCTION_AGENTS) {
            rc = source_fragment(
                s, l, src,
                src->kind == COG_INSTRUCTION_HOST ? COG_PROMPT_HOST : COG_PROMPT_WORKSPACE, 0);
            if (rc != COG_OK)
                goto done;
        }
    }
    if (agent) {
        rc = source_fragment(s, l, source_for(s, agent), COG_PROMPT_AGENT, body_for(c, agent));
        if (rc != COG_OK)
            goto done;
    }
    for (size_t i = 0; i < m->available_skill_count; ++i) {
        const cog_instruction_entry_t *e = &s->skills[i];
        char *id = join(&s->arena, e->root_id, ":", e->path, "");
        char *text = join(&s->arena, e->name, "\n", e->description, "\n");
        if (!id || !text) {
            rc = COG_ERROR_LIMIT;
            goto done;
        }
        rc = fragment(s, l, COG_PROMPT_SKILL_CATALOG, id, "", text, strlen(text));
        if (rc != COG_OK)
            goto done;
    }
    for (size_t i = 0; i < m->active_skill_count; ++i) {
        const cog_instruction_entry_t *e = find_entry(c, COG_INSTRUCTION_SKILL, s->active[i]);
        rc = source_fragment(s, l, source_for(s, e), COG_PROMPT_SKILL, body_for(c, e));
        if (rc != COG_OK)
            goto done;
    }
    for (size_t i = 0; i < total; ++i)
        if (s->sources[i].kind == COG_INSTRUCTION_RESOURCE) {
            rc = source_fragment(s, l, &s->sources[i], COG_PROMPT_RESOURCE, 0);
            if (rc != COG_OK)
                goto done;
        }
    for (size_t i = 0; i < m->tool_count; ++i) {
        char *text =
            join(&s->arena, "Tool ", s->tools[i].name, ": ",
                 s->tools[i].policy == COG_PROMPT_TOOL_ALLOW ? "allowed\n" : "decision required\n");
        if (!text) {
            rc = COG_ERROR_LIMIT;
            goto done;
        }
        rc = fragment(s, l, COG_PROMPT_TOOL_GUIDANCE, s->tools[i].name, "", text, strlen(text));
        if (rc != COG_OK)
            goto done;
    }
    {
        size_t bytes = 0;
        for (size_t i = 0; i < m->fragment_count; ++i)
            if (!ci_add(bytes, s->fragments[i].text_bytes, &bytes) ||
                bytes > m->max_context_bytes) {
                rc = COG_ERROR_LIMIT;
                goto done;
            }
    }
    *out = s;
    ci_diag(err, COG_OK, NULL, 0, false);
    return COG_OK;
done:
    cog_instruction_snapshot_free(s);
    ci_diag(err, rc, bad, 0, false);
    return rc;
}
cog_result_t cog_instruction_snapshot_view(const cog_instruction_snapshot_t *s,
                                           cog_instruction_manifest_t *m) {
    if (!s || !m)
        return COG_ERROR_NULL_PTR;
    *m = s->manifest;
    return COG_OK;
}
cog_result_t cog_instruction_prompt_view(const cog_instruction_snapshot_t *s,
                                         cog_prompt_bundle_t *out) {
    if (!s || !out)
        return COG_ERROR_NULL_PTR;
    if (s->inspection)
        return COG_ERROR_INVALID_STATE;
    const cog_instruction_manifest_t *m = &s->manifest;
    *out = (cog_prompt_bundle_t){m->catalog_generation, m->snapshot_id,       m->model_id,
                                 m->template_id,        m->template_revision, m->max_context_bytes,
                                 m->max_output_tokens,  m->max_tool_rounds,   m->fragments,
                                 m->fragment_count,     s->bindings,          m->tool_count};
    return COG_OK;
}

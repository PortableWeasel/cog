#include "internal.h"
static bool entry_eq(const cog_instruction_entry_t *a, const cog_instruction_entry_t *b) {
    return a->kind == b->kind && ci_equal(a->name, b->name) &&
           ci_equal(a->description, b->description) && ci_equal(a->root_id, b->root_id) &&
           ci_equal(a->path, b->path) && ci_equal(a->model_hint, b->model_hint) &&
           ci_equal(a->tool_names, b->tool_names) && ci_equal(a->skill_names, b->skill_names) &&
           a->context_byte_limit == b->context_byte_limit &&
           a->output_token_limit == b->output_token_limit &&
           a->tool_round_limit == b->tool_round_limit && memcmp(a->sha256, b->sha256, 32) == 0;
}
static bool bytes_eq(const char *a, const char *b, size_t n) {
    return n == 0U || (a && b && memcmp(a, b, n) == 0);
}
static bool same(const cog_instruction_manifest_t *a, const cog_instruction_manifest_t *b) {
    if (a->version != b->version || a->catalog_generation != b->catalog_generation ||
        a->snapshot_id != b->snapshot_id || !ci_equal(a->workspace_root_id, b->workspace_root_id) ||
        !ci_equal(a->scope, b->scope) || !ci_equal(a->agent_name, b->agent_name) ||
        !ci_equal(a->model_id, b->model_id) || !ci_equal(a->template_id, b->template_id) ||
        a->template_revision != b->template_revision ||
        a->max_context_bytes != b->max_context_bytes ||
        a->max_output_tokens != b->max_output_tokens || a->max_tool_rounds != b->max_tool_rounds ||
        a->source_count != b->source_count ||
        a->available_skill_count != b->available_skill_count ||
        a->active_skill_count != b->active_skill_count || a->tool_count != b->tool_count ||
        a->fragment_count != b->fragment_count)
        return false;
    for (size_t i = 0; i < a->source_count; ++i) {
        const cog_instruction_source_t *x = &a->sources[i], *y = &b->sources[i];
        if (x->kind != y->kind || x->precedence != y->precedence ||
            !ci_equal(x->root_id, y->root_id) || !ci_equal(x->path, y->path) ||
            !ci_equal(x->scope, y->scope) || !ci_equal(x->owner_skill, y->owner_skill) ||
            x->text_bytes != y->text_bytes || !bytes_eq(x->text, y->text, x->text_bytes) ||
            memcmp(a->source_sha256[i], b->source_sha256[i], 32) != 0)
            return false;
    }
    for (size_t i = 0; i < a->available_skill_count; ++i)
        if (!entry_eq(&a->available_skills[i], &b->available_skills[i]))
            return false;
    for (size_t i = 0; i < a->active_skill_count; ++i)
        if (!ci_equal(a->active_skills[i], b->active_skills[i]))
            return false;
    for (size_t i = 0; i < a->tool_count; ++i) {
        const cog_instruction_tool_record_t *x = &a->tools[i], *y = &b->tools[i];
        if (!ci_equal(x->name, y->name) || !ci_equal(x->description, y->description) ||
            !ci_equal(x->input_schema, y->input_schema) || x->policy != y->policy)
            return false;
    }
    for (size_t i = 0; i < a->fragment_count; ++i) {
        const cog_prompt_fragment_t *x = &a->fragments[i], *y = &b->fragments[i];
        if (x->origin != y->origin || !ci_equal(x->source_id, y->source_id) ||
            !ci_equal(x->scope, y->scope) || x->text_bytes != y->text_bytes ||
            !bytes_eq(x->text, y->text, x->text_bytes))
            return false;
    }
    return true;
}
cog_result_t cog_instruction_snapshot_restore(cog_instruction_snapshot_t **out,
                                              const cog_instruction_limits_t *l,
                                              const cog_instruction_manifest_t *m,
                                              cog_instruction_restore_mode_t mode,
                                              const cog_prompt_tool_binding_t *host, size_t count,
                                              cog_instruction_diagnostic_t *err) {
    ci_diag(err, COG_OK, NULL, 0, false);
    if (!out) {
        ci_diag(err, COG_ERROR_NULL_PTR, NULL, 0, false);
        return COG_ERROR_NULL_PTR;
    }
    *out = NULL;
    cog_result_t rc = COG_OK;
    cog_instruction_catalog_t *cat = NULL;
    cog_instruction_snapshot_t *snap = NULL;
    cog_instruction_source_t *sources = NULL, *adds = NULL;
    cog_tool_t *tools = NULL;
    cog_prompt_tool_binding_t *bindings = NULL;
    if (!l || !m || (count && !host)) {
        rc = COG_ERROR_NULL_PTR;
        goto done;
    }
    if (!ci_limits(l) || (mode != COG_INSTRUCTION_EXECUTE && mode != COG_INSTRUCTION_INSPECT) ||
        (mode == COG_INSTRUCTION_INSPECT && (host || count))) {
        rc = COG_ERROR_INVALID_ARG;
        goto done;
    }
    if (m->version != 1U) {
        rc = COG_ERROR_UNSUPPORTED;
        goto done;
    }
    if (m->source_count > l->max_sources || m->available_skill_count > l->max_entries ||
        m->active_skill_count > l->max_active_skills || m->tool_count > l->max_tools ||
        m->fragment_count > l->max_fragments || count > l->max_tools ||
        m->source_count > SIZE_MAX / sizeof(*sources) ||
        m->tool_count > SIZE_MAX / sizeof(*tools)) {
        rc = COG_ERROR_LIMIT;
        goto done;
    }
    if ((m->source_count && (!m->sources || !m->source_sha256)) ||
        (m->available_skill_count && !m->available_skills) ||
        (m->active_skill_count && !m->active_skills) || (m->tool_count && !m->tools) ||
        (m->fragment_count && !m->fragments)) {
        rc = COG_ERROR_PROTOCOL;
        goto done;
    }
    for (size_t i = 0; i < count; ++i) {
        if (!host[i].descriptor || !host[i].descriptor->name ||
            host[i].policy < COG_PROMPT_TOOL_ALLOW || host[i].policy > COG_PROMPT_TOOL_DENY) {
            rc = COG_ERROR_INVALID_ARG;
            goto done;
        }
        for (size_t j = 0; j < i; ++j)
            if (ci_equal(host[i].descriptor->name, host[j].descriptor->name)) {
                rc = COG_ERROR_INVALID_ARG;
                goto done;
            }
    }
    sources = ci_array(m->source_count, sizeof(*sources));
    adds = ci_array(m->source_count, sizeof(*adds));
    tools = ci_array(m->tool_count, sizeof(*tools));
    bindings = ci_array(m->tool_count, sizeof(*bindings));
    if (!sources || !adds || !tools || !bindings) {
        rc = COG_ERROR_NOMEM;
        goto done;
    }
    size_t nc = 0, na = 0;
    for (size_t i = 0; i < m->source_count; ++i) {
        const cog_instruction_source_t *s = &m->sources[i];
        if (s->kind == COG_INSTRUCTION_HOST || s->kind == COG_INSTRUCTION_AGENT ||
            s->kind == COG_INSTRUCTION_SKILL)
            sources[nc++] = *s;
        else
            adds[na++] = *s;
    }
    for (size_t i = 0; i < m->tool_count; ++i) {
        const cog_instruction_tool_record_t *t = &m->tools[i];
        if (!t->name || t->policy < COG_PROMPT_TOOL_ALLOW || t->policy >= COG_PROMPT_TOOL_DENY) {
            rc = COG_ERROR_PROTOCOL;
            goto done;
        }
        tools[i] = (cog_tool_t){
            .name = t->name, .description = t->description, .input_schema = t->input_schema};
        bindings[i] = (cog_prompt_tool_binding_t){&tools[i], t->policy};
        if (mode == COG_INSTRUCTION_EXECUTE) {
            size_t j = 0;
            for (; j < count; ++j)
                if (ci_equal(host[j].descriptor->name, t->name))
                    break;
            if (j == count || host[j].policy == COG_PROMPT_TOOL_DENY ||
                (t->policy == COG_PROMPT_TOOL_ALLOW && host[j].policy != COG_PROMPT_TOOL_ALLOW)) {
                rc = COG_ERROR_DENIED;
                goto done;
            }
            if (!ci_equal(host[j].descriptor->description, t->description) ||
                !ci_equal(host[j].descriptor->input_schema, t->input_schema)) {
                rc = COG_ERROR_UNSUPPORTED;
                goto done;
            }
            bindings[i].descriptor = host[j].descriptor;
        }
    }
    rc = cog_instruction_catalog_new(&cat, l, m->catalog_generation, sources, nc, NULL);
    if (rc != COG_OK) {
        if (rc == COG_ERROR_INVALID_ARG || rc == COG_ERROR_NOT_FOUND || rc == COG_ERROR_DENIED)
            rc = COG_ERROR_PROTOCOL;
        goto done;
    }
    cog_instruction_selection_t selection = {.snapshot_id = m->snapshot_id,
                                             .workspace_root_id = m->workspace_root_id,
                                             .scope = m->scope,
                                             .agent_name = m->agent_name,
                                             .skill_names = m->active_skills,
                                             .skill_count = m->active_skill_count,
                                             .model_id = m->model_id,
                                             .template_id = m->template_id,
                                             .template_revision = m->template_revision,
                                             .max_context_bytes = m->max_context_bytes,
                                             .max_output_tokens = m->max_output_tokens,
                                             .max_tool_rounds = m->max_tool_rounds,
                                             .bindings = bindings,
                                             .binding_count = m->tool_count};
    rc = cog_instruction_snapshot_new(&snap, cat, l, &selection, adds, na, NULL);
    if (rc != COG_OK) {
        if (rc == COG_ERROR_INVALID_ARG || rc == COG_ERROR_NOT_FOUND || rc == COG_ERROR_DENIED)
            rc = COG_ERROR_PROTOCOL;
        goto done;
    }
    if (!same(m, &snap->manifest)) {
        rc = COG_ERROR_PROTOCOL;
        goto done;
    }
    snap->inspection = mode == COG_INSTRUCTION_INSPECT;
    if (snap->inspection)
        for (size_t i = 0; i < snap->manifest.tool_count; ++i)
            snap->bindings[i].descriptor = NULL;
    *out = snap;
    snap = NULL;
done:
    cog_instruction_snapshot_free(snap);
    cog_instruction_catalog_free(cat);
    free(sources);
    free(adds);
    free(tools);
    free(bindings);
    ci_diag(err, rc, NULL, 0, false);
    return rc;
}

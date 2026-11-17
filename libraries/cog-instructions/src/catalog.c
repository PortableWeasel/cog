#include "internal.h"
static cog_result_t string_copy(ci_arena *a, const cog_instruction_limits_t *l, const char *v,
                                bool nullable, const char **out) {
    if (v == NULL) {
        if (!nullable)
            return COG_ERROR_INVALID_ARG;
        *out = NULL;
        return COG_OK;
    }
    size_t n = strlen(v);
    if (n >= l->max_string_bytes)
        return COG_ERROR_LIMIT;
    if (!ci_text(v, n, NULL))
        return COG_ERROR_PROTOCOL;
    *out = ci_copy(a, v, n);
    return *out ? COG_OK : COG_ERROR_LIMIT;
}
static cog_result_t source_copy(ci_arena *a, const cog_instruction_limits_t *l,
                                const cog_instruction_source_t *s, cog_instruction_source_t *out) {
    if (s->kind < COG_INSTRUCTION_HOST || s->kind > COG_INSTRUCTION_RESOURCE ||
        !ci_root(s->root_id) || !ci_path(s->path, false, NULL) || !ci_path(s->scope, true, NULL) ||
        (s->kind != COG_INSTRUCTION_AGENTS && s->scope[0] != '\0') ||
        (s->kind == COG_INSTRUCTION_RESOURCE ? !ci_name(s->owner_skill) : s->owner_skill != NULL))
        return COG_ERROR_INVALID_ARG;
    if (s->text_bytes > l->max_source_bytes)
        return COG_ERROR_LIMIT;
    if (!ci_text(s->text, s->text_bytes, NULL))
        return COG_ERROR_PROTOCOL;
    *out = *s;
    cog_result_t rc = string_copy(a, l, s->root_id, false, &out->root_id);
    if (rc == COG_OK)
        rc = string_copy(a, l, s->path, false, &out->path);
    if (rc == COG_OK)
        rc = string_copy(a, l, s->scope, false, &out->scope);
    if (rc == COG_OK)
        rc = string_copy(a, l, s->owner_skill, true, &out->owner_skill);
    if (rc != COG_OK)
        return rc;
    out->text = ci_copy(a, s->text, s->text_bytes);
    return out->text ? COG_OK : COG_ERROR_LIMIT;
}
static int source_cmp(const void *pa, const void *pb) {
    const cog_instruction_source_t *a = pa, *b = pb;
    if (a->kind != b->kind)
        return a->kind < b->kind ? -1 : 1;
    if (a->precedence != b->precedence)
        return a->precedence < b->precedence ? -1 : 1;
    int k = strcmp(a->root_id, b->root_id);
    return k ? k : strcmp(a->path, b->path);
}
static int entry_cmp(const void *pa, const void *pb) {
    const cog_instruction_entry_t *a = pa, *b = pb;
    if (a->kind != b->kind)
        return a->kind < b->kind ? -1 : 1;
    return strcmp(a->name, b->name);
}
static const cog_instruction_source_t *entry_source(const cog_instruction_catalog_t *c,
                                                    const cog_instruction_entry_t *e) {
    for (size_t i = 0; i < c->source_count; ++i)
        if (ci_equal(c->sources[i].root_id, e->root_id) && ci_equal(c->sources[i].path, e->path))
            return &c->sources[i];
    return NULL;
}
cog_result_t cog_instruction_catalog_free(cog_instruction_catalog_t *c) {
    if (c) {
        ci_wipe(c->arena.data, c->arena.capacity);
        free(c->arena.data);
        free(c->sources);
        free(c->entries);
        free(c->source_index);
        free(c->body_offset);
        free(c);
    }
    return COG_OK;
}
cog_result_t cog_instruction_catalog_new(cog_instruction_catalog_t **out,
                                         const cog_instruction_limits_t *l, uint64_t generation,
                                         const cog_instruction_source_t *sources, size_t count,
                                         cog_instruction_diagnostic_t *err) {
    ci_diag(err, COG_OK, NULL, 0, false);
    if (out == NULL) {
        ci_diag(err, COG_ERROR_NULL_PTR, NULL, 0, false);
        return COG_ERROR_NULL_PTR;
    }
    *out = NULL;
    cog_result_t rc = COG_OK;
    const cog_instruction_source_t *bad = NULL;
    size_t off = 0;
    bool known = false;
    if (l == NULL || (sources == NULL && count)) {
        rc = COG_ERROR_NULL_PTR;
        goto done;
    }
    if (!ci_limits(l) || generation == 0U) {
        rc = COG_ERROR_INVALID_ARG;
        goto done;
    }
    if (count > l->max_sources || l->max_metadata_bytes == SIZE_MAX ||
        l->max_entries > SIZE_MAX / sizeof(cog_instruction_entry_t) ||
        count > SIZE_MAX / sizeof(cog_instruction_source_t)) {
        rc = COG_ERROR_LIMIT;
        goto done;
    }
    cog_instruction_catalog_t *c = calloc(1, sizeof(*c));
    if (!c) {
        rc = COG_ERROR_NOMEM;
        goto done;
    }
    c->limits = *l;
    c->generation = generation;
    c->arena.capacity = l->max_total_bytes;
    c->arena.data = malloc(l->max_total_bytes);
    c->sources = ci_array(count, sizeof(*c->sources));
    c->entries = ci_array(l->max_entries, sizeof(*c->entries));
    c->body_offset = ci_array(count, sizeof(size_t));
    if (!c->arena.data || !c->sources || !c->entries || !c->body_offset) {
        rc = COG_ERROR_NOMEM;
        goto fail;
    }
    for (size_t i = 0; i < count; ++i) {
        bad = &sources[i];
        if (bad->kind != COG_INSTRUCTION_HOST && bad->kind != COG_INSTRUCTION_AGENT &&
            bad->kind != COG_INSTRUCTION_SKILL) {
            rc = COG_ERROR_INVALID_ARG;
            goto fail;
        }
        for (size_t j = 0; j < i; ++j)
            if (ci_equal(bad->root_id, sources[j].root_id) &&
                ci_equal(bad->path, sources[j].path)) {
                rc = COG_ERROR_INVALID_ARG;
                goto fail;
            }
        rc = source_copy(&c->arena, l, bad, &c->sources[i]);
        if (rc != COG_OK)
            goto fail;
        ++c->source_count;
    }
    qsort(c->sources, count, sizeof(*c->sources), source_cmp);
    for (size_t i = 0; i < count; ++i) {
        const cog_instruction_source_t *s = &c->sources[i];
        if (s->kind == COG_INSTRUCTION_HOST)
            continue;
        for (size_t j = 0; j < count; ++j)
            if (ci_equal(s->root_id, sources[j].root_id) && ci_equal(s->path, sources[j].path)) {
                bad = &sources[j];
                break;
            }
        cog_instruction_entry_t entry;
        rc = ci_parse(&c->arena, l, s, &entry, &c->body_offset[i], &off);
        known = rc != COG_OK;
        if (rc != COG_OK)
            goto fail;
        size_t j = 0;
        for (; j < c->entry_count; ++j)
            if (entry.kind == c->entries[j].kind && strcmp(entry.name, c->entries[j].name) == 0)
                break;
        if (j < c->entry_count) {
            const cog_instruction_source_t *old = entry_source(c, &c->entries[j]);
            if (old->precedence == s->precedence) {
                rc = COG_ERROR_PROTOCOL;
                goto fail;
            }
            if (old->precedence < s->precedence)
                c->entries[j] = entry;
        } else {
            if (c->entry_count == l->max_entries) {
                rc = COG_ERROR_LIMIT;
                goto fail;
            }
            c->entries[c->entry_count++] = entry;
        }
    }
    qsort(c->entries, c->entry_count, sizeof(*c->entries), entry_cmp);
    *out = c;
    return COG_OK;
fail:
    cog_instruction_catalog_free(c);
done:
    ci_diag(err, rc, bad, off, known);
    return rc;
}
cog_result_t cog_instruction_catalog_view(const cog_instruction_catalog_t *c,
                                          const cog_instruction_entry_t **entries, size_t *count) {
    if (!c || !entries || !count)
        return COG_ERROR_NULL_PTR;
    *entries = c->entry_count ? c->entries : NULL;
    *count = c->entry_count;
    return COG_OK;
}
/* Internal snapshot construction helpers deliberately share source validation. */
cog_result_t ci_source_copy(ci_arena *a, const cog_instruction_limits_t *l,
                            const cog_instruction_source_t *s, cog_instruction_source_t *out) {
    return source_copy(a, l, s, out);
}
cog_result_t ci_string_copy(ci_arena *a, const cog_instruction_limits_t *l, const char *s,
                            bool nullable, const char **out) {
    return string_copy(a, l, s, nullable, out);
}

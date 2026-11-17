/** @file codec.c @brief Portable, bounded canonical instruction manifests. */
#include "internal.h"
#include <stdalign.h>

/* The first pass is read-only and checks every field before any caller output
 * changes. The identical walk writes bytes or streams them into SHA-256. */
typedef struct ci_writer {
    unsigned char *output;
    size_t used;
    cog_result_t error;
    ci_sha *hash;
} ci_writer;
static void emit(ci_writer *w, const void *p, size_t n)
{
    if (w->error != COG_OK) return;
    if ((p == NULL && n != 0U) || n > SIZE_MAX - w->used) {
        w->error = p == NULL && n != 0U ? COG_ERROR_INVALID_ARG : COG_ERROR_LIMIT;
        return;
    }
    if (n != 0U && w->output != NULL) memcpy(w->output + w->used, p, n);
    if (n != 0U && w->hash != NULL) ci_sha_update(w->hash, p, n);
    w->used += n;
}
static void put32(ci_writer *w, uint32_t v)
{
    unsigned char b[4];
    for (size_t k = 0U; k < 4U; ++k) b[k] = (unsigned char)(v >> (8U * k));
    emit(w, b, sizeof(b));
}
static void put64(ci_writer *w, uint64_t v)
{
    unsigned char b[8];
    for (size_t k = 0U; k < 8U; ++k) b[k] = (unsigned char)(v >> (8U * k));
    emit(w, b, sizeof(b));
}
static void put_string(ci_writer *w, const char *p)
{
    if (w->error != COG_OK) return;
    if (p == NULL) { put32(w, UINT32_MAX); return; }
    const size_t n = strlen(p);
    if (n >= UINT32_MAX) { w->error = COG_ERROR_LIMIT; return; }
    if (!ci_text(p, n, NULL)) { w->error = COG_ERROR_PROTOCOL; return; }
    put32(w, (uint32_t)n); emit(w, p, n);
}
static void put_text(ci_writer *w, const char *p, size_t n)
{
    if (w->error != COG_OK) return;
    if (n > UINT32_MAX) { w->error = COG_ERROR_LIMIT; return; }
    if (!ci_text(p, n, NULL)) { w->error = COG_ERROR_PROTOCOL; return; }
    put64(w, (uint64_t)n); emit(w, p, n);
}
static bool put_count(ci_writer *w, const void *array, size_t n)
{
    if (w->error != COG_OK) return false;
    if (n > UINT32_MAX) { w->error = COG_ERROR_LIMIT; return false; }
    if (n != 0U && array == NULL) { w->error = COG_ERROR_INVALID_ARG; return false; }
    put32(w, (uint32_t)n);
    return w->error == COG_OK;
}
static void put_entry(ci_writer *w, const cog_instruction_entry_t *e)
{
    if (e->kind != COG_INSTRUCTION_SKILL || !ci_name(e->name) || !ci_root(e->root_id) ||
        !ci_path(e->path, false, NULL) || e->description == NULL) {
        w->error = COG_ERROR_PROTOCOL; return;
    }
    put32(w, (uint32_t)e->kind);
    put_string(w, e->name); put_string(w, e->description); put_string(w, e->root_id);
    put_string(w, e->path); put_string(w, e->model_hint); put_string(w, e->tool_names);
    put_string(w, e->skill_names); put64(w, (uint64_t)e->context_byte_limit);
    put32(w, e->output_token_limit); put32(w, e->tool_round_limit);
    emit(w, e->sha256, sizeof(e->sha256));
}
static void put_manifest(ci_writer *w, const cog_instruction_manifest_t *m)
{
    if (m->version != COG_INSTRUCTION_MANIFEST_V1) { w->error = COG_ERROR_UNSUPPORTED; return; }
    if (m->catalog_generation == 0U || m->snapshot_id == 0U || !ci_root(m->workspace_root_id) ||
        !ci_path(m->scope, true, NULL) || (m->agent_name != NULL && !ci_name(m->agent_name)) ||
        m->model_id == NULL || m->model_id[0] == '\0' || m->template_id == NULL || m->template_id[0] == '\0' ||
        m->template_revision == 0U || m->max_context_bytes == 0U || m->max_output_tokens == 0U ||
        m->max_tool_rounds == 0U) { w->error = COG_ERROR_PROTOCOL; return; }
    emit(w, "CGIM", 4U); put32(w, 1U); put32(w, m->version);
    put64(w, m->catalog_generation); put64(w, m->snapshot_id);
    put_string(w, m->workspace_root_id); put_string(w, m->scope); put_string(w, m->agent_name);
    put_string(w, m->model_id); put_string(w, m->template_id); put32(w, m->template_revision);
    put64(w, (uint64_t)m->max_context_bytes); put32(w, m->max_output_tokens); put32(w, m->max_tool_rounds);
    if (!put_count(w, m->sources, m->source_count)) return;
    if (m->source_count != 0U && m->source_sha256 == NULL) { w->error = COG_ERROR_INVALID_ARG; return; }
    for (size_t i = 0U; i < m->source_count && w->error == COG_OK; ++i) {
        const cog_instruction_source_t *s = &m->sources[i];
        if (s->kind < COG_INSTRUCTION_HOST || s->kind > COG_INSTRUCTION_RESOURCE ||
            !ci_root(s->root_id) || !ci_path(s->path, false, NULL) || !ci_path(s->scope, true, NULL)) {
            w->error = COG_ERROR_PROTOCOL; return;
        }
        put32(w, (uint32_t)s->kind); put_string(w, s->root_id); put_string(w, s->path);
        put_string(w, s->scope); put_string(w, s->owner_skill); put32(w, s->precedence);
        put_text(w, s->text, s->text_bytes);
        if (w->error != COG_OK) return;
        unsigned char digest[32]; ci_hash(s->text, s->text_bytes, digest);
        if (memcmp(digest, m->source_sha256[i], sizeof(digest)) != 0) { w->error = COG_ERROR_PROTOCOL; return; }
        emit(w, m->source_sha256[i], sizeof(digest));
    }
    if (!put_count(w, m->available_skills, m->available_skill_count)) return;
    for (size_t i = 0U; i < m->available_skill_count && w->error == COG_OK; ++i) put_entry(w, &m->available_skills[i]);
    if (!put_count(w, m->active_skills, m->active_skill_count)) return;
    for (size_t i = 0U; i < m->active_skill_count && w->error == COG_OK; ++i) {
        if (!ci_name(m->active_skills[i]) || (i != 0U && strcmp(m->active_skills[i-1U], m->active_skills[i]) >= 0)) {
            w->error = COG_ERROR_PROTOCOL; return;
        }
        put_string(w, m->active_skills[i]);
    }
    if (!put_count(w, m->tools, m->tool_count)) return;
    for (size_t i = 0U; i < m->tool_count && w->error == COG_OK; ++i) {
        const cog_instruction_tool_record_t *t = &m->tools[i];
        if (t->name == NULL || t->name[0] == '\0' || t->policy < COG_PROMPT_TOOL_ALLOW ||
            t->policy >= COG_PROMPT_TOOL_DENY) { w->error = COG_ERROR_PROTOCOL; return; }
        put_string(w, t->name); put_string(w, t->description); put_string(w, t->input_schema);
        put32(w, (uint32_t)t->policy);
    }
    if (!put_count(w, m->fragments, m->fragment_count)) return;
    for (size_t i = 0U; i < m->fragment_count && w->error == COG_OK; ++i) {
        const cog_prompt_fragment_t *f = &m->fragments[i];
        if (f->origin < COG_PROMPT_HOST || f->origin > COG_PROMPT_TOOL_GUIDANCE ||
            f->source_id == NULL || f->source_id[0] == '\0' || !ci_path(f->scope, true, NULL)) {
            w->error = COG_ERROR_PROTOCOL; return;
        }
        put32(w, (uint32_t)f->origin); put_string(w, f->source_id); put_string(w, f->scope);
        put_text(w, f->text, f->text_bytes);
    }
}
cog_result_t cog_instruction_manifest_encode(const cog_instruction_manifest_t *m,
    void *output, size_t capacity, size_t *required)
{
    if (m == NULL || required == NULL || (output == NULL && capacity != 0U)) return COG_ERROR_NULL_PTR;
    ci_writer measure = {0}; put_manifest(&measure, m);
    if (measure.error != COG_OK) return measure.error;
    if (capacity < measure.used) { *required = measure.used; return COG_ERROR_OVERFLOW; }
    ci_writer write = {.output=output}; put_manifest(&write, m);
    *required = write.used;
    return COG_OK;
}
cog_result_t cog_instruction_snapshot_fingerprint(const cog_instruction_snapshot_t *snapshot,
    unsigned char digest[32])
{
    if (snapshot == NULL || digest == NULL) return COG_ERROR_NULL_PTR;
    ci_writer measure = {0}; put_manifest(&measure, &snapshot->manifest);
    if (measure.error != COG_OK) return measure.error;
    if (measure.used > UINT64_MAX / 8U) return COG_ERROR_LIMIT;
    ci_sha hash; ci_sha_init(&hash);
    ci_writer write = {.hash=&hash}; put_manifest(&write, &snapshot->manifest);
    unsigned char result[32]; ci_sha_finish(&hash, result); memcpy(digest, result, sizeof(result));
    return COG_OK;
}

/* Decode first proves every count/string bound and computes exact temporary
 * arena storage. Only then may its explicit constructor allocate. Temporary
 * repeated wire strings are separate from snapshot ownership and are discarded
 * after semantic restore, which enforces the owning max_total_bytes ceiling. */
typedef struct ci_reader {
    const unsigned char *input;
    size_t length, at;
    const cog_instruction_limits_t *limits;
    unsigned char *storage;
    size_t used, capacity;
    cog_result_t error;
} ci_reader;
static const unsigned char *take(ci_reader *r, size_t n)
{
    if (r->error != COG_OK) return NULL;
    if (n > r->length - r->at) { r->error = COG_ERROR_PROTOCOL; return NULL; }
    const unsigned char *p = r->input + r->at; r->at += n; return p;
}
static uint32_t get32(ci_reader *r)
{
    const unsigned char *p=take(r,4U); uint32_t v=0U;
    if (p != NULL) for (size_t k=0U;k<4U;++k) v |= (uint32_t)p[k] << (8U*k);
    return v;
}
static uint64_t get64(ci_reader *r)
{
    const unsigned char *p=take(r,8U); uint64_t v=0U;
    if (p != NULL) for (size_t k=0U;k<8U;++k) v |= (uint64_t)p[k] << (8U*k);
    return v;
}
static void *reserve(ci_reader *r, size_t count_, size_t width)
{
    if (r->error != COG_OK || count_ == 0U) return NULL;
    const size_t align=alignof(max_align_t), padding=(align-r->used%align)%align;
    if (width != 0U && count_ > SIZE_MAX/width) { r->error=COG_ERROR_LIMIT; return NULL; }
    const size_t bytes=count_*width;
    if (padding > SIZE_MAX-r->used || bytes > SIZE_MAX-r->used-padding) { r->error=COG_ERROR_LIMIT; return NULL; }
    const size_t at=r->used+padding;
    r->used=at+bytes;
    if (r->storage == NULL) return NULL;
    if (r->used > r->capacity) { r->error=COG_ERROR_LIMIT; return NULL; }
    void *p=r->storage+at; memset(p,0,bytes); return p;
}
static const char *get_text_bytes(ci_reader *r, size_t n, size_t limit)
{
    if (r->error != COG_OK) return NULL;
    if (n > limit || n==SIZE_MAX || n>UINT32_MAX) { r->error=COG_ERROR_LIMIT; return NULL; }
    const unsigned char *p=take(r,n);
    if (p == NULL) return NULL;
    if (!ci_text((const char *)p,n,NULL)) { r->error=COG_ERROR_PROTOCOL; return NULL; }
    char *copy=reserve(r,n+1U,1U);
    if (copy != NULL) { if(n!=0U)memcpy(copy,p,n);copy[n]='\0'; }
    return copy;
}
static const char *get_string(ci_reader *r)
{
    const uint32_t n=get32(r);
    if (r->error != COG_OK || n==UINT32_MAX) return NULL;
    return get_text_bytes(r,(size_t)n,r->limits->max_string_bytes-1U);
}
static size_t get_size(ci_reader *r)
{
    const uint64_t n=get64(r);
    if(n>SIZE_MAX){r->error=COG_ERROR_LIMIT;return 0U;}
    return (size_t)n;
}
static size_t get_count(ci_reader *r, size_t limit, size_t minimum_bytes)
{
    const uint32_t n=get32(r);
    if (r->error != COG_OK) return 0U;
#if SIZE_MAX < UINT32_MAX
    if (n>SIZE_MAX) {r->error=COG_ERROR_LIMIT;return 0U;}
#endif
    if ((size_t)n>limit) {r->error=COG_ERROR_LIMIT;return 0U;}
    if (minimum_bytes != 0U && (size_t)n>(r->length-r->at)/minimum_bytes) {
        r->error=COG_ERROR_PROTOCOL;return 0U;
    }
    return (size_t)n;
}
static void get_entry(ci_reader *r, cog_instruction_entry_t *e)
{
    const uint32_t kind=get32(r);
    if(kind!=(uint32_t)COG_INSTRUCTION_SKILL){r->error=COG_ERROR_PROTOCOL;return;}
    e->kind=COG_INSTRUCTION_SKILL;
    e->name=get_string(r);e->description=get_string(r);e->root_id=get_string(r);e->path=get_string(r);
    e->model_hint=get_string(r);e->tool_names=get_string(r);e->skill_names=get_string(r);
    e->context_byte_limit=get_size(r);e->output_token_limit=get32(r);e->tool_round_limit=get32(r);
    const unsigned char *hash=take(r,32U);if(hash!=NULL)memcpy(e->sha256,hash,32U);
}
static void get_manifest(ci_reader *r, cog_instruction_manifest_t *m)
{
    const unsigned char *magic=take(r,4U);
    if(magic==NULL)return;
    if(memcmp(magic,"CGIM",4U)!=0){r->error=COG_ERROR_PROTOCOL;return;}
    const uint32_t version=get32(r);m->version=get32(r);
    if(r->error!=COG_OK)return;
    if(version!=1U || m->version!=1U){r->error=COG_ERROR_UNSUPPORTED;return;}
    m->catalog_generation=get64(r);m->snapshot_id=get64(r);
    m->workspace_root_id=get_string(r);m->scope=get_string(r);m->agent_name=get_string(r);
    m->model_id=get_string(r);m->template_id=get_string(r);m->template_revision=get32(r);
    m->max_context_bytes=get_size(r);m->max_output_tokens=get32(r);m->max_tool_rounds=get32(r);
    m->source_count=get_count(r,r->limits->max_sources,64U);
    cog_instruction_source_t *sources=reserve(r,m->source_count,sizeof(*sources));m->sources=sources;
    unsigned char (*hashes)[32]=reserve(r,m->source_count,sizeof(*hashes));
    m->source_sha256=(const unsigned char (*)[32])hashes;
    for(size_t i=0U;i<m->source_count && r->error==COG_OK;++i){
        cog_instruction_source_t s={0};const uint32_t kind=get32(r);
        if(kind>(uint32_t)COG_INSTRUCTION_RESOURCE){r->error=COG_ERROR_PROTOCOL;break;}
        s.kind=(cog_instruction_kind_t)kind;s.root_id=get_string(r);s.path=get_string(r);
        s.scope=get_string(r);s.owner_skill=get_string(r);s.precedence=get32(r);
        s.text_bytes=get_size(r);s.text=get_text_bytes(r,s.text_bytes,r->limits->max_source_bytes);
        const unsigned char *hash=take(r,32U);
        if(sources!=NULL)sources[i]=s;
        if(hashes!=NULL && hash!=NULL)memcpy(hashes[i],hash,32U);
    }
    m->available_skill_count=get_count(r,r->limits->max_entries,80U);
    cog_instruction_entry_t *skills=reserve(r,m->available_skill_count,sizeof(*skills));m->available_skills=skills;
    for(size_t i=0U;i<m->available_skill_count && r->error==COG_OK;++i){
        cog_instruction_entry_t e={0};get_entry(r,&e);if(skills!=NULL)skills[i]=e;
    }
    m->active_skill_count=get_count(r,r->limits->max_active_skills,4U);
    const char **active=reserve(r,m->active_skill_count,sizeof(*active));m->active_skills=active;
    for(size_t i=0U;i<m->active_skill_count && r->error==COG_OK;++i){
        const char *name=get_string(r);if(active!=NULL)active[i]=name;
    }
    m->tool_count=get_count(r,r->limits->max_tools,16U);
    cog_instruction_tool_record_t *tools=reserve(r,m->tool_count,sizeof(*tools));m->tools=tools;
    for(size_t i=0U;i<m->tool_count && r->error==COG_OK;++i){
        cog_instruction_tool_record_t t={0};t.name=get_string(r);t.description=get_string(r);t.input_schema=get_string(r);
        const uint32_t policy=get32(r);
        if(policy>=(uint32_t)COG_PROMPT_TOOL_DENY){r->error=COG_ERROR_PROTOCOL;break;}
        t.policy=(cog_prompt_tool_policy_t)policy;if(tools!=NULL)tools[i]=t;
    }
    m->fragment_count=get_count(r,r->limits->max_fragments,20U);
    cog_prompt_fragment_t *fragments=reserve(r,m->fragment_count,sizeof(*fragments));m->fragments=fragments;
    for(size_t i=0U;i<m->fragment_count && r->error==COG_OK;++i){
        cog_prompt_fragment_t f={0};const uint32_t origin=get32(r);
        if(origin>(uint32_t)COG_PROMPT_TOOL_GUIDANCE){r->error=COG_ERROR_PROTOCOL;break;}
        f.origin=(cog_prompt_origin_t)origin;f.source_id=get_string(r);f.scope=get_string(r);
        f.text_bytes=get_size(r);f.text=get_text_bytes(r,f.text_bytes,r->limits->max_total_bytes);
        if(fragments!=NULL)fragments[i]=f;
    }
    if(r->error==COG_OK && r->at!=r->length)r->error=COG_ERROR_PROTOCOL;
}
cog_result_t cog_instruction_snapshot_decode(cog_instruction_snapshot_t **out,
    const cog_instruction_limits_t *limits,const void *input,size_t input_bytes,
    cog_instruction_restore_mode_t mode,const cog_prompt_tool_binding_t *bindings,size_t count)
{
    if(out==NULL)return COG_ERROR_NULL_PTR;
    *out=NULL;
    if(limits==NULL || input==NULL || (bindings==NULL && count!=0U))return COG_ERROR_NULL_PTR;
    if(!ci_limits(limits) || (mode!=COG_INSTRUCTION_EXECUTE && mode!=COG_INSTRUCTION_INSPECT) ||
        (mode==COG_INSTRUCTION_INSPECT && (bindings!=NULL || count!=0U)))return COG_ERROR_INVALID_ARG;
    if(count>limits->max_tools)return COG_ERROR_LIMIT;
    ci_reader measure={.input=input,.length=input_bytes,.limits=limits};
    cog_instruction_manifest_t manifest={0};get_manifest(&measure,&manifest);
    if(measure.error!=COG_OK)return measure.error;
    unsigned char *storage=malloc(measure.used==0U?1U:measure.used);
    if(storage==NULL)return COG_ERROR_NOMEM;
    ci_reader reader={.input=input,.length=input_bytes,.limits=limits,.storage=storage,.capacity=measure.used};
    manifest=(cog_instruction_manifest_t){0};get_manifest(&reader,&manifest);
    cog_result_t rc=reader.error;
    if(rc==COG_OK) {
        rc=cog_instruction_snapshot_restore(out,limits,&manifest,mode,bindings,count,NULL);
        /* Caller mode/limits were checked before decoding. Remaining argument
         * and missing-selection errors describe malformed serialized data. */
        if(rc==COG_ERROR_INVALID_ARG || rc==COG_ERROR_NOT_FOUND ||
            (mode==COG_INSTRUCTION_INSPECT && rc==COG_ERROR_DENIED))rc=COG_ERROR_PROTOCOL;
    }
    /* Private source bytes do not survive the temporary constructor arena. */
    volatile unsigned char *wipe=storage;
    for(size_t i=0U;i<measure.used;++i)wipe[i]=0U;
    free(storage);
    return rc;
}

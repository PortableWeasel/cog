#ifndef COG_INSTRUCTION_INTERNAL_H
#define COG_INSTRUCTION_INTERNAL_H
#include <cog/instruction_codec.h>
#include <cog/instructions.h>
#include <cog/tool.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
typedef struct ci_arena {
    char *data;
    size_t used, capacity;
} ci_arena;
typedef struct ci_sha {
    uint32_t h[8];
    unsigned char block[64];
    size_t used;
    uint64_t bytes;
} ci_sha;
typedef struct ci_utf8 {
    uint32_t value, minimum;
    unsigned remaining;
} ci_utf8;
void ci_wipe(void *bytes, size_t capacity);
bool ci_add(size_t a, size_t b, size_t *out);
void *ci_array(size_t n, size_t width);
char *ci_copy(ci_arena *a, const char *s, size_t n);
char *ci_string(ci_arena *a, const char *s);
bool ci_utf8_feed(ci_utf8 *s, const unsigned char *p, size_t n);
bool ci_text(const char *s, size_t n, size_t *characters);
bool ci_path(const char *s, bool empty, size_t *depth);
bool ci_root(const char *s);
bool ci_name(const char *s);
bool ci_equal(const char *a, const char *b);
void ci_sha_init(ci_sha *s);
void ci_sha_update(ci_sha *s, const void *bytes, size_t n);
void ci_sha_finish(ci_sha *s, unsigned char out[32]);
void ci_hash(const void *p, size_t n, unsigned char out[32]);
void ci_diag(cog_instruction_diagnostic_t *e, cog_result_t rc, const cog_instruction_source_t *s,
             size_t offset, bool known);
bool ci_limits(const cog_instruction_limits_t *l);
cog_result_t ci_parse(ci_arena *a, const cog_instruction_limits_t *l,
                      const cog_instruction_source_t *s, cog_instruction_entry_t *entry,
                      size_t *body, size_t *error_offset);
cog_result_t ci_source_copy(ci_arena *, const cog_instruction_limits_t *,
                            const cog_instruction_source_t *, cog_instruction_source_t *);
cog_result_t ci_string_copy(ci_arena *, const cog_instruction_limits_t *, const char *, bool,
                            const char **);
struct cog_instruction_catalog {
    cog_instruction_limits_t limits;
    uint64_t generation;
    ci_arena arena;
    cog_instruction_source_t *sources;
    size_t source_count;
    cog_instruction_entry_t *entries;
    size_t entry_count;
    size_t *source_index, *body_offset;
};
struct cog_instruction_snapshot {
    cog_instruction_manifest_t manifest;
    ci_arena arena;
    cog_instruction_source_t *sources;
    unsigned char (*hashes)[32];
    cog_instruction_entry_t *skills;
    const char **active;
    cog_instruction_tool_record_t *tools;
    cog_prompt_tool_binding_t *bindings;
    cog_prompt_fragment_t *fragments;
    bool inspection;
};
#endif

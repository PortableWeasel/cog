/** @file prompt_internal.h @brief Private bounded prompt codec primitives. */
#ifndef COG_PROVIDER_PROMPT_INTERNAL_H
#define COG_PROVIDER_PROMPT_INTERNAL_H
#include <cog/provider_prompt.h>
#include <stdalign.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

typedef struct prompt_arena {
    unsigned char *data;
    size_t capacity, used;
    cog_result_t error;
} prompt_arena_t;

static inline void *prompt_reserve(prompt_arena_t *a, size_t bytes, size_t alignment)
{
    if (a->error != COG_OK) return NULL;
    size_t padding = (alignment - a->used % alignment) % alignment;
    if (padding > a->capacity - a->used || bytes > a->capacity - a->used - padding) {
        a->error = COG_ERROR_LIMIT; return NULL;
    }
    void *p = a->data + a->used + padding;
    a->used += padding + bytes;
    return p;
}
static inline void *prompt_array(prompt_arena_t *a, size_t n, size_t bytes)
{
    if (n > SIZE_MAX / bytes) { a->error = COG_ERROR_LIMIT; return NULL; }
    return n == 0U ? NULL : prompt_reserve(a, n * bytes, alignof(max_align_t));
}
static inline const char *prompt_copy(prompt_arena_t *a, const char *s, size_t bytes)
{
    if (s == NULL) return NULL;
    if (bytes == SIZE_MAX) { a->error = COG_ERROR_LIMIT; return NULL; }
    char *p = prompt_reserve(a, bytes + 1U, 1U);
    if (p != NULL) { if (bytes != 0U) memcpy(p, s, bytes); p[bytes] = '\0'; }
    return p;
}
static inline const char *prompt_string(prompt_arena_t *a, const char *s)
{ return s == NULL ? NULL : prompt_copy(a, s, strlen(s)); }

cog_result_t prompt_json(const char *text, size_t bytes, bool object,
    size_t max_depth, void *scratch, size_t scratch_bytes);
cog_result_t prompt_copy_history(prompt_arena_t *arena,
    const cog_provider_request_t *input, size_t prefix, cog_provider_request_t *output);
#endif

#include <cog/ipc.h>

#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define REPLAY_HEADER 47u

#include "ipc_replay_internal.h"

/* Strict UTF-8: reject NUL, overlong forms, surrogates and out-of-range codepoints. */
static int valid_utf8(const unsigned char *s, size_t n)
{
    size_t i = 0;
    while (i < n) {
        unsigned char c = s[i++];
        uint32_t cp;
        size_t k, j;
        if (c == 0) return 0;
        if (c < 0x80u) continue;
        if (c >= 0xc2u && c <= 0xdfu) { cp = (uint32_t)(c & 0x1fu); k = 1; }
        else if (c >= 0xe0u && c <= 0xefu) { cp = (uint32_t)(c & 0x0fu); k = 2; }
        else if (c >= 0xf0u && c <= 0xf4u) { cp = (uint32_t)(c & 0x07u); k = 3; }
        else return 0;
        if (k > n - i) return 0;
        for (j = 0; j < k; ++j) {
            c = s[i++];
            if ((c & 0xc0u) != 0x80u) return 0;
            cp = (cp << 6) | (uint32_t)(c & 0x3fu);
        }
        if ((k == 2 && cp < 0x800u) || (k == 3 && cp < 0x10000u) ||
            (cp >= 0xd800u && cp <= 0xdfffu) || cp > 0x10ffffu) return 0;
    }
    return 1;
}
static size_t id_len(const char *s)
{
    size_t n = 0;
    while (n < 128u && s[n] != '\0') ++n;
    return n;
}
static int valid_record(const cog_ipc_replay_record_t *r)
{
    if (r->generation == 0 || (r->payload_len != 0 && r->payload == NULL)) return 0;
    switch (r->kind) {
    case COG_IPC_REPLAY_DATA:
        return r->type != 0 && r->first_sequence != 0 &&
            r->first_sequence <= r->sequence && r->sequence <= r->last_sequence;
    case COG_IPC_REPLAY_GAP:
        return r->type == 0 && r->payload_len == 0 &&
            ((r->first_sequence == 0 && r->last_sequence == 0 && r->sequence == 0) ||
             (r->first_sequence != 0 && r->first_sequence <= r->last_sequence &&
              r->sequence == r->first_sequence - 1));
    case COG_IPC_REPLAY_CURSOR:
        return r->type == 0 && r->payload_len == 0 && r->first_sequence == 0 && r->last_sequence == 0;
    default: return 0;
    }
}

cog_result_t cog_ipc_replay_new(cog_ipc_replay_t **out, const char *session_id,
                                uint64_t generation, size_t max_entries, size_t max_bytes)
{
    cog_ipc_replay_t *r;
    size_t n;
    if (out == NULL) return COG_ERROR_NULL_PTR;
    *out = NULL;
    if (session_id == NULL) return COG_ERROR_NULL_PTR;
    n = id_len(session_id);
    if (n == 0 || n > 127u || !valid_utf8((const unsigned char *)session_id, n) ||
        generation == 0 || max_entries == 0 || max_bytes == 0 ||
        max_entries > SIZE_MAX / sizeof(replay_entry_t)) return COG_ERROR_INVALID_ARG;
    r = calloc(1, sizeof(*r));
    if (r == NULL) return COG_ERROR_NOMEM;
    r->entries = calloc(max_entries, sizeof(*r->entries));
    r->bytes = malloc(max_bytes);
    if (r->entries == NULL || r->bytes == NULL) { cog_ipc_replay_free(r); return COG_ERROR_NOMEM; }
    memcpy(r->session, session_id, n + 1);
    r->session_len = n; r->generation = generation; r->max_entries = max_entries; r->max_bytes = max_bytes;
    *out = r;
    return COG_OK;
}
void cog_ipc_replay_free(cog_ipc_replay_t *r)
{
    if (r != NULL) { free(r->entries); free(r->bytes); free(r); }
}
cog_result_t cog_ipc_replay_append(cog_ipc_replay_t *r, uint16_t type,
                                   const void *payload, size_t len, uint64_t *out_sequence)
{
    size_t remove = 0, removed_bytes = 0, i;
    if (r == NULL || out_sequence == NULL) return COG_ERROR_NULL_PTR;
    if (type == 0 || (payload == NULL && len != 0)) return COG_ERROR_INVALID_ARG;
    if (len > r->max_bytes || r->tail == UINT64_MAX) return COG_ERROR_LIMIT;
    while (r->count - remove >= r->max_entries || len > r->max_bytes - (r->used - removed_bytes)) {
        removed_bytes += r->entries[remove++].len;
    }
    if (remove != 0) {
        memmove(r->bytes, r->bytes + removed_bytes, r->used - removed_bytes);
        memmove(r->entries, r->entries + remove, (r->count - remove) * sizeof(*r->entries));
        r->count -= remove; r->used -= removed_bytes;
        for (i = 0; i < r->count; ++i) r->entries[i].offset -= removed_bytes;
    }
    r->entries[r->count].offset = r->used; r->entries[r->count].len = len; r->entries[r->count].type = type;
    if (len != 0) memcpy(r->bytes + r->used, payload, len);
    r->used += len; ++r->count; ++r->tail; *out_sequence = r->tail;
    return COG_OK;
}
cog_result_t cog_ipc_replay_copy_after(const cog_ipc_replay_t *r, uint64_t generation,
                                       uint64_t after, cog_ipc_replay_record_t *out,
                                       void *storage, size_t capacity, size_t *required)
{
    cog_ipc_replay_record_t rec = {0};
    uint64_t first = r != NULL && r->count != 0 ? r->tail - (uint64_t)r->count + 1u : 0;
    size_t len = 0, index = 0, need;
    unsigned char *dst = storage;
    if (r == NULL || out == NULL || required == NULL || (storage == NULL && capacity != 0)) return COG_ERROR_NULL_PTR;
    if (generation == r->generation && after > r->tail) return COG_ERROR_INVALID_ARG;
    rec.generation = r->generation; rec.first_sequence = first; rec.last_sequence = r->count ? r->tail : 0;
    if (generation != r->generation || (first != 0 && after < first - 1u)) {
        rec.kind = COG_IPC_REPLAY_GAP; rec.sequence = first ? first - 1u : 0;
    } else if (after == r->tail) { *required = 0; return COG_ERROR_AGAIN; }
    else {
        rec.kind = COG_IPC_REPLAY_DATA; rec.sequence = after + 1u;
        index = (size_t)(rec.sequence - first); len = r->entries[index].len;
        rec.type = r->entries[index].type; rec.payload_len = len;
    }
    if (len > SIZE_MAX - r->session_len - 1u) return COG_ERROR_LIMIT;
    need = r->session_len + 1u + len;
    *required = need;
    if (capacity < need) return COG_ERROR_OVERFLOW;
    memcpy(dst, r->session, r->session_len + 1u); rec.session_id = (const char *)dst;
    if (len != 0) { memcpy(dst + r->session_len + 1u, r->bytes + r->entries[index].offset, len); rec.payload = dst + r->session_len + 1u; }
    *out = rec;
    return COG_OK;
}
static void put(unsigned char *p, uint64_t v, size_t n)
{
    while (n != 0) { p[--n] = (unsigned char)(v & 0xffu); v >>= 8; }
}
static uint64_t get(const unsigned char *p, size_t n)
{
    uint64_t v = 0; size_t i;
    for (i = 0; i < n; ++i) v = (v << 8) | p[i];
    return v;
}
cog_result_t cog_ipc_replay_encode(const cog_ipc_replay_record_t *r, void *output,
                                   size_t capacity, size_t *required)
{
    unsigned char *p = output; size_t n, need;
    if (r == NULL || required == NULL || (output == NULL && capacity != 0)) return COG_ERROR_NULL_PTR;
    if (r->session_id == NULL) return COG_ERROR_NULL_PTR;
    n = id_len(r->session_id);
    if (n == 0 || n > 127u || !valid_utf8((const unsigned char *)r->session_id, n) || !valid_record(r)) return COG_ERROR_INVALID_ARG;
    if (r->payload_len > UINT32_MAX || r->payload_len > SIZE_MAX - REPLAY_HEADER - n) return COG_ERROR_LIMIT;
    need = REPLAY_HEADER + n + r->payload_len; *required = need;
    if (capacity < need) return COG_ERROR_OVERFLOW;
    memcpy(p, "CGR1", 4); p[4] = (unsigned char)r->kind;
    put(p+5, r->generation, 8); put(p+13, r->sequence, 8); put(p+21, r->first_sequence, 8); put(p+29, r->last_sequence, 8);
    put(p+37, r->type, 2); put(p+39, n, 4); put(p+43, r->payload_len, 4);
    memcpy(p+REPLAY_HEADER, r->session_id, n);
    if (r->payload_len != 0) memcpy(p+REPLAY_HEADER+n, r->payload, r->payload_len);
    return COG_OK;
}
cog_result_t cog_ipc_replay_decode(const void *input, size_t input_len, cog_ipc_replay_record_t *out,
                                   void *storage, size_t capacity, size_t *required)
{
    const unsigned char *p = input; unsigned char *dst = storage;
    cog_ipc_replay_record_t rec = {0}; size_t n, len, need;
    if (input == NULL || out == NULL || required == NULL || (storage == NULL && capacity != 0)) return COG_ERROR_NULL_PTR;
    if (input_len < 4u || memcmp(p, "CGR", 3) != 0) return COG_ERROR_PROTOCOL;
    if (p[3] != '1') return COG_ERROR_UNSUPPORTED;
    if (input_len < REPLAY_HEADER) return COG_ERROR_PROTOCOL;
    if (p[4] < 1u || p[4] > 3u) return COG_ERROR_UNSUPPORTED;
    rec.kind = (cog_ipc_replay_kind_t)p[4]; rec.generation = get(p+5,8); rec.sequence = get(p+13,8);
    rec.first_sequence = get(p+21,8); rec.last_sequence = get(p+29,8); rec.type = (uint16_t)get(p+37,2);
    n = (size_t)get(p+39,4); len = (size_t)get(p+43,4);
    if (n == 0 || n > 127u || n > input_len - REPLAY_HEADER || len != input_len - REPLAY_HEADER - n ||
        !valid_utf8(p+REPLAY_HEADER,n)) return COG_ERROR_PROTOCOL;
    rec.payload_len = len; rec.payload = len ? p+REPLAY_HEADER+n : NULL;
    if (!valid_record(&rec)) return COG_ERROR_PROTOCOL;
    if (len > SIZE_MAX - n - 1u) return COG_ERROR_PROTOCOL;
    need = n + 1u + len; *required = need;
    if (capacity < need) return COG_ERROR_OVERFLOW;
    memcpy(dst, p+REPLAY_HEADER, n); dst[n] = 0; rec.session_id = (const char *)dst;
    if (len != 0) { memcpy(dst+n+1u,p+REPLAY_HEADER+n,len); rec.payload = dst+n+1u; }
    *out = rec; return COG_OK;
}

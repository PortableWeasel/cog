/**
 * @file session_store.c
 * @brief Implementation of the pluggable session-persistence backend declared
 *        in cog/session_store.h.
 *
 * Ticket: 0efef06 (Stage 12.2d — Implement session_store.c)
 *
 * Design notes (the full executable contract is recorded in
 * libraries/cog-core/contracts/session_store/CONTRACT.md):
 *
 * - Two built-in backends, both plain `static const` vtable instances:
 *   `cog_store_file_backend` (default) and `cog_store_null_backend`.
 *   There is no backend registry and no global mutable state; the store
 *   struct is heap-allocated by cog_store_new and passed by pointer, and
 *   every operation dispatches through caller-supplied or built-in
 *   vtables.
 * - The file backend persists one directory per session:
 *     <base_dir>/<session_id>/meta.json
 *     <base_dir>/<session_id>/nodes.jsonl
 *     <base_dir>/<session_id>/index.bin
 *   meta.json holds session metadata (id, parent id, max_nodes,
 *   node_count, next_node_id, active node id).  nodes.jsonl is an
 *   append-only log of one JSON object per node, written in DFS
 *   pre-order so every parent precedes its children.  index.bin is an
 *   advisory node_id -> byte-offset cache (native byte order) maintained
 *   by save/append; load treats nodes.jsonl as the source of truth and a
 *   missing or corrupt index.bin is non-fatal (it is regenerated on the
 *   next full save).
 * - Message content is length-prefixed and may contain embedded NUL
 *   bytes; every string payload is base64-encoded inside the JSON
 *   objects so byte lengths survive round-trips exactly.  tool_args /
 *   tool_result / tool_name are NUL-terminated by construction in
 *   session.c (their internal lengths derive from strlen), so their wire
 *   representation is lossless through the public API.
 * - The store includes the private session layout (session_internal.h)
 *   so a load can restore node ids, raw provider fields, chunk counts,
 *   the active node, next_node_id and max_nodes exactly.  This serves
 *   the same single-definition purpose the header was created for; no
 *   public API is expanded.
 * - Null backend: every operation returns COG_OK and does nothing; load
 *   sets *out to NULL so the caller never receives uninitialised output.
 * - `compress` and `auto_save` are accepted and retained by the store.
 *   The file backend does not compress (the documented on-disk layout is
 *   plain JSONL + index; no compression is pretended).  `auto_save` has
 *   no trigger in this module: per cog/core.h the *core* auto-saves
 *   after each exchange when a store is configured, and that trigger
 *   lives in the core integration layer, so this module records the flag
 *   and documents the boundary honestly.
 * - Every session id is validated for path safety (no '/', '\', "." ,
 *   "..", control bytes; length capped at 128) before it is used as a
 *   directory name; hostile ids return COG_ERROR_INVALID_ARG /
 *   COG_ERROR_OVERFLOW.
 */
#define _POSIX_C_SOURCE 200809L

#include "cog/session_store.h"

#include "session_internal.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* Constants                                                           */
/* ------------------------------------------------------------------ */

/** Longest session id accepted by the store (path-safety bound). */
#define COG_STORE_ID_MAX_LEN ((size_t)128)

/** On-disk format version stored in meta.json. */
#define COG_STORE_FORMAT_VERSION ((uint64_t)1)

/** index.bin magic (8 bytes; data thereafter is native byte order). */
static const unsigned char kIndexMagic[8] = {'C', 'O', 'G', 'I',
                                            'D', 'X', '0', '1'};

#define COG_STORE_FILE_META "meta.json"
#define COG_STORE_FILE_NODES "nodes.jsonl"
#define COG_STORE_FILE_INDEX "index.bin"

/* ------------------------------------------------------------------ */
/* Internal layout of the opaque cog_store_t                           */
/* ------------------------------------------------------------------ */

struct cog_store {
    const cog_store_vtable_t *backend;
    char *base_dir;      /* owned copy; never NULL */
    size_t max_sessions; /* 0 = unlimited */
    bool auto_save;      /* retained; the trigger point is the core */
    bool compress;       /* retained; the file backend does not compress */
};

/* ------------------------------------------------------------------ */
/* Growable byte buffer                                                */
/* ------------------------------------------------------------------ */

struct cbuf {
    char *data;
    size_t len;
    size_t cap;
};

static cog_result_t cbuf_reserve(struct cbuf *b, size_t extra)
{
    size_t need;
    size_t ncap;
    char *nd;

    if (extra > SIZE_MAX - b->len) {
        return COG_ERROR_OVERFLOW;
    }
    need = b->len + extra;
    if (need <= b->cap) {
        return COG_OK;
    }
    ncap = (b->cap == 0U) ? 256U : b->cap;
    while (ncap < need) {
        if (ncap > SIZE_MAX / 2U) {
            ncap = need;
            break;
        }
        ncap *= 2U;
    }
    nd = realloc(b->data, ncap);
    if (nd == NULL) {
        return COG_ERROR_NOMEM;
    }
    b->data = nd;
    b->cap = ncap;
    return COG_OK;
}

static cog_result_t cbuf_puts(struct cbuf *b, const char *s)
{
    size_t n = strlen(s);
    cog_result_t rc = cbuf_reserve(b, n);

    if (rc != COG_OK) {
        return rc;
    }
    if (n > 0U) {
        (void)memcpy(b->data + b->len, s, n);
    }
    b->len += n;
    return COG_OK;
}

/* ------------------------------------------------------------------ */
/* Base64 (payload strings may contain embedded NUL bytes)            */
/* ------------------------------------------------------------------ */

static const char kB64Alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static char *b64_encode(const void *data, size_t len, size_t *out_len)
{
    const unsigned char *in = (const unsigned char *)data;
    size_t olen;
    size_t o;
    size_t i;
    char *out;

    if (len > SIZE_MAX - 2U) {
        return NULL; /* cannot be reached on hosted platforms; fail closed */
    }
    olen = ((len + 2U) / 3U) * 4U;
    out = malloc(olen + 1U);
    if (out == NULL) {
        return NULL;
    }
    o = 0U;
    for (i = 0U; i < len; i += 3U) {
        uint32_t v = (uint32_t)in[i] << 16U;
        size_t rem = len - i;

        if (rem > 1U) {
            v |= (uint32_t)in[i + 1U] << 8U;
        }
        if (rem > 2U) {
            v |= (uint32_t)in[i + 2U];
        }
        out[o++] = kB64Alphabet[(v >> 18U) & 0x3fU];
        out[o++] = kB64Alphabet[(v >> 12U) & 0x3fU];
        out[o++] = (rem > 1U) ? kB64Alphabet[(v >> 6U) & 0x3fU] : '=';
        out[o++] = (rem > 2U) ? kB64Alphabet[v & 0x3fU] : '=';
    }
    out[o] = '\0';
    if (out_len != NULL) {
        *out_len = olen;
    }
    return out;
}

static int b64_val(unsigned char c)
{
    if (c >= (unsigned char)'A' && c <= (unsigned char)'Z') {
        return (int)c - (int)'A';
    }
    if (c >= (unsigned char)'a' && c <= (unsigned char)'z') {
        return (int)c - (int)'a' + 26;
    }
    if (c >= (unsigned char)'0' && c <= (unsigned char)'9') {
        return (int)c - (int)'0' + 52;
    }
    if (c == (unsigned char)'+') {
        return 62;
    }
    if (c == (unsigned char)'/') {
        return 63;
    }
    return -1;
}

static cog_result_t b64_decode(const char *s, size_t slen,
                               unsigned char **out, size_t *out_len)
{
    size_t pad = 0U;
    size_t olen;
    size_t o;
    size_t i;
    unsigned char *buf;
    uint32_t acc = 0U;
    unsigned bits = 0U;

    *out = NULL;
    if (out_len != NULL) {
        *out_len = 0U;
    }
    if (slen % 4U != 0U) {
        return COG_ERROR_IO; /* the writer always emits padded base64 */
    }
    while (pad < slen && s[slen - 1U - pad] == '=') {
        pad++;
    }
    if (pad > 2U) {
        return COG_ERROR_IO;
    }
    /* Content bytes = 3 per 4-char group minus the trailing padding;
     * standard base64 allows a final group with 2 or 3 content chars
     * followed by 2 or 1 padding chars respectively. */
    olen = (slen / 4U) * 3U - pad;
    buf = malloc(olen + 1U);
    if (buf == NULL) {
        return COG_ERROR_NOMEM;
    }
    o = 0U;
    for (i = 0U; i < slen - pad; i++) {
        int v = b64_val((unsigned char)s[i]);

        if (v < 0) {
            free(buf);
            return COG_ERROR_IO;
        }
        acc = (acc << 6U) | (uint32_t)v;
        bits += 6U;
        if (bits >= 8U) {
            bits -= 8U;
            buf[o++] = (unsigned char)((acc >> bits) & 0xffU);
        }
    }
    if (o != olen) {
        free(buf);
        return COG_ERROR_IO;
    }
    buf[o] = '\0';
    *out = buf;
    if (out_len != NULL) {
        *out_len = olen;
    }
    return COG_OK;
}

/* ------------------------------------------------------------------ */
/* JSON subset scanner                                                */
/*                                                                     */
/* Strict by design: double-quoted strings (no escape sequences),       */
/* numbers, the literal null, and one level of object nesting.  Any     */
/* deviation fails the parse — load never guesses at malformed data.   */
/* ------------------------------------------------------------------ */

typedef struct json_scan {
    const char *p;
    const char *end;
} json_scan_t;

static void js_skip_ws(json_scan_t *j)
{
    while (j->p < j->end && isspace((unsigned char)*j->p) != 0) {
        j->p++;
    }
}

static bool js_char(json_scan_t *j, char c)
{
    js_skip_ws(j);
    if (j->p < j->end && *j->p == c) {
        j->p++;
        return true;
    }
    return false;
}

static bool js_null(json_scan_t *j)
{
    js_skip_ws(j);
    if ((size_t)(j->end - j->p) >= 4U &&
        memcmp(j->p, "null", 4U) == 0) {
        j->p += 4;
        return true;
    }
    return false;
}

/** Read a double-quoted string (any bytes except '"'; no escapes). */
static bool js_string(json_scan_t *j, const char **out, size_t *out_len)
{
    const char *start;
    const char *q;

    js_skip_ws(j);
    if (j->p >= j->end || *j->p != '"') {
        return false;
    }
    j->p++;
    start = j->p;
    q = memchr(j->p, '"', (size_t)(j->end - j->p));
    if (q == NULL) {
        return false;
    }
    j->p = q + 1;
    *out = start;
    *out_len = (size_t)(q - start);
    return true;
}

/** Read a number token ([0-9.eE+-]*); ','/ws terminate it. */
static bool js_number(json_scan_t *j, const char **out, size_t *out_len)
{
    const char *start = j->p;

    while (j->p < j->end &&
           (isdigit((unsigned char)*j->p) != 0 || *j->p == '.' ||
            *j->p == 'e' || *j->p == 'E' || *j->p == '+' || *j->p == '-')) {
        j->p++;
    }
    if (j->p == start) {
        return false;
    }
    *out = start;
    *out_len = (size_t)(j->p - start);
    return true;
}

static bool js_value_u64(json_scan_t *j, uint64_t *out)
{
    const char *tok = NULL;
    size_t toklen = 0U;
    size_t k;
    char tmp[32];
    char *endp = NULL;

    if (!js_number(j, &tok, &toklen)) {
        return false;
    }
    if (toklen == 0U || toklen >= sizeof(tmp)) {
        return false;
    }
    for (k = 0U; k < toklen; k++) {
        if (isdigit((unsigned char)tok[k]) == 0) {
            return false; /* uint64 fields must be plain digits */
        }
    }
    (void)memcpy(tmp, tok, toklen);
    tmp[toklen] = '\0';
    errno = 0;
    *out = (uint64_t)strtoull(tmp, &endp, 10);
    if (errno != 0 || endp != tmp + toklen) {
        return false;
    }
    return true;
}

static bool js_value_double(json_scan_t *j, double *out)
{
    const char *tok = NULL;
    size_t toklen = 0U;
    char tmp[64];
    char *endp = NULL;

    if (!js_number(j, &tok, &toklen)) {
        return false;
    }
    if (toklen == 0U || toklen >= sizeof(tmp)) {
        return false;
    }
    (void)memcpy(tmp, tok, toklen);
    tmp[toklen] = '\0';
    errno = 0;
    *out = strtod(tmp, &endp);
    if (errno != 0 || endp != tmp + toklen) {
        return false;
    }
    return true;
}

/** After '{', read one `"key":value` pair; false at the closing '}'. */
static bool js_obj_field(json_scan_t *j, char *key, size_t keycap)
{
    const char *ks = NULL;
    size_t klen = 0U;

    if (js_char(j, '}')) {
        return false;
    }
    if (!js_string(j, &ks, &klen) || klen == 0U || klen >= keycap) {
        return false;
    }
    (void)memcpy(key, ks, klen);
    key[klen] = '\0';
    return js_char(j, ':');
}

/* ------------------------------------------------------------------ */
/* Node record (one JSONL line in nodes.jsonl)                         */
/* ------------------------------------------------------------------ */

typedef struct cog_node_record {
    uint64_t node_id;
    uint64_t parent_id;
    int role; /* -1 while unset */
    char *content;
    size_t content_len;
    char *tool_name;
    size_t tool_name_len;
    char *tool_args;
    size_t tool_args_len;
    char *tool_result;
    size_t tool_result_len;
    char *raw_request;
    size_t raw_request_len;
    char *raw_response;
    size_t raw_response_len;
    char *model;
    size_t model_len;
    char *provider;
    size_t provider_len;
    size_t chunk_count;
    cog_timing_t timing;
    cog_cost_t cost;
} cog_node_record_t;

static void node_record_free(cog_node_record_t *r)
{
    free(r->content);
    free(r->tool_name);
    free(r->tool_args);
    free(r->tool_result);
    free(r->raw_request);
    free(r->raw_response);
    free(r->model);
    free(r->provider);
    memset(r, 0, sizeof(*r));
    r->role = -1;
}

/** Decode a "key":"<b64>" / "key":null field into the record. */
static cog_result_t node_record_field_str(json_scan_t *j,
                                          char **field, size_t *field_len)
{
    const char *val = NULL;
    size_t vlen = 0U;
    unsigned char *dec = NULL;
    size_t declen = 0U;
    cog_result_t rc;

    if (js_null(j)) {
        return COG_OK; /* field stays NULL */
    }
    if (!js_string(j, &val, &vlen)) {
        return COG_ERROR_IO;
    }
    rc = b64_decode(val, vlen, &dec, &declen);
    if (rc != COG_OK) {
        return rc;
    }
    *field = (char *)dec;
    *field_len = declen;
    return COG_OK;
}

static cog_result_t node_record_field_u64(json_scan_t *j, uint64_t *out)
{
    if (js_null(j)) {
        return COG_ERROR_IO; /* numeric fields must be present */
    }
    if (!js_value_u64(j, out)) {
        return COG_ERROR_IO;
    }
    return COG_OK;
}

static cog_result_t timing_parse(json_scan_t *j, cog_timing_t *t)
{
    char key[32];

    if (!js_char(j, '{')) {
        return COG_ERROR_IO;
    }
    while (true) {
        if (!js_obj_field(j, key, sizeof(key))) {
            return COG_ERROR_IO;
        }
        if (strcmp(key, "request_sent_ms") == 0) {
            if (!js_value_u64(j, &t->request_sent_ms)) {
                return COG_ERROR_IO;
            }
        } else if (strcmp(key, "first_token_ms") == 0) {
            if (!js_value_u64(j, &t->first_token_ms)) {
                return COG_ERROR_IO;
            }
        } else if (strcmp(key, "last_token_ms") == 0) {
            if (!js_value_u64(j, &t->last_token_ms)) {
                return COG_ERROR_IO;
            }
        } else if (strcmp(key, "duration_ms") == 0) {
            if (!js_value_u64(j, &t->duration_ms)) {
                return COG_ERROR_IO;
            }
        } else if (strcmp(key, "ttft_ms") == 0) {
            if (!js_value_u64(j, &t->ttft_ms)) {
                return COG_ERROR_IO;
            }
        } else if (strcmp(key, "tokens_per_second") == 0) {
            if (!js_value_double(j, &t->tokens_per_second)) {
                return COG_ERROR_IO;
            }
        } else {
            return COG_ERROR_IO; /* unknown key: strict schema */
        }
        if (js_char(j, '}')) {
            return COG_OK;
        }
        if (!js_char(j, ',')) {
            return COG_ERROR_IO;
        }
    }
}

static cog_result_t cost_parse(json_scan_t *j, cog_cost_t *c)
{
    char key[32];

    if (!js_char(j, '{')) {
        return COG_ERROR_IO;
    }
    while (true) {
        if (!js_obj_field(j, key, sizeof(key))) {
            return COG_ERROR_IO;
        }
        if (strcmp(key, "input_tokens") == 0) {
            if (!js_value_u64(j, &c->input_tokens)) {
                return COG_ERROR_IO;
            }
        } else if (strcmp(key, "output_tokens") == 0) {
            if (!js_value_u64(j, &c->output_tokens)) {
                return COG_ERROR_IO;
            }
        } else if (strcmp(key, "cost_per_input_token") == 0) {
            if (!js_value_double(j, &c->cost_per_input_token)) {
                return COG_ERROR_IO;
            }
        } else if (strcmp(key, "cost_per_output_token") == 0) {
            if (!js_value_double(j, &c->cost_per_output_token)) {
                return COG_ERROR_IO;
            }
        } else if (strcmp(key, "total_cost") == 0) {
            if (!js_value_double(j, &c->total_cost)) {
                return COG_ERROR_IO;
            }
        } else {
            return COG_ERROR_IO;
        }
        if (js_char(j, '}')) {
            return COG_OK;
        }
        if (!js_char(j, ',')) {
            return COG_ERROR_IO;
        }
    }
}

/** Parse one nodes.jsonl line into @p r.  On failure @p r is freed. */
static cog_result_t node_record_parse(const char *line, size_t len,
                                      cog_node_record_t *r)
{
    json_scan_t j;
    char key[16];
    cog_result_t rc;

    memset(r, 0, sizeof(*r));
    r->role = -1;
    j.p = line;
    j.end = line + len;
    if (!js_char(&j, '{')) {
        return COG_ERROR_IO;
    }
    while (true) {
        rc = COG_OK;
        if (!js_obj_field(&j, key, sizeof(key))) {
            rc = COG_ERROR_IO;
        } else if (strcmp(key, "node_id") == 0) {
            rc = node_record_field_u64(&j, &r->node_id);
        } else if (strcmp(key, "parent_id") == 0) {
            rc = node_record_field_u64(&j, &r->parent_id);
        } else if (strcmp(key, "role") == 0) {
            uint64_t v = 0U;

            rc = node_record_field_u64(&j, &v);
            if (rc == COG_OK) {
                if (v > 4U) {
                    rc = COG_ERROR_IO;
                } else {
                    r->role = (int)v;
                }
            }
        } else if (strcmp(key, "content") == 0) {
            rc = node_record_field_str(&j, &r->content, &r->content_len);
        } else if (strcmp(key, "tool_name") == 0) {
            rc = node_record_field_str(&j, &r->tool_name, &r->tool_name_len);
        } else if (strcmp(key, "tool_args") == 0) {
            rc = node_record_field_str(&j, &r->tool_args, &r->tool_args_len);
        } else if (strcmp(key, "tool_result") == 0) {
            rc = node_record_field_str(&j, &r->tool_result,
                                       &r->tool_result_len);
        } else if (strcmp(key, "raw_request") == 0) {
            rc = node_record_field_str(&j, &r->raw_request,
                                       &r->raw_request_len);
        } else if (strcmp(key, "raw_response") == 0) {
            rc = node_record_field_str(&j, &r->raw_response,
                                       &r->raw_response_len);
        } else if (strcmp(key, "chunk_count") == 0) {
            uint64_t v = 0U;

            rc = node_record_field_u64(&j, &v);
            if (rc == COG_OK) {
                if (v > (uint64_t)SIZE_MAX) {
                    rc = COG_ERROR_IO;
                } else {
                    r->chunk_count = (size_t)v;
                }
            }
        } else if (strcmp(key, "timing") == 0) {
            rc = timing_parse(&j, &r->timing);
        } else if (strcmp(key, "cost") == 0) {
            rc = cost_parse(&j, &r->cost);
        } else if (strcmp(key, "model") == 0) {
            rc = node_record_field_str(&j, &r->model, &r->model_len);
        } else if (strcmp(key, "provider") == 0) {
            rc = node_record_field_str(&j, &r->provider, &r->provider_len);
        } else {
            rc = COG_ERROR_IO; /* unknown key: strict schema */
        }
        if (rc != COG_OK) {
            node_record_free(r);
            return rc;
        }
        if (js_char(&j, '}')) {
            break;
        }
        if (!js_char(&j, ',')) {
            node_record_free(r);
            return COG_ERROR_IO;
        }
    }
    js_skip_ws(&j);
    if (j.p != j.end || r->role < 0) {
        node_record_free(r);
        return COG_ERROR_IO;
    }
    return COG_OK;
}

/* ------------------------------------------------------------------ */
/* JSON writers                                                        */
/* ------------------------------------------------------------------ */

static cog_result_t field_b64(struct cbuf *b, const char *key,
                              const void *data, size_t len)
{
    cog_result_t rc;
    char *enc = NULL;
    size_t enclen = 0U;

    rc = cbuf_puts(b, "\"");
    if (rc != COG_OK) {
        return rc;
    }
    rc = cbuf_puts(b, key);
    if (rc != COG_OK) {
        return rc;
    }
    rc = cbuf_puts(b, "\":");
    if (rc != COG_OK) {
        return rc;
    }
    if (data == NULL) {
        return cbuf_puts(b, "null");
    }
    enc = b64_encode(data, len, &enclen);
    if (enc == NULL) {
        return COG_ERROR_NOMEM;
    }
    rc = cbuf_puts(b, "\"");
    if (rc == COG_OK) {
        rc = cbuf_puts(b, enc);
    }
    if (rc == COG_OK) {
        rc = cbuf_puts(b, "\"");
    }
    free(enc);
    return rc;
}

static cog_result_t field_cb64(struct cbuf *b, const char *key,
                               const char *s, size_t len)
{
    return field_b64(b, key, (const void *)s, len);
}

static cog_result_t field_u64(struct cbuf *b, const char *key, uint64_t v)
{
    char num[32];
    int n = snprintf(num, sizeof(num), "%llu", (unsigned long long)v);
    cog_result_t rc;

    if (n < 0 || (size_t)n >= sizeof(num)) {
        return COG_ERROR_IO;
    }
    rc = cbuf_puts(b, "\"");
    if (rc != COG_OK) {
        return rc;
    }
    rc = cbuf_puts(b, key);
    if (rc != COG_OK) {
        return rc;
    }
    rc = cbuf_puts(b, "\":");
    if (rc != COG_OK) {
        return rc;
    }
    return cbuf_puts(b, num);
}

static cog_result_t field_dbl(struct cbuf *b, const char *key, double v)
{
    char num[64];
    int n = snprintf(num, sizeof(num), "%.17g", v);
    cog_result_t rc;

    if (n < 0 || (size_t)n >= sizeof(num)) {
        return COG_ERROR_IO;
    }
    rc = cbuf_puts(b, "\"");
    if (rc != COG_OK) {
        return rc;
    }
    rc = cbuf_puts(b, key);
    if (rc != COG_OK) {
        return rc;
    }
    rc = cbuf_puts(b, "\":");
    if (rc != COG_OK) {
        return rc;
    }
    return cbuf_puts(b, num);
}

/** Serialize one node as a single JSONL record (no trailing newline). */
static cog_result_t node_record_write(struct cbuf *b,
                                      const cog_node_data_t *d)
{
    cog_result_t rc;

    rc = cbuf_puts(b, "{");
    if (rc != COG_OK) {
        return rc;
    }
    rc = field_u64(b, "node_id", d->node_id);
    if (rc != COG_OK) {
        return rc;
    }
    rc = cbuf_puts(b, ",");
    if (rc != COG_OK) {
        return rc;
    }
    rc = field_u64(b, "parent_id", d->parent_id);
    if (rc != COG_OK) {
        return rc;
    }
    rc = cbuf_puts(b, ",");
    if (rc != COG_OK) {
        return rc;
    }
    rc = field_u64(b, "role", (uint64_t)d->role);
    if (rc != COG_OK) {
        return rc;
    }
    rc = cbuf_puts(b, ",");
    if (rc != COG_OK) {
        return rc;
    }
    rc = field_cb64(b, "content", d->content, d->content_len);
    if (rc != COG_OK) {
        return rc;
    }
    rc = cbuf_puts(b, ",");
    if (rc != COG_OK) {
        return rc;
    }
    rc = field_cb64(b, "tool_name", d->tool_name,
                    (d->tool_name != NULL) ? strlen(d->tool_name) : 0U);
    if (rc != COG_OK) {
        return rc;
    }
    rc = cbuf_puts(b, ",");
    if (rc != COG_OK) {
        return rc;
    }
    rc = field_cb64(b, "tool_args", d->tool_args,
                    (d->tool_args != NULL) ? strlen(d->tool_args) : 0U);
    if (rc != COG_OK) {
        return rc;
    }
    rc = cbuf_puts(b, ",");
    if (rc != COG_OK) {
        return rc;
    }
    rc = field_cb64(b, "tool_result", d->tool_result,
                    (d->tool_result != NULL) ? strlen(d->tool_result) : 0U);
    if (rc != COG_OK) {
        return rc;
    }
    rc = cbuf_puts(b, ",");
    if (rc != COG_OK) {
        return rc;
    }
    rc = field_cb64(b, "raw_request", d->raw_request,
                    (d->raw_request != NULL) ? strlen(d->raw_request) : 0U);
    if (rc != COG_OK) {
        return rc;
    }
    rc = cbuf_puts(b, ",");
    if (rc != COG_OK) {
        return rc;
    }
    rc = field_cb64(b, "raw_response", d->raw_response,
                    (d->raw_response != NULL) ? strlen(d->raw_response)
                                               : 0U);
    if (rc != COG_OK) {
        return rc;
    }
    rc = cbuf_puts(b, ",");
    if (rc != COG_OK) {
        return rc;
    }
    rc = field_u64(b, "chunk_count", (uint64_t)d->chunk_count);
    if (rc != COG_OK) {
        return rc;
    }
    rc = cbuf_puts(b, ",");
    if (rc != COG_OK) {
        return rc;
    }
    rc = cbuf_puts(b, "\"timing\":{");
    if (rc != COG_OK) {
        return rc;
    }
    rc = field_u64(b, "request_sent_ms", d->timing.request_sent_ms);
    if (rc != COG_OK) {
        return rc;
    }
    rc = cbuf_puts(b, ",");
    if (rc != COG_OK) {
        return rc;
    }
    rc = field_u64(b, "first_token_ms", d->timing.first_token_ms);
    if (rc != COG_OK) {
        return rc;
    }
    rc = cbuf_puts(b, ",");
    if (rc != COG_OK) {
        return rc;
    }
    rc = field_u64(b, "last_token_ms", d->timing.last_token_ms);
    if (rc != COG_OK) {
        return rc;
    }
    rc = cbuf_puts(b, ",");
    if (rc != COG_OK) {
        return rc;
    }
    rc = field_u64(b, "duration_ms", d->timing.duration_ms);
    if (rc != COG_OK) {
        return rc;
    }
    rc = cbuf_puts(b, ",");
    if (rc != COG_OK) {
        return rc;
    }
    rc = field_u64(b, "ttft_ms", d->timing.ttft_ms);
    if (rc != COG_OK) {
        return rc;
    }
    rc = cbuf_puts(b, ",");
    if (rc != COG_OK) {
        return rc;
    }
    rc = field_dbl(b, "tokens_per_second", d->timing.tokens_per_second);
    if (rc != COG_OK) {
        return rc;
    }
    rc = cbuf_puts(b, "},\"cost\":{");
    if (rc != COG_OK) {
        return rc;
    }
    rc = field_u64(b, "input_tokens", d->cost.input_tokens);
    if (rc != COG_OK) {
        return rc;
    }
    rc = cbuf_puts(b, ",");
    if (rc != COG_OK) {
        return rc;
    }
    rc = field_u64(b, "output_tokens", d->cost.output_tokens);
    if (rc != COG_OK) {
        return rc;
    }
    rc = cbuf_puts(b, ",");
    if (rc != COG_OK) {
        return rc;
    }
    rc = field_dbl(b, "cost_per_input_token", d->cost.cost_per_input_token);
    if (rc != COG_OK) {
        return rc;
    }
    rc = cbuf_puts(b, ",");
    if (rc != COG_OK) {
        return rc;
    }
    rc = field_dbl(b, "cost_per_output_token", d->cost.cost_per_output_token);
    if (rc != COG_OK) {
        return rc;
    }
    rc = cbuf_puts(b, ",");
    if (rc != COG_OK) {
        return rc;
    }
    rc = field_dbl(b, "total_cost", d->cost.total_cost);
    if (rc != COG_OK) {
        return rc;
    }
    rc = cbuf_puts(b, "},");
    if (rc != COG_OK) {
        return rc;
    }
    rc = field_cb64(b, "model", d->model,
                    (d->model != NULL) ? strlen(d->model) : 0U);
    if (rc != COG_OK) {
        return rc;
    }
    rc = cbuf_puts(b, ",");
    if (rc != COG_OK) {
        return rc;
    }
    rc = field_cb64(b, "provider", d->provider,
                    (d->provider != NULL) ? strlen(d->provider) : 0U);
    if (rc != COG_OK) {
        return rc;
    }
    return cbuf_puts(b, "}");
}

static cog_result_t meta_write(struct cbuf *b, const char *id,
                               const char *parent_id, size_t max_nodes,
                               size_t node_count, uint64_t next_node_id,
                               uint64_t active_node_id)
{
    cog_result_t rc;

    rc = cbuf_puts(b, "{");
    if (rc != COG_OK) {
        return rc;
    }
    rc = field_u64(b, "version", COG_STORE_FORMAT_VERSION);
    if (rc != COG_OK) {
        return rc;
    }
    rc = cbuf_puts(b, ",");
    if (rc != COG_OK) {
        return rc;
    }
    rc = field_cb64(b, "id", id, strlen(id));
    if (rc != COG_OK) {
        return rc;
    }
    rc = cbuf_puts(b, ",");
    if (rc != COG_OK) {
        return rc;
    }
    rc = field_b64(b, "parent_id", (const void *)parent_id,
                   (parent_id != NULL) ? strlen(parent_id) : 0U);
    if (rc != COG_OK) {
        return rc;
    }
    rc = cbuf_puts(b, ",");
    if (rc != COG_OK) {
        return rc;
    }
    rc = field_u64(b, "max_nodes", (uint64_t)max_nodes);
    if (rc != COG_OK) {
        return rc;
    }
    rc = cbuf_puts(b, ",");
    if (rc != COG_OK) {
        return rc;
    }
    rc = field_u64(b, "node_count", (uint64_t)node_count);
    if (rc != COG_OK) {
        return rc;
    }
    rc = cbuf_puts(b, ",");
    if (rc != COG_OK) {
        return rc;
    }
    rc = field_u64(b, "next_node_id", next_node_id);
    if (rc != COG_OK) {
        return rc;
    }
    rc = cbuf_puts(b, ",");
    if (rc != COG_OK) {
        return rc;
    }
    rc = field_u64(b, "active_node_id", active_node_id);
    if (rc != COG_OK) {
        return rc;
    }
    return cbuf_puts(b, "}");
}

/* ------------------------------------------------------------------ */
/* Filesystem helpers                                                  */
/* ------------------------------------------------------------------ */

static char *path_join2(const char *a, const char *b)
{
    size_t la = strlen(a);
    size_t lb = strlen(b);
    char *p;

    if (la > SIZE_MAX - lb - 2U) {
        return NULL;
    }
    p = malloc(la + 1U + lb + 1U);
    if (p == NULL) {
        return NULL;
    }
    (void)memcpy(p, a, la);
    p[la] = '/';
    (void)memcpy(p + la + 1U, b, lb + 1U); /* includes the NUL */
    return p;
}

static cog_result_t ensure_dir(const char *path)
{
    if (mkdir(path, 0700) == 0) {
        return COG_OK;
    }
    if (errno == EEXIST) {
        return COG_OK;
    }
    return COG_ERROR_IO;
}

static cog_result_t read_file(const char *path, char **out, size_t *out_len)
{
    FILE *f;
    long size;
    char *buf;
    size_t got;

    *out = NULL;
    *out_len = 0U;
    f = fopen(path, "rb");
    if (f == NULL) {
        return COG_ERROR_IO;
    }
    if (fseek(f, 0L, SEEK_END) != 0) {
        (void)fclose(f);
        return COG_ERROR_IO;
    }
    size = ftell(f);
    if (size < 0L) {
        (void)fclose(f);
        return COG_ERROR_IO;
    }
    if (fseek(f, 0L, SEEK_SET) != 0) {
        (void)fclose(f);
        return COG_ERROR_IO;
    }
    buf = malloc((size_t)size + 1U);
    if (buf == NULL) {
        (void)fclose(f);
        return COG_ERROR_NOMEM;
    }
    got = fread(buf, 1U, (size_t)size, f);
    if (got != (size_t)size) {
        free(buf);
        (void)fclose(f);
        return COG_ERROR_IO;
    }
    if (fclose(f) != 0) {
        free(buf);
        return COG_ERROR_IO;
    }
    buf[(size_t)size] = '\0';
    *out = buf;
    *out_len = (size_t)size;
    return COG_OK;
}

/** Write @p data then atomically rename @p tmp_path over @p final_path. */
static cog_result_t write_file_atomic(const char *final_path,
                                      const char *tmp_path,
                                      const char *data, size_t len)
{
    FILE *f = fopen(tmp_path, "wb");

    if (f == NULL) {
        return COG_ERROR_IO;
    }
    if (len > 0U && fwrite(data, 1U, len, f) != len) {
        (void)fclose(f);
        (void)remove(tmp_path);
        return COG_ERROR_IO;
    }
    if (fclose(f) != 0) {
        (void)remove(tmp_path);
        return COG_ERROR_IO;
    }
    if (rename(tmp_path, final_path) != 0) {
        (void)remove(tmp_path);
        return COG_ERROR_IO;
    }
    return COG_OK;
}

/**
 * Delete one session directory after preflighting its complete contents.
 *
 * Unknown entries and known names that are directories are rejected before
 * anything is unlinked, so an expected rmdir obstruction (for example a
 * stray file) cannot leave a ghost session with its metadata removed.
 * Known entries are then unlinked and the directory is removed.
 *
 * The store's single-threaded contract covers callers of this API; it does
 * not make directory enumeration plus unlinking atomic against unrelated
 * external filesystem mutation.  Such concurrent mutation is unsupported
 * and can still produce COG_ERROR_IO after a partial unlink.  Within the
 * documented threading model, all expected content-related failures are
 * detected by this preflight and leave the session intact.
 */
static cog_result_t delete_session_dir(const char *dir)
{
    static const char *const kFiles[] = {
        COG_STORE_FILE_META,
        COG_STORE_FILE_NODES,
        COG_STORE_FILE_INDEX,
        COG_STORE_FILE_META ".tmp",
        COG_STORE_FILE_NODES ".tmp",
        COG_STORE_FILE_INDEX ".tmp",
    };
    const size_t file_count = sizeof(kFiles) / sizeof(kFiles[0]);
    DIR *dp;
    struct dirent *de;
    size_t i;

    dp = opendir(dir);
    if (dp == NULL) {
        return COG_ERROR_IO;
    }
    while (true) {
        errno = 0;
        de = readdir(dp);
        if (de == NULL) {
            if (errno != 0) {
                (void)closedir(dp);
                return COG_ERROR_IO; /* incomplete preflight: do nothing */
            }
            break;
        }
        if (strcmp(de->d_name, ".") == 0 ||
            strcmp(de->d_name, "..") == 0) {
            continue;
        }
        for (i = 0U; i < file_count; i++) {
            if (strcmp(de->d_name, kFiles[i]) == 0) {
                break;
            }
        }
        if (i == file_count) {
            (void)closedir(dp);
            return COG_ERROR_IO; /* unknown entry: nothing was removed */
        }
    }
    if (closedir(dp) != 0) {
        return COG_ERROR_IO;
    }

    /* A directory masquerading as a known file would make unlink fail
     * after earlier files had been removed; reject it during preflight. */
    for (i = 0U; i < file_count; i++) {
        char *p = path_join2(dir, kFiles[i]);
        struct stat sb;

        if (p == NULL) {
            return COG_ERROR_NOMEM;
        }
        if (lstat(p, &sb) == 0) {
            if (S_ISDIR(sb.st_mode)) {
                free(p);
                return COG_ERROR_IO;
            }
        } else if (errno != ENOENT) {
            free(p);
            return COG_ERROR_IO;
        }
        free(p);
    }

    for (i = 0U; i < file_count; i++) {
        char *p = path_join2(dir, kFiles[i]);
        int e;

        if (p == NULL) {
            return COG_ERROR_NOMEM;
        }
        e = unlink(p);
        free(p);
        if (e != 0 && errno != ENOENT) {
            return COG_ERROR_IO;
        }
    }
    if (rmdir(dir) != 0) {
        return COG_ERROR_IO;
    }
    return COG_OK;
}

/* ------------------------------------------------------------------ */
/* Id validation / path safety                                         */
/* ------------------------------------------------------------------ */

static cog_result_t store_validate_id(const char *id)
{
    size_t i;
    size_t n;

    if (id == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    n = strlen(id);
    if (n == 0U) {
        return COG_ERROR_INVALID_ARG;
    }
    if (n > COG_STORE_ID_MAX_LEN) {
        return COG_ERROR_OVERFLOW;
    }
    if (strcmp(id, ".") == 0 || strcmp(id, "..") == 0) {
        return COG_ERROR_INVALID_ARG;
    }
    for (i = 0U; i < n; i++) {
        unsigned char c = (unsigned char)id[i];

        if (c == '/' || c == '\\') {
            return COG_ERROR_INVALID_ARG;
        }
        if (c < 0x20U || c == 0x7fU) {
            return COG_ERROR_INVALID_ARG;
        }
    }
    return COG_OK;
}

/* ------------------------------------------------------------------ */
/* index.bin (advisory node_id -> offset cache)                       */
/* ------------------------------------------------------------------ */

/** True if the index is consistent and @p new_id can append in order. */
static bool index_can_extend(const char *path, uint64_t new_id)
{
    FILE *f;
    unsigned char magic[8];
    uint64_t count = 0U;
    uint64_t last_id = 0U;

    f = fopen(path, "rb");
    if (f == NULL) {
        return false;
    }
    if (fread(magic, 1U, sizeof(magic), f) != sizeof(magic) ||
        memcmp(magic, kIndexMagic, sizeof(kIndexMagic)) != 0) {
        (void)fclose(f);
        return false;
    }
    if (fread(&count, 1U, sizeof(count), f) != sizeof(count)) {
        (void)fclose(f);
        return false;
    }
    if (count == 0U) {
        (void)fclose(f);
        return true;
    }
    if (count > (uint64_t)(SIZE_MAX / 16U)) {
        (void)fclose(f);
        return false;
    }
    if (fseek(f, (long)(16U + (count - 1U) * 16U), SEEK_SET) != 0) {
        (void)fclose(f);
        return false;
    }
    if (fread(&last_id, 1U, sizeof(last_id), f) != sizeof(last_id)) {
        (void)fclose(f);
        return false;
    }
    (void)fclose(f);
    return new_id > last_id;
}

/**
 * Append one (node_id, offset) entry to index.bin when that can be done
 * cheaply and consistently; otherwise leave the cache stale (the next
 * full save regenerates it).  Returns COG_OK either way; *did reports
 * whether the cache was updated.
 */
static cog_result_t index_append(const char *path, uint64_t node_id,
                                 uint64_t offset, bool *did)
{
    FILE *f;

    *did = false;
    if (!index_can_extend(path, node_id)) {
        return COG_OK;
    }
    f = fopen(path, "r+b");
    if (f == NULL) {
        return COG_OK; /* cache is advisory; non-fatal */
    }
    if (fseek(f, 0L, SEEK_END) != 0) {
        (void)fclose(f);
        return COG_OK;
    }
    if (fwrite(&node_id, 1U, sizeof(node_id), f) != sizeof(node_id) ||
        fwrite(&offset, 1U, sizeof(offset), f) != sizeof(offset)) {
        (void)fclose(f);
        return COG_OK;
    }
    if (fclose(f) != 0) {
        return COG_OK;
    }
    *did = true;
    return COG_OK;
}

/* ------------------------------------------------------------------ */
/* Null backend                                                        */
/* ------------------------------------------------------------------ */

static cog_result_t null_save(void *store, const cog_session_t *session)
{
    (void)store;
    (void)session;
    return COG_OK;
}

static cog_result_t null_load(void *store, const char *id,
                              cog_session_t **out)
{
    (void)store;
    (void)id;
    *out = NULL;
    return COG_OK;
}

static cog_result_t null_append(void *store, const cog_session_t *session,
                                const cog_session_node_t *node)
{
    (void)store;
    (void)session;
    (void)node;
    return COG_OK;
}

static cog_result_t null_delete(void *store, const char *id)
{
    (void)store;
    (void)id;
    return COG_OK;
}

static cog_result_t null_list(void *store, char ***out_ids,
                              size_t *out_count)
{
    (void)store;
    *out_ids = NULL;
    *out_count = 0U;
    return COG_OK;
}

static bool null_exists(void *store, const char *id)
{
    (void)store;
    (void)id;
    return false;
}

const cog_store_vtable_t cog_store_null_backend = {
    "null",
    null_save,
    null_load,
    null_append,
    null_delete,
    null_list,
    null_exists,
};

/* ------------------------------------------------------------------ */
/* File backend: shared helpers                                        */
/* ------------------------------------------------------------------ */

/** Collect session ids (base_dir subdirectories that contain meta.json)
 * into a heap array; *out is NULL with count 0 when the store is empty. */
static cog_result_t collect_sessions(const char *base_dir, char ***out,
                                     size_t *out_count)
{
    DIR *dp;
    struct dirent *de;
    char **names = NULL;
    size_t count = 0U;
    size_t cap = 0U;
    char *full = NULL;
    char *meta = NULL;

    *out = NULL;
    *out_count = 0U;
    dp = opendir(base_dir);
    if (dp == NULL) {
        if (errno == ENOENT) {
            return COG_OK; /* missing base_dir == empty store */
        }
        return COG_ERROR_IO;
    }
    while ((de = readdir(dp)) != NULL) {
        struct stat sb;
        const char *name = de->d_name;

        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
            continue;
        }
        full = path_join2(base_dir, name);
        if (full == NULL) {
            (void)closedir(dp);
            goto nomem;
        }
        if (stat(full, &sb) != 0 || !S_ISDIR(sb.st_mode)) {
            free(full);
            full = NULL;
            continue;
        }
        meta = path_join2(full, COG_STORE_FILE_META);
        free(full);
        full = NULL;
        if (meta == NULL) {
            (void)closedir(dp);
            goto nomem;
        }
        if (access(meta, F_OK) != 0) {
            free(meta);
            meta = NULL;
            continue; /* directory without meta.json is not a session */
        }
        free(meta);
        meta = NULL;
        if (count == cap) {
            size_t ncap = (cap == 0U) ? 8U : cap * 2U;
            char **nn;

            if (cap > SIZE_MAX / 2U) {
                (void)closedir(dp);
                goto nomem;
            }
            nn = realloc(names, ncap * sizeof(*nn));
            if (nn == NULL) {
                (void)closedir(dp);
                goto nomem;
            }
            names = nn;
            cap = ncap;
        }
        names[count] = strdup(name);
        if (names[count] == NULL) {
            (void)closedir(dp);
            goto nomem;
        }
        count++;
    }
    if (closedir(dp) != 0) {
        goto io;
    }
    *out = names;
    *out_count = count;
    return COG_OK;

nomem:
    {
        size_t i;

        for (i = 0U; i < count; i++) {
            free(names[i]);
        }
        free(names);
    }
    return COG_ERROR_NOMEM;

io:
    {
        size_t i;

        for (i = 0U; i < count; i++) {
            free(names[i]);
        }
        free(names);
    }
    return COG_ERROR_IO;
}

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

struct prune_item {
    char *name;
    int64_t mtime_sec;
    int64_t mtime_nsec;
};

static int cmp_prune_item(const void *a, const void *b)
{
    const struct prune_item *pa = (const struct prune_item *)a;
    const struct prune_item *pb = (const struct prune_item *)b;

    if (pa->mtime_sec != pb->mtime_sec) {
        return (pa->mtime_sec < pb->mtime_sec) ? -1 : 1;
    }
    if (pa->mtime_nsec != pb->mtime_nsec) {
        return (pa->mtime_nsec < pb->mtime_nsec) ? -1 : 1;
    }
    return strcmp(pa->name, pb->name);
}

/**
 * Enforce max_sessions after a successful save: delete the oldest
 * session directories (by mtime) beyond the cap, never the session that
 * was just saved.
 */
static cog_result_t prune_oldest(struct cog_store *st, const char *keep_id)
{
    char **names = NULL;
    size_t count = 0U;
    struct prune_item *items = NULL;
    size_t i;
    size_t j;
    size_t excess;
    size_t deleted = 0U;
    cog_result_t rc;

    rc = collect_sessions(st->base_dir, &names, &count);
    if (rc != COG_OK) {
        return rc;
    }
    if (count <= st->max_sessions) {
        for (i = 0U; i < count; i++) {
            free(names[i]);
        }
        free(names);
        return COG_OK;
    }
    items = malloc(count * sizeof(*items));
    if (items == NULL) {
        for (i = 0U; i < count; i++) {
            free(names[i]);
        }
        free(names);
        return COG_ERROR_NOMEM;
    }
    for (i = 0U; i < count; i++) {
        struct stat sb;
        char *full = path_join2(st->base_dir, names[i]);

        /* Ownership of the name string transfers to the item. */
        items[i].name = names[i];
        items[i].mtime_sec = 0;
        items[i].mtime_nsec = 0;
        if (full == NULL) {
            for (j = 0U; j < count; j++) {
                free(items[j].name);
            }
            free(items);
            free(names); /* array only; strings are owned by items */
            return COG_ERROR_NOMEM;
        }
        if (stat(full, &sb) == 0) {
            items[i].mtime_sec = (int64_t)sb.st_mtim.tv_sec;
            items[i].mtime_nsec = (int64_t)sb.st_mtim.tv_nsec;
        }
        free(full);
    }
    free(names); /* array only; strings are owned by items */
    qsort(items, count, sizeof(*items), cmp_prune_item);

    excess = count - st->max_sessions;
    for (i = 0U; i < count && deleted < excess; i++) {
        char *dir;

        if (strcmp(items[i].name, keep_id) == 0) {
            continue;
        }
        dir = path_join2(st->base_dir, items[i].name);
        if (dir == NULL) {
            for (j = 0U; j < count; j++) {
                free(items[j].name);
            }
            free(items);
            return COG_ERROR_NOMEM;
        }
        rc = delete_session_dir(dir);
        free(dir);
        if (rc != COG_OK) {
            for (j = 0U; j < count; j++) {
                free(items[j].name);
            }
            free(items);
            return rc;
        }
        deleted++;
    }
    for (i = 0U; i < count; i++) {
        free(items[i].name);
    }
    free(items);
    return COG_OK;
}

/* ------------------------------------------------------------------ */
/* File backend: meta load                                             */
/* ------------------------------------------------------------------ */

/**
 * Parse meta.json.  Requires format version 1, an id that matches
 * @p expect_id (the directory name used to reach this meta), and the
 * max_nodes / node_count / next_node_id / active_node_id fields.
 * parent_id may be null.  *out_parent_id, when non-NULL, receives a
 * malloc'd copy (NULL for absent).
 */
static cog_result_t file_load_meta(const char *path, const char *expect_id,
                                   size_t *max_nodes, size_t *node_count,
                                   uint64_t *next_node_id,
                                   uint64_t *active_node_id,
                                   char **out_parent_id)
{
    char *buf = NULL;
    size_t len = 0U;
    json_scan_t j;
    char key[16];
    unsigned char *id_dec = NULL;
    unsigned char *parent_dec = NULL;
    size_t id_len = 0U;
    size_t parent_len = 0U;
    bool have_version = false;
    bool have_id = false;
    bool have_max = false;
    bool have_count = false;
    bool have_next = false;
    bool have_active = false;
    uint64_t version = 0U;
    uint64_t mn = 0U;
    uint64_t nc = 0U;
    uint64_t nn = 0U;
    uint64_t an = 0U;
    cog_result_t rc;

    if (out_parent_id != NULL) {
        *out_parent_id = NULL;
    }
    rc = read_file(path, &buf, &len);
    if (rc != COG_OK) {
        return rc;
    }
    j.p = buf;
    j.end = buf + len;
    if (!js_char(&j, '{')) {
        rc = COG_ERROR_IO;
        goto out;
    }
    while (true) {
        const char *span = NULL;
        size_t span_len = 0U;

        if (!js_obj_field(&j, key, sizeof(key))) {
            rc = COG_ERROR_IO;
            goto out;
        }
        if (strcmp(key, "version") == 0) {
            if (!js_value_u64(&j, &version)) {
                rc = COG_ERROR_IO;
                goto out;
            }
            have_version = true;
        } else if (strcmp(key, "id") == 0) {
            if (js_null(&j)) {
                rc = COG_ERROR_IO; /* id is required */
                goto out;
            }
            if (!js_string(&j, &span, &span_len)) {
                rc = COG_ERROR_IO;
                goto out;
            }
            rc = b64_decode(span, span_len, &id_dec, &id_len);
            if (rc != COG_OK) {
                goto out;
            }
            have_id = true;
        } else if (strcmp(key, "parent_id") == 0) {
            if (!js_null(&j)) {
                if (!js_string(&j, &span, &span_len)) {
                    rc = COG_ERROR_IO;
                    goto out;
                }
                rc = b64_decode(span, span_len, &parent_dec, &parent_len);
                if (rc != COG_OK) {
                    goto out;
                }
            }
        } else if (strcmp(key, "max_nodes") == 0) {
            if (!js_value_u64(&j, &mn)) {
                rc = COG_ERROR_IO;
                goto out;
            }
            have_max = true;
        } else if (strcmp(key, "node_count") == 0) {
            if (!js_value_u64(&j, &nc)) {
                rc = COG_ERROR_IO;
                goto out;
            }
            have_count = true;
        } else if (strcmp(key, "next_node_id") == 0) {
            if (!js_value_u64(&j, &nn)) {
                rc = COG_ERROR_IO;
                goto out;
            }
            have_next = true;
        } else if (strcmp(key, "active_node_id") == 0) {
            if (!js_value_u64(&j, &an)) {
                rc = COG_ERROR_IO;
                goto out;
            }
            have_active = true;
        } else {
            rc = COG_ERROR_IO; /* unknown key: strict schema */
            goto out;
        }
        if (js_char(&j, '}')) {
            break;
        }
        if (!js_char(&j, ',')) {
            rc = COG_ERROR_IO;
            goto out;
        }
    }
    js_skip_ws(&j);
    if (j.p != j.end) {
        rc = COG_ERROR_IO;
        goto out;
    }
    if (!have_version || !have_id || !have_max || !have_count ||
        !have_next || !have_active) {
        rc = COG_ERROR_IO;
        goto out;
    }
    if (version != COG_STORE_FORMAT_VERSION) {
        rc = COG_ERROR_IO;
        goto out;
    }
    if (id_len == 0U || strcmp((const char *)id_dec, expect_id) != 0) {
        rc = COG_ERROR_IO; /* meta id must match the directory name */
        goto out;
    }
    if (mn > (uint64_t)SIZE_MAX || nc > (uint64_t)SIZE_MAX) {
        rc = COG_ERROR_IO;
        goto out;
    }
    *max_nodes = (size_t)mn;
    *node_count = (size_t)nc;
    *next_node_id = nn;
    *active_node_id = an;
    if (out_parent_id != NULL && parent_dec != NULL) {
        *out_parent_id = strdup((const char *)parent_dec);
        if (*out_parent_id == NULL) {
            rc = COG_ERROR_NOMEM;
            goto out;
        }
    }
    rc = COG_OK;

out:
    free(buf);
    free(id_dec);
    free(parent_dec);
    return rc;
}

/* ------------------------------------------------------------------ */
/* File backend: save                                                 */
/* ------------------------------------------------------------------ */

static cog_result_t file_save(void *vstore, const cog_session_t *session)
{
    struct cog_store *st = (struct cog_store *)vstore;
    const struct cog_session *s = (const struct cog_session *)session;
    struct cog_session_node **stack = NULL;
    size_t stack_cap = 0U;
    size_t stack_len = 0U;
    struct idx_pair {
        uint64_t node_id;
        uint64_t offset;
    } *idx = NULL;
    size_t idx_cap = 0U;
    size_t idx_len = 0U;
    char *dir = NULL;
    char *meta_p = NULL;
    char *meta_t = NULL;
    char *nodes_p = NULL;
    char *nodes_t = NULL;
    char *index_p = NULL;
    char *index_t = NULL;
    FILE *nodesf = NULL;
    FILE *indexf = NULL;
    struct cbuf meta = {0};
    struct cbuf line = {0};
    cog_result_t rc;
    uint64_t active_id = 0U;
    size_t node_count = 0U;
    size_t i;

    rc = store_validate_id(s->id);
    if (rc != COG_OK) {
        return rc;
    }
    rc = ensure_dir(st->base_dir);
    if (rc != COG_OK) {
        return rc;
    }
    dir = path_join2(st->base_dir, s->id);
    if (dir == NULL) {
        return COG_ERROR_NOMEM;
    }
    rc = ensure_dir(dir);
    if (rc != COG_OK) {
        goto out;
    }
    meta_p = path_join2(dir, COG_STORE_FILE_META);
    nodes_p = path_join2(dir, COG_STORE_FILE_NODES);
    index_p = path_join2(dir, COG_STORE_FILE_INDEX);
    meta_t = path_join2(dir, COG_STORE_FILE_META ".tmp");
    nodes_t = path_join2(dir, COG_STORE_FILE_NODES ".tmp");
    index_t = path_join2(dir, COG_STORE_FILE_INDEX ".tmp");
    if (meta_p == NULL || nodes_p == NULL || index_p == NULL ||
        meta_t == NULL || nodes_t == NULL || index_t == NULL) {
        rc = COG_ERROR_NOMEM;
        goto out;
    }

    nodesf = fopen(nodes_t, "wb");
    if (nodesf == NULL) {
        rc = COG_ERROR_IO;
        goto out;
    }

    /* Iterative DFS pre-order (children pushed in reverse so sibling
     * order is preserved in the file), writing records streaming. */
    if (s->root != NULL) {
        if (stack_len == stack_cap) {
            size_t ncap = (stack_cap == 0U) ? 64U : stack_cap * 2U;
            struct cog_session_node **tmp2;

            if (stack_cap > SIZE_MAX / 2U) {
                rc = COG_ERROR_NOMEM;
                goto out;
            }
            tmp2 = realloc(stack, ncap * sizeof(*tmp2));
            if (tmp2 == NULL) {
                rc = COG_ERROR_NOMEM;
                goto out;
            }
            stack = tmp2;
            stack_cap = ncap;
        }
        stack[stack_len++] = s->root;
    }
    while (stack_len > 0U) {
        struct cog_session_node *n = stack[--stack_len];
        struct cog_session_node *child;
        const cog_node_data_t *d = &n->data;
        long off;

        if (idx_len == idx_cap) {
            size_t ncap = (idx_cap == 0U) ? 64U : idx_cap * 2U;
            struct idx_pair *tmp2;

            if (idx_cap > SIZE_MAX / 2U) {
                rc = COG_ERROR_NOMEM;
                goto out;
            }
            tmp2 = realloc(idx, ncap * sizeof(*tmp2));
            if (tmp2 == NULL) {
                rc = COG_ERROR_NOMEM;
                goto out;
            }
            idx = tmp2;
            idx_cap = ncap;
        }

        line.len = 0U;
        if (line.data != NULL) {
            line.data[0] = '\0';
        }
        rc = node_record_write(&line, d);
        if (rc != COG_OK) {
            goto out;
        }
        off = ftell(nodesf);
        if (off < 0L) {
            rc = COG_ERROR_IO;
            goto out;
        }
        if (fwrite(line.data, 1U, line.len, nodesf) != line.len ||
            fputc('\n', nodesf) == EOF) {
            rc = COG_ERROR_IO;
            goto out;
        }
        idx[idx_len].node_id = d->node_id;
        idx[idx_len].offset = (uint64_t)off;
        idx_len++;
        node_count++;

        if (n->first_child != NULL) {
            size_t cnt = n->child_count;
            struct cog_session_node **base;
            size_t k;

            if (cnt > SIZE_MAX - stack_len) {
                rc = COG_ERROR_NOMEM;
                goto out;
            }
            if (stack_len + cnt > stack_cap) {
                size_t ncap = stack_len + cnt;
                struct cog_session_node **tmp2;

                tmp2 = realloc(stack, ncap * sizeof(*tmp2));
                if (tmp2 == NULL) {
                    rc = COG_ERROR_NOMEM;
                    goto out;
                }
                stack = tmp2;
                stack_cap = ncap;
            }
            base = stack + stack_len;
            k = cnt;
            for (child = n->first_child; child != NULL;
                 child = child->next_sibling) {
                base[--k] = child;
            }
            stack_len += cnt;
        }
    }
    if (fclose(nodesf) != 0) {
        nodesf = NULL;
        rc = COG_ERROR_IO;
        goto out;
    }
    nodesf = NULL;

    /* meta.json — node_count and the active id are known after the walk. */
    if (s->active != NULL) {
        active_id = s->active->data.node_id;
    }
    rc = meta_write(&meta, s->id, s->parent_id, s->max_nodes, node_count,
                    s->next_node_id, active_id);
    if (rc != COG_OK) {
        goto out;
    }
    rc = write_file_atomic(meta_p, meta_t, meta.data, meta.len);
    if (rc != COG_OK) {
        goto out;
    }

    /* index.bin — magic + count + (node_id, offset) pairs sorted by id. */
    {
        size_t a;
        size_t b;

        /* insertion sort: indices are small and nearly ordered */
        for (a = 1U; a < idx_len; a++) {
            struct idx_pair key = idx[a];

            b = a;
            while (b > 0U && idx[b - 1U].node_id > key.node_id) {
                idx[b] = idx[b - 1U];
                b--;
            }
            idx[b] = key;
        }
    }
    indexf = fopen(index_t, "wb");
    if (indexf == NULL) {
        rc = COG_ERROR_IO;
        goto out;
    }
    if (fwrite(kIndexMagic, 1U, sizeof(kIndexMagic), indexf) !=
            sizeof(kIndexMagic) ||
        fwrite(&idx_len, 1U, sizeof(idx_len), indexf) != sizeof(idx_len)) {
        rc = COG_ERROR_IO;
        goto out;
    }
    for (i = 0U; i < idx_len; i++) {
        if (fwrite(&idx[i].node_id, 1U, sizeof(idx[i].node_id), indexf) !=
                sizeof(idx[i].node_id) ||
            fwrite(&idx[i].offset, 1U, sizeof(idx[i].offset), indexf) !=
                sizeof(idx[i].offset)) {
            rc = COG_ERROR_IO;
            goto out;
        }
    }
    if (fclose(indexf) != 0) {
        indexf = NULL;
        rc = COG_ERROR_IO;
        goto out;
    }
    indexf = NULL;
    /* The tmp files were already fully written above; a plain atomic
     * rename publishes them without reopening (which would truncate). */
    if (rename(index_t, index_p) != 0) {
        rc = COG_ERROR_IO;
        goto out;
    }
    if (rename(nodes_t, nodes_p) != 0) {
        rc = COG_ERROR_IO;
        goto out;
    }

    if (st->max_sessions > 0U) {
        rc = prune_oldest(st, s->id);
    }

out:
    if (nodesf != NULL) {
        (void)fclose(nodesf);
    }
    if (indexf != NULL) {
        (void)fclose(indexf);
    }
    if (rc != COG_OK) {
        (void)remove(meta_t);
        (void)remove(nodes_t);
        (void)remove(index_t);
    }
    free(stack);
    free(idx);
    free(dir);
    free(meta_p);
    free(meta_t);
    free(nodes_p);
    free(nodes_t);
    free(index_p);
    free(index_t);
    free(meta.data);
    free(line.data);
    return rc;
}

/* ------------------------------------------------------------------ */
/* File backend: append                                               */
/* ------------------------------------------------------------------ */

static cog_result_t file_append(void *vstore, const cog_session_t *session,
                                const cog_session_node_t *node)
{
    struct cog_store *st = (struct cog_store *)vstore;
    const struct cog_session *s = (const struct cog_session *)session;
    const struct cog_session_node *n = (const struct cog_session_node *)node;
    const cog_node_data_t *d = &n->data;
    cog_result_t rc;
    char *dir = NULL;
    char *meta_p = NULL;
    char *nodes_p = NULL;
    char *index_p = NULL;
    char *meta_t = NULL;
    struct cbuf line = {0};
    FILE *f = NULL;
    bool bootstrapped = false;
    size_t file_count = 0U;
    uint64_t disk_next = 0U;
    uint64_t disk_active = 0U;
    size_t disk_max = 0U;

    rc = store_validate_id(s->id);
    if (rc != COG_OK) {
        return rc;
    }
    if (n->session != s) {
        return COG_ERROR_INVALID_STATE; /* foreign node */
    }
    rc = ensure_dir(st->base_dir);
    if (rc != COG_OK) {
        return rc;
    }
    dir = path_join2(st->base_dir, s->id);
    if (dir == NULL) {
        return COG_ERROR_NOMEM;
    }
    meta_p = path_join2(dir, COG_STORE_FILE_META);
    nodes_p = path_join2(dir, COG_STORE_FILE_NODES);
    index_p = path_join2(dir, COG_STORE_FILE_INDEX);
    meta_t = path_join2(dir, COG_STORE_FILE_META ".tmp");
    if (meta_p == NULL || nodes_p == NULL || index_p == NULL ||
        meta_t == NULL) {
        rc = COG_ERROR_NOMEM;
        goto out;
    }

    {
        struct stat sb;

        if (stat(dir, &sb) != 0) {
            if (errno != ENOENT) {
                rc = COG_ERROR_IO;
                goto out;
            }
            /* Bootstrap: the session was never saved; persist just this
             * node so append stays a single-record incremental op. */
            rc = ensure_dir(dir);
            if (rc != COG_OK) {
                goto out;
            }
            file_count = 1U;
            disk_next = d->node_id + 1U;
            disk_active = d->node_id;
            disk_max = s->max_nodes;
            bootstrapped = true;
        } else {
            if (!S_ISDIR(sb.st_mode)) {
                rc = COG_ERROR_IO;
                goto out;
            }
            if (access(meta_p, F_OK) != 0) {
                rc = COG_ERROR_IO; /* dir without meta.json is corrupt */
                goto out;
            }
            rc = file_load_meta(meta_p, s->id, &disk_max, &file_count,
                                &disk_next, &disk_active, NULL);
            if (rc != COG_OK) {
                goto out;
            }
            file_count++;
            if (disk_next < s->next_node_id) {
                disk_next = s->next_node_id;
            }
            disk_active = d->node_id;
        }
    }

    /* Rewrite meta.json with the updated record count and active id. */
    {
        struct cbuf meta = {0};

        rc = meta_write(&meta, s->id, s->parent_id, disk_max, file_count,
                        disk_next, disk_active);
        if (rc == COG_OK) {
            rc = write_file_atomic(meta_p, meta_t, meta.data, meta.len);
        }
        free(meta.data);
        if (rc != COG_OK) {
            goto out;
        }
    }

    /* Append the node record to nodes.jsonl.  A bootstrap persists this
     * node alone, so a non-root node is snapshotted as a standalone root
     * (its in-memory parent is not part of the persisted store yet). */
    {
        cog_node_data_t snap = *d;

        if (bootstrapped && snap.parent_id != 0U) {
            snap.parent_id = 0U;
        }
        rc = node_record_write(&line, &snap);
    }
    if (rc != COG_OK) {
        goto out;
    }
    f = fopen(nodes_p, (bootstrapped) ? "wb" : "ab");
    if (f == NULL) {
        rc = COG_ERROR_IO;
        goto out;
    }
    {
        long off = ftell(f);
        size_t wrote;

        if (off < 0L) {
            rc = COG_ERROR_IO;
            goto out;
        }
        wrote = fwrite(line.data, 1U, line.len, f);
        if (wrote != line.len || fputc('\n', f) == EOF) {
            rc = COG_ERROR_IO;
            goto out;
        }
        if (fclose(f) != 0) {
            f = NULL;
            rc = COG_ERROR_IO;
            goto out;
        }
        f = NULL;
        if (bootstrapped) {
            /* Create a fresh one-entry index.bin for layout fidelity. */
            uint64_t one = 1U;
            FILE *ix = fopen(index_p, "wb");

            if (ix == NULL) {
                rc = COG_ERROR_IO;
                goto out;
            }
            if (fwrite(kIndexMagic, 1U, sizeof(kIndexMagic), ix) !=
                    sizeof(kIndexMagic) ||
                fwrite(&one, 1U, sizeof(one), ix) != sizeof(one) ||
                fwrite(&d->node_id, 1U, sizeof(d->node_id), ix) !=
                    sizeof(d->node_id) ||
                fwrite(&off, 1U, sizeof(off), ix) != sizeof(off)) {
                (void)fclose(ix);
                rc = COG_ERROR_IO;
                goto out;
            }
            if (fclose(ix) != 0) {
                rc = COG_ERROR_IO;
                goto out;
            }
        } else {
            bool did = false;

            /* Index update failure is non-fatal (advisory cache); the
             * next full save regenerates a fresh index. */
            rc = index_append(index_p, d->node_id, (uint64_t)off, &did);
            if (rc != COG_OK) {
                goto out;
            }
        }
    }

out:
    if (f != NULL) {
        (void)fclose(f);
    }
    free(dir);
    free(meta_p);
    free(nodes_p);
    free(index_p);
    free(meta_t);
    free(line.data);
    return rc;
}

/* ------------------------------------------------------------------ */
/* File backend: load                                                 */
/* ------------------------------------------------------------------ */

static void recount_prunable_node(struct cog_session *s,
                                  struct cog_session_node *n)
{
    struct cog_session_node *c;
    bool prunable;

    for (c = n->first_child; c != NULL; c = c->next_sibling) {
        recount_prunable_node(s, c);
    }
    prunable = (n != s->root && n != s->active && n->child_count == 0U);
    n->prunable_counted = prunable;
    if (prunable) {
        s->prunable_leaf_count++;
    }
}

/** Recompute the prunable-leaf bookkeeping after changing the active
 * node (load restores the stored active node, which may not be the last
 * node inserted while rebuilding). */
static void recount_prunable(struct cog_session *s)
{
    s->prunable_leaf_count = 0U;
    if (s->root != NULL) {
        recount_prunable_node(s, s->root);
    }
}

static cog_result_t file_load(void *vstore, const char *id,
                              cog_session_t **out)
{
    struct cog_store *st = (struct cog_store *)vstore;
    struct id_map {
        uint64_t id;
        struct cog_session_node *node;
    } *map = NULL;
    size_t map_len = 0U;
    size_t map_cap = 0U;
    cog_node_record_t *recs = NULL;
    size_t rec_count = 0U;
    size_t rec_cap = 0U;
    uint64_t max_id = 0U;
    cog_session_t *s = NULL;
    char *dir = NULL;
    char *meta_p = NULL;
    char *nodes_p = NULL;
    char *nodes_buf = NULL;
    size_t nodes_len = 0U;
    char *m_parent = NULL;
    size_t m_max = 0U;
    size_t m_count = 0U;
    uint64_t m_next = 0U;
    uint64_t m_active = 0U;
    struct stat sb;
    cog_result_t rc;
    size_t i;

    rc = store_validate_id(id);
    if (rc != COG_OK) {
        return rc;
    }
    dir = path_join2(st->base_dir, id);
    if (dir == NULL) {
        return COG_ERROR_NOMEM;
    }
    if (stat(dir, &sb) != 0) {
        free(dir);
        if (errno == ENOENT) {
            return COG_ERROR_NOT_FOUND;
        }
        return COG_ERROR_IO;
    }
    if (!S_ISDIR(sb.st_mode)) {
        free(dir);
        return COG_ERROR_IO;
    }
    meta_p = path_join2(dir, COG_STORE_FILE_META);
    nodes_p = path_join2(dir, COG_STORE_FILE_NODES);
    if (meta_p == NULL || nodes_p == NULL) {
        rc = COG_ERROR_NOMEM;
        goto fail;
    }
    if (access(meta_p, F_OK) != 0) {
        rc = COG_ERROR_NOT_FOUND;
        goto fail;
    }
    rc = file_load_meta(meta_p, id, &m_max, &m_count, &m_next, &m_active,
                        &m_parent);
    if (rc != COG_OK) {
        goto fail;
    }
    rc = read_file(nodes_p, &nodes_buf, &nodes_len);
    if (rc != COG_OK) {
        goto fail;
    }

    /* Parse nodes.jsonl records in file order.  Blank lines are
     * tolerated; a final line without a trailing newline is accepted if
     * it parses, so a truncated record fails the parse and yields IO. */
    {
        const char *p = nodes_buf;
        size_t rem = nodes_len;

        while (rem > 0U) {
            const char *nl = memchr(p, '\n', rem);
            size_t linelen = (nl != NULL) ? (size_t)(nl - p) : rem;
            size_t k;
            bool blank = true;

            for (k = 0U; k < linelen; k++) {
                if (isspace((unsigned char)p[k]) == 0) {
                    blank = false;
                    break;
                }
            }
            if (!blank) {
                if (rec_count == rec_cap) {
                    size_t ncap = (rec_cap == 0U) ? 16U : rec_cap * 2U;
                    cog_node_record_t *tmp2;

                    if (rec_cap > SIZE_MAX / 2U) {
                        rc = COG_ERROR_NOMEM;
                        goto fail;
                    }
                    tmp2 = realloc(recs, ncap * sizeof(*tmp2));
                    if (tmp2 == NULL) {
                        rc = COG_ERROR_NOMEM;
                        goto fail;
                    }
                    recs = tmp2;
                    rec_cap = ncap;
                }
                rc = node_record_parse(p, linelen, &recs[rec_count]);
                if (rc != COG_OK) {
                    goto fail;
                }
                /* A successfully parsed record immediately joins the
                 * cleanup-owned prefix.  Every later failure path frees
                 * its decoded payload exactly once, including node_id 0. */
                rec_count++;
                if (recs[rec_count - 1U].node_id == 0U) {
                    rc = COG_ERROR_IO;
                    goto fail;
                }
                if (recs[rec_count - 1U].node_id > max_id) {
                    max_id = recs[rec_count - 1U].node_id;
                }
            }
            if (nl == NULL) {
                break;
            }
            p = nl + 1;
            rem -= linelen + 1U;
        }
    }

    /* Structural validation (fail closed on malformed data). */
    if (rec_count != m_count) {
        rc = COG_ERROR_IO;
        goto fail;
    }
    if (rec_count > 0U) {
        if (recs[0].parent_id != 0U) {
            rc = COG_ERROR_IO; /* the root must be the first record */
            goto fail;
        }
        for (i = 1U; i < rec_count; i++) {
            if (recs[i].parent_id == 0U) {
                rc = COG_ERROR_IO; /* exactly one root */
                goto fail;
            }
        }
    }
    if (m_count > 0U && m_active == 0U) {
        rc = COG_ERROR_IO;
        goto fail;
    }
    if (m_count == 0U && m_active != 0U) {
        rc = COG_ERROR_IO;
        goto fail;
    }
    if (m_next == 0U || m_next <= max_id) {
        rc = COG_ERROR_IO;
        goto fail;
    }

    {
        cog_session_config_t cfg;

        memset(&cfg, 0, sizeof(cfg));
        cfg.id = id;
        cfg.max_nodes = 0U;
        rc = cog_session_new(&s, &cfg);
        if (rc != COG_OK) {
            goto fail;
        }
        /* Headroom so a tree larger than its stored cap can be rebuilt
         * without mid-build pruning; the cap is restored at the end. */
        s->max_nodes = (m_count > m_max) ? m_count : m_max;
        if (m_parent != NULL) {
            rc = cog_session_set_parent(s, m_parent);
            if (rc != COG_OK) {
                goto fail;
            }
        }
    }
    for (i = 0U; i < rec_count; i++) {
        cog_node_record_t *r = &recs[i];
        struct cog_session_node *parent = NULL;
        struct cog_session_node *node = NULL;
        size_t k;

        if (i > 0U) {
            k = map_len;
            while (k > 0U) {
                k--;
                if (map[k].id == r->parent_id) {
                    parent = map[k].node;
                    break;
                }
            }
            if (parent == NULL) {
                rc = COG_ERROR_IO; /* parent must precede the child */
                goto fail;
            }
        }
        /* Duplicate node ids are corrupt. */
        k = map_len;
        while (k > 0U) {
            k--;
            if (map[k].id == r->node_id) {
                rc = COG_ERROR_IO;
                goto fail;
            }
        }
        switch (r->role) {
        case COG_NODE_SYSTEM:
        case COG_NODE_USER:
        case COG_NODE_ASSISTANT: {
            cog_message_t m;

            memset(&m, 0, sizeof(m));
            m.role = (cog_message_role_t)r->role;
            m.content = r->content;
            m.content_len = r->content_len;
            rc = cog_session_add_message(s, parent, &m, &node);
            if (rc != COG_OK) {
                goto fail;
            }
            /* Transfer the parsed payload (it is byte-exact, including
             * embedded NULs) into the freshly created node. */
            free((void *)node->data.content);
            node->data.content = r->content;
            node->data.content_len = r->content_len;
            r->content = NULL;
            r->content_len = 0U;
            break;
        }
        case COG_NODE_TOOL_CALL:
            if (r->tool_name == NULL || r->tool_args == NULL) {
                rc = COG_ERROR_IO;
                goto fail;
            }
            rc = cog_session_add_tool_call(s, parent, r->tool_name,
                                           r->tool_args, &node);
            if (rc != COG_OK) {
                goto fail;
            }
            free((void *)node->data.tool_name);
            free((void *)node->data.tool_args);
            node->data.tool_name = r->tool_name;
            node->data.tool_args = r->tool_args;
            node->tool_args_len = r->tool_args_len;
            r->tool_name = NULL;
            r->tool_args = NULL;
            break;
        case COG_NODE_TOOL_RESULT:
            if (r->tool_result == NULL) {
                rc = COG_ERROR_IO;
                goto fail;
            }
            rc = cog_session_add_tool_result(s, parent, r->tool_result,
                                             &node);
            if (rc != COG_OK) {
                goto fail;
            }
            free((void *)node->data.tool_result);
            node->data.tool_result = r->tool_result;
            node->tool_result_len = r->tool_result_len;
            r->tool_result = NULL;
            break;
        default:
            rc = COG_ERROR_IO; /* role validity is enforced at parse */
            goto fail;
        }
        /* Restore the stored identity and the fields with no public
         * setter (raw provider payloads, chunk count). */
        node->data.node_id = r->node_id;
        free((void *)node->data.raw_request);
        node->data.raw_request = r->raw_request;
        r->raw_request = NULL;
        free((void *)node->data.raw_response);
        node->data.raw_response = r->raw_response;
        r->raw_response = NULL;
        node->data.chunk_count = r->chunk_count;
        free((void *)node->data.model);
        node->data.model = r->model;
        r->model = NULL;
        free((void *)node->data.provider);
        node->data.provider = r->provider;
        r->provider = NULL;
        rc = cog_session_set_timing(s, node, &r->timing);
        if (rc != COG_OK) {
            goto fail;
        }
        rc = cog_session_set_cost(s, node, &r->cost);
        if (rc != COG_OK) {
            goto fail;
        }
        if (map_len == map_cap) {
            size_t ncap = (map_cap == 0U) ? 16U : map_cap * 2U;
            struct id_map *tmp2;

            if (map_cap > SIZE_MAX / 2U) {
                rc = COG_ERROR_NOMEM;
                goto fail;
            }
            tmp2 = realloc(map, ncap * sizeof(*tmp2));
            if (tmp2 == NULL) {
                rc = COG_ERROR_NOMEM;
                goto fail;
            }
            map = tmp2;
            map_cap = ncap;
        }
        map[map_len].id = r->node_id;
        map[map_len].node = node;
        map_len++;
    }

    /* Restore the stored active node, next id, and node cap. */
    if (m_active != 0U) {
        size_t k = map_len;
        bool found = false;

        while (k > 0U) {
            k--;
            if (map[k].id == m_active) {
                s->active = map[k].node;
                found = true;
                break;
            }
        }
        if (!found) {
            rc = COG_ERROR_IO;
            goto fail;
        }
    }
    s->next_node_id = m_next;
    s->max_nodes = m_max;
    recount_prunable(s);
    *out = s;
    s = NULL; /* ownership transferred to the caller */
    rc = COG_OK;

fail:
    if (s != NULL) {
        cog_session_free(s);
    }
    for (i = 0U; i < rec_count; i++) {
        node_record_free(&recs[i]);
    }
    free(recs);
    free(map);
    free(m_parent);
    free(nodes_buf);
    free(dir);
    free(meta_p);
    free(nodes_p);
    return rc;
}

/* ------------------------------------------------------------------ */
/* File backend: delete / list / exists                               */
/* ------------------------------------------------------------------ */

static cog_result_t file_delete(void *vstore, const char *id)
{
    struct cog_store *st = (struct cog_store *)vstore;
    cog_result_t rc;
    char *dir = NULL;
    char *meta_p = NULL;
    struct stat sb;

    rc = store_validate_id(id);
    if (rc != COG_OK) {
        return rc;
    }
    dir = path_join2(st->base_dir, id);
    if (dir == NULL) {
        return COG_ERROR_NOMEM;
    }
    if (stat(dir, &sb) != 0) {
        free(dir);
        if (errno == ENOENT) {
            return COG_ERROR_NOT_FOUND;
        }
        return COG_ERROR_IO;
    }
    if (!S_ISDIR(sb.st_mode)) {
        free(dir);
        return COG_ERROR_IO;
    }
    meta_p = path_join2(dir, COG_STORE_FILE_META);
    if (meta_p == NULL) {
        free(dir);
        return COG_ERROR_NOMEM;
    }
    if (access(meta_p, F_OK) != 0) {
        free(meta_p);
        free(dir);
        return COG_ERROR_NOT_FOUND;
    }
    free(meta_p);
    rc = delete_session_dir(dir);
    free(dir);
    return rc;
}

static cog_result_t file_list(void *vstore, char ***out_ids,
                              size_t *out_count)
{
    struct cog_store *st = (struct cog_store *)vstore;
    cog_result_t rc = collect_sessions(st->base_dir, out_ids, out_count);

    if (rc == COG_OK && *out_count > 1U) {
        qsort(*out_ids, *out_count, sizeof(char *), cmp_str);
    }
    return rc;
}

static bool file_exists(void *vstore, const char *id)
{
    struct cog_store *st = (struct cog_store *)vstore;
    char *dir = NULL;
    char *meta_p = NULL;
    struct stat sb;
    bool ok;

    if (store_validate_id(id) != COG_OK) {
        return false;
    }
    dir = path_join2(st->base_dir, id);
    if (dir == NULL) {
        return false;
    }
    if (stat(dir, &sb) != 0 || !S_ISDIR(sb.st_mode)) {
        free(dir);
        return false;
    }
    meta_p = path_join2(dir, COG_STORE_FILE_META);
    if (meta_p == NULL) {
        free(dir);
        return false;
    }
    ok = (access(meta_p, F_OK) == 0);
    free(meta_p);
    free(dir);
    return ok;
}

const cog_store_vtable_t cog_store_file_backend = {
    "file",
    file_save,
    file_load,
    file_append,
    file_delete,
    file_list,
    file_exists,
};

/* ------------------------------------------------------------------ */
/* Public lifecycle                                                    */
/* ------------------------------------------------------------------ */

cog_result_t cog_store_new(cog_store_t **out,
                           const cog_store_config_t *config)
{
    cog_store_t *st;
    const cog_store_vtable_t *backend;
    const char *base_dir;

    if (out == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    *out = NULL;
    backend = (config != NULL && config->backend != NULL)
                  ? config->backend
                  : &cog_store_file_backend;
    if (backend->save == NULL || backend->load == NULL ||
        backend->append == NULL || backend->delete_ == NULL ||
        backend->list == NULL || backend->exists == NULL) {
        return COG_ERROR_INVALID_ARG; /* malformed backend vtable */
    }
    st = calloc(1, sizeof(*st));
    if (st == NULL) {
        return COG_ERROR_NOMEM;
    }
    st->backend = backend;
    base_dir = (config != NULL && config->base_dir != NULL &&
                config->base_dir[0] != '\0')
                   ? config->base_dir
                   : ".";
    st->base_dir = strdup(base_dir);
    if (st->base_dir == NULL) {
        free(st);
        return COG_ERROR_NOMEM;
    }
    st->max_sessions = (config != NULL) ? config->max_sessions : 0U;
    st->auto_save = (config != NULL) && config->auto_save;
    st->compress = (config != NULL) && config->compress;
    *out = st;
    return COG_OK;
}

void cog_store_free(cog_store_t *store)
{
    if (store == NULL) {
        return;
    }
    free(store->base_dir);
    free(store);
}

/* ------------------------------------------------------------------ */
/* Public dispatch (validates arguments, then delegates)               */
/* ------------------------------------------------------------------ */

cog_result_t cog_store_save(cog_store_t *store,
                            const cog_session_t *session)
{
    if (store == NULL || session == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    return store->backend->save(store, session);
}

cog_result_t cog_store_load(cog_store_t *store,
                            const char *id,
                            cog_session_t **out)
{
    if (store == NULL || id == NULL || out == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    return store->backend->load(store, id, out);
}

cog_result_t cog_store_append(cog_store_t *store,
                              const cog_session_t *session,
                              const cog_session_node_t *node)
{
    if (store == NULL || session == NULL || node == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    return store->backend->append(store, session, node);
}

cog_result_t cog_store_delete(cog_store_t *store, const char *id)
{
    if (store == NULL || id == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    return store->backend->delete_(store, id);
}

cog_result_t cog_store_list(cog_store_t *store,
                            char ***out_ids,
                            size_t *out_count)
{
    if (store == NULL || out_ids == NULL || out_count == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    return store->backend->list(store, out_ids, out_count);
}

bool cog_store_exists(cog_store_t *store, const char *id)
{
    if (store == NULL || id == NULL) {
        return false;
    }
    return store->backend->exists(store, id);
}

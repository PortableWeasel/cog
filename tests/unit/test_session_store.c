/**
 * @file test_session_store.c
 * @brief Contract test for cog/session_store.h (Stage 12.2d, ticket 0efef06).
 *
 * Exercises the full public surface of the session store:
 *
 *   - lifecycle: cog_store_new / cog_store_free, defaults, NULL-arg
 *     contracts, malformed backend vtable rejection;
 *   - null backend: every operation succeeds and does nothing;
 *   - file backend on-disk layout: meta.json + append-only nodes.jsonl +
 *     index.bin per session directory under base_dir;
 *   - round-trips: empty, linear (with embedded NUL content, timing,
 *     cost, raw provider fields, tool nodes), forked trees with sibling
 *     order and active-node preservation, byte-identical re-save;
 *   - incremental append: to a saved session, bootstrap (no prior save),
 *     append-after-load id continuity, foreign-node rejection;
 *   - delete / exists / list (sorted, junk-files excluded);
 *   - max_sessions pruning by directory mtime;
 *   - malformed / truncated / corrupt files: every documented error code
 *     (COG_ERROR_NOT_FOUND, COG_ERROR_IO), corrupt index.bin tolerated;
 *   - path safety: traversal ids rejected, blank/control ids rejected,
 *     over-long ids overflow, nothing escapes base_dir;
 *   - compress / auto_save config accepted with honest semantics;
 *   - stress: 2000-node chain and 300-child fan-out round-trip exactly.
 *
 * The test is white-box in one respect: it includes the private
 * session_internal.h layout (like test_agent includes agent_internal.h)
 * so raw provider fields, chunk counts, private tool-args lengths, the
 * stored cap and the next-node id can be populated and verified through
 * the same struct access the store itself uses.  The public contract is
 * still exercised through cog/session_store.h and cog/session.h.
 */

#define _POSIX_C_SOURCE 200809L

#include "cog/session_store.h"

#include "session_internal.h"

#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#ifndef COG_SESSION_STORE_H
#error "COG_SESSION_STORE_H include guard missing"
#endif

/* ------------------------------------------------------------------ */
/* Compile-time checks                                                 */
/* ------------------------------------------------------------------ */

_Static_assert(_Generic(((cog_store_vtable_t){0}).save,
                        cog_store_save_fn : 1, default : 0),
               "cog_store_vtable_t.save must be cog_store_save_fn");
_Static_assert(_Generic(((cog_store_vtable_t){0}).load,
                        cog_store_load_fn : 1, default : 0),
               "cog_store_vtable_t.load must be cog_store_load_fn");
_Static_assert(_Generic(((cog_store_vtable_t){0}).append,
                        cog_store_append_fn : 1, default : 0),
               "cog_store_vtable_t.append must be cog_store_append_fn");
_Static_assert(_Generic(((cog_store_vtable_t){0}).delete_,
                        cog_store_delete_fn : 1, default : 0),
               "cog_store_vtable_t.delete_ must be cog_store_delete_fn");
_Static_assert(_Generic(((cog_store_vtable_t){0}).list,
                        cog_store_list_fn : 1, default : 0),
               "cog_store_vtable_t.list must be cog_store_list_fn");
_Static_assert(_Generic(((cog_store_vtable_t){0}).exists,
                        cog_store_exists_fn : 1, default : 0),
               "cog_store_vtable_t.exists must be cog_store_exists_fn");
_Static_assert(_Generic(((cog_store_vtable_t){0}).name,
                        const char * : 1, default : 0),
               "cog_store_vtable_t.name must be const char *");

#define FIELD_TYPE_ASSERT(stype, field, type)                              \
    _Static_assert(                                                        \
        _Generic(((stype){0}).field, type : 1, default : 0),               \
        #stype "." #field " must have type " #type)

FIELD_TYPE_ASSERT(cog_store_config_t, backend, const cog_store_vtable_t *);
FIELD_TYPE_ASSERT(cog_store_config_t, base_dir, const char *);
FIELD_TYPE_ASSERT(cog_store_config_t, max_sessions, size_t);
FIELD_TYPE_ASSERT(cog_store_config_t, auto_save, bool);
FIELD_TYPE_ASSERT(cog_store_config_t, compress, bool);

#undef FIELD_TYPE_ASSERT

/* ------------------------------------------------------------------ */
/* Test helpers                                                        */
/* ------------------------------------------------------------------ */

#define KREC_FMT                                                           \
    "{\"node_id\":%llu,\"parent_id\":%llu,\"role\":%d,"            \
    "\"content\":\"%s\",\"tool_name\":null,\"tool_args\":null,"  \
    "\"tool_result\":null,\"raw_request\":null,\"raw_response\":null," \
    "\"chunk_count\":0,"                                                    \
    "\"timing\":{\"request_sent_ms\":0,\"first_token_ms\":0,"       \
    "\"last_token_ms\":0,\"duration_ms\":0,\"ttft_ms\":0,"        \
    "\"tokens_per_second\":0},"                                            \
    "\"cost\":{\"input_tokens\":0,\"output_tokens\":0,"          \
    "\"cost_per_input_token\":0,\"cost_per_output_token\":0,"      \
    "\"total_cost\":0},"                                                    \
    "\"model\":null,\"provider\":null}\n"

static char *path_join(const char *a, const char *b)
{
    size_t la = strlen(a);
    size_t lb = strlen(b);
    char *p = malloc(la + 1U + lb + 1U);

    if (p == NULL) {
        return NULL;
    }
    (void)memcpy(p, a, la);
    p[la] = '/';
    (void)memcpy(p + la + 1U, b, lb + 1U);
    return p;
}

/** Fresh per-test temp directory (caller frees after rmtree). */
static char *make_tmp(const char *tag)
{
    char templ[128];
    int n = snprintf(templ, sizeof(templ), "/tmp/cog-store-%s-XXXXXX", tag);
    char *dir;

    if (n <= 0 || (size_t)n >= sizeof(templ)) {
        return NULL;
    }
    dir = strdup(templ);
    if (dir == NULL) {
        return NULL;
    }
    if (mkdtemp(dir) == NULL) {
        free(dir);
        return NULL;
    }
    return dir;
}

static void rmtree(const char *dir)
{
    DIR *dp = opendir(dir);
    struct dirent *de;

    if (dp == NULL) {
        return;
    }
    while ((de = readdir(dp)) != NULL) {
        struct stat sb;
        char *p;

        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) {
            continue;
        }
        p = path_join(dir, de->d_name);
        if (p == NULL) {
            continue;
        }
        if (lstat(p, &sb) == 0 && S_ISDIR(sb.st_mode)) {
            rmtree(p);
        } else {
            (void)unlink(p);
        }
        free(p);
    }
    (void)closedir(dp);
    (void)rmdir(dir);
}

static void write_file(const char *path, const char *data, size_t len)
{
    FILE *f = fopen(path, "wb");

    if (f == NULL) {
        return;
    }
    if (len > 0U) {
        (void)fwrite(data, 1U, len, f);
    }
    (void)fclose(f);
}

static int read_file(const char *path, char **out, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    long size;
    char *buf;

    *out = NULL;
    *out_len = 0U;
    if (f == NULL) {
        return -1;
    }
    if (fseek(f, 0L, SEEK_END) != 0) {
        (void)fclose(f);
        return -1;
    }
    size = ftell(f);
    if (size < 0L || fseek(f, 0L, SEEK_SET) != 0) {
        (void)fclose(f);
        return -1;
    }
    buf = malloc((size_t)size + 1U);
    if (buf == NULL) {
        (void)fclose(f);
        return -1;
    }
    if (fread(buf, 1U, (size_t)size, f) != (size_t)size) {
        free(buf);
        (void)fclose(f);
        return -1;
    }
    (void)fclose(f);
    buf[(size_t)size] = '\0';
    *out = buf;
    *out_len = (size_t)size;
    return 0;
}

static void set_mtime(const char *path, time_t sec)
{
    struct timespec ts[2];

    ts[0].tv_sec = sec;
    ts[0].tv_nsec = 0L;
    ts[1] = ts[0];
    (void)utimensat(AT_FDCWD, path, ts, 0);
}

/** Replace the unsigned value after "key": in a small JSON file. */
static int meta_set_uint(const char *path, const char *key,
                         unsigned long long val)
{
    char *buf = NULL;
    size_t len = 0U;
    char needle[64];
    char valbuf[32];
    char *p;
    char *q;
    size_t prefix_len;
    size_t val_len;
    size_t suffix_len;
    char *out;
    int rc = -1;

    if (read_file(path, &buf, &len) != 0) {
        return -1;
    }
    (void)snprintf(needle, sizeof(needle), "\"%s\":", key);
    p = strstr(buf, needle);
    if (p == NULL) {
        goto done;
    }
    p += strlen(needle);
    q = p;
    while (isdigit((unsigned char)*q) != 0) {
        q++;
    }
    (void)snprintf(valbuf, sizeof(valbuf), "%llu", val);
    prefix_len = (size_t)(p - buf);
    val_len = strlen(valbuf);
    suffix_len = (size_t)(q - p);
    out = malloc(prefix_len + val_len + (len - prefix_len - suffix_len) + 1U);
    if (out == NULL) {
        goto done;
    }
    (void)memcpy(out, buf, prefix_len);
    (void)memcpy(out + prefix_len, valbuf, val_len);
    (void)memcpy(out + prefix_len + val_len, q,
                 len - prefix_len - suffix_len);
    out[prefix_len + val_len + (len - prefix_len - suffix_len)] = '\0';
    write_file(path, out, prefix_len + val_len + (len - prefix_len -
                                                  suffix_len));
    free(out);
    rc = 0;
done:
    free(buf);
    return rc;
}

/* Message builders. */
#define MSG(role, text)                                                    \
    (&(cog_message_t){role, (text),                                        \
                      (text) != NULL ? strlen(text) : 0U, NULL})

static cog_message_t blob_msg(cog_message_role_t role, const char *data,
                              size_t len)
{
    cog_message_t m;

    m.role = role;
    m.content = data;
    m.content_len = len;
    m.name = NULL;
    return m;
}

static int str_eq(const char *a, const char *b)
{
    if (a == NULL || b == NULL) {
        return a == b;
    }
    return strcmp(a, b) == 0;
}

static int bytes_eq(const char *a, size_t alen, const char *b, size_t blen)
{
    if (alen != blen) {
        return 0;
    }
    if (a == NULL || b == NULL) {
        return a == b;
    }
    if (alen == 0U) {
        return 1;
    }
    return memcmp(a, b, alen) == 0;
}

static int dbl_eq(double a, double b)
{
    double scale = (fabs(a) > fabs(b)) ? fabs(a) : fabs(b);

    if (scale < 1.0) {
        scale = 1.0;
    }
    return fabs(a - b) <= 1e-9 * scale;
}

/* Full field-by-field comparison of two node views. */
static int node_eq(const cog_node_data_t *a, const cog_node_data_t *b)
{
    if (a->node_id != b->node_id || a->parent_id != b->parent_id ||
        a->role != b->role || a->chunk_count != b->chunk_count) {
        return 0;
    }
    if (!bytes_eq(a->content, a->content_len, b->content, b->content_len)) {
        return 0;
    }
    if (!str_eq(a->tool_name, b->tool_name)) {
        return 0;
    }
    if (!str_eq(a->tool_args, b->tool_args)) {
        return 0;
    }
    if (!str_eq(a->tool_result, b->tool_result)) {
        return 0;
    }
    if (!str_eq(a->raw_request, b->raw_request)) {
        return 0;
    }
    if (!str_eq(a->raw_response, b->raw_response)) {
        return 0;
    }
    if (!str_eq(a->model, b->model)) {
        return 0;
    }
    if (!str_eq(a->provider, b->provider)) {
        return 0;
    }
    if (a->timing.request_sent_ms != b->timing.request_sent_ms ||
        a->timing.first_token_ms != b->timing.first_token_ms ||
        a->timing.last_token_ms != b->timing.last_token_ms ||
        a->timing.duration_ms != b->timing.duration_ms ||
        a->timing.ttft_ms != b->timing.ttft_ms ||
        !dbl_eq(a->timing.tokens_per_second,
                b->timing.tokens_per_second)) {
        return 0;
    }
    if (a->cost.input_tokens != b->cost.input_tokens ||
        a->cost.output_tokens != b->cost.output_tokens ||
        !dbl_eq(a->cost.cost_per_input_token,
                b->cost.cost_per_input_token) ||
        !dbl_eq(a->cost.cost_per_output_token,
                b->cost.cost_per_output_token) ||
        !dbl_eq(a->cost.total_cost, b->cost.total_cost)) {
        return 0;
    }
    return 1;
}

static size_t count_nodes(const cog_session_node_t *n)
{
    const cog_session_node_t *c;
    size_t total = 1U;

    for (c = cog_node_first_child(n); c != NULL;
         c = cog_node_next_sibling(c)) {
        total += count_nodes(c);
    }
    return total;
}

static void fill_preorder(const cog_session_node_t *n,
                          const cog_node_data_t **arr, size_t *idx)
{
    const cog_session_node_t *c;

    arr[(*idx)++] = cog_node_data(n);
    for (c = cog_node_first_child(n); c != NULL;
         c = cog_node_next_sibling(c)) {
        fill_preorder(c, arr, idx);
    }
}

/** Pre-order view of a session (structure + data), caller frees array. */
static int session_preorder(const cog_session_t *s,
                            const cog_node_data_t ***out, size_t *out_count)
{
    const cog_node_data_t **arr;
    size_t n;
    size_t idx = 0U;

    *out = NULL;
    *out_count = 0U;
    if (cog_session_is_empty(s)) {
        return 0;
    }
    n = count_nodes(cog_session_root(s));
    arr = malloc(n * sizeof(*arr));
    if (arr == NULL) {
        return -1;
    }
    fill_preorder(cog_session_root(s), arr, &idx);
    *out = arr;
    *out_count = n;
    return 0;
}

static int trees_eq(const cog_session_t *a, const cog_session_t *b)
{
    const cog_node_data_t **pa = NULL;
    const cog_node_data_t **pb = NULL;
    size_t na = 0U;
    size_t nb = 0U;
    size_t i;
    int ok = 0;

    if (session_preorder(a, &pa, &na) != 0 ||
        session_preorder(b, &pb, &nb) != 0) {
        goto done;
    }
    if (na != nb) {
        goto done;
    }
    for (i = 0U; i < na; i++) {
        if (!node_eq(pa[i], pb[i])) {
            goto done;
        }
    }
    ok = 1;
done:
    free(pa);
    free(pb);
    return ok;
}

static int history_eq(const cog_session_t *a, const cog_session_node_t *na,
                      const cog_session_t *b, const cog_session_node_t *nb)
{
    const cog_message_t *ma = NULL;
    const cog_message_t *mb = NULL;
    size_t ca = 0U;
    size_t cb = 0U;
    size_t i;

    if (cog_session_get_history(a, na, &ma, &ca) != COG_OK ||
        cog_session_get_history(b, nb, &mb, &cb) != COG_OK) {
        return 0;
    }
    if (ca != cb) {
        return 0;
    }
    for (i = 0U; i < ca; i++) {
        if (ma[i].role != mb[i].role) {
            return 0;
        }
        if (!bytes_eq(ma[i].content, ma[i].content_len, mb[i].content,
                      mb[i].content_len)) {
            return 0;
        }
        if (!str_eq(ma[i].name, mb[i].name)) {
            return 0;
        }
    }
    return 1;
}

/* White-box accessors via session_internal.h (mirrors what the store
 * itself sees). */
static uint64_t sess_next(const cog_session_t *s)
{
    return ((const struct cog_session *)s)->next_node_id;
}

static size_t sess_cap(const cog_session_t *s)
{
    return ((const struct cog_session *)s)->max_nodes;
}

static void node_fill_raw(cog_session_node_t *node, const char *req,
                          const char *resp, size_t chunks,
                          const char *model, const char *prov)
{
    struct cog_session_node *n = (struct cog_session_node *)node;

    free((void *)n->data.raw_request);
    free((void *)n->data.raw_response);
    free((void *)n->data.model);
    free((void *)n->data.provider);
    n->data.raw_request = (req != NULL) ? strdup(req) : NULL;
    n->data.raw_response = (resp != NULL) ? strdup(resp) : NULL;
    n->data.model = (model != NULL) ? strdup(model) : NULL;
    n->data.provider = (prov != NULL) ? strdup(prov) : NULL;
    n->data.chunk_count = chunks;
}

/** Are the three on-disk files present for a saved session? */
static int store_files_present(const char *base, const char *id)
{
    char *d = path_join(base, id);
    char *m = path_join(d, "meta.json");
    char *n = path_join(d, "nodes.jsonl");
    char *x = path_join(d, "index.bin");
    int ok = (access(m, F_OK) == 0) && (access(n, F_OK) == 0) &&
             (access(x, F_OK) == 0);

    free(d);
    free(m);
    free(n);
    free(x);
    return ok;
}

/** Read one of the three store files, owning the joined path. */
static int read_store_file(const char *base, const char *id,
                           const char *fname, char **out, size_t *out_len)
{
    char *d = path_join(base, id);
    char *p = path_join(d, fname);
    int rc;

    rc = read_file(p, out, out_len);
    free(d);
    free(p);
    return rc;
}

/** Collect leaf node pointers in pre-order (caller frees array). */
static size_t collect_leaves(const cog_session_node_t *n,
                             cog_session_node_t **arr, size_t idx)
{
    const cog_session_node_t *c;

    if (cog_node_child_count(n) == 0U) {
        arr[idx++] = (cog_session_node_t *)n;
        return idx;
    }
    for (c = cog_node_first_child(n); c != NULL;
         c = cog_node_next_sibling(c)) {
        idx = collect_leaves(c, arr, idx);
    }
    return idx;
}

/** Root-to-leaf history equivalence for every leaf of both trees. */
static int leaf_histories_eq(const cog_session_t *a, const cog_session_t *b)
{
    cog_session_node_t **la = NULL;
    cog_session_node_t **lb = NULL;
    size_t na = 0U;
    size_t nb = 0U;
    size_t i;
    size_t idx;
    int ok = 0;

    if (cog_session_is_empty(a)) {
        return cog_session_is_empty(b);
    }
    na = count_nodes(cog_session_root(a));
    nb = count_nodes(cog_session_root(b));
    if (na != nb) {
        return 0;
    }
    la = malloc(na * sizeof(*la));
    lb = malloc(nb * sizeof(*lb));
    if (la == NULL || lb == NULL) {
        free(la);
        free(lb);
        return 0;
    }
    idx = 0U;
    idx = collect_leaves(cog_session_root(a), la, idx);
    na = idx;
    idx = 0U;
    idx = collect_leaves(cog_session_root(b), lb, idx);
    nb = idx;
    if (na != nb) {
        goto done;
    }
    for (i = 0U; i < na; i++) {
        if (!history_eq(a, la[i], b, lb[i])) {
            goto done;
        }
    }
    ok = 1;
done:
    free(la);
    free(lb);
    return ok;
}

/** Count newline-terminated records in nodes.jsonl. */
static int count_records(const char *base, const char *id)
{
    char *d = path_join(base, id);
    char *p = path_join(d, "nodes.jsonl");
    char *buf = NULL;
    size_t len = 0U;
    size_t i;
    int count = 0;

    free(d);
    if (read_file(p, &buf, &len) != 0) {
        free(p);
        return -1;
    }
    free(p);
    for (i = 0U; i < len; i++) {
        if (buf[i] == '\n') {
            count++;
        }
    }
    free(buf);
    return count;
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

static int verify_lifecycle(void)
{
    cog_store_t *st = (cog_store_t *)0x1;
    cog_store_config_t cfg;
    cog_store_vtable_t bad;

    (void)&cog_store_file_backend;
    (void)&cog_store_null_backend;

    if (cog_store_new(&st, NULL) != COG_OK || st == NULL) {
        return 1;
    }
    cog_store_free(st);
    cog_store_free(NULL); /* NULL is a no-op */

    if (cog_store_new(NULL, NULL) != COG_ERROR_NULL_PTR) {
        return 1;
    }
    /* Defaults with NULL config: file backend, base_dir ".". */
    memset(&cfg, 0, sizeof(cfg));
    if (cog_store_new(&st, &cfg) != COG_OK || st == NULL) {
        return 1;
    }
    cog_store_free(st);

    /* Malformed custom backend vtable is rejected fail-closed. */
    memset(&bad, 0, sizeof(bad));
    cfg.backend = &bad;
    if (cog_store_new(&st, &cfg) != COG_ERROR_INVALID_ARG) {
        return 1;
    }

    /* Custom backend with all six operations is accepted. */
    bad.name = "test";
    bad.save = cog_store_null_backend.save;
    bad.load = cog_store_null_backend.load;
    bad.append = cog_store_null_backend.append;
    bad.delete_ = cog_store_null_backend.delete_;
    bad.list = cog_store_null_backend.list;
    bad.exists = cog_store_null_backend.exists;
    cfg.backend = &bad;
    if (cog_store_new(&st, &cfg) != COG_OK || st == NULL) {
        return 1;
    }
    cog_store_free(st);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Null backend                                                        */
/* ------------------------------------------------------------------ */

static int verify_null_backend(void)
{
    char *base = make_tmp("null");
    cog_store_config_t cfg;
    cog_store_t *st = NULL;
    cog_session_t *s = NULL;
    cog_session_t *out = (cog_session_t *)0x1;
    cog_session_node_t *node = NULL;
    char **ids = (char **)0x1;
    size_t count = 123U;
    int fail = 0;
    char *id = NULL;

    if (base == NULL) {
        return 1;
    }
    memset(&cfg, 0, sizeof(cfg));
    cfg.backend = &cog_store_null_backend;
    cfg.base_dir = base;
    cfg.max_sessions = 3U;
    cfg.auto_save = true;
    cfg.compress = true;

    if (cog_store_new(&st, &cfg) != COG_OK) {
        fail = 1;
        goto done;
    }
    if (cog_session_new(&s, NULL) != COG_OK) {
        fail = 1;
        goto done;
    }
    id = strdup(cog_session_id(s));
    if (cog_session_append(s, MSG(COG_MESSAGE_ROLE_USER, "hi"), &node) !=
        COG_OK) {
        fail = 1;
        goto done;
    }

    /* Every operation succeeds and does nothing. */
    if (cog_store_save(st, s) != COG_OK) {
        fail = 1;
        goto done;
    }
    if (cog_store_append(st, s, node) != COG_OK) {
        fail = 1;
        goto done;
    }
    if (cog_store_delete(st, id) != COG_OK) {
        fail = 1;
        goto done;
    }
    if (cog_store_load(st, id, &out) != COG_OK || out != NULL) {
        fail = 1;
        goto done;
    }
    if (cog_store_list(st, &ids, &count) != COG_OK || ids != NULL ||
        count != 0U) {
        fail = 1;
        goto done;
    }
    if (cog_store_exists(st, id)) {
        fail = 1;
        goto done;
    }

    /* Argument validation still applies. */
    if (cog_store_save(NULL, s) != COG_ERROR_NULL_PTR ||
        cog_store_save(st, NULL) != COG_ERROR_NULL_PTR ||
        cog_store_load(NULL, id, &out) != COG_ERROR_NULL_PTR ||
        cog_store_load(st, NULL, &out) != COG_ERROR_NULL_PTR ||
        cog_store_load(st, id, NULL) != COG_ERROR_NULL_PTR ||
        cog_store_append(NULL, s, node) != COG_ERROR_NULL_PTR ||
        cog_store_append(st, NULL, node) != COG_ERROR_NULL_PTR ||
        cog_store_append(st, s, NULL) != COG_ERROR_NULL_PTR ||
        cog_store_delete(NULL, id) != COG_ERROR_NULL_PTR ||
        cog_store_delete(st, NULL) != COG_ERROR_NULL_PTR ||
        cog_store_list(NULL, &ids, &count) != COG_ERROR_NULL_PTR ||
        cog_store_list(st, NULL, &count) != COG_ERROR_NULL_PTR ||
        cog_store_list(st, &ids, NULL) != COG_ERROR_NULL_PTR ||
        cog_store_exists(NULL, id) || cog_store_exists(st, NULL)) {
        fail = 1;
        goto done;
    }

done:
    cog_store_free(st);
    cog_session_free(s);
    free(ids);
    free(id);
    free(base);
    return fail;
}

/* ------------------------------------------------------------------ */
/* NULL-argument contracts on the file backend                         */
/* ------------------------------------------------------------------ */

static int verify_null_args(void)
{
    char *base = make_tmp("nulargs");
    cog_store_config_t cfg;
    cog_store_t *st = NULL;
    cog_session_t *s = NULL;
    cog_session_node_t *node = NULL;
    cog_session_t *out = NULL;
    char **ids = NULL;
    size_t count = 0U;
    int fail = 0;

    if (base == NULL) {
        return 1;
    }
    memset(&cfg, 0, sizeof(cfg));
    cfg.base_dir = base;
    if (cog_store_new(&st, &cfg) != COG_OK) {
        fail = 1;
        goto done;
    }
    if (cog_session_new(&s, NULL) != COG_OK ||
        cog_session_append(s, MSG(COG_MESSAGE_ROLE_USER, "x"), &node) !=
            COG_OK) {
        fail = 1;
        goto done;
    }
    if (cog_store_save(NULL, s) != COG_ERROR_NULL_PTR ||
        cog_store_save(st, NULL) != COG_ERROR_NULL_PTR ||
        cog_store_load(NULL, "x", &out) != COG_ERROR_NULL_PTR ||
        cog_store_load(st, NULL, &out) != COG_ERROR_NULL_PTR ||
        cog_store_load(st, "x", NULL) != COG_ERROR_NULL_PTR ||
        cog_store_append(NULL, s, node) != COG_ERROR_NULL_PTR ||
        cog_store_append(st, NULL, node) != COG_ERROR_NULL_PTR ||
        cog_store_append(st, s, NULL) != COG_ERROR_NULL_PTR ||
        cog_store_delete(NULL, "x") != COG_ERROR_NULL_PTR ||
        cog_store_delete(st, NULL) != COG_ERROR_NULL_PTR ||
        cog_store_list(NULL, &ids, &count) != COG_ERROR_NULL_PTR ||
        cog_store_list(st, NULL, &count) != COG_ERROR_NULL_PTR ||
        cog_store_list(st, &ids, NULL) != COG_ERROR_NULL_PTR ||
        cog_store_exists(NULL, "x") || cog_store_exists(st, NULL)) {
        fail = 1;
        goto done;
    }

done:
    cog_store_free(st);
    cog_session_free(s);
    free(ids);
    free(base);
    return fail;
}

/* ------------------------------------------------------------------ */
/* Round-trips                                                         */
/* ------------------------------------------------------------------ */

static int verify_empty_roundtrip(void)
{
    char *base = make_tmp("empty");
    cog_store_config_t cfg;
    cog_store_t *st = NULL;
    cog_session_t *s = NULL;
    cog_session_t *out = NULL;
    int fail = 0;
    cog_session_config_t scfg;

    if (base == NULL) {
        return 1;
    }
    memset(&scfg, 0, sizeof(scfg));
    scfg.id = "empty-sess";
    if (cog_session_new(&s, &scfg) != COG_OK) {
        fail = 1;
        goto done;
    }
    memset(&cfg, 0, sizeof(cfg));
    cfg.base_dir = base;
    if (cog_store_new(&st, &cfg) != COG_OK) {
        fail = 1;
        goto done;
    }
    if (!cog_session_is_empty(s) || cog_store_save(st, s) != COG_OK) {
        fail = 1;
        goto done;
    }
    if (!store_files_present(base, "empty-sess")) {
        fail = 1;
        goto done;
    }
    if (count_records(base, "empty-sess") != 0) {
        fail = 1;
        goto done;
    }
    if (cog_store_load(st, "empty-sess", &out) != COG_OK || out == NULL) {
        fail = 1;
        goto done;
    }
    if (!cog_session_is_empty(out) ||
        strcmp(cog_session_id(out), "empty-sess") != 0 ||
        sess_next(out) != 1U) {
        fail = 1;
        goto done;
    }
    if (!cog_store_exists(st, "empty-sess")) {
        fail = 1;
        goto done;
    }
    if (cog_store_delete(st, "empty-sess") != COG_OK ||
        cog_store_exists(st, "empty-sess") ||
        cog_store_delete(st, "empty-sess") != COG_ERROR_NOT_FOUND) {
        fail = 1;
        goto done;
    }

done:
    cog_store_free(st);
    cog_session_free(s);
    cog_session_free(out);
    free(base);
    return fail;
}

/** Build the rich linear conversation used by several round-trips. */
static cog_result_t build_linear(cog_session_t **out_s)
{
    cog_session_t *s = NULL;
    cog_session_node_t *asst = NULL;
    cog_session_node_t *toolcall = NULL;
    cog_session_node_t *dummy = NULL;
    cog_session_config_t scfg;
    cog_timing_t timing;
    cog_cost_t cost;
    cog_result_t rc;
    char blob[4] = {'h', 'i', '\0', 'x'}; /* embedded NUL is payload */

    memset(&scfg, 0, sizeof(scfg));
    scfg.id = "rt-line";
    rc = cog_session_new(&s, &scfg);
    if (rc != COG_OK) {
        return rc;
    }
    rc = cog_session_append(s, MSG(COG_MESSAGE_ROLE_SYSTEM, "sys"), &dummy);
    if (rc != COG_OK) {
        goto fail;
    }
    {
        cog_message_t bm = blob_msg(COG_MESSAGE_ROLE_USER, blob, 4U);

        rc = cog_session_append(s, &bm, &dummy);
    }
    if (rc != COG_OK) {
        goto fail;
    }
    rc = cog_session_append(s, MSG(COG_MESSAGE_ROLE_ASSISTANT, "hello"),
                           &asst);
    if (rc != COG_OK) {
        goto fail;
    }
    rc = cog_session_add_tool_call(s, asst, "read_file",
                                   "{\"path\":\"a b\"}", &toolcall);
    if (rc != COG_OK) {
        goto fail;
    }
    rc = cog_session_add_tool_result(s, toolcall, "file contents", &dummy);
    if (rc != COG_OK) {
        goto fail;
    }
    rc = cog_session_set_parent(s, "parent-rt");
    if (rc != COG_OK) {
        goto fail;
    }

    /* Timing and cost via the public setters. */
    memset(&timing, 0, sizeof(timing));
    timing.request_sent_ms = 10U;
    timing.first_token_ms = 40U;
    timing.last_token_ms = 90U;
    timing.duration_ms = 80U;
    timing.ttft_ms = 30U;
    timing.tokens_per_second = 12.5;
    rc = cog_session_set_timing(s, asst, &timing);
    if (rc != COG_OK) {
        goto fail;
    }
    memset(&cost, 0, sizeof(cost));
    cost.input_tokens = 31U;
    cost.output_tokens = 17U;
    cost.cost_per_input_token = 1.5e-06;
    cost.cost_per_output_token = 7.5e-06;
    cost.total_cost = 0.000174;
    rc = cog_session_set_cost(s, asst, &cost);
    if (rc != COG_OK) {
        goto fail;
    }

    /* Raw provider fields have no public setter; populate them through
     * the same private layout the store reads (white-box test). */
    node_fill_raw(asst, "{\"req\":1}", "{\"resp\":2}", 42U,
                  "claude-sonnet-4-20250514", "anthropic");
    node_fill_raw(toolcall, NULL, NULL, 3U, NULL, NULL);

    /* The active leaf after the tool result. */
    if (cog_session_active(s) != dummy) {
        rc = COG_ERROR_INVALID_STATE;
        goto fail;
    }
    *out_s = s;
    return COG_OK;

fail:
    cog_session_free(s);
    return rc;
}

static int verify_linear_roundtrip(void)
{
    char *base = make_tmp("linear");
    cog_store_config_t cfg;
    cog_store_t *st = NULL;
    cog_store_t *st2 = NULL;
    cog_session_t *s = NULL;
    cog_session_t *out = NULL;
    cog_session_t *out2 = NULL;
    int fail = 0;
    char *d = NULL;
    char *meta1 = NULL;
    char *meta2 = NULL;
    size_t l1 = 0U;
    size_t l2 = 0U;
    char *nodes1 = NULL;
    char *nodes2 = NULL;
    size_t n1 = 0U;
    size_t n2 = 0U;
    char *idx1 = NULL;
    char *idx2 = NULL;
    size_t i1 = 0U;
    size_t i2 = 0U;

    if (base == NULL) {
        return 1;
    }
    if (build_linear(&s) != COG_OK) {
        fail = 1;
        goto done;
    }
    memset(&cfg, 0, sizeof(cfg));
    cfg.base_dir = base;
    if (cog_store_new(&st, &cfg) != COG_OK ||
        cog_store_new(&st2, &cfg) != COG_OK) {
        fail = 1;
        goto done;
    }
    if (cog_store_save(st, s) != COG_OK) {
        fail = 1;
        goto done;
    }
    if (!store_files_present(base, "rt-line") ||
        count_records(base, "rt-line") != 5) {
        fail = 1;
        goto done;
    }
    /* Load through an independent store instance on the same base_dir. */
    if (cog_store_load(st2, "rt-line", &out) != COG_OK || out == NULL) {
        fail = 1;
        goto done;
    }
    if (!trees_eq(s, out)) {
        fail = 1;
        goto done;
    }
    /* Identity, active node, cap, next id, parent id survive exactly. */
    if (strcmp(cog_session_id(out), "rt-line") != 0 ||
        sess_next(out) != sess_next(s) ||
        sess_cap(out) != sess_cap(s) ||
        strcmp(cog_session_parent_id(out), "parent-rt") != 0) {
        fail = 1;
        goto done;
    }
    if (cog_session_active(s) == NULL ||
        cog_session_active(out) == NULL ||
        cog_session_active(s)->data.node_id !=
            cog_session_active(out)->data.node_id) {
        fail = 1;
        goto done;
    }

    /* Byte-identical re-save proves no field is dropped or re-encoded. */
    d = path_join(base, "rt-line");
    if (d == NULL) {
        fail = 1;
        goto done;
    }
    read_store_file(base, "rt-line", "meta.json", &meta1, &l1);
    read_store_file(base, "rt-line", "nodes.jsonl", &nodes1, &n1);
    read_store_file(base, "rt-line", "index.bin", &idx1, &i1);
    if (cog_store_save(st2, s) != COG_OK) {
        fail = 1;
        goto done;
    }
    read_store_file(base, "rt-line", "meta.json", &meta2, &l2);
    read_store_file(base, "rt-line", "nodes.jsonl", &nodes2, &n2);
    read_store_file(base, "rt-line", "index.bin", &idx2, &i2);
    if (l1 != l2 || (l1 > 0U && memcmp(meta1, meta2, l1) != 0) ||
        n1 != n2 || (n1 > 0U && memcmp(nodes1, nodes2, n1) != 0) ||
        i1 != i2 || (i1 > 0U && memcmp(idx1, idx2, i1) != 0)) {
        fail = 1;
        goto done;
    }

    /* Re-load after re-save is identical to the first load. */
    if (cog_store_load(st2, "rt-line", &out2) != COG_OK ||
        !trees_eq(out, out2) ||
        sess_next(out2) != sess_next(s)) {
        fail = 1;
        goto done;
    }

done:
    free(d);
    free(meta1);
    free(meta2);
    free(nodes1);
    free(nodes2);
    free(idx1);
    free(idx2);
    cog_store_free(st);
    cog_store_free(st2);
    cog_session_free(s);
    cog_session_free(out);
    cog_session_free(out2);
    free(base);
    return fail;
}

static int verify_fork_roundtrip(void)
{
    char *base = make_tmp("fork");
    cog_store_config_t cfg;
    cog_store_t *st = NULL;
    cog_session_t *s = NULL;
    cog_session_t *out = NULL;
    cog_session_node_t *root = NULL;
    cog_session_node_t *a1 = NULL;
    cog_session_node_t *fork_a1 = NULL;
    cog_session_node_t *rootfork = NULL;
    int fail = 0;
    cog_session_config_t scfg;

    if (base == NULL) {
        return 1;
    }
    memset(&scfg, 0, sizeof(scfg));
    scfg.id = "rt-fork";
    if (cog_session_new(&s, &scfg) != COG_OK) {
        fail = 1;
        goto done;
    }
    if (cog_session_append(s, MSG(COG_MESSAGE_ROLE_USER, "root q"),
                          &root) != COG_OK ||
        cog_session_append(s, MSG(COG_MESSAGE_ROLE_ASSISTANT, "a1"),
                          &a1) != COG_OK) {
        fail = 1;
        goto done;
    }
    /* Branch 1: continue from a1. */
    if (cog_session_append(s, MSG(COG_MESSAGE_ROLE_USER, "b1"), NULL) !=
        COG_OK) {
        fail = 1;
        goto done;
    }
    /* Fork a1 -> new sibling branch; continue it. */
    if (cog_session_fork(s, a1, &fork_a1) != COG_OK ||
        cog_session_append(s, MSG(COG_MESSAGE_ROLE_USER, "b2"), NULL) !=
        COG_OK) {
        fail = 1;
        goto done;
    }
    /* Fork the root -> second top-level branch with a child. */
    if (cog_session_fork(s, root, &rootfork) != COG_OK ||
        cog_session_append(s, MSG(COG_MESSAGE_ROLE_ASSISTANT, "b3"),
                           NULL) != COG_OK) {
        fail = 1;
        goto done;
    }

    memset(&cfg, 0, sizeof(cfg));
    cfg.base_dir = base;
    if (cog_store_new(&st, &cfg) != COG_OK ||
        cog_store_save(st, s) != COG_OK ||
        cog_store_load(st, "rt-fork", &out) != COG_OK || out == NULL) {
        fail = 1;
        goto done;
    }
    if (!trees_eq(s, out)) {
        fail = 1;
        goto done;
    }
    /* Sibling order preserved at every level. */
    if (cog_node_child_count(cog_session_root(out)) !=
        cog_node_child_count(cog_session_root(s))) {
        fail = 1;
        goto done;
    }
    /* Active node preserved (the last node built). */
    if (cog_session_active(out) == NULL ||
        cog_session_active(s)->data.node_id !=
            cog_session_active(out)->data.node_id) {
        fail = 1;
        goto done;
    }
    /* History equivalence for every leaf in both trees. */
    if (!leaf_histories_eq(s, out)) {
        fail = 1;
        goto done;
    }

done:
    cog_store_free(st);
    cog_session_free(s);
    cog_session_free(out);
    free(base);
    return fail;
}

/* ------------------------------------------------------------------ */
/* Append                                                              */
/* ------------------------------------------------------------------ */

static int verify_append_flows(void)
{
    char *base = make_tmp("append");
    cog_store_config_t cfg;
    cog_store_t *st = NULL;
    cog_session_t *s = NULL;
    cog_session_t *out = NULL;
    cog_session_t *out2 = NULL;
    cog_session_node_t *n1 = NULL;
    cog_session_node_t *n2 = NULL;
    cog_session_node_t *n3 = NULL;
    int fail = 0;
    cog_session_config_t scfg;
    uint64_t next_after_load;

    if (base == NULL) {
        return 1;
    }
    memset(&cfg, 0, sizeof(cfg));
    cfg.base_dir = base;
    if (cog_store_new(&st, &cfg) != COG_OK) {
        fail = 1;
        goto done;
    }

    /* (a) save two nodes, then append the third incrementally. */
    memset(&scfg, 0, sizeof(scfg));
    scfg.id = "ap-1";
    if (cog_session_new(&s, &scfg) != COG_OK ||
        cog_session_append(s, MSG(COG_MESSAGE_ROLE_USER, "u1"), &n1) !=
            COG_OK ||
        cog_session_append(s, MSG(COG_MESSAGE_ROLE_ASSISTANT, "a1"), &n2) !=
            COG_OK ||
        cog_store_save(st, s) != COG_OK) {
        fail = 1;
        goto done;
    }
    if (cog_session_append(s, MSG(COG_MESSAGE_ROLE_USER, "u2"), &n3) !=
            COG_OK ||
        cog_store_append(st, s, n3) != COG_OK) {
        fail = 1;
        goto done;
    }
    if (count_records(base, "ap-1") != 3) {
        fail = 1;
        goto done;
    }
    if (cog_store_load(st, "ap-1", &out) != COG_OK || out == NULL ||
        !trees_eq(s, out)) {
        fail = 1;
        goto done;
    }
    next_after_load = sess_next(out);
    if (next_after_load != 4U) {
        fail = 1;
        goto done;
    }
    /* Appending to the loaded session continues the id sequence. */
    if (cog_session_append(out, MSG(COG_MESSAGE_ROLE_USER, "u3"), &n1) !=
            COG_OK ||
        cog_store_append(st, out, n1) != COG_OK) {
        fail = 1;
        goto done;
    }
    if (cog_store_load(st, "ap-1", &out2) != COG_OK || out2 == NULL) {
        fail = 1;
        goto done;
    }
    if (sess_next(out2) != 5U || count_records(base, "ap-1") != 4) {
        fail = 1;
        goto done;
    }
    cog_session_free(s);
    s = NULL;
    cog_session_free(out);
    out = NULL;
    cog_session_free(out2);
    out2 = NULL;

    /* (b) bootstrap: append without any prior save persists one node. */
    memset(&scfg, 0, sizeof(scfg));
    scfg.id = "ap-2";
    if (cog_session_new(&s, &scfg) != COG_OK ||
        cog_session_append(s, MSG(COG_MESSAGE_ROLE_USER, "u1"), &n1) !=
            COG_OK ||
        cog_session_append(s, MSG(COG_MESSAGE_ROLE_ASSISTANT, "a1"), &n2) !=
            COG_OK ||
        cog_store_append(st, s, n2) != COG_OK) {
        fail = 1;
        goto done;
    }
    if (cog_store_load(st, "ap-2", &out) != COG_OK || out == NULL) {
        fail = 1;
        goto done;
    }
    {
        const cog_node_data_t *rd = NULL;

        if (cog_session_root(out) != NULL) {
            rd = cog_node_data(cog_session_root(out));
        }
        /* Bootstrapped single node loads as a standalone root. */
        if (rd == NULL || rd->role != COG_NODE_ASSISTANT ||
            !str_eq(rd->content, "a1") || rd->parent_id != 0U) {
            fail = 1;
            goto done;
        }
    }
    cog_session_free(s);
    s = NULL;
    cog_session_free(out);
    out = NULL;

    /* (c) foreign node: append a node of another session -> INVALID_STATE. */
    if (cog_session_new(&s, NULL) != COG_OK ||
        cog_session_append(s, MSG(COG_MESSAGE_ROLE_USER, "x"), &n1) !=
            COG_OK ||
        cog_session_append(s, MSG(COG_MESSAGE_ROLE_ASSISTANT, "y"), &n2) !=
            COG_OK) {
        fail = 1;
        goto done;
    }
    if (cog_store_append(st, s, n2) != COG_OK ||
        cog_session_new(&out, NULL) != COG_OK ||
        cog_session_append(out, MSG(COG_MESSAGE_ROLE_USER, "z"), &n3) !=
            COG_OK ||
        cog_store_append(st, out, n2) != COG_ERROR_INVALID_STATE) {
        fail = 1;
        goto done;
    }

done:
    cog_store_free(st);
    cog_session_free(s);
    cog_session_free(out);
    cog_session_free(out2);
    free(base);
    return fail;
}

/* ------------------------------------------------------------------ */
/* Delete / list / exists                                              */
/* ------------------------------------------------------------------ */

static int verify_delete_list_exists(void)
{
    char *base = make_tmp("list");
    cog_store_config_t cfg;
    cog_store_t *st = NULL;
    cog_session_t *s = NULL;
    cog_session_t *out = NULL;
    char **ids = NULL;
    size_t count = 0U;
    size_t i;
    cog_session_config_t scfg;
    cog_store_config_t cfg2;
    cog_store_t *st2 = NULL;
    cog_result_t rc;
    int fail = 0;
    char *junkdir = NULL;
    char *nolist_dir = NULL;
    char *mm_path = NULL;

    if (base == NULL) {
        return 1;
    }
    memset(&cfg, 0, sizeof(cfg));
    cfg.base_dir = base;
    if (cog_store_new(&st, &cfg) != COG_OK) {
        fail = 1;
        goto done;
    }
    /* Save three sessions with distinct ids. */
    memset(&scfg, 0, sizeof(scfg));
    scfg.id = "zz";
    if (cog_session_new(&s, &scfg) != COG_OK ||
        cog_store_save(st, s) != COG_OK) {
        fail = 1;
        goto done;
    }
    cog_session_free(s);
    s = NULL;
    scfg.id = "aa";
    if (cog_session_new(&s, &scfg) != COG_OK ||
        cog_store_save(st, s) != COG_OK) {
        fail = 1;
        goto done;
    }
    cog_session_free(s);
    s = NULL;
    scfg.id = "mm";
    if (cog_session_new(&s, &scfg) != COG_OK ||
        cog_store_save(st, s) != COG_OK) {
        fail = 1;
        goto done;
    }
    cog_session_free(s);
    s = NULL;

    /* Junk in base_dir: a dir without meta.json and a plain file. */
    junkdir = path_join(base, "not-a-session");
    if (junkdir == NULL) {
        fail = 1;
        goto done;
    }
    if (mkdir(junkdir, 0700) != 0) {
        fail = 1;
        goto done;
    }
    {
        char *notes = path_join(base, "notes.txt");

        write_file(notes, "hi", 2U);
        free(notes);
    }

    if (cog_store_list(st, &ids, &count) != COG_OK) {
        fail = 1;
        goto done;
    }
    if (count != 3U || strcmp(ids[0], "aa") != 0 ||
        strcmp(ids[1], "mm") != 0 || strcmp(ids[2], "zz") != 0) {
        fail = 1;
        goto done;
    }
    for (i = 0U; i < count; i++) {
        free(ids[i]);
    }
    free(ids);
    ids = NULL;

    /* exists across a fresh store instance. */
    memset(&cfg2, 0, sizeof(cfg2));
    cfg2.base_dir = base;
    if (cog_store_new(&st2, &cfg2) != COG_OK) {
        fail = 1;
        goto done;
    }
    if (!cog_store_exists(st2, "aa") || !cog_store_exists(st2, "zz") ||
        cog_store_exists(st2, "nope") || cog_store_exists(st2, "not-a-session")) {
        fail = 1;
        goto done;
    }

    /* delete. */
    if (cog_store_delete(st2, "mm") != COG_OK ||
        cog_store_delete(st2, "mm") != COG_ERROR_NOT_FOUND ||
        cog_store_delete(st2, "nope") != COG_ERROR_NOT_FOUND ||
        cog_store_exists(st2, "mm")) {
        fail = 1;
        goto done;
    }
    /* The deleted session's directory is fully gone. */
    mm_path = path_join(base, "mm");
    if (mm_path == NULL || access(mm_path, F_OK) == 0) {
        fail = 1;
        goto done;
    }
    free(mm_path);
    mm_path = NULL;
    if (cog_store_list(st2, &ids, &count) != COG_OK || count != 2U ||
        strcmp(ids[0], "aa") != 0 || strcmp(ids[1], "zz") != 0) {
        fail = 1;
        goto done;
    }
    for (i = 0U; i < count; i++) {
        free(ids[i]);
    }
    free(ids);
    ids = NULL;

    /* load of a deleted session. */
    rc = cog_store_load(st2, "mm", &out);
    if (rc != COG_ERROR_NOT_FOUND || out != NULL) {
        fail = 1;
        goto done;
    }

    /* Missing base_dir behaves as an empty store. */
    cog_store_free(st);
    st = NULL;
    nolist_dir = path_join(base, "no-such-dir");
    if (nolist_dir == NULL) {
        fail = 1;
        goto done;
    }
    cfg2.base_dir = nolist_dir;
    if (cog_store_new(&st, &cfg2) != COG_OK) {
        fail = 1;
        goto done;
    }
    if (cog_store_list(st, &ids, &count) != COG_OK || count != 0U ||
        ids != NULL) {
        fail = 1;
        goto done;
    }
    free(ids);
    ids = NULL;

done:
    free(junkdir);
    free(nolist_dir);
    free(mm_path);
    for (i = 0U; i < count; i++) {
        free(ids[i]);
    }
    free(ids);
    cog_store_free(st);
    cog_store_free(st2);
    cog_session_free(s);
    cog_session_free(out);
    free(base);
    return fail;
}

/* ------------------------------------------------------------------ */
/* max_sessions pruning                                                */
/* ------------------------------------------------------------------ */

static int verify_prune(void)
{
    char *base = make_tmp("prune");
    cog_store_config_t cfg;
    cog_store_t *st = NULL;
    cog_session_t *s = NULL;
    char **ids = NULL;
    size_t count = 0U;
    size_t i;
    cog_session_config_t scfg;
    int fail = 0;
    char *d = NULL;

    if (base == NULL) {
        return 1;
    }
    memset(&cfg, 0, sizeof(cfg));
    cfg.base_dir = base;
    cfg.max_sessions = 3U;
    if (cog_store_new(&st, &cfg) != COG_OK) {
        fail = 1;
        goto done;
    }
    /* Save three sessions and assign deliberately scrambled mtimes so a
     * later prune provably uses mtime order, not lexical order: the
     * mtime-oldest session is s3 (1000), then s2 (1001), then s1. */
    scfg.id = "s1";
    if (cog_session_new(&s, &scfg) != COG_OK ||
        cog_store_save(st, s) != COG_OK) {
        fail = 1;
        goto done;
    }
    cog_session_free(s);
    s = NULL;
    d = path_join(base, "s1");
    set_mtime(d, (time_t)1002);
    free(d);
    d = NULL;

    scfg.id = "s2";
    if (cog_session_new(&s, &scfg) != COG_OK ||
        cog_store_save(st, s) != COG_OK) {
        fail = 1;
        goto done;
    }
    cog_session_free(s);
    s = NULL;
    d = path_join(base, "s2");
    set_mtime(d, (time_t)1001);
    free(d);
    d = NULL;

    scfg.id = "s3";
    if (cog_session_new(&s, &scfg) != COG_OK ||
        cog_store_save(st, s) != COG_OK) {
        fail = 1;
        goto done;
    }
    cog_session_free(s);
    s = NULL;
    d = path_join(base, "s3");
    set_mtime(d, (time_t)1000);
    free(d);
    d = NULL;
    if (cog_store_list(st, &ids, &count) != COG_OK || count != 3U) {
        fail = 1;
        goto done;
    }
    for (i = 0U; i < count; i++) {
        free(ids[i]);
    }
    free(ids);
    ids = NULL;

    /* Saving the fourth session prunes the mtime-oldest (s3), and the
     * just-saved session always survives. */
    scfg.id = "s4";
    if (cog_session_new(&s, &scfg) != COG_OK ||
        cog_store_save(st, s) != COG_OK) {
        fail = 1;
        goto done;
    }
    cog_session_free(s);
    s = NULL;
    if (cog_store_list(st, &ids, &count) != COG_OK || count != 3U ||
        cog_store_exists(st, "s3")) {
        fail = 1;
        goto done;
    }
    {
        int haves1 = 0;
        int haves2 = 0;
        int haves4 = 0;

        for (i = 0U; i < count; i++) {
            if (strcmp(ids[i], "s1") == 0) {
                haves1 = 1;
            }
            if (strcmp(ids[i], "s2") == 0) {
                haves2 = 1;
            }
            if (strcmp(ids[i], "s4") == 0) {
                haves4 = 1;
            }
        }
        if (!haves1 || !haves2 || !haves4) {
            fail = 1;
        }
    }
    for (i = 0U; i < count; i++) {
        free(ids[i]);
    }
    free(ids);
    ids = NULL;

    /* max_sessions == 0 means unlimited (count stays 3 + 2 new). */
    cfg.max_sessions = 0U;
    cog_store_free(st);
    st = NULL;
    if (cog_store_new(&st, &cfg) != COG_OK) {
        fail = 1;
        goto done;
    }
    scfg.id = "s5";
    if (cog_session_new(&s, &scfg) != COG_OK ||
        cog_store_save(st, s) != COG_OK) {
        fail = 1;
        goto done;
    }
    cog_session_free(s);
    s = NULL;
    scfg.id = "s6";
    if (cog_session_new(&s, &scfg) != COG_OK ||
        cog_store_save(st, s) != COG_OK) {
        fail = 1;
        goto done;
    }
    cog_session_free(s);
    s = NULL;
    if (cog_store_list(st, &ids, &count) != COG_OK || count != 5U) {
        fail = 1;
        goto done;
    }
    for (i = 0U; i < count; i++) {
        free(ids[i]);
    }
    free(ids);
    ids = NULL;

done:
    if (ids != NULL) {
        for (i = 0U; i < count; i++) {
            free(ids[i]);
        }
        free(ids);
    }
    cog_store_free(st);
    cog_session_free(s);
    free(base);
    return fail;
}

/* ------------------------------------------------------------------ */
/* Corruption and error codes                                          */
/* ------------------------------------------------------------------ */

static int save_small(const char *base, const char *id)
{
    cog_store_config_t cfg;
    cog_store_t *st = NULL;
    cog_session_t *s = NULL;
    cog_session_config_t scfg;
    cog_result_t rc;

    memset(&cfg, 0, sizeof(cfg));
    cfg.base_dir = base;
    rc = cog_store_new(&st, &cfg);
    if (rc != COG_OK) {
        goto done;
    }
    memset(&scfg, 0, sizeof(scfg));
    scfg.id = id;
    rc = cog_session_new(&s, &scfg);
    if (rc != COG_OK) {
        goto done;
    }
    rc = cog_session_append(s, MSG(COG_MESSAGE_ROLE_USER, "u1"), NULL);
    if (rc != COG_OK) {
        goto done;
    }
    rc = cog_session_append(s, MSG(COG_MESSAGE_ROLE_ASSISTANT, "a1"), NULL);
    if (rc != COG_OK) {
        goto done;
    }
    rc = cog_store_save(st, s);
done:
    cog_store_free(st);
    cog_session_free(s);
    return rc == COG_OK ? 0 : 1;
}

static int verify_corruption(void)
{
    char *base = make_tmp("corrupt");
    cog_store_config_t cfg;
    cog_store_t *st = NULL;
    cog_session_t *out = NULL;
    cog_session_t *sentinel = (cog_session_t *)0x1234;
    char *d = NULL;
    char *p = NULL;
    char rec[4096];
    int fail = 0;

    if (base == NULL) {
        return 1;
    }
    memset(&cfg, 0, sizeof(cfg));
    cfg.base_dir = base;
    if (cog_store_new(&st, &cfg) != COG_OK) {
        fail = 1;
        goto done;
    }
    if (save_small(base, "good") != 0) {
        fail = 1;
        goto done;
    }
    if (cog_store_load(st, "good", &out) != COG_OK || out == NULL) {
        fail = 1;
        goto done;
    }
    cog_session_free(out);
    out = NULL;

    /* Missing session / missing meta -> NOT_FOUND. */
    out = sentinel;
    if (cog_store_load(st, "missing", &out) != COG_ERROR_NOT_FOUND ||
        out != sentinel) {
        fail = 1;
        goto done;
    }
    d = path_join(base, "nosess");
    if (mkdir(d, 0700) != 0) {
        fail = 1;
        goto done;
    }
    free(d);
    d = NULL;
    if (cog_store_load(st, "nosess", &out) != COG_ERROR_NOT_FOUND ||
        cog_store_delete(st, "nosess") != COG_ERROR_NOT_FOUND ||
        cog_store_exists(st, "nosess")) {
        fail = 1;
        goto done;
    }

    /* Garbage meta. */
    d = path_join(base, "gmeta");
    if (mkdir(d, 0700) != 0) {
        fail = 1;
        goto done;
    }
    p = path_join(d, "meta.json");
    write_file(p, "this is not json", 16U);
    free(p);
    p = path_join(d, "nodes.jsonl");
    write_file(p, "", 0U);
    free(p);
    free(d);
    d = NULL;
    if (cog_store_load(st, "gmeta", &out) != COG_ERROR_IO) {
        fail = 1;
        goto done;
    }

    /* Truncated meta. */
    d = path_join(base, "tmeta");
    if (mkdir(d, 0700) != 0) {
        fail = 1;
        goto done;
    }
    p = path_join(d, "meta.json");
    write_file(p, "{\"version\":1", 12U);
    free(p);
    p = path_join(d, "nodes.jsonl");
    write_file(p, "", 0U);
    free(p);
    free(d);
    d = NULL;
    if (cog_store_load(st, "tmeta", &out) != COG_ERROR_IO) {
        fail = 1;
        goto done;
    }

    /* Meta version mismatch. */
    if (save_small(base, "vbad") != 0) {
        fail = 1;
        goto done;
    }
    d = path_join(base, "vbad");
    p = path_join(d, "meta.json");
    if (meta_set_uint(p, "version", 2) != 0) {
        fail = 1;
        goto done;
    }
    free(p);
    free(d);
    d = NULL;
    if (cog_store_load(st, "vbad", &out) != COG_ERROR_IO) {
        fail = 1;
        goto done;
    }

    /* Meta id does not match the directory name. */
    if (save_small(base, "abc") != 0) {
        fail = 1;
        goto done;
    }
    d = path_join(base, "xmeta");
    if (mkdir(d, 0700) != 0) {
        fail = 1;
        goto done;
    }
    {
        char *src = path_join(base, "abc");
        char *mdst = path_join(d, "meta.json");
        char *ndst = path_join(d, "nodes.jsonl");
        char *xdst = path_join(d, "index.bin");
        size_t sl = 0U;
        char *sbuf = NULL;
        char *sp = path_join(src, "meta.json");

        if (read_file(sp, &sbuf, &sl) == 0) {
            write_file(mdst, sbuf, sl);
        }
        free(sbuf);
        free(sp);
        sbuf = NULL;
        sp = path_join(src, "nodes.jsonl");
        if (read_file(sp, &sbuf, &sl) == 0) {
            write_file(ndst, sbuf, sl);
        }
        free(sbuf);
        free(sp);
        sbuf = NULL;
        sp = path_join(src, "index.bin");
        if (read_file(sp, &sbuf, &sl) == 0) {
            write_file(xdst, sbuf, sl);
        }
        free(sbuf);
        free(sp);
        free(src);
        free(mdst);
        free(ndst);
        free(xdst);
    }
    free(d);
    d = NULL;
    if (cog_store_load(st, "xmeta", &out) != COG_ERROR_IO) {
        fail = 1;
        goto done;
    }

    /* Meta node_count mismatch. */
    if (save_small(base, "cntbad") != 0) {
        fail = 1;
        goto done;
    }
    d = path_join(base, "cntbad");
    p = path_join(d, "meta.json");
    if (meta_set_uint(p, "node_count", 99) != 0) {
        fail = 1;
        goto done;
    }
    free(p);
    free(d);
    d = NULL;
    if (cog_store_load(st, "cntbad", &out) != COG_ERROR_IO) {
        fail = 1;
        goto done;
    }

    /* Meta active_node_id pointing nowhere. */
    if (save_small(base, "actbad") != 0) {
        fail = 1;
        goto done;
    }
    d = path_join(base, "actbad");
    p = path_join(d, "meta.json");
    if (meta_set_uint(p, "active_node_id", 99999) != 0) {
        fail = 1;
        goto done;
    }
    free(p);
    free(d);
    d = NULL;
    if (cog_store_load(st, "actbad", &out) != COG_ERROR_IO) {
        fail = 1;
        goto done;
    }

    /* Meta next_node_id too small to be unique. */
    if (save_small(base, "nextbad") != 0) {
        fail = 1;
        goto done;
    }
    d = path_join(base, "nextbad");
    p = path_join(d, "meta.json");
    if (meta_set_uint(p, "next_node_id", 1) != 0) {
        fail = 1;
        goto done;
    }
    free(p);
    free(d);
    d = NULL;
    if (cog_store_load(st, "nextbad", &out) != COG_ERROR_IO) {
        fail = 1;
        goto done;
    }

    /* Missing nodes.jsonl. */
    if (save_small(base, "nonodes") != 0) {
        fail = 1;
        goto done;
    }
    d = path_join(base, "nonodes");
    p = path_join(d, "nodes.jsonl");
    (void)unlink(p);
    free(p);
    free(d);
    d = NULL;
    if (cog_store_load(st, "nonodes", &out) != COG_ERROR_IO) {
        fail = 1;
        goto done;
    }

    /* Truncated nodes.jsonl: a partial record line. */
    if (save_small(base, "trunc") != 0) {
        fail = 1;
        goto done;
    }
    d = path_join(base, "trunc");
    p = path_join(d, "nodes.jsonl");
    write_file(p, "{\"node_id\":1,\"parent_i", 23U);
    free(p);
    free(d);
    d = NULL;
    if (cog_store_load(st, "trunc", &out) != COG_ERROR_IO) {
        fail = 1;
        goto done;
    }

    /* Invalid base64 inside a record. */
    if (save_small(base, "badb64") != 0) {
        fail = 1;
        goto done;
    }
    d = path_join(base, "badb64");
    p = path_join(d, "nodes.jsonl");
    (void)snprintf(rec, sizeof(rec), KREC_FMT, 1ULL, 0ULL, 0, "!!!!");
    write_file(p, rec, strlen(rec));
    free(p);
    free(d);
    d = NULL;
    if (cog_store_load(st, "badb64", &out) != COG_ERROR_IO) {
        fail = 1;
        goto done;
    }

    /* Unknown role. */
    if (save_small(base, "badrole") != 0) {
        fail = 1;
        goto done;
    }
    d = path_join(base, "badrole");
    p = path_join(d, "nodes.jsonl");
    (void)snprintf(rec, sizeof(rec), KREC_FMT, 1ULL, 0ULL, 7, "aGk=");
    write_file(p, rec, strlen(rec));
    free(p);
    free(d);
    d = NULL;
    if (cog_store_load(st, "badrole", &out) != COG_ERROR_IO) {
        fail = 1;
        goto done;
    }

    /* Duplicate node ids (meta count bumped so only the dup trips IO). */
    if (save_small(base, "dupid") != 0) {
        fail = 1;
        goto done;
    }
    d = path_join(base, "dupid");
    p = path_join(d, "meta.json");
    if (meta_set_uint(p, "node_count", 2) != 0) {
        fail = 1;
        goto done;
    }
    free(p);
    p = path_join(d, "nodes.jsonl");
    (void)snprintf(rec, sizeof(rec), KREC_FMT, 1ULL, 0ULL, 0, "aGk=");
    write_file(p, rec, strlen(rec));
    {
        FILE *fp = fopen(p, "ab");

        if (fp != NULL) {
            (void)snprintf(rec, sizeof(rec), KREC_FMT, 1ULL, 1ULL, 1,
                           "aGk=");
            (void)fwrite(rec, 1U, strlen(rec), fp);
            (void)fclose(fp);
        }
    }
    free(p);
    p = NULL;
    free(d);
    d = NULL;
    if (cog_store_load(st, "dupid", &out) != COG_ERROR_IO) {
        fail = 1;
        goto done;
    }

    /* Corrupt index.bin is tolerated (advisory cache only). */
    if (save_small(base, "badidx") != 0) {
        fail = 1;
        goto done;
    }
    d = path_join(base, "badidx");
    p = path_join(d, "index.bin");
    write_file(p, "GARBAGE-GARBAGE-GARBAGE", 23U);
    free(p);
    p = NULL;
    free(d);
    d = NULL;
    if (cog_store_load(st, "badidx", &out) != COG_OK || out == NULL) {
        fail = 1;
        goto done;
    }
    /* The corrupt cache does not corrupt the load: the tree is intact. */
    if (cog_session_is_empty(out) || cog_session_root(out) == NULL ||
        cog_node_child_count(cog_session_root(out)) != 1U) {
        fail = 1;
        goto done;
    }
    cog_session_free(out);
    out = NULL;

    /* Unwritable setup: base_dir whose parent does not exist -> IO. */
    {
        cog_store_config_t cfg3;
        cog_store_t *st3 = NULL;
        cog_session_config_t scfg3;
        cog_session_t *s3 = NULL;
        char *nested = path_join(base, "nope/deeper");

        memset(&cfg3, 0, sizeof(cfg3));
        cfg3.base_dir = nested;
        if (cog_store_new(&st3, &cfg3) != COG_OK) {
            fail = 1;
        } else {
            memset(&scfg3, 0, sizeof(scfg3));
            scfg3.id = "x";
            if (cog_session_new(&s3, &scfg3) == COG_OK) {
                if (cog_store_save(st3, s3) != COG_ERROR_IO) {
                    fail = 1;
                }
                cog_session_free(s3);
            } else {
                fail = 1;
            }
            cog_store_free(st3);
        }
        free(nested);
    }

done:
    free(d);
    free(p);
    cog_store_free(st);
    cog_session_free(out);
    free(base);
    return fail;
}

/* ------------------------------------------------------------------ */
/* Path safety                                                         */
/* ------------------------------------------------------------------ */

static int verify_path_safety(void)
{
    char *base = make_tmp("pathsafe");
    cog_store_config_t cfg;
    cog_store_t *st = NULL;
    cog_session_t *s = NULL;
    cog_session_config_t scfg;
    char **ids = NULL;
    size_t count = 0U;
    size_t i;
    char long_id[200];
    const char *bad_ids[] = {"../../escape", "a/b", "a\\b", ".", "..", "",
                             "bad\x01id", "bad\x7fid"};
    int fail = 0;

    if (base == NULL) {
        return 1;
    }
    memset(&cfg, 0, sizeof(cfg));
    cfg.base_dir = base;
    if (cog_store_new(&st, &cfg) != COG_OK) {
        fail = 1;
        goto done;
    }

    /* Unsafe ids are rejected by save/load/delete; exists returns false. */
    for (i = 0U; i < sizeof(bad_ids) / sizeof(bad_ids[0]); i++) {
        memset(&scfg, 0, sizeof(scfg));
        scfg.id = bad_ids[i];
        if (cog_session_new(&s, &scfg) != COG_OK) {
            fail = 1;
            goto done;
        }
        if (cog_store_save(st, s) != COG_ERROR_INVALID_ARG) {
            fail = 1;
            goto done;
        }
        cog_session_free(s);
        s = NULL;
        if (cog_store_load(st, bad_ids[i], &s) != COG_ERROR_INVALID_ARG ||
            cog_store_delete(st, bad_ids[i]) != COG_ERROR_INVALID_ARG ||
            cog_store_exists(st, bad_ids[i])) {
            fail = 1;
            goto done;
        }
    }

    /* An over-long id overflows. */
    memset(long_id, 'x', sizeof(long_id) - 1U);
    long_id[sizeof(long_id) - 1U] = '\0';
    memset(&scfg, 0, sizeof(scfg));
    scfg.id = long_id;
    if (cog_session_new(&s, &scfg) != COG_OK) {
        fail = 1;
        goto done;
    }
    if (cog_store_save(st, s) != COG_ERROR_OVERFLOW ||
        cog_store_load(st, long_id, &s) != COG_ERROR_OVERFLOW ||
        cog_store_delete(st, long_id) != COG_ERROR_OVERFLOW ||
        cog_store_exists(st, long_id)) {
        fail = 1;
        goto done;
    }
    cog_session_free(s);
    s = NULL;

    /* Nothing was written outside base_dir by the rejected ids. */
    if (cog_store_list(st, &ids, &count) != COG_OK || count != 0U) {
        fail = 1;
        goto done;
    }
    free(ids);
    ids = NULL;
    {
        char *parent = path_join(base, "..");

        if (parent == NULL || access(parent, F_OK) != 0) {
            /* base_dir parent must still exist */
            fail = 1;
            goto done;
        }
        free(parent);
    }
    if (access("/tmp/escape", F_OK) == 0) {
        fail = 1; /* traversal must not have escaped */
        goto done;
    }

    /* A peculiar-but-safe id round-trips. */
    scfg.id = "under_score.dash-1";
    if (cog_session_new(&s, &scfg) != COG_OK ||
        cog_store_save(st, s) != COG_OK) {
        fail = 1;
        goto done;
    }
    cog_session_free(s);
    s = NULL;
    if (cog_store_load(st, "under_score.dash-1", &s) != COG_OK ||
        s == NULL) {
        fail = 1;
        goto done;
    }
    cog_session_free(s);
    s = NULL;

done:
    if (ids != NULL) {
        for (i = 0U; i < count; i++) {
            free(ids[i]);
        }
        free(ids);
    }
    cog_store_free(st);
    cog_session_free(s);
    free(base);
    return fail;
}

/* ------------------------------------------------------------------ */
/* compress / auto_save configuration                                  */
/* ------------------------------------------------------------------ */

static int verify_config_flags(void)
{
    char *base = make_tmp("flags");
    cog_store_config_t cfg;
    cog_store_t *st = NULL;
    cog_session_t *s = NULL;
    cog_session_t *out = NULL;
    cog_session_config_t scfg;
    char *d = NULL;
    char *p = NULL;
    char *buf = NULL;
    size_t len = 0U;
    int fail = 0;

    if (base == NULL) {
        return 1;
    }
    memset(&cfg, 0, sizeof(cfg));
    cfg.base_dir = base;
    cfg.compress = true;
    cfg.auto_save = true;
    if (cog_store_new(&st, &cfg) != COG_OK) {
        fail = 1;
        goto done;
    }
    memset(&scfg, 0, sizeof(scfg));
    scfg.id = "flags-sess";
    if (cog_session_new(&s, &scfg) != COG_OK ||
        cog_session_append(s, MSG(COG_MESSAGE_ROLE_USER, "payload"), NULL) !=
            COG_OK ||
        cog_store_save(st, s) != COG_OK) {
        fail = 1;
        goto done;
    }
    /* The store does not pretend to compress: nodes.jsonl stays plain
     * JSONL and meta.json stays plain JSON. */
    d = path_join(base, "flags-sess");
    p = path_join(d, "nodes.jsonl");
    if (read_file(p, &buf, &len) != 0) {
        fail = 1;
        goto done;
    }
    if (strstr(buf, "\"node_id\"") == NULL) {
        fail = 1;
        goto done;
    }
    free(buf);
    buf = NULL;
    free(p);
    p = path_join(d, "meta.json");
    if (read_file(p, &buf, &len) != 0) {
        fail = 1;
        goto done;
    }
    if (strstr(buf, "\"version\"") == NULL) {
        fail = 1;
        goto done;
    }
    free(buf);
    buf = NULL;
    free(p);
    p = NULL;
    free(d);
    d = NULL;
    /* auto_save=true is accepted; the trigger lives in the core
     * integration layer (documented boundary), so the store itself
     * performs no implicit writes. */
    if (cog_store_load(st, "flags-sess", &out) != COG_OK || out == NULL) {
        fail = 1;
        goto done;
    }
    {
        const cog_node_data_t *rd = NULL;

        if (cog_session_root(out) != NULL) {
            rd = cog_node_data(cog_session_root(out));
        }
        if (rd == NULL || rd->content_len != 7U ||
            memcmp(rd->content, "payload", 7U) != 0) {
            fail = 1;
            goto done;
        }
    }

done:
    free(d);
    free(p);
    free(buf);
    cog_store_free(st);
    cog_session_free(s);
    cog_session_free(out);
    free(base);
    return fail;
}

/* ------------------------------------------------------------------ */
/* Stress                                                              */
/* ------------------------------------------------------------------ */

static int verify_stress(void)
{
    char *base = make_tmp("stress");
    cog_store_config_t cfg;
    cog_store_t *st = NULL;
    cog_session_t *s = NULL;
    cog_session_t *out = NULL;
    cog_session_t *fan = NULL;
    cog_session_t *fanout = NULL;
    cog_session_node_t *root = NULL;
    cog_session_node_t *node = NULL;
    cog_session_config_t scfg;
    size_t i;
    int fail = 0;

    if (base == NULL) {
        return 1;
    }
    memset(&cfg, 0, sizeof(cfg));
    cfg.base_dir = base;
    if (cog_store_new(&st, &cfg) != COG_OK) {
        fail = 1;
        goto done;
    }

    /* 2000-node chain (node ids 1..2000). */
    memset(&scfg, 0, sizeof(scfg));
    scfg.id = "stress-chain";
    if (cog_session_new(&s, &scfg) != COG_OK) {
        fail = 1;
        goto done;
    }
    for (i = 0U; i < 2000U; i++) {
        cog_message_t m =
            (i % 2U == 0U)
                ? blob_msg(COG_MESSAGE_ROLE_USER, "u", 1U)
                : blob_msg(COG_MESSAGE_ROLE_ASSISTANT, "a", 1U);

        if (cog_session_append(s, &m, NULL) != COG_OK) {
            fail = 1;
            goto done;
        }
    }
    if (cog_store_save(st, s) != COG_OK) {
        fail = 1;
        goto done;
    }
    if (cog_store_load(st, "stress-chain", &out) != COG_OK || out == NULL) {
        fail = 1;
        goto done;
    }
    if (!trees_eq(s, out) || sess_next(out) != 2001U) {
        fail = 1;
        goto done;
    }
    /* Verify id continuity down the chain. */
    {
        cog_session_node_t *n = cog_session_root(out);
        uint64_t expect = 1U;

        while (n != NULL) {
            if (cog_node_data(n)->node_id != expect) {
                fail = 1;
                goto done;
            }
            expect++;
            n = cog_node_first_child(n);
        }
        if (expect != 2001U) {
            fail = 1;
            goto done;
        }
    }
    cog_session_free(s);
    s = NULL;
    cog_session_free(out);
    out = NULL;

    /* 300-child fan-out from the root. */
    memset(&scfg, 0, sizeof(scfg));
    scfg.id = "stress-fan";
    if (cog_session_new(&fan, &scfg) != COG_OK ||
        cog_session_append(fan, MSG(COG_MESSAGE_ROLE_USER, "root"), &root) !=
            COG_OK) {
        fail = 1;
        goto done;
    }
    for (i = 0U; i < 300U; i++) {
        cog_message_t m = blob_msg(COG_MESSAGE_ROLE_USER, "c", 1U);

        if (cog_session_add_message(fan, root, &m, NULL) != COG_OK) {
            fail = 1;
            goto done;
        }
    }
    if (cog_store_save(st, fan) != COG_OK ||
        cog_store_load(st, "stress-fan", &fanout) != COG_OK ||
        fanout == NULL) {
        fail = 1;
        goto done;
    }
    if (!trees_eq(fan, fanout) ||
        cog_node_child_count(cog_session_root(fanout)) != 300U) {
        fail = 1;
        goto done;
    }
    {
        cog_session_node_t *c = cog_node_first_child(cog_session_root(fanout));
        size_t seen = 0U;

        while (c != NULL) {
            seen++;
            c = cog_node_next_sibling(c);
        }
        if (seen != 300U) {
            fail = 1;
            goto done;
        }
    }

done:
    (void)node;
    cog_store_free(st);
    cog_session_free(s);
    cog_session_free(out);
    cog_session_free(fan);
    cog_session_free(fanout);
    free(base);
    return fail;
}

/* ------------------------------------------------------------------ */
/* Failed delete leaves the session and its metadata intact            */
/* ------------------------------------------------------------------ */

static int verify_delete_preflight(void)
{
    char *base = make_tmp("delpreflight");
    cog_store_config_t cfg;
    cog_store_t *st = NULL;
    cog_session_t *s = NULL;
    cog_session_config_t scfg;
    char *dir = NULL;
    char *stray = NULL;
    char *meta = NULL;
    size_t meta_len = 0U;
    int fail = 0;

    if (base == NULL) {
        return 1;
    }
    memset(&cfg, 0, sizeof(cfg));
    cfg.base_dir = base;
    memset(&scfg, 0, sizeof(scfg));
    scfg.id = "keep-intact";
    if (cog_store_new(&st, &cfg) != COG_OK ||
        cog_session_new(&s, &scfg) != COG_OK ||
        cog_session_append(s, MSG(COG_MESSAGE_ROLE_USER, "keep"), NULL) !=
            COG_OK ||
        cog_store_save(st, s) != COG_OK) {
        fail = 1;
        goto done;
    }
    dir = path_join(base, "keep-intact");
    stray = path_join(dir, "stray.txt");
    if (dir == NULL || stray == NULL) {
        fail = 1;
        goto done;
    }
    write_file(stray, "x", 1U);

    /* The unknown entry is rejected before unlinking.  A repeated delete
     * reports IO (not NOT_FOUND), and meta.json remains readable. */
    if (cog_store_delete(st, "keep-intact") != COG_ERROR_IO ||
        !cog_store_exists(st, "keep-intact") ||
        cog_store_delete(st, "keep-intact") != COG_ERROR_IO ||
        read_store_file(base, "keep-intact", "meta.json", &meta,
                        &meta_len) != 0 ||
        meta == NULL || strstr(meta, "\"version\":1") == NULL) {
        fail = 1;
        goto done;
    }

    /* Removing the obstruction makes the same delete succeed normally. */
    if (unlink(stray) != 0 ||
        cog_store_delete(st, "keep-intact") != COG_OK ||
        cog_store_exists(st, "keep-intact") ||
        cog_store_delete(st, "keep-intact") != COG_ERROR_NOT_FOUND) {
        fail = 1;
        goto done;
    }

done:
    free(meta);
    free(stray);
    free(dir);
    cog_store_free(st);
    cog_session_free(s);
    rmtree(base);
    free(base);
    return fail;
}

/* ------------------------------------------------------------------ */
/* Default config: file backend in the current directory               */
/* ------------------------------------------------------------------ */

static int verify_default_base_dir(void)
{
    char cwd[4096];
    char *tmp = NULL;
    char *orig = NULL;
    char *sessdir = NULL;
    cog_store_t *st = NULL;
    cog_session_t *s = NULL;
    cog_session_t *out = NULL;
    cog_session_config_t scfg;
    int fail = 0;

    orig = getcwd(cwd, sizeof(cwd));
    if (orig == NULL) {
        return 1;
    }
    tmp = make_tmp("defaultcwd");
    if (tmp == NULL) {
        return 1;
    }
    if (chdir(tmp) != 0) {
        rmtree(tmp);
        free(tmp);
        return 1;
    }
    /* NULL config: file backend rooted at "." (the current directory). */
    if (cog_store_new(&st, NULL) != COG_OK) {
        fail = 1;
        goto done;
    }
    memset(&scfg, 0, sizeof(scfg));
    scfg.id = "cwd-sess";
    if (cog_session_new(&s, &scfg) != COG_OK ||
        cog_store_save(st, s) != COG_OK) {
        fail = 1;
        goto done;
    }
    if (cog_store_load(st, "cwd-sess", &out) != COG_OK || out == NULL) {
        fail = 1;
        goto done;
    }
    sessdir = path_join(tmp, "cwd-sess");
    if (access(sessdir, F_OK) != 0) {
        fail = 1;
        goto done;
    }

done:
    (void)chdir(orig);
    cog_store_free(st);
    cog_session_free(s);
    cog_session_free(out);
    if (sessdir != NULL) {
        rmtree(sessdir);
    }
    free(sessdir);
    rmtree(tmp);
    free(tmp);
    return fail;
}

int main(void)
{
    unsigned failures = 0U;

#define CHECK(label, expr)                                                   \
    do {                                                                     \
        if ((expr) != 0) {                                                   \
            (void)fprintf(stderr, "FAIL: %s\n", label);                      \
            ++failures;                                                      \
        }                                                                    \
    } while (0)

    CHECK("lifecycle", verify_lifecycle());
    CHECK("null backend", verify_null_backend());
    CHECK("file null args", verify_null_args());
    CHECK("empty roundtrip", verify_empty_roundtrip());
    CHECK("linear roundtrip", verify_linear_roundtrip());
    CHECK("fork roundtrip", verify_fork_roundtrip());
    CHECK("append flows", verify_append_flows());
    CHECK("delete/list/exists", verify_delete_list_exists());
    CHECK("prune", verify_prune());
    CHECK("corruption", verify_corruption());
    CHECK("path safety", verify_path_safety());
    CHECK("config flags", verify_config_flags());
    CHECK("stress", verify_stress());
    CHECK("delete preflight", verify_delete_preflight());
    CHECK("default base dir", verify_default_base_dir());

#undef CHECK

    if (failures == 0U) {
        (void)printf("test_session_store: ok\n");
    }
    return failures == 0U ? 0 : 1;
}

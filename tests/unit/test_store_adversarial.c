/**
 * @file test_store_adversarial.c
 * @brief Critic adversarial tests for session_store.c (ticket 7ba4714).
 *
 * These tests probe gaps the Writer's test_session_store.c does not cover:
 *   - strict-schema rejections the Writer's corruption test omits (unknown
 *     field, missing required field, multiple roots, root-not-first,
 *     node_id == 0, parent-not-preceding-child);
 *   - a USER node whose content is null (must load without crashing);
 *   - append of a duplicate node id, then load must fail closed (IO);
 *   - append of a node whose parent is not yet persisted, then load fails
 *     closed (IO) — caller misuse, never a crash;
 *   - max_sessions pruning must never delete the just-saved session even
 *     when it is the oldest on disk;
 *   - a full save after appends regenerates a consistent index.bin;
 *   - a stray file inside a session directory makes delete fail closed (IO);
 *   - bootstrap append round-trips raw provider fields and next_node_id.
 *
 * White-box via session_internal.h (same private layout session_store.c
 * uses) for raw fields / next-node ids, exactly like test_session_store.c.
 *
 * Ticket: 7ba4714 (Critic review of 0efef06).
 */

#define _POSIX_C_SOURCE 200809L

#include "cog/session_store.h"

#include "session_internal.h"

#include <dirent.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

static int failures = 0;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            (void)fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__,         \
                          __LINE__, #cond);                             \
            ++failures;                                                 \
        }                                                               \
    } while (0)

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

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

static char *make_tmp(const char *tag)
{
    char templ[128];
    int n = snprintf(templ, sizeof(templ), "/tmp/cog-adv-%s-XXXXXX", tag);
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

static void set_mtime(const char *path, time_t sec)
{
    struct timespec ts[2];

    ts[0].tv_sec = sec;
    ts[0].tv_nsec = 0;
    ts[1].tv_sec = sec;
    ts[1].tv_nsec = 0;
    (void)utimensat(AT_FDCWD, path, ts, 0);
}

/* Minimal base64 encoder for short ASCII session ids (used to build a
 * meta.json whose id matches the directory name). */
static void b64_id(const char *s, char *out, size_t outcap)
{
    static const char tab[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t len = strlen(s);
    size_t i;
    size_t o = 0U;

    for (i = 0U; i < len; i += 3U) {
        uint32_t v;
        size_t rem = len - i;

        v = (uint32_t)(unsigned char)s[i] << 16U;
        if (rem > 1U) {
            v |= (uint32_t)(unsigned char)s[i + 1U] << 8U;
        }
        if (rem > 2U) {
            v |= (uint32_t)(unsigned char)s[i + 2U];
        }
        if (o + 4U < outcap) {
            out[o++] = tab[(v >> 18U) & 0x3fU];
            out[o++] = tab[(v >> 12U) & 0x3fU];
            out[o++] = (rem > 1U) ? tab[(v >> 6U) & 0x3fU] : '=';
            out[o++] = (rem > 2U) ? tab[v & 0x3fU] : '=';
        }
    }
    out[o] = '\0';
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

static uint64_t sess_next(const cog_session_t *s)
{
    const struct cog_session *cs = (const struct cog_session *)s;

    return cs->next_node_id;
}

/* A minimal valid single-node record (content base64 "aGk=" == "hi"). */
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

/* Build a session dir with a single valid record and matching meta. */
static int seed_session(const char *base, const char *id, uint64_t node_id)
{
    char *d = path_join(base, id);
    char *p;
    char rec[4096];
    char meta[512];
    char idb64[64];

    if (d == NULL) {
        return -1;
    }
    if (mkdir(d, 0700) != 0) {
        free(d);
        return -1;
    }
    p = path_join(d, "nodes.jsonl");
    if (p == NULL) {
        free(d);
        return -1;
    }
    (void)snprintf(rec, sizeof(rec), KREC_FMT,
                   (unsigned long long)node_id, 0ULL, 0, "aGk=");
    write_file(p, rec, strlen(rec));
    free(p);
    p = path_join(d, "meta.json");
    if (p == NULL) {
        free(d);
        return -1;
    }
    b64_id(id, idb64, sizeof(idb64));
    (void)snprintf(meta, sizeof(meta),
                   "{\"version\":1,\"id\":\"%s\",\"parent_id\":null,"
                   "\"max_nodes\":0,\"node_count\":1,"
                   "\"next_node_id\":%llu,\"active_node_id\":%llu}",
                   idb64, (unsigned long long)(node_id + 1U),
                   (unsigned long long)node_id);
    write_file(p, meta, strlen(meta));
    free(p);
    free(d);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Strict-schema rejections                                            */
/* ------------------------------------------------------------------ */

static void verify_unknown_field(void)
{
    char *base = make_tmp("unkfield");
    cog_store_config_t cfg;
    cog_store_t *st = NULL;
    cog_session_t *out = NULL;
    char *d;
    char *p;
    char rec[4096];

    if (base == NULL) {
        return;
    }
    memset(&cfg, 0, sizeof(cfg));
    cfg.base_dir = base;
    CHECK(cog_store_new(&st, &cfg) == COG_OK);
    if (seed_session(base, "s", 1U) != 0) {
        goto done;
    }
    d = path_join(base, "s");
    p = path_join(d, "nodes.jsonl");
    (void)snprintf(rec, sizeof(rec), KREC_FMT, 1ULL, 0ULL, 0, "aGk=");
    /* Insert an unknown field before the closing brace. */
    {
        size_t len = strlen(rec);
        char *buf = malloc(len + 32U);

        if (buf != NULL) {
            (void)memcpy(buf, rec, len - 2U); /* drop "}\n" */
            (void)memcpy(buf + len - 2U, ",\"bogus\":1}\n", 12U);
            write_file(p, buf, len - 2U + 12U);
            free(buf);
        }
    }
    free(p);
    free(d);
    CHECK(cog_store_load(st, "s", &out) == COG_ERROR_IO);
    CHECK(out == NULL);
done:
    cog_store_free(st);
    cog_session_free(out);
    rmtree(base);
    free(base);
}

static void verify_missing_role(void)
{
    char *base = make_tmp("norole");
    cog_store_config_t cfg;
    cog_store_t *st = NULL;
    cog_session_t *out = NULL;
    char *d;
    char *p;
    const char *rec =
        "{\"node_id\":1,\"parent_id\":0,\"content\":\"aGk=\","
        "\"tool_name\":null,\"tool_args\":null,\"tool_result\":null,"
        "\"raw_request\":null,\"raw_response\":null,\"chunk_count\":0,"
        "\"timing\":{\"request_sent_ms\":0,\"first_token_ms\":0,"
        "\"last_token_ms\":0,\"duration_ms\":0,\"ttft_ms\":0,"
        "\"tokens_per_second\":0},"
        "\"cost\":{\"input_tokens\":0,\"output_tokens\":0,"
        "\"cost_per_input_token\":0,\"cost_per_output_token\":0,"
        "\"total_cost\":0},\"model\":null,\"provider\":null}\n";

    if (base == NULL) {
        return;
    }
    memset(&cfg, 0, sizeof(cfg));
    cfg.base_dir = base;
    CHECK(cog_store_new(&st, &cfg) == COG_OK);
    if (seed_session(base, "s", 1U) != 0) {
        goto done;
    }
    d = path_join(base, "s");
    p = path_join(d, "nodes.jsonl");
    write_file(p, rec, strlen(rec));
    free(p);
    free(d);
    CHECK(cog_store_load(st, "s", &out) == COG_ERROR_IO);
done:
    cog_store_free(st);
    cog_session_free(out);
    rmtree(base);
    free(base);
}

static void verify_multiple_roots(void)
{
    char *base = make_tmp("multroot");
    cog_store_config_t cfg;
    cog_store_t *st = NULL;
    cog_session_t *out = NULL;
    char *d;
    char *p;
    char rec[8192];

    if (base == NULL) {
        return;
    }
    memset(&cfg, 0, sizeof(cfg));
    cfg.base_dir = base;
    CHECK(cog_store_new(&st, &cfg) == COG_OK);
    if (seed_session(base, "s", 1U) != 0) {
        goto done;
    }
    d = path_join(base, "s");
    p = path_join(d, "nodes.jsonl");
    (void)snprintf(rec, sizeof(rec), KREC_FMT, 1ULL, 0ULL, 0, "aGk=");
    (void)snprintf(rec + strlen(rec), sizeof(rec) - strlen(rec), KREC_FMT,
                   2ULL, 0ULL, 1, "aGk=");
    write_file(p, rec, strlen(rec));
    free(p);
    /* meta must claim 2 records so only the multi-root trips IO. */
    p = path_join(d, "meta.json");
    {
        char idb64[64];
        char meta[512];

        b64_id("s", idb64, sizeof(idb64));
        (void)snprintf(meta, sizeof(meta),
                       "{\"version\":1,\"id\":\"%s\",\"parent_id\":null,"
                       "\"max_nodes\":0,\"node_count\":2,"
                       "\"next_node_id\":3,\"active_node_id\":2}",
                       idb64);
        write_file(p, meta, strlen(meta));
    }
    free(p);
    free(d);
    CHECK(cog_store_load(st, "s", &out) == COG_ERROR_IO);
done:
    cog_store_free(st);
    cog_session_free(out);
    rmtree(base);
    free(base);
}

static void verify_root_not_first(void)
{
    char *base = make_tmp("rootnotfirst");
    cog_store_config_t cfg;
    cog_store_t *st = NULL;
    cog_session_t *out = NULL;
    char *d;
    char *p;
    char rec[8192];

    if (base == NULL) {
        return;
    }
    memset(&cfg, 0, sizeof(cfg));
    cfg.base_dir = base;
    CHECK(cog_store_new(&st, &cfg) == COG_OK);
    if (seed_session(base, "s", 1U) != 0) {
        goto done;
    }
    d = path_join(base, "s");
    p = path_join(d, "nodes.jsonl");
    /* First record is a child (parent 2) — root must be first. */
    (void)snprintf(rec, sizeof(rec), KREC_FMT, 1ULL, 2ULL, 1, "aGk=");
    (void)snprintf(rec + strlen(rec), sizeof(rec) - strlen(rec), KREC_FMT,
                   2ULL, 0ULL, 0, "aGk=");
    write_file(p, rec, strlen(rec));
    free(p);
    p = path_join(d, "meta.json");
    {
        char idb64[64];
        char meta[512];

        b64_id("s", idb64, sizeof(idb64));
        (void)snprintf(meta, sizeof(meta),
                       "{\"version\":1,\"id\":\"%s\",\"parent_id\":null,"
                       "\"max_nodes\":0,\"node_count\":2,"
                       "\"next_node_id\":3,\"active_node_id\":2}",
                       idb64);
        write_file(p, meta, strlen(meta));
    }
    free(p);
    free(d);
    CHECK(cog_store_load(st, "s", &out) == COG_ERROR_IO);
done:
    cog_store_free(st);
    cog_session_free(out);
    rmtree(base);
    free(base);
}

static void verify_node_id_zero(void)
{
    char *base = make_tmp("idzero");
    cog_store_config_t cfg;
    cog_store_t *st = NULL;
    cog_session_t *out = NULL;
    char *d;
    char *p;
    char rec[4096];

    if (base == NULL) {
        return;
    }
    memset(&cfg, 0, sizeof(cfg));
    cfg.base_dir = base;
    CHECK(cog_store_new(&st, &cfg) == COG_OK);
    if (seed_session(base, "s", 1U) != 0) {
        goto done;
    }
    d = path_join(base, "s");
    p = path_join(d, "nodes.jsonl");
    (void)snprintf(rec, sizeof(rec), KREC_FMT, 0ULL, 0ULL, 0, "aGk=");
    write_file(p, rec, strlen(rec));
    free(p);
    free(d);
    CHECK(cog_store_load(st, "s", &out) == COG_ERROR_IO);
done:
    cog_store_free(st);
    cog_session_free(out);
    rmtree(base);
    free(base);
}

static void verify_parent_not_preceding(void)
{
    char *base = make_tmp("parentlate");
    cog_store_config_t cfg;
    cog_store_t *st = NULL;
    cog_session_t *out = NULL;
    char *d;
    char *p;
    char rec[8192];

    if (base == NULL) {
        return;
    }
    memset(&cfg, 0, sizeof(cfg));
    cfg.base_dir = base;
    CHECK(cog_store_new(&st, &cfg) == COG_OK);
    if (seed_session(base, "s", 1U) != 0) {
        goto done;
    }
    d = path_join(base, "s");
    p = path_join(d, "nodes.jsonl");
    /* Node 1 is root; node 2's parent is 3 which appears later. */
    (void)snprintf(rec, sizeof(rec), KREC_FMT, 1ULL, 0ULL, 0, "aGk=");
    (void)snprintf(rec + strlen(rec), sizeof(rec) - strlen(rec), KREC_FMT,
                   2ULL, 3ULL, 1, "aGk=");
    (void)snprintf(rec + strlen(rec), sizeof(rec) - strlen(rec), KREC_FMT,
                   3ULL, 1ULL, 1, "aGk=");
    write_file(p, rec, strlen(rec));
    free(p);
    p = path_join(d, "meta.json");
    {
        char idb64[64];
        char meta[512];

        b64_id("s", idb64, sizeof(idb64));
        (void)snprintf(meta, sizeof(meta),
                       "{\"version\":1,\"id\":\"%s\",\"parent_id\":null,"
                       "\"max_nodes\":0,\"node_count\":3,"
                       "\"next_node_id\":4,\"active_node_id\":3}",
                       idb64);
        write_file(p, meta, strlen(meta));
    }
    free(p);
    free(d);
    CHECK(cog_store_load(st, "s", &out) == COG_ERROR_IO);
done:
    cog_store_free(st);
    cog_session_free(out);
    rmtree(base);
    free(base);
}

static void verify_null_content_user(void)
{
    char *base = make_tmp("nullcontent");
    cog_store_config_t cfg;
    cog_store_t *st = NULL;
    cog_session_t *out = NULL;
    char *d;
    char *p;
    const char *rec =
        "{\"node_id\":1,\"parent_id\":0,\"role\":1,\"content\":null,"
        "\"tool_name\":null,\"tool_args\":null,\"tool_result\":null,"
        "\"raw_request\":null,\"raw_response\":null,\"chunk_count\":0,"
        "\"timing\":{\"request_sent_ms\":0,\"first_token_ms\":0,"
        "\"last_token_ms\":0,\"duration_ms\":0,\"ttft_ms\":0,"
        "\"tokens_per_second\":0},"
        "\"cost\":{\"input_tokens\":0,\"output_tokens\":0,"
        "\"cost_per_input_token\":0,\"cost_per_output_token\":0,"
        "\"total_cost\":0},\"model\":null,\"provider\":null}\n";

    if (base == NULL) {
        return;
    }
    memset(&cfg, 0, sizeof(cfg));
    cfg.base_dir = base;
    CHECK(cog_store_new(&st, &cfg) == COG_OK);
    if (seed_session(base, "s", 1U) != 0) {
        goto done;
    }
    d = path_join(base, "s");
    p = path_join(d, "nodes.jsonl");
    write_file(p, rec, strlen(rec));
    free(p);
    free(d);
    /* Must load without crashing; content is NULL, len 0. */
    CHECK(cog_store_load(st, "s", &out) == COG_OK);
    if (out != NULL) {
        const cog_node_data_t *nd =
            cog_node_data(cog_session_root(out));

        CHECK(nd != NULL);
        if (nd != NULL) {
            CHECK(nd->content == NULL);
            CHECK(nd->content_len == 0U);
        }
        cog_session_free(out);
        out = NULL;
    }
done:
    cog_store_free(st);
    cog_session_free(out);
    rmtree(base);
    free(base);
}

/* ------------------------------------------------------------------ */
/* Append misuse / fail-closed                                         */
/* ------------------------------------------------------------------ */

static void verify_append_duplicate_then_load_fails(void)
{
    char *base = make_tmp("dupappend");
    cog_store_config_t cfg;
    cog_store_t *st = NULL;
    cog_session_t *s = NULL;
    cog_session_t *out = NULL;
    cog_session_node_t *n1 = NULL;
    cog_session_node_t *n2 = NULL;
    cog_session_config_t scfg;
    int fail = 0;

    if (base == NULL) {
        return;
    }
    memset(&cfg, 0, sizeof(cfg));
    cfg.base_dir = base;
    CHECK(cog_store_new(&st, &cfg) == COG_OK);
    memset(&scfg, 0, sizeof(scfg));
    scfg.id = "dup";
    if (cog_session_new(&s, &scfg) != COG_OK ||
        cog_session_append(s, &(cog_message_t){.role = COG_MESSAGE_ROLE_USER,
                                               .content = "a",
                                               .content_len = 1U},
                           &n1) != COG_OK ||
        cog_store_save(st, s) != COG_OK) {
        fail = 1;
        goto done;
    }
    /* Append the SAME node again (caller misuse) -> duplicate record. */
    if (cog_store_append(st, s, n1) != COG_OK) {
        fail = 1;
        goto done;
    }
    /* Load must fail closed on the duplicate id. */
    CHECK(cog_store_load(st, "dup", &out) == COG_ERROR_IO);
    (void)n2;
done:
    cog_store_free(st);
    cog_session_free(s);
    cog_session_free(out);
    rmtree(base);
    free(base);
    (void)fail;
}

static void verify_append_foreign_parent_load_fails(void)
{
    char *base = make_tmp("foreignparent");
    cog_store_config_t cfg;
    cog_store_t *st = NULL;
    cog_session_t *s = NULL;
    cog_session_t *out = NULL;
    cog_session_node_t *root = NULL;
    cog_session_node_t *child = NULL;
    cog_session_config_t scfg;

    if (base == NULL) {
        return;
    }
    memset(&cfg, 0, sizeof(cfg));
    cfg.base_dir = base;
    CHECK(cog_store_new(&st, &cfg) == COG_OK);
    memset(&scfg, 0, sizeof(scfg));
    scfg.id = "fp";
    if (cog_session_new(&s, &scfg) != COG_OK ||
        cog_session_append(s, &(cog_message_t){.role = COG_MESSAGE_ROLE_USER,
                                               .content = "root",
                                               .content_len = 4U},
                           &root) != COG_OK ||
        cog_store_save(st, s) != COG_OK) {
        goto done;
    }
    /* Create a child in memory but never persist the parent's new child
     * list; append the child whose parent (root) IS persisted, so this
     * actually loads fine.  Then append a grandchild whose parent (child)
     * is NOT persisted -> load must fail closed. */
    if (cog_session_add_message(s, root,
                                &(cog_message_t){
                                    .role = COG_MESSAGE_ROLE_ASSISTANT,
                                    .content = "c",
                                    .content_len = 1U},
                                &child) != COG_OK ||
        cog_store_append(st, s, child) != COG_OK) {
        goto done;
    }
    {
        cog_session_node_t *gc = NULL;

        if (cog_session_add_message(s, child,
                                    &(cog_message_t){
                                        .role = COG_MESSAGE_ROLE_USER,
                                        .content = "g",
                                        .content_len = 1U},
                                    &gc) != COG_OK ||
            cog_store_append(st, s, gc) != COG_OK) {
            goto done;
        }
    }
    /* child was persisted, so the tree loads; the grandchild's parent
     * (child) IS in the file, so this loads fine too.  To exercise the
     * foreign-parent path we fork from the persisted `child` (creating a
     * brand-new node never persisted) and append a node under it. */
    {
        cog_session_node_t *forked = NULL;
        cog_session_node_t *fchild = NULL;

        if (cog_session_fork(s, child, &forked) != COG_OK ||
            cog_session_add_message(s, forked,
                                    &(cog_message_t){
                                        .role = COG_MESSAGE_ROLE_USER,
                                        .content = "f",
                                        .content_len = 1U},
                                    &fchild) != COG_OK ||
            cog_store_append(st, s, fchild) != COG_OK) {
            goto done;
        }
        /* forked node was never persisted; fchild's parent_id points at
         * it, so load must fail closed (IO), never crash. */
        CHECK(cog_store_load(st, "fp", &out) == COG_ERROR_IO);
    }
done:
    cog_store_free(st);
    cog_session_free(s);
    cog_session_free(out);
    rmtree(base);
    free(base);
}

/* ------------------------------------------------------------------ */
/* Pruning never deletes the just-saved session                        */
/* ------------------------------------------------------------------ */

static void verify_prune_keeps_just_saved(void)
{
    char *base = make_tmp("prunekeep");
    cog_store_config_t cfg;
    cog_store_t *st = NULL;
    cog_session_t *a = NULL;
    cog_session_t *b = NULL;
    cog_session_config_t scfg;
    char *d;

    if (base == NULL) {
        return;
    }
    memset(&cfg, 0, sizeof(cfg));
    cfg.base_dir = base;
    cfg.max_sessions = 1U;
    CHECK(cog_store_new(&st, &cfg) == COG_OK);

    memset(&scfg, 0, sizeof(scfg));
    scfg.id = "old";
    CHECK(cog_session_new(&a, &scfg) == COG_OK);
    CHECK(cog_store_save(st, a) == COG_OK);
    /* Make "old" the oldest by far. */
    d = path_join(base, "old");
    if (d != NULL) {
        set_mtime(d, (time_t)1000000);
        free(d);
    }

    memset(&scfg, 0, sizeof(scfg));
    scfg.id = "new";
    CHECK(cog_session_new(&b, &scfg) == COG_OK);
    CHECK(cog_store_save(st, b) == COG_OK);

    /* max_sessions=1: "old" is pruned, "new" (just saved) survives. */
    CHECK(cog_store_exists(st, "new"));
    CHECK(!cog_store_exists(st, "old"));

    cog_session_free(a);
    cog_session_free(b);
    cog_store_free(st);
    rmtree(base);
    free(base);
}

/* ------------------------------------------------------------------ */
/* Full save after append regenerates a consistent index               */
/* ------------------------------------------------------------------ */

static void verify_save_after_append_index(void)
{
    char *base = make_tmp("idxregen");
    cog_store_config_t cfg;
    cog_store_t *st = NULL;
    cog_session_t *s = NULL;
    cog_session_t *out = NULL;
    cog_session_node_t *n1 = NULL;
    cog_session_node_t *n2 = NULL;
    cog_session_config_t scfg;
    char *d;
    char *p;
    char *buf = NULL;
    size_t len = 0U;

    if (base == NULL) {
        return;
    }
    memset(&cfg, 0, sizeof(cfg));
    cfg.base_dir = base;
    CHECK(cog_store_new(&st, &cfg) == COG_OK);
    memset(&scfg, 0, sizeof(scfg));
    scfg.id = "idx";
    if (cog_session_new(&s, &scfg) != COG_OK ||
        cog_session_append(s, &(cog_message_t){.role = COG_MESSAGE_ROLE_USER,
                                               .content = "a",
                                               .content_len = 1U},
                           &n1) != COG_OK ||
        cog_store_save(st, s) != COG_OK ||
        cog_session_append(s, &(cog_message_t){
                                  .role = COG_MESSAGE_ROLE_ASSISTANT,
                                  .content = "b",
                                  .content_len = 1U},
                           &n2) != COG_OK ||
        cog_store_append(st, s, n2) != COG_OK) {
        goto done;
    }
    /* Full save regenerates index.bin with the correct count (2). */
    CHECK(cog_store_save(st, s) == COG_OK);
    d = path_join(base, "idx");
    p = path_join(d, "index.bin");
    if (p != NULL) {
        if (read_file(p, &buf, &len) == 0 && len >= 16U) {
            uint64_t count = 0U;

            (void)memcpy(&count, buf + 8U, sizeof(count));
            CHECK(count == 2U);
        }
        free(buf);
        buf = NULL;
        free(p);
    }
    free(d);
    CHECK(cog_store_load(st, "idx", &out) == COG_OK);
done:
    free(buf);
    cog_store_free(st);
    cog_session_free(s);
    cog_session_free(out);
    rmtree(base);
    free(base);
}

/* ------------------------------------------------------------------ */
/* Stray file in a session dir makes delete fail closed                */
/* ------------------------------------------------------------------ */

static void verify_stray_file_delete_io(void)
{
    char *base = make_tmp("stray");
    cog_store_config_t cfg;
    cog_store_t *st = NULL;
    cog_session_t *s = NULL;
    cog_session_config_t scfg;
    char *d;
    char *p;

    if (base == NULL) {
        return;
    }
    memset(&cfg, 0, sizeof(cfg));
    cfg.base_dir = base;
    CHECK(cog_store_new(&st, &cfg) == COG_OK);
    memset(&scfg, 0, sizeof(scfg));
    scfg.id = "s";
    CHECK(cog_session_new(&s, &scfg) == COG_OK);
    CHECK(cog_store_save(st, s) == COG_OK);
    /* Drop a stray file into the session dir. */
    d = path_join(base, "s");
    p = path_join(d, "stray.txt");
    if (p != NULL) {
        write_file(p, "x", 1U);
        free(p);
    }
    free(d);
    /* delete_session_dir unlinks known files then rmdir; the stray file
     * makes rmdir fail -> IO (fail closed, no partial deletion). */
    CHECK(cog_store_delete(st, "s") == COG_ERROR_IO);
    /* The session is still present (delete did not half-remove it). */
    CHECK(cog_store_exists(st, "s"));
    cog_session_free(s);
    cog_store_free(st);
    rmtree(base);
    free(base);
}

/* ------------------------------------------------------------------ */
/* Bootstrap append round-trips raw fields and next id                 */
/* ------------------------------------------------------------------ */

static void verify_bootstrap_raw_fields(void)
{
    char *base = make_tmp("bootraw");
    cog_store_config_t cfg;
    cog_store_t *st = NULL;
    cog_session_t *s = NULL;
    cog_session_t *out = NULL;
    cog_session_node_t *n = NULL;
    cog_session_config_t scfg;

    if (base == NULL) {
        return;
    }
    memset(&cfg, 0, sizeof(cfg));
    cfg.base_dir = base;
    CHECK(cog_store_new(&st, &cfg) == COG_OK);
    memset(&scfg, 0, sizeof(scfg));
    scfg.id = "br";
    if (cog_session_new(&s, &scfg) != COG_OK ||
        cog_session_append(s, &(cog_message_t){.role = COG_MESSAGE_ROLE_USER,
                                               .content = "x",
                                               .content_len = 1U},
                           &n) != COG_OK) {
        goto done;
    }
    /* Populate raw provider fields via the private layout. */
    n->data.raw_request = strdup("{\"r\":1}");
    n->data.raw_response = strdup("{\"s\":2}");
    n->data.chunk_count = 7U;
    n->data.model = strdup("m-1");
    n->data.provider = strdup("p-1");
    CHECK(cog_store_append(st, s, n) == COG_OK);
    CHECK(cog_store_load(st, "br", &out) == COG_OK);
    if (out != NULL) {
        const cog_node_data_t *nd =
            cog_node_data(cog_session_root(out));

        CHECK(nd != NULL);
        if (nd != NULL) {
            CHECK(nd->chunk_count == 7U);
            CHECK(nd->raw_request != NULL &&
                  strcmp(nd->raw_request, "{\"r\":1}") == 0);
            CHECK(nd->raw_response != NULL &&
                  strcmp(nd->raw_response, "{\"s\":2}") == 0);
            CHECK(nd->model != NULL && strcmp(nd->model, "m-1") == 0);
            CHECK(nd->provider != NULL && strcmp(nd->provider, "p-1") == 0);
        }
        CHECK(sess_next(out) == 2U);
        cog_session_free(out);
        out = NULL;
    }
done:
    cog_store_free(st);
    cog_session_free(s);
    cog_session_free(out);
    rmtree(base);
    free(base);
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int main(void)
{
    verify_unknown_field();
    verify_missing_role();
    verify_multiple_roots();
    verify_root_not_first();
    verify_node_id_zero();
    verify_parent_not_preceding();
    verify_null_content_user();
    verify_append_duplicate_then_load_fails();
    verify_append_foreign_parent_load_fails();
    verify_prune_keeps_just_saved();
    verify_save_after_append_index();
    verify_stray_file_delete_io();
    verify_bootstrap_raw_fields();

    if (failures == 0U) {
        (void)printf("test_store_adversarial: ok\n");
    }
    return failures == 0U ? 0 : 1;
}

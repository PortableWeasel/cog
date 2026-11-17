/** @file session_store.c
 * @brief Constructor-bounded immutable transactions and same-host fencing.
 */
#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "cog/session_store.h"
#include "session_store_codec.h"
#include "session_store_file.h"
#include "session_internal.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#define STORE_CAP ((size_t)16 * 1024U * 1024U)
#define ID_CAP 128U
#define MAN_SIZE 64U
#define GEN_SIZE 40U
#define DOMAIN ".cog-domain.lock"
#define ANCHOR ".owner.lock"
#define MANIFEST "manifest.bin"

typedef struct manifest {
    uint64_t generation, size, checksum;
    cog_store_owner_t owner;
    bool present;
} manifest_t;
struct cog_store_owner_guard {
    cog_store_t *store;
    struct cog_store_owner_guard *next;
    char id[ID_CAP + 1U];
    cog_store_owner_t owner;
    void *backend_guard;
    int fd;
    size_t borrowers;
    bool recovery, blocked;
};
struct cog_store {
    const cog_store_vtable_t *backend;
    void *context;
    char *base;
    size_t capacity, max_sessions;
    unsigned char *write_buffer, *read_buffer, *scratch, *compare_buffer;
    bool auto_save, require_owner, recover_tail;
    cog_store_owner_guard_t *guards;
};

static const char *const legacy_names[] = {
    "meta.json", "nodes.jsonl", "index.bin",
    "meta.json.tmp", "nodes.jsonl.tmp", "index.bin.tmp"
};
cog_result_t cog_store_file_legacy_probe(int dir, cog_store_legacy_files_t *out)
{
    if (out == NULL) return COG_ERROR_NULL_PTR;
    cog_store_legacy_files_t found = {0};
    for (size_t i = 0U; i < sizeof(legacy_names) / sizeof(legacy_names[0]); ++i) {
        struct stat info;
        if (fstatat(dir, legacy_names[i], &info, AT_SYMLINK_NOFOLLOW) == 0) {
            found.present = true;
            if (i == 0U) found.metadata = true;
        } else if (errno != ENOENT) return COG_ERROR_IO;
    }
    *out = found;
    return COG_OK;
}

static cog_result_t valid_id(const char *id)
{
    if (id == NULL) return COG_ERROR_NULL_PTR;
    size_t n = strlen(id);
    if (n > ID_CAP) return COG_ERROR_OVERFLOW;
    if (n == 0U || strcmp(id, ".") == 0 || strcmp(id, "..") == 0 || strcmp(id, DOMAIN) == 0)
        return COG_ERROR_INVALID_ARG;
    for (size_t i = 0U; i < n; ++i)
        if ((unsigned char)id[i] < 32U || (unsigned char)id[i] == 127U || id[i] == '/' || id[i] == '\\')
            return COG_ERROR_INVALID_ARG;
    return COG_OK;
}
static uint64_t checksum(const unsigned char *p, size_t n)
{
    uint64_t h = UINT64_C(14695981039346656037);
    for (size_t i = 0U; i < n; ++i) { h ^= p[i]; h *= UINT64_C(1099511628211); }
    return h;
}
static void put64(unsigned char *p, uint64_t n)
{ for (unsigned i = 0U; i < 8U; ++i) p[i] = (unsigned char)(n >> (i * 8U)); }
static uint64_t get64(const unsigned char *p)
{
    uint64_t n = 0U;
    for (unsigned i = 0U; i < 8U; ++i) n |= (uint64_t)p[i] << (i * 8U);
    return n;
}
static bool same_owner(cog_store_owner_t a, cog_store_owner_t b)
{ return a.epoch == b.epoch && a.instance_id == b.instance_id; }
static cog_result_t io_write(int fd, const unsigned char *p, size_t n)
{
    while (n != 0U) {
        ssize_t k = write(fd, p, n);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) return COG_ERROR_IO;
        p += (size_t)k; n -= (size_t)k;
    }
    return COG_OK;
}
static cog_result_t read_at(int dir, const char *name, unsigned char *p, size_t cap, size_t *size)
{
    int fd = openat(dir, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return errno == ENOENT ? COG_ERROR_NOT_FOUND : COG_ERROR_IO;
    struct stat st;
    cog_result_t rc = COG_OK;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0) rc = COG_ERROR_IO;
    else if ((uintmax_t)st.st_size > (uintmax_t)cap) rc = COG_ERROR_LIMIT;
    else {
        size_t n = (size_t)st.st_size, at = 0U;
        while (at < n) {
            ssize_t k = read(fd, p + at, n - at);
            if (k < 0 && errno == EINTR) continue;
            if (k <= 0) { rc = COG_ERROR_IO; break; }
            at += (size_t)k;
        }
        unsigned char extra;
        if (rc == COG_OK && read(fd, &extra, 1U) != 0) rc = COG_ERROR_IO;
        if (rc == COG_OK) *size = n;
    }
    if (close(fd) != 0 && rc == COG_OK) rc = COG_ERROR_IO;
    return rc;
}
static int open_base(cog_store_t *s, bool create)
{
    if (create && mkdir(s->base, 0700) != 0 && errno != EEXIST) return -1;
    int base = open(s->base, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (base < 0 || !create) return base;
    /* Persist the configured store root's directory entry before any durable
     * owner/data success. Confirm even when mkdir returns EEXIST: an earlier
     * attempt may have created the root then failed this parent fsync. Opening
     * the actual parent's descriptor also handles relative/trailing-slash paths
     * without allocating or guessing a pathname split. */
    int parent = openat(base, "..", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (parent < 0) { int saved = errno; (void)close(base); errno = saved; return -1; }
    if (fsync(parent) != 0) {
        int saved = errno; (void)close(parent); (void)close(base); errno = saved; return -1;
    }
    (void)close(parent); return base;
}
static cog_result_t domain_lock(cog_store_t *s, int *base, int *lock)
{
    *base = open_base(s, true);
    *lock = -1;
    if (*base < 0) return COG_ERROR_IO;
    *lock = openat(*base, DOMAIN, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (*lock < 0 || flock(*lock, LOCK_EX | LOCK_NB) != 0) {
        cog_result_t rc = errno == EWOULDBLOCK ? COG_ERROR_AGAIN : COG_ERROR_IO;
        if (*lock >= 0) (void)close(*lock);
        (void)close(*base); *base = -1; *lock = -1; return rc;
    }
    if (fsync(*lock) != 0 || fsync(*base) != 0) {
        (void)close(*lock); (void)close(*base); *base = -1; *lock = -1; return COG_ERROR_IO;
    }
    return COG_OK;
}
/* All writers create/open the anchor under the stable ownership-domain lock.
 * No path in this implementation replaces or unlinks either lock inode. */
static cog_result_t session_lock(cog_store_t *s, const char *id, int *dir, int *lock)
{
    int base, domain;
    *dir = -1; *lock = -1;
    cog_result_t rc = domain_lock(s, &base, &domain);
    if (rc != COG_OK) return rc;
    if (mkdirat(base, id, 0700) != 0 && errno != EEXIST) rc = COG_ERROR_IO;
    if (rc == COG_OK) {
        *dir = openat(base, id, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (*dir < 0) rc = COG_ERROR_IO;
    }
    if (rc == COG_OK) {
        *lock = openat(*dir, ANCHOR, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (*lock < 0) rc = COG_ERROR_IO;
        else if (flock(*lock, LOCK_EX | LOCK_NB) != 0)
            rc = errno == EWOULDBLOCK ? COG_ERROR_AGAIN : COG_ERROR_IO;
        else if (fsync(*lock) != 0 || fsync(*dir) != 0 || fsync(base) != 0) rc = COG_ERROR_IO;
    }
    (void)close(domain); (void)close(base);
    if (rc != COG_OK) {
        if (*lock >= 0) (void)close(*lock);
        if (*dir >= 0) (void)close(*dir);
        *lock = -1; *dir = -1;
    }
    return rc;
}
static int session_dir(cog_store_t *s, const char *id)
{
    int base = open_base(s, false);
    if (base < 0) return -1;
    int dir = openat(base, id, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    int saved = errno; (void)close(base); errno = saved;
    return dir;
}
static cog_result_t manifest_read(int dir, manifest_t *m)
{
    unsigned char p[MAN_SIZE]; size_t n = 0U;
    cog_result_t rc = read_at(dir, MANIFEST, p, sizeof(p), &n);
    if (rc == COG_ERROR_NOT_FOUND) { *m = (manifest_t){0}; return COG_OK; }
    if (rc == COG_ERROR_LIMIT) return COG_ERROR_IO;
    if (rc != COG_OK) return rc;
    if (n != MAN_SIZE || memcmp(p, "COGMAN2\0", 8U) != 0 ||
        get64(p + 56U) != checksum(p, 56U)) return COG_ERROR_IO;
    if (get64(p + 8U) != 2U) return COG_ERROR_UNSUPPORTED;
    manifest_t v = {get64(p + 16U), get64(p + 24U), get64(p + 32U),
        {get64(p + 40U), get64(p + 48U)}, true};
    if ((v.owner.epoch == 0U) != (v.owner.instance_id == 0U) ||
        (v.generation == 0U && (v.size != 0U || v.checksum != 0U)) ||
        (v.generation == 0U && v.owner.epoch == 0U)) return COG_ERROR_PROTOCOL;
    *m = v; return COG_OK;
}
static void generation_name(char *p, size_t cap, uint64_t g)
{ (void)snprintf(p, cap, "generation-%020" PRIu64 ".bin", g); }
/* Fixed temporary names are safe under the exclusive session lock. Immutable
 * generation installation uses linkat (never rename-overwrite an old token). */
static cog_result_t publish_file(int dir, const char *tmp, const char *name,
    const unsigned char *head, size_t hsize, const unsigned char *body, size_t bsize,
    bool immutable, bool *published)
{
    *published = false;
    /* A crash/failed cleanup can leave tmp hardlinked to a previously installed
     * immutable generation. Detach that name before allocating a fresh inode;
     * truncating an old temporary name could corrupt an exact old checkpoint. */
    if (unlinkat(dir, tmp, 0) != 0 && errno != ENOENT) return COG_ERROR_IO;
    int fd = openat(dir, tmp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return COG_ERROR_IO;
    cog_result_t rc = io_write(fd, head, hsize);
    if (rc == COG_OK) rc = io_write(fd, body, bsize);
    if (rc == COG_OK && fsync(fd) != 0) rc = COG_ERROR_IO;
    if (close(fd) != 0 && rc == COG_OK) rc = COG_ERROR_IO;
    if (rc == COG_OK) {
        int status = immutable ? linkat(dir, tmp, dir, name, 0) : renameat(dir, tmp, dir, name);
        if (status != 0) rc = COG_ERROR_IO;
        else *published = true;
    }
    if (immutable || rc != COG_OK) {
        if (unlinkat(dir, tmp, 0) != 0 && errno != ENOENT && rc == COG_OK) rc = COG_ERROR_IO;
    }
    return rc;
}
static cog_result_t manifest_publish(int dir, const manifest_t *m, bool *published)
{
    unsigned char p[MAN_SIZE] = {0};
    (void)memcpy(p, "COGMAN2\0", 8U); put64(p + 8U, 2U);
    put64(p + 16U, m->generation); put64(p + 24U, m->size); put64(p + 32U, m->checksum);
    put64(p + 40U, m->owner.epoch); put64(p + 48U, m->owner.instance_id);
    put64(p + 56U, checksum(p, 56U));
    cog_result_t rc = publish_file(dir, ".manifest.tmp", MANIFEST, p, sizeof(p), NULL, 0U, false, published);
    if (rc == COG_OK && fsync(dir) != 0) rc = COG_ERROR_IO;
    return rc;
}
static cog_result_t durable_confirm(int dir, const manifest_t *m)
{
    char name[64];
    if (m->generation != 0U) {
        generation_name(name, sizeof(name), m->generation);
        int fd = openat(dir, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (fd < 0) return COG_ERROR_IO;
        int status = fsync(fd); (void)close(fd);
        if (status != 0) return COG_ERROR_IO;
    }
    int fd = openat(dir, MANIFEST, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return COG_ERROR_IO;
    int status = fsync(fd); (void)close(fd);
    return status == 0 && fsync(dir) == 0 ? COG_OK : COG_ERROR_IO;
}
static cog_result_t generation_read(cog_store_t *s, int dir, uint64_t g,
    const manifest_t *authoritative, cog_session_snapshot_t *snapshot)
{
    char name[64]; size_t n = 0U;
    generation_name(name, sizeof(name), g);
    cog_result_t rc = read_at(dir, name, s->read_buffer, s->capacity, &n);
    if (rc == COG_ERROR_NOT_FOUND && authoritative != NULL) return COG_ERROR_IO;
    if (rc != COG_OK) return rc;
    const unsigned char *p = s->read_buffer;
    if (n < GEN_SIZE || memcmp(p, "COGGEN2\0", 8U) != 0 || get64(p + 8U) != g ||
        get64(p + 16U) != (uint64_t)(n - GEN_SIZE) || get64(p + 32U) != checksum(p, 32U) ||
        get64(p + 24U) != checksum(p + GEN_SIZE, n - GEN_SIZE)) return COG_ERROR_IO;
    if (authoritative != NULL && (authoritative->size != (uint64_t)n ||
        authoritative->checksum != checksum(p, n))) return COG_ERROR_IO;
    cog_store_codec_workspace_t w = {s->scratch, s->capacity, 0U};
    return cog_store_codec_decode(p + GEN_SIZE, n - GEN_SIZE, &w, snapshot);
}
static cog_result_t legacy_read(cog_store_t *s, int dir, const char *id,
    cog_store_load_mode_t mode, cog_session_snapshot_t *v, cog_store_load_report_t *report)
{
    size_t mn = 0U, nn = 0U;
    cog_store_legacy_files_t files;
    cog_result_t rc = cog_store_file_legacy_probe(dir, &files);
    if (rc != COG_OK) return rc;
    if (!files.metadata) return files.present ? COG_ERROR_UNSUPPORTED : COG_ERROR_NOT_FOUND;
    rc = read_at(dir, "meta.json", s->write_buffer, s->capacity, &mn);
    if (rc != COG_OK) return rc;
    rc = read_at(dir, "nodes.jsonl", s->read_buffer, s->capacity, &nn);
    if (rc != COG_OK) return rc == COG_ERROR_NOT_FOUND ? COG_ERROR_IO : rc;
    cog_store_codec_workspace_t w = {s->scratch, s->capacity, 0U};
    return cog_store_codec_decode_v1(id, s->write_buffer, mn, s->read_buffer, nn,
        mode, s->recover_tail, &w, v, report);
}
static bool uncertainty(const cog_session_snapshot_t *v)
{
    for (size_t i = 0U; i < v->ledger_count; ++i)
        if (v->ledger[i].state == COG_TOOL_EXECUTION_STARTED ||
            v->ledger[i].state == COG_TOOL_EXECUTION_INDETERMINATE) return true;
    return false;
}
static bool unresolved(const cog_session_snapshot_t *v)
{
    for (size_t i = 0U; i < v->ledger_count; ++i)
        if (v->ledger[i].state == COG_TOOL_EXECUTION_PROPOSED ||
            v->ledger[i].state == COG_TOOL_EXECUTION_STARTED ||
            v->ledger[i].state == COG_TOOL_EXECUTION_INDETERMINATE) return true;
    return false;
}
static bool str_eq(const char *a, const char *b)
{ return a == NULL || b == NULL ? a == b : strcmp(a, b) == 0; }
static bool bytes_eq(const void *a, size_t an, const void *b, size_t bn)
{ return an == bn && (an == 0U || (a != NULL && b != NULL && memcmp(a, b, an) == 0)); }
/* Compare a complete immutable node using the canonical codec, avoiding native
 * padding, pointer identity and accidentally omitted structured fields. */
static bool node_equal(cog_store_t *s, const cog_node_data_t *a, const cog_node_data_t *b)
{
    if (a->context_binding_id != b->context_binding_id) return false;
    cog_node_data_t av = *a, bv = *b;
    cog_message_t am = {0}, bm = {0};
    if (av.message == NULL && av.role <= COG_NODE_ASSISTANT) {
        am.role = (cog_message_role_t)av.role; am.content = av.content; am.content_len = av.content_len; av.message = &am;
    }
    if (bv.message == NULL && bv.role <= COG_NODE_ASSISTANT) {
        bm.role = (cog_message_role_t)bv.role; bm.content = bv.content; bm.content_len = bv.content_len; bv.message = &bm;
    }
    cog_session_snapshot_t v = {.session_id = "node", .nodes = &av, .node_count = 1U};
    size_t an = 0U, bn = 0U;
    if (cog_store_codec_encode(&v, s->write_buffer, s->capacity, &an) != COG_OK) return false;
    /* read_buffer and scratch hold the committed decoded data. The constructor
     * reserves a separate comparison buffer so full-capacity records still fit. */
    v.nodes = &bv;
    if (cog_store_codec_encode(&v, s->compare_buffer, s->capacity, &bn) != COG_OK) return false;
    return an == bn && memcmp(s->write_buffer, s->compare_buffer, an) == 0;
}
static const cog_node_data_t *node_find(const cog_session_snapshot_t *v, uint64_t id)
{
    for (size_t i = 0U; i < v->node_count; ++i) if (v->nodes[i].node_id == id) return &v->nodes[i];
    return NULL;
}
static const cog_tool_ledger_record_t *ledger_find(const cog_session_snapshot_t *v,
    uint64_t assistant, const char *id)
{
    for (size_t i = 0U; i < v->ledger_count; ++i)
        if (v->ledger[i].assistant_node_id == assistant && str_eq(v->ledger[i].call_id, id)) return &v->ledger[i];
    return NULL;
}
static bool audit_equal(const cog_tool_audit_record_t *a, const cog_tool_audit_record_t *b)
{
    return a->sequence == b->sequence && a->previous == b->previous && a->next == b->next &&
        a->explicit_reconciliation == b->explicit_reconciliation &&
        bytes_eq(a->evidence, a->evidence_len, b->evidence, b->evidence_len);
}
static bool recovery_result(const cog_session_snapshot_t *old, const cog_session_snapshot_t *now,
    const cog_node_data_t *n, bool recovery)
{
    if (n->message == NULL || n->message->role != COG_MESSAGE_ROLE_TOOL) return false;
    const cog_node_data_t *a = node_find(now, n->parent_id);
    while (a != NULL && a->role != COG_NODE_ASSISTANT) a = node_find(now, a->parent_id);
    if (a == NULL) return false;
    const cog_tool_ledger_record_t *l = ledger_find(old, a->node_id, n->message->tool_call_id);
    return l != NULL && ((!recovery && l->state == COG_TOOL_EXECUTION_PROPOSED) ||
        l->state == COG_TOOL_EXECUTION_STARTED || l->state == COG_TOOL_EXECUTION_INDETERMINATE);
}
static bool indeterminate(const cog_session_snapshot_t *v);
static bool context_history(const cog_session_snapshot_t *v)
{
    if (v->context_count != 0U || v->context_binding_count != 0U || v->context_high_water != 0U ||
        v->context_binding_high_water != 0U || v->selected_context_binding_id != 0U) return true;
    for (size_t i = 0U; i < v->node_count; ++i) if (v->nodes[i].context_binding_id != 0U) return true;
    return false;
}
static cog_result_t context_monotonic(const cog_session_snapshot_t *old,
    const cog_session_snapshot_t *now, bool recovery)
{
    if (now->context_count < old->context_count || now->context_binding_count < old->context_binding_count ||
        now->context_high_water < old->context_high_water ||
        now->context_binding_high_water < old->context_binding_high_water) return COG_ERROR_INVALID_STATE;
    for (size_t i = 0U; i < old->context_count; ++i) {
        const cog_session_context_t *a = &old->contexts[i], *b = &now->contexts[i];
        if (a->context_id != b->context_id || !str_eq(a->namespace_id, b->namespace_id) ||
            a->format != b->format || !bytes_eq(a->data, a->data_bytes, b->data, b->data_bytes)) return COG_ERROR_INVALID_STATE;
    }
    for (size_t i = old->context_count; i < now->context_count; ++i)
        if (now->contexts[i].context_id <= old->context_high_water) return COG_ERROR_INVALID_STATE;
    for (size_t i = 0U; i < old->context_binding_count; ++i) {
        const cog_session_context_binding_t *a = &old->context_bindings[i], *b = &now->context_bindings[i];
        if (a->binding_id != b->binding_id || a->previous_binding_id != b->previous_binding_id ||
            a->anchor_node_id != b->anchor_node_id || a->context_id != b->context_id) return COG_ERROR_INVALID_STATE;
    }
    for (size_t i = old->context_binding_count; i < now->context_binding_count; ++i)
        if (now->context_bindings[i].binding_id <= old->context_binding_high_water) return COG_ERROR_INVALID_STATE;
    if (recovery && (now->context_count != old->context_count ||
        now->context_high_water != old->context_high_water ||
        now->context_binding_high_water != old->context_binding_high_water)) return COG_ERROR_INVALID_STATE;
    bool old_unresolved_remains = false;
    for (size_t i = 0U; i < old->ledger_count; ++i) {
        const cog_tool_ledger_record_t *l = &old->ledger[i];
        if (l->state != COG_TOOL_EXECUTION_PROPOSED && l->state != COG_TOOL_EXECUTION_STARTED &&
            l->state != COG_TOOL_EXECUTION_INDETERMINATE) continue;
        const cog_tool_ledger_record_t *next = ledger_find(now, l->assistant_node_id, l->call_id);
        if (next != NULL && (next->state == COG_TOOL_EXECUTION_PROPOSED || next->state == COG_TOOL_EXECUTION_STARTED ||
            next->state == COG_TOOL_EXECUTION_INDETERMINATE)) old_unresolved_remains = true;
    }
    if ((recovery || old_unresolved_remains) &&
        (now->context_binding_count != old->context_binding_count ||
         now->selected_context_binding_id != old->selected_context_binding_id)) return COG_ERROR_INVALID_STATE;
    return COG_OK;
}

static cog_result_t monotonic(cog_store_t *s, const cog_session_snapshot_t *old,
    const cog_session_snapshot_t *now, bool recovery)
{
    if (!str_eq(old->session_id, now->session_id) || !str_eq(old->parent_session_id, now->parent_session_id) ||
        old->inspection_only != now->inspection_only || now->node_id_high_water < old->node_id_high_water ||
        now->audit_high_water < old->audit_high_water) return COG_ERROR_INVALID_STATE;
    cog_result_t context_rc = context_monotonic(old, now, recovery);
    if (context_rc != COG_OK) return context_rc;
    size_t cursor = 0U;
    for (size_t i = 0U; i < old->node_count; ++i) {
        while (cursor < now->node_count && now->nodes[cursor].node_id != old->nodes[i].node_id) ++cursor;
        if (cursor == now->node_count || !node_equal(s, &old->nodes[i], &now->nodes[cursor]))
            return COG_ERROR_INVALID_STATE;
        ++cursor;
    }
    for (size_t i = 0U; i < now->ledger_count; ++i) {
        const cog_tool_ledger_record_t *l = &now->ledger[i];
        if (ledger_find(old, l->assistant_node_id, l->call_id) == NULL)
            for (size_t k = 0U; k < l->audit_count; ++k)
                if (l->audit[k].sequence <= old->audit_high_water) return COG_ERROR_INVALID_STATE;
    }
    for (size_t i = 0U; i < now->node_count; ++i) {
        const cog_node_data_t *n = &now->nodes[i];
        if (node_find(old, n->node_id) == NULL &&
            (n->node_id <= old->node_id_high_water ||
            ((recovery || unresolved(old) || indeterminate(now)) && !recovery_result(old, now, n, recovery)))) return COG_ERROR_INVALID_STATE;
    }
    if (recovery && old->ledger_count != now->ledger_count) return COG_ERROR_INVALID_STATE;
    for (size_t i = 0U; i < old->ledger_count; ++i) {
        const cog_tool_ledger_record_t *a = &old->ledger[i];
        const cog_tool_ledger_record_t *b = ledger_find(now, a->assistant_node_id, a->call_id);
        if (b == NULL || b->audit_count < a->audit_count) return COG_ERROR_INVALID_STATE;
        for (size_t k = 0U; k < a->audit_count; ++k)
            if (!audit_equal(&a->audit[k], &b->audit[k])) return COG_ERROR_INVALID_STATE;
        cog_tool_execution_state_t state = a->state;
        for (size_t k = a->audit_count; k < b->audit_count; ++k) {
            const cog_tool_audit_record_t *q = &b->audit[k];
            if (q->sequence <= old->audit_high_water || q->previous != state) return COG_ERROR_INVALID_STATE;
            bool normal = (state == COG_TOOL_EXECUTION_PROPOSED &&
                (q->next == COG_TOOL_EXECUTION_STARTED || q->next == COG_TOOL_EXECUTION_DENIED)) ||
                (state == COG_TOOL_EXECUTION_STARTED && (q->next == COG_TOOL_EXECUTION_SUCCEEDED ||
                 q->next == COG_TOOL_EXECUTION_FAILED || q->next == COG_TOOL_EXECUTION_CANCELLED ||
                 q->next == COG_TOOL_EXECUTION_INDETERMINATE));
            bool reconcile = recovery && state == COG_TOOL_EXECUTION_INDETERMINATE &&
                q->explicit_reconciliation && q->evidence_len != 0U &&
                (q->next == COG_TOOL_EXECUTION_SUCCEEDED || q->next == COG_TOOL_EXECUTION_FAILED ||
                 q->next == COG_TOOL_EXECUTION_CANCELLED || q->next == COG_TOOL_EXECUTION_ACKNOWLEDGED);
            if ((!normal || q->explicit_reconciliation) && !reconcile) return COG_ERROR_INVALID_STATE;
            if (recovery && !reconcile && !(state == COG_TOOL_EXECUTION_STARTED &&
                q->next == COG_TOOL_EXECUTION_INDETERMINATE)) return COG_ERROR_INVALID_STATE;
            state = q->next;
        }
        if (state != b->state) return COG_ERROR_INVALID_STATE;
    }
    return COG_OK;
}
static cog_store_owner_guard_t *find_guard(cog_store_t *s, const char *id)
{
    for (cog_store_owner_guard_t *g = s->guards; g != NULL; g = g->next)
        if (strcmp(g->id, id) == 0) return g;
    return NULL;
}
/* The allocation-free semantic validator is shared with the restore constructor. */
static cog_result_t validate(const cog_session_snapshot_t *v, const char *id)
{
    if (!str_eq(v->session_id, id)) return COG_ERROR_PROTOCOL;
    return cog_session_snapshot_validate(v);
}
/* Linux getdents64 uses caller-provided storage, unlike fdopendir/readdir,
 * whose opaque DIR allocation would violate constructor-bounded runtime work.
 * The initial fenced file profile already requires Linux flock semantics. */
typedef struct directory_cursor {
    int fd;
    size_t at, size;
    unsigned char bytes[4096];
} directory_cursor_t;
static cog_result_t directory_open(int dir, directory_cursor_t *cursor)
{
    *cursor = (directory_cursor_t){.fd = openat(dir, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC)};
    return cursor->fd >= 0 ? COG_OK : COG_ERROR_IO;
}
static cog_result_t directory_next(directory_cursor_t *cursor, const char **name)
{
    *name = NULL;
    if (cursor->at == cursor->size) {
        long n;
        do { n = syscall(SYS_getdents64, cursor->fd, cursor->bytes, sizeof(cursor->bytes)); }
        while (n < 0 && errno == EINTR);
        if (n < 0) return COG_ERROR_IO;
        if (n == 0) return COG_OK;
        cursor->at = 0U; cursor->size = (size_t)n;
    }
    /* linux_dirent64: ino64, off64, reclen16, type8, name[]. Read native
     * scalars with memcpy, avoiding alignment assumptions about byte storage. */
    if (cursor->size - cursor->at < 20U) return COG_ERROR_IO;
    unsigned short length;
    (void)memcpy(&length, cursor->bytes + cursor->at + 16U, sizeof(length));
    if (length < 20U || (size_t)length > cursor->size - cursor->at ||
        memchr(cursor->bytes + cursor->at + 19U, '\0', (size_t)length - 19U) == NULL)
        return COG_ERROR_IO;
    *name = (const char *)cursor->bytes + cursor->at + 19U;
    cursor->at += length; return COG_OK;
}
static cog_result_t count_sessions(cog_store_t *s, size_t *count)
{
    int base = open_base(s, false);
    if (base < 0) return COG_ERROR_IO;
    directory_cursor_t cursor;
    cog_result_t rc = directory_open(base, &cursor); (void)close(base);
    if (rc != COG_OK) return rc;
    size_t n = 0U; const char *name;
    while ((rc = directory_next(&cursor, &name)) == COG_OK && name != NULL) {
        if (valid_id(name) != COG_OK) continue;
        int dir = session_dir(s, name);
        if (dir < 0) continue;
        struct stat st;
        if (fstatat(dir, MANIFEST, &st, AT_SYMLINK_NOFOLLOW) == 0 ||
            fstatat(dir, "meta.json", &st, AT_SYMLINK_NOFOLLOW) == 0) ++n;
        (void)close(dir);
    }
    (void)close(cursor.fd);
    if (rc == COG_OK) *count = n;
    return rc;
}
static cog_result_t next_generation(int dir, uint64_t current, uint64_t *next)
{
    char name[64]; struct stat st;
    do {
        if (current == UINT64_MAX) return COG_ERROR_LIMIT;
        ++current; generation_name(name, sizeof(name), current);
        if (fstatat(dir, name, &st, AT_SYMLINK_NOFOLLOW) != 0) {
            if (errno != ENOENT) return COG_ERROR_IO;
            *next = current; return COG_OK;
        }
    } while (true);
}
static bool file_exists(void *ctx, const char *id)
{
    cog_store_t *s = ctx;
    if (s == NULL || valid_id(id) != COG_OK) return false;
    int dir = session_dir(s, id);
    if (dir < 0) return false;
    struct stat st;
    bool found = fstatat(dir, MANIFEST, &st, AT_SYMLINK_NOFOLLOW) == 0 ||
        fstatat(dir, "meta.json", &st, AT_SYMLINK_NOFOLLOW) == 0;
    (void)close(dir); return found;
}
static cog_result_t file_list(void *ctx, char ***out, size_t *count);
static bool session_contains(const cog_session_t *s, const cog_session_node_t *wanted)
{
    const cog_session_node_t *n = cog_session_root(s);
    while (n != NULL) {
        if (n == wanted) return true;
        if (cog_node_first_child(n) != NULL) n = cog_node_first_child(n);
        else {
            while (n != NULL && cog_node_next_sibling(n) == NULL) n = cog_node_parent(n);
            if (n != NULL) n = cog_node_next_sibling(n);
        }
    }
    return false;
}
static bool indeterminate(const cog_session_snapshot_t *v)
{
    for (size_t i = 0U; i < v->ledger_count; ++i)
        if (v->ledger[i].state == COG_TOOL_EXECUTION_INDETERMINATE) return true;
    return false;
}
static bool complete_path(const cog_session_snapshot_t *v)
{
    const cog_node_data_t *n = node_find(v, v->active_node_id);
    while (n != NULL) {
        if (n->message != NULL && n->message->status != COG_MESSAGE_COMPLETE) return false;
        n = node_find(v, n->parent_id);
    }
    return true;
}
static cog_result_t file_commit(cog_store_t *s, const cog_session_t *session,
    cog_store_owner_guard_t *guard, bool checkpoint, bool append, const cog_session_node_t *node,
    cog_store_checkpoint_t *out)
{
    cog_session_snapshot_t v, old;
    cog_result_t rc = cog_session_snapshot_view(session, &v);
    if (rc != COG_OK) return rc;
    rc = valid_id(v.session_id);
    if (rc != COG_OK) return rc;
    rc = validate(&v, v.session_id);
    if (rc != COG_OK) return rc;
    if (s->require_owner && (guard == NULL || guard->store != s || strcmp(guard->id, v.session_id) != 0))
        return COG_ERROR_INVALID_STATE;
    if (guard != NULL && (append || checkpoint) && guard->recovery) return COG_ERROR_INVALID_STATE;
    if (checkpoint && (unresolved(&v) || v.inspection_only || !complete_path(&v))) return COG_ERROR_INDETERMINATE;
    int dir = -1, lock = -1, admission_base = -1, admission_domain = -1;
    if (guard != NULL) {
        if (guard->store != s || find_guard(s, v.session_id) != guard) return COG_ERROR_INVALID_STATE;
        dir = session_dir(s, v.session_id);
        if (dir < 0) return COG_ERROR_IO;
    } else {
        rc = session_lock(s, v.session_id, &dir, &lock);
        if (rc != COG_OK) return rc;
    }
    manifest_t m;
    rc = manifest_read(dir, &m);
    if (rc != COG_OK) goto done;
    if (m.owner.epoch != 0U && (guard == NULL || !same_owner(m.owner, guard->owner))) {
        rc = COG_ERROR_INVALID_STATE; goto done;
    }
    if (guard != NULL && !same_owner(m.owner, guard->owner)) { rc = COG_ERROR_INVALID_STATE; goto done; }
    if (m.generation != 0U) {
        rc = generation_read(s, dir, m.generation, &m, &old);
        if (rc == COG_OK) rc = validate(&old, v.session_id);
        if (rc != COG_OK) goto done;
        if (append && (node == NULL || node_find(&old, cog_node_data(node)->node_id) != NULL)) {
            rc = COG_ERROR_INVALID_STATE; goto done;
        }
        rc = monotonic(s, &old, &v, guard != NULL && guard->recovery);
        if (rc != COG_OK) goto done;
        if (append && v.node_count != old.node_count + 1U) { rc = COG_ERROR_INVALID_STATE; goto done; }
        if (append) for (size_t i = 0U; i < old.ledger_count; ++i) {
            const cog_tool_ledger_record_t *a = &old.ledger[i];
            const cog_tool_ledger_record_t *b = ledger_find(&v, a->assistant_node_id, a->call_id);
            if (b == NULL || a->state != b->state || a->audit_count != b->audit_count) {
                rc = COG_ERROR_INVALID_STATE; goto done;
            }
        }
    } else {
        cog_store_legacy_files_t files;
        rc = cog_store_file_legacy_probe(dir, &files);
        if (rc != COG_OK) goto done;
        if (files.present && !files.metadata) { rc = COG_ERROR_UNSUPPORTED; goto done; }
        if (files.metadata) {
            if (append) { rc = COG_ERROR_UNSUPPORTED; goto done; }
            cog_store_load_report_t report;
            rc = legacy_read(s, dir, v.session_id, COG_STORE_LOAD_REPLAY, &old, &report);
            if (rc == COG_ERROR_UNSUPPORTED && v.inspection_only)
                rc = legacy_read(s, dir, v.session_id, COG_STORE_LOAD_INSPECT, &old, &report);
            if (rc == COG_OK) rc = validate(&old, v.session_id);
            if (rc == COG_OK) rc = monotonic(s, &old, &v, false);
            if (rc != COG_OK) goto done;
        }
        if (s->max_sessions != 0U && !file_exists(s, v.session_id)) {
            /* Admission and publication share the ownership-domain lock, so
             * distinct session locks cannot oversubscribe the global policy. */
            rc = domain_lock(s, &admission_base, &admission_domain);
            if (rc != COG_OK) goto done;
            size_t n = 0U; rc = count_sessions(s, &n);
            if (rc != COG_OK) goto done;
            if (n >= s->max_sessions) { rc = COG_ERROR_LIMIT; goto done; }
        }
    }
    if (append && (node == NULL || !session_contains(session, node))) {
        rc = COG_ERROR_INVALID_STATE; goto done;
    }
    size_t n = 0U;
    rc = cog_store_codec_encode(&v, s->write_buffer + GEN_SIZE, s->capacity - GEN_SIZE, &n);
    if (rc != COG_OK) goto done;
    rc = next_generation(dir, m.generation, &m.generation);
    if (rc != COG_OK) goto done;
    unsigned char *p = s->write_buffer;
    (void)memcpy(p, "COGGEN2\0", 8U); put64(p + 8U, m.generation); put64(p + 16U, (uint64_t)n);
    put64(p + 24U, checksum(p + GEN_SIZE, n)); put64(p + 32U, checksum(p, 32U));
    m.size = (uint64_t)(n + GEN_SIZE); m.checksum = checksum(p, n + GEN_SIZE);
    char name[64]; generation_name(name, sizeof(name), m.generation);
    bool published;
    rc = publish_file(dir, ".generation.tmp", name, p, n + GEN_SIZE, NULL, 0U, true, &published);
    if (rc != COG_OK) goto done;
    rc = manifest_publish(dir, &m, &published);
    if (published) cog_session_mark_committed(session);
    if (guard != NULL && (published || rc == COG_OK)) guard->blocked = rc != COG_OK || indeterminate(&v);
    if (rc == COG_OK && out != NULL) out->generation = m.generation;
 done:
    if (admission_domain >= 0) (void)close(admission_domain);
    if (admission_base >= 0) (void)close(admission_base);
    if (lock >= 0) (void)close(lock);
    (void)close(dir);
    return rc;
}
static cog_result_t file_save(void *ctx, const cog_session_t *session)
{
    cog_store_t *s = ctx;
    if (s == NULL || session == NULL) return COG_ERROR_NULL_PTR;
    return file_commit(s, session, find_guard(s, cog_session_id(session)), false, false, NULL, NULL);
}
static cog_result_t file_append(void *ctx, const cog_session_t *session, const cog_session_node_t *node)
{
    cog_store_t *s = ctx;
    if (s == NULL || session == NULL || node == NULL) return COG_ERROR_NULL_PTR;
    if (!session_contains(session, node)) return COG_ERROR_INVALID_STATE;
    return file_commit(s, session, find_guard(s, cog_session_id(session)), false, true, node, NULL);
}
static cog_result_t file_load_common(cog_store_t *s, const char *id, uint64_t exact,
    cog_store_load_mode_t mode, cog_session_t **out, cog_store_load_report_t *report)
{
    if (out != NULL) *out = NULL;
    if (s == NULL || out == NULL || report == NULL) return COG_ERROR_NULL_PTR;
    cog_result_t arg_rc = valid_id(id);
    if (arg_rc != COG_OK) return arg_rc;
    if (mode != COG_STORE_LOAD_REPLAY && mode != COG_STORE_LOAD_INSPECT) return COG_ERROR_INVALID_ARG;
    int dir = session_dir(s, id);
    if (dir < 0) return errno == ENOENT ? COG_ERROR_NOT_FOUND : COG_ERROR_IO;
    manifest_t m;
    cog_session_snapshot_t v;
    cog_store_load_report_t r = {0};
    cog_result_t rc = manifest_read(dir, &m);
    if (rc == COG_OK && (exact != 0U || m.generation != 0U)) {
        uint64_t g = exact != 0U ? exact : m.generation;
        rc = generation_read(s, dir, g, g == m.generation ? &m : NULL, &v);
        r.source_version = rc == COG_OK && s->read_buffer[GEN_SIZE + 7U] == '3' ? 3U : 2U;
        r.checkpoint_generation = g;
    } else if (rc == COG_OK) rc = legacy_read(s, dir, id, mode, &v, &r);
    (void)close(dir);
    if (rc != COG_OK) return rc;
    if (!str_eq(v.session_id, id)) return COG_ERROR_PROTOCOL;
    if (v.inspection_only && mode == COG_STORE_LOAD_REPLAY) return COG_ERROR_UNSUPPORTED;
    cog_session_config_t cfg = {.id = id, .max_nodes = v.node_count > 4095U ? v.node_count + 1U : 4096U,
        .max_payload_bytes = s->capacity, .max_tool_calls_per_message = 16U};
    for (size_t i = 0U; i < v.node_count; ++i)
        if (v.nodes[i].message != NULL && v.nodes[i].message->tool_call_count > cfg.max_tool_calls_per_message)
            cfg.max_tool_calls_per_message = v.nodes[i].message->tool_call_count;
    r.source_audit_high_water = v.audit_high_water;
    r.contexts_present = context_history(&v);
    if (r.contexts_present) {
        cfg.max_contexts = v.context_count == 0U ? 1U : v.context_count;
        cfg.max_context_bindings = v.context_binding_count == 0U ? 1U : v.context_binding_count;
        for (size_t i = 0U; i < v.context_count; ++i) {
            if (v.contexts[i].namespace_id == NULL) return COG_ERROR_PROTOCOL;
            const size_t name = strlen(v.contexts[i].namespace_id);
            if (name == SIZE_MAX || v.contexts[i].data_bytes > SIZE_MAX - name - 1U ||
                name + 1U + v.contexts[i].data_bytes > s->capacity - cfg.max_context_bytes) return COG_ERROR_LIMIT;
            cfg.max_context_bytes += name + 1U + v.contexts[i].data_bytes;
        }
        if (cfg.max_context_bytes == 0U) cfg.max_context_bytes = 1U;
    }
    rc = cog_session_restore(out, &cfg, &v);
    if (rc != COG_OK) return rc;
    r.replay_safe = !v.inspection_only;
    for (size_t i = 0U; i < v.ledger_count; ++i)
        if (v.ledger[i].state == COG_TOOL_EXECUTION_STARTED || v.ledger[i].state == COG_TOOL_EXECUTION_INDETERMINATE)
            ++r.indeterminate_tools;
    *report = r; return COG_OK;
}
static cog_result_t file_load_ex(void *ctx, const char *id, cog_store_load_mode_t mode,
    cog_session_t **out, cog_store_load_report_t *report)
{ return file_load_common(ctx, id, 0U, mode, out, report); }
static cog_result_t file_load(void *ctx, const char *id, cog_session_t **out)
{ cog_store_load_report_t report; return file_load_ex(ctx, id, COG_STORE_LOAD_REPLAY, out, &report); }
static cog_result_t file_checkpoint_load(void *ctx, const char *id, cog_store_checkpoint_t cp,
    cog_store_load_mode_t mode, cog_session_t **out, cog_store_load_report_t *report)
{ return cp.generation == 0U ? COG_ERROR_NOT_FOUND : file_load_common(ctx, id, cp.generation, mode, out, report); }
static cog_result_t file_checkpoint_save(void *ctx, const cog_session_t *session, const void *guard,
    cog_store_checkpoint_t *out)
{
    cog_store_t *s = ctx; const cog_store_owner_guard_t *g = guard;
    if (s == NULL || session == NULL || g == NULL || out == NULL) return COG_ERROR_NULL_PTR;
    if (!s->require_owner || g->store != s || g->recovery || g->borrowers != 0U ||
        find_guard(s, cog_session_id(session)) != g) return COG_ERROR_INVALID_STATE;
    return file_commit(s, session, (cog_store_owner_guard_t *)g, true, false, NULL, out);
}
static cog_result_t file_owner_read(void *ctx, const char *id, cog_store_owner_t *owner, cog_store_checkpoint_t *cp)
{
    cog_store_t *s = ctx;
    if (s == NULL || owner == NULL || cp == NULL) return COG_ERROR_NULL_PTR;
    cog_result_t arg_rc = valid_id(id); if (arg_rc != COG_OK) return arg_rc;
    int dir = session_dir(s, id);
    if (dir < 0 && errno == ENOENT) { *owner = (cog_store_owner_t){0}; *cp = (cog_store_checkpoint_t){0}; return COG_OK; }
    if (dir < 0) return COG_ERROR_IO;
    manifest_t m;
    cog_result_t rc = manifest_read(dir, &m);
    if (rc == COG_OK && m.generation != 0U) {
        cog_session_snapshot_t v;
        rc = generation_read(s, dir, m.generation, &m, &v);
        if (rc == COG_OK) rc = validate(&v, id);
    }
    (void)close(dir);
    if (rc == COG_OK) { *owner = m.owner; cp->generation = m.generation; }
    return rc;
}
static cog_result_t file_owner_cas(void *ctx, const char *id, cog_store_owner_t expected,
    cog_store_owner_t next, cog_store_checkpoint_t cp)
{
    cog_store_t *s = ctx;
    if (s == NULL) return COG_ERROR_NULL_PTR;
    cog_result_t arg_rc = valid_id(id); if (arg_rc != COG_OK) return arg_rc;
    if ((expected.epoch == 0U) != (expected.instance_id == 0U) || expected.epoch == UINT64_MAX ||
        next.epoch != expected.epoch + 1U || next.instance_id == 0U) return COG_ERROR_INVALID_ARG;
    int dir, lock, admission_base = -1, admission_domain = -1;
    cog_result_t rc = session_lock(s, id, &dir, &lock);
    if (rc != COG_OK) return rc;
    manifest_t m;
    rc = manifest_read(dir, &m);
    if (rc != COG_OK) goto done;
    if (m.generation != cp.generation || (!same_owner(m.owner, expected) && !same_owner(m.owner, next))) {
        rc = COG_ERROR_INVALID_STATE; goto done;
    }
    if (m.generation != 0U) {
        cog_session_snapshot_t v;
        rc = generation_read(s, dir, m.generation, &m, &v);
        if (rc == COG_OK) rc = validate(&v, id);
        if (rc != COG_OK) goto done;
        if (v.inspection_only || (m.owner.epoch == 0U && uncertainty(&v))) { rc = COG_ERROR_INDETERMINATE; goto done; }
    } else {
        cog_store_legacy_files_t files;
        rc = cog_store_file_legacy_probe(dir, &files);
        if (rc != COG_OK) goto done;
        if (files.present) { rc = COG_ERROR_UNSUPPORTED; goto done; }
    }
    if (!m.present && s->max_sessions != 0U) {
        rc = domain_lock(s, &admission_base, &admission_domain);
        if (rc != COG_OK) goto done;
        size_t n = 0U; rc = count_sessions(s, &n);
        if (rc != COG_OK) goto done;
        if (n >= s->max_sessions) { rc = COG_ERROR_LIMIT; goto done; }
    }
    if (same_owner(m.owner, next)) {
        rc = durable_confirm(dir, &m);
        if (rc != COG_OK) rc = COG_ERROR_INDETERMINATE;
    } else {
        bool published; m.owner = next;
        rc = manifest_publish(dir, &m, &published);
        if (rc != COG_OK && published) rc = COG_ERROR_INDETERMINATE;
    }
 done:
    if (admission_domain >= 0) (void)close(admission_domain);
    if (admission_base >= 0) (void)close(admission_base);
    (void)close(lock); (void)close(dir); return rc;
}
static cog_result_t file_acquire_common(cog_store_t *s, const char *id, cog_store_owner_t expected,
    bool recovery, void **out)
{
    if (out != NULL) *out = NULL;
    if (s == NULL || out == NULL) return COG_ERROR_NULL_PTR;
    cog_result_t arg_rc = valid_id(id); if (arg_rc != COG_OK) return arg_rc;
    if (!s->require_owner) return COG_ERROR_UNSUPPORTED;
    int dir, lock;
    cog_result_t rc = session_lock(s, id, &dir, &lock);
    if (rc != COG_OK) return rc;
    manifest_t m;
    rc = manifest_read(dir, &m);
    bool uncertain = false;
    if (rc == COG_OK && (!same_owner(m.owner, expected) || expected.epoch == 0U)) rc = COG_ERROR_INVALID_STATE;
    if (rc == COG_OK && m.generation != 0U) {
        cog_session_snapshot_t v;
        rc = generation_read(s, dir, m.generation, &m, &v);
        if (rc == COG_OK) rc = validate(&v, id);
        if (rc == COG_OK && v.inspection_only) rc = COG_ERROR_UNSUPPORTED;
        if (rc == COG_OK) uncertain = uncertainty(&v);
    } else if (rc == COG_OK) {
        cog_store_legacy_files_t files;
        rc = cog_store_file_legacy_probe(dir, &files);
        if (rc == COG_OK && files.present) rc = COG_ERROR_UNSUPPORTED;
    }
    if (rc == COG_OK && uncertain != recovery) rc = recovery ? COG_ERROR_INVALID_STATE : COG_ERROR_INDETERMINATE;
    if (rc == COG_OK && durable_confirm(dir, &m) != COG_OK) rc = COG_ERROR_INDETERMINATE;
    if (rc == COG_OK) {
        /* Explicit guard constructor is the sole runtime allocation here. */
        cog_store_owner_guard_t *g = calloc(1U, sizeof(*g));
        if (g == NULL) rc = COG_ERROR_NOMEM;
        else {
            g->store = s; g->fd = lock; g->owner = expected; g->recovery = recovery;
            (void)memcpy(g->id, id, strlen(id) + 1U);
            g->next = s->guards; s->guards = g; *out = g;
        }
    }
    (void)close(dir);
    if (rc != COG_OK) (void)close(lock);
    return rc;
}
static cog_result_t file_acquire(void *ctx, const char *id, cog_store_owner_t owner, void **out)
{ return file_acquire_common(ctx, id, owner, false, out); }
static cog_result_t file_acquire_recovery(void *ctx, const char *id, cog_store_owner_t owner, void **out)
{ return file_acquire_common(ctx, id, owner, true, out); }
static cog_result_t file_release(void *ctx, void *guard)
{
    cog_store_t *s = ctx; cog_store_owner_guard_t *g = guard;
    if (g == NULL) return COG_OK;
    if (s == NULL) return COG_ERROR_NULL_PTR;
    if (g->store != s || g->borrowers != 0U) return COG_ERROR_INVALID_STATE;
    cog_store_owner_guard_t **p = &s->guards;
    while (*p != NULL && *p != g) p = &(*p)->next;
    if (*p == NULL) return COG_ERROR_INVALID_STATE;
    *p = g->next;
    cog_result_t rc = close(g->fd) == 0 ? COG_OK : COG_ERROR_IO;
    free(g); return rc;
}

static bool legacy_file(const char *name)
{
    for (size_t i = 0U; i < sizeof(legacy_names) / sizeof(legacy_names[0]); ++i)
        if (strcmp(name, legacy_names[i]) == 0) return true;
    return false;
}
static cog_result_t file_delete(void *ctx, const char *id)
{
    cog_store_t *s = ctx;
    if (s == NULL) return COG_ERROR_NULL_PTR;
    cog_result_t arg_rc = valid_id(id); if (arg_rc != COG_OK) return arg_rc;
    int base, domain;
    cog_result_t rc = domain_lock(s, &base, &domain);
    if (rc != COG_OK) return rc;
    int dir = openat(base, id, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (dir < 0) { rc = errno == ENOENT ? COG_ERROR_NOT_FOUND : COG_ERROR_IO; goto done; }
    struct stat st;
    if (fstatat(dir, ANCHOR, &st, AT_SYMLINK_NOFOLLOW) == 0 ||
        fstatat(dir, MANIFEST, &st, AT_SYMLINK_NOFOLLOW) == 0) { rc = COG_ERROR_UNSUPPORTED; goto close_dir; }
    if (fstatat(dir, "meta.json", &st, AT_SYMLINK_NOFOLLOW) != 0) { rc = COG_ERROR_NOT_FOUND; goto close_dir; }
    directory_cursor_t cursor;
    rc = directory_open(dir, &cursor);
    if (rc != COG_OK) goto close_dir;
    const char *name;
    while ((rc = directory_next(&cursor, &name)) == COG_OK && name != NULL) {
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;
        if (!legacy_file(name) || fstatat(dir, name, &st, AT_SYMLINK_NOFOLLOW) != 0 || !S_ISREG(st.st_mode)) {
            rc = COG_ERROR_IO; break;
        }
    }
    (void)close(cursor.fd);
    if (rc == COG_OK) rc = directory_open(dir, &cursor);
    if (rc == COG_OK) {
        while ((rc = directory_next(&cursor, &name)) == COG_OK && name != NULL)
            if (legacy_file(name) && unlinkat(dir, name, 0) != 0) { rc = COG_ERROR_IO; break; }
        (void)close(cursor.fd);
    }
    if (rc == COG_OK && (unlinkat(base, id, AT_REMOVEDIR) != 0 || fsync(base) != 0)) rc = COG_ERROR_IO;
 close_dir:
    (void)close(dir);
 done:
    (void)close(domain); (void)close(base); return rc;
}
static int compare_ids(const void *a, const void *b)
{ return strcmp(*(char *const *)a, *(char *const *)b); }
static cog_result_t file_list(void *ctx, char ***out, size_t *count)
{
    cog_store_t *s = ctx;
    if (s == NULL || out == NULL || count == NULL) return COG_ERROR_NULL_PTR;
    *out = NULL; *count = 0U;
    int base = open_base(s, false);
    if (base < 0) return errno == ENOENT ? COG_OK : COG_ERROR_IO;
    directory_cursor_t cursor;
    cog_result_t rc = directory_open(base, &cursor); (void)close(base);
    if (rc != COG_OK) return rc;
    /* list is explicitly an output constructor; allocation belongs to caller. */
    size_t n = 0U; const char *name;
    while ((rc = directory_next(&cursor, &name)) == COG_OK && name != NULL)
        if (file_exists(s, name)) ++n;
    if (rc != COG_OK) { (void)close(cursor.fd); return rc; }
    if (n > SIZE_MAX / sizeof(char *)) { (void)close(cursor.fd); return COG_ERROR_LIMIT; }
    char **ids = n == 0U ? NULL : calloc(n, sizeof(*ids));
    if (n != 0U && ids == NULL) { (void)close(cursor.fd); return COG_ERROR_NOMEM; }
    if (lseek(cursor.fd, 0, SEEK_SET) < 0) { (void)close(cursor.fd); free(ids); return COG_ERROR_IO; }
    cursor.at = 0U; cursor.size = 0U; size_t at = 0U;
    while ((rc = directory_next(&cursor, &name)) == COG_OK && name != NULL) if (file_exists(s, name)) {
        if (at == n) { rc = COG_ERROR_AGAIN; break; }
        ids[at] = strdup(name);
        if (ids[at] == NULL) { rc = COG_ERROR_NOMEM; break; }
        ++at;
    }
    (void)close(cursor.fd);
    if (rc != COG_OK) { for (size_t i = 0U; i < at; ++i) free(ids[i]); free(ids); return rc; }
    if (at > 1U) qsort(ids, at, sizeof(*ids), compare_ids);
    *out = ids; *count = at; return COG_OK;
}
static cog_result_t null_save(void *ctx, const cog_session_t *s)
{ (void)ctx; (void)s; return COG_ERROR_UNSUPPORTED; }
static cog_result_t null_append(void *ctx, const cog_session_t *s, const cog_session_node_t *n)
{ (void)n; return null_save(ctx, s); }
static cog_result_t null_load(void *ctx, const char *id, cog_session_t **out)
{ (void)ctx; (void)id; *out = NULL; return COG_ERROR_NOT_FOUND; }
static cog_result_t null_load_ex(void *ctx, const char *id, cog_store_load_mode_t mode,
    cog_session_t **out, cog_store_load_report_t *report)
{ (void)mode; (void)report; return null_load(ctx, id, out); }
static cog_result_t null_delete(void *ctx, const char *id)
{ (void)ctx; (void)id; return COG_ERROR_NOT_FOUND; }
static cog_result_t null_list(void *ctx, char ***ids, size_t *count)
{ (void)ctx; *ids = NULL; *count = 0U; return COG_OK; }
static bool null_exists(void *ctx, const char *id)
{ (void)ctx; (void)id; return false; }
const cog_store_vtable_t cog_store_file_backend = {
    .name = "file", .save = file_save, .load = file_load, .append = file_append,
    .delete_ = file_delete, .list = file_list, .exists = file_exists, .load_ex = file_load_ex,
    .checkpoint_save = file_checkpoint_save, .checkpoint_load = file_checkpoint_load,
    .owner_read = file_owner_read, .owner_compare_exchange = file_owner_cas,
    .owner_acquire = file_acquire, .owner_release = file_release, .owner_acquire_recovery = file_acquire_recovery,
    .capabilities = COG_STORE_CAP_CONTEXT_V3
};
const cog_store_vtable_t cog_store_null_backend = {
    .name = "null", .save = null_save, .load = null_load, .append = null_append,
    .delete_ = null_delete, .list = null_list, .exists = null_exists, .load_ex = null_load_ex
};
static bool fenced(const cog_store_vtable_t *v)
{
    return v->checkpoint_save != NULL && v->checkpoint_load != NULL && v->owner_read != NULL &&
        v->owner_compare_exchange != NULL && v->owner_acquire != NULL && v->owner_release != NULL &&
        v->owner_acquire_recovery != NULL;
}
/** @brief Explicit store/workspace constructor. */
cog_result_t cog_store_new(cog_store_t **out, const cog_store_config_t *config)
{
    if (out == NULL) return COG_ERROR_NULL_PTR;
    *out = NULL;
    cog_store_config_t c = config == NULL ? (cog_store_config_t){0} : *config;
    const cog_store_vtable_t *v = c.backend == NULL ? &cog_store_file_backend : c.backend;
    if ((v->capabilities & ~COG_STORE_CAP_CONTEXT_V3) != 0U ||
        (c.required_capabilities & ~v->capabilities) != 0U) return COG_ERROR_UNSUPPORTED;
    if (v->save == NULL || v->load == NULL || v->append == NULL || v->delete_ == NULL ||
        v->list == NULL || v->exists == NULL) return COG_ERROR_INVALID_ARG;
    bool any_fenced = v->checkpoint_save != NULL || v->checkpoint_load != NULL || v->owner_read != NULL ||
        v->owner_compare_exchange != NULL || v->owner_acquire != NULL || v->owner_release != NULL ||
        v->owner_acquire_recovery != NULL;
    if (v->load_ex == NULL || ((c.require_owner || any_fenced) && !fenced(v)) || c.compress)
        return COG_ERROR_UNSUPPORTED;
    size_t capacity = c.max_record_bytes == 0U ? STORE_CAP : c.max_record_bytes;
    if (capacity <= GEN_SIZE) return COG_ERROR_INVALID_ARG;
    cog_store_t *s = calloc(1U, sizeof(*s));
    if (s == NULL) return COG_ERROR_NOMEM;
    s->backend = v; s->auto_save = c.auto_save; s->require_owner = c.require_owner;
    s->recover_tail = c.recover_truncated_tail; s->max_sessions = c.max_sessions; s->capacity = capacity;
    s->context = c.backend_context;
    if (v == &cog_store_file_backend || v == &cog_store_null_backend) s->context = s;
    if (v == &cog_store_file_backend) {
        s->base = strdup(c.base_dir == NULL ? "." : c.base_dir);
        s->write_buffer = malloc(capacity); s->read_buffer = malloc(capacity); s->scratch = malloc(capacity); s->compare_buffer = malloc(capacity);
        if (s->base == NULL || s->write_buffer == NULL || s->read_buffer == NULL || s->scratch == NULL || s->compare_buffer == NULL) {
            cog_store_free(s); return COG_ERROR_NOMEM;
        }
    }
    *out = s; return COG_OK;
}
/** @brief Destroy an explicitly quiesced store. */
void cog_store_free(cog_store_t *s)
{
    if (s == NULL) return;
    assert(s->guards == NULL);
    if (s->guards != NULL) return;
    free(s->compare_buffer); free(s->scratch); free(s->read_buffer); free(s->write_buffer); free(s->base); free(s);
}
static cog_result_t context_capability(const cog_store_t *s, const cog_session_t *session)
{
    if ((s->backend->capabilities & COG_STORE_CAP_CONTEXT_V3) != 0U) return COG_OK;
    cog_session_snapshot_t snapshot;
    cog_result_t rc = cog_session_snapshot_view(session, &snapshot);
    if (rc != COG_OK) return rc;
    return context_history(&snapshot) ? COG_ERROR_UNSUPPORTED : COG_OK;
}
/** @brief Dispatch a monotonic durable save. */
cog_result_t cog_store_save(cog_store_t *s, const cog_session_t *session)
{
    if (s == NULL || session == NULL) return COG_ERROR_NULL_PTR;
    cog_result_t rc = valid_id(cog_session_id(session));
    if (rc != COG_OK) return rc;
    if (s->require_owner && find_guard(s, cog_session_id(session)) == NULL) return COG_ERROR_INVALID_STATE;
    rc = context_capability(s, session);
    if (rc != COG_OK) return rc;
    rc = s->backend->save(s->context, session);
    if (rc == COG_OK || (rc == COG_ERROR_IO && s->backend != &cog_store_file_backend))
        cog_session_mark_committed(session);
    return rc;
}
/** @brief Dispatch version-aware constructor load. */
cog_result_t cog_store_load_ex(cog_store_t *s, const char *id, cog_store_load_mode_t mode,
    cog_session_t **out, cog_store_load_report_t *report)
{
    if (out != NULL) *out = NULL;
    if (s == NULL || id == NULL || out == NULL || report == NULL) return COG_ERROR_NULL_PTR;
    cog_result_t rc = valid_id(id);
    if (rc != COG_OK) return rc;
    if (mode != COG_STORE_LOAD_REPLAY && mode != COG_STORE_LOAD_INSPECT) return COG_ERROR_INVALID_ARG;
    return s->backend->load_ex(s->context, id, mode, out, report);
}
/** @brief Replay-mode constructor load. */
cog_result_t cog_store_load(cog_store_t *s, const char *id, cog_session_t **out)
{ cog_store_load_report_t r; return cog_store_load_ex(s, id, COG_STORE_LOAD_REPLAY, out, &r); }
/** @brief Append through the same complete generation transaction. */
cog_result_t cog_store_append(cog_store_t *s, const cog_session_t *session, const cog_session_node_t *node)
{
    if (s == NULL || session == NULL || node == NULL) return COG_ERROR_NULL_PTR;
    cog_result_t rc = valid_id(cog_session_id(session));
    if (rc != COG_OK) return rc;
    cog_store_owner_guard_t *g = find_guard(s, cog_session_id(session));
    if (s->require_owner && (g == NULL || g->recovery)) return COG_ERROR_INVALID_STATE;
    rc = context_capability(s, session);
    if (rc != COG_OK) return rc;
    rc = s->backend->append(s->context, session, node);
    if (rc == COG_OK || (rc == COG_ERROR_IO && s->backend != &cog_store_file_backend))
        cog_session_mark_committed(session);
    return rc;
}
/** @brief Delete only supported unowned legacy data. */
cog_result_t cog_store_delete(cog_store_t *s, const char *id)
{
    if (s == NULL) return COG_ERROR_NULL_PTR;
    cog_result_t rc = valid_id(id);
    return rc == COG_OK ? s->backend->delete_(s->context, id) : rc;
}
/** @brief Construct a caller-owned ID listing. */
cog_result_t cog_store_list(cog_store_t *s, char ***ids, size_t *count)
{
    if (s == NULL || ids == NULL || count == NULL) return COG_ERROR_NULL_PTR;
    *ids = NULL; *count = 0U; return s->backend->list(s->context, ids, count);
}
/** @brief Query presence without granting replay or execution. */
bool cog_store_exists(cog_store_t *s, const char *id)
{ return s != NULL && valid_id(id) == COG_OK && s->backend->exists(s->context, id); }
/** @brief Query configured automatic-save policy. */
bool cog_store_auto_save(const cog_store_t *s) { return s != NULL && s->auto_save; }
/** @brief Query fenced profile. */
bool cog_store_requires_owner(const cog_store_t *s) { return s != NULL && s->require_owner; }
/** @brief Save a quiescent exact checkpoint under a matching normal guard. */
cog_result_t cog_store_checkpoint_save(cog_store_t *s, const cog_session_t *session,
    const cog_store_owner_guard_t *g, cog_store_checkpoint_t *cp)
{
    if (s == NULL || session == NULL || g == NULL || cp == NULL) return COG_ERROR_NULL_PTR;
    if (!s->require_owner || g->store != s || g->recovery || g->borrowers != 0U ||
        find_guard(s, cog_session_id(session)) != g) return COG_ERROR_INVALID_STATE;
    cog_result_t rc = context_capability(s, session);
    if (rc != COG_OK) return rc;
    rc = s->backend->checkpoint_save(s->context, session,
        s->backend == &cog_store_file_backend ? (const void *)g : g->backend_guard, cp);
    if (rc == COG_OK || (rc == COG_ERROR_IO && s->backend != &cog_store_file_backend))
        cog_session_mark_committed(session);
    return rc;
}
/** @brief Load one exact immutable token. */
cog_result_t cog_store_checkpoint_load(cog_store_t *s, const char *id, cog_store_checkpoint_t cp,
    cog_store_load_mode_t mode, cog_session_t **out, cog_store_load_report_t *report)
{
    if (out != NULL) *out = NULL;
    if (s == NULL || out == NULL || report == NULL) return COG_ERROR_NULL_PTR;
    cog_result_t rc = valid_id(id); if (rc != COG_OK) return rc;
    if (mode != COG_STORE_LOAD_REPLAY && mode != COG_STORE_LOAD_INSPECT) return COG_ERROR_INVALID_ARG;
    if (!fenced(s->backend)) return COG_ERROR_UNSUPPORTED;
    return s->backend->checkpoint_load(s->context, id, cp, mode, out, report);
}
/** @brief Read diagnostics from validated committed state. */
cog_result_t cog_store_owner_read(cog_store_t *s, const char *id, cog_store_owner_t *owner, cog_store_checkpoint_t *cp)
{
    if (s == NULL || owner == NULL || cp == NULL) return COG_ERROR_NULL_PTR;
    cog_result_t rc = valid_id(id); if (rc != COG_OK) return rc;
    if (!fenced(s->backend)) return COG_ERROR_UNSUPPORTED;
    return s->backend->owner_read(s->context, id, owner, cp);
}
/** @brief Durably compare/exchange an exact owner and latest checkpoint. */
cog_result_t cog_store_owner_compare_exchange(cog_store_t *s, const char *id, cog_store_owner_t expected,
    cog_store_owner_t next, cog_store_checkpoint_t cp)
{
    if (s == NULL) return COG_ERROR_NULL_PTR;
    cog_result_t rc = valid_id(id); if (rc != COG_OK) return rc;
    if (!fenced(s->backend)) return COG_ERROR_UNSUPPORTED;
    if ((expected.epoch == 0U) != (expected.instance_id == 0U) || expected.epoch == UINT64_MAX ||
        next.epoch != expected.epoch + 1U || next.instance_id == 0U) return COG_ERROR_INVALID_ARG;
    return s->backend->owner_compare_exchange(s->context, id, expected, next, cp);
}
static cog_result_t acquire(cog_store_t *s, const char *id, cog_store_owner_t expected,
    bool recovery, cog_store_owner_guard_t **out)
{
    if (out != NULL) *out = NULL;
    if (s == NULL || out == NULL) return COG_ERROR_NULL_PTR;
    cog_result_t rc = valid_id(id); if (rc != COG_OK) return rc;
    if (!s->require_owner || !fenced(s->backend)) return COG_ERROR_UNSUPPORTED;
    if (find_guard(s, id) != NULL) return COG_ERROR_AGAIN;
    cog_store_owner_guard_t *g = NULL;
    if (s->backend != &cog_store_file_backend) {
        g = calloc(1U, sizeof(*g));
        if (g == NULL) return COG_ERROR_NOMEM;
    }
    void *handle = NULL;
    rc = recovery ? s->backend->owner_acquire_recovery(s->context, id, expected, &handle) :
        s->backend->owner_acquire(s->context, id, expected, &handle);
    if (rc != COG_OK) { free(g); return rc; }
    if (s->backend == &cog_store_file_backend) g = handle;
    else {
        g->store = s; g->backend_guard = handle; g->owner = expected; g->recovery = recovery;
        (void)memcpy(g->id, id, strlen(id) + 1U);
    }
    if (s->backend != &cog_store_file_backend) { g->next = s->guards; s->guards = g; }
    *out = g; return COG_OK;
}
/** @brief Construct a normal exclusive execution guard. */
cog_result_t cog_store_owner_acquire(cog_store_t *s, const char *id, cog_store_owner_t expected,
    cog_store_owner_guard_t **out) { return acquire(s, id, expected, false, out); }
/** @brief Construct a restricted recovery guard. */
cog_result_t cog_store_owner_acquire_recovery(cog_store_t *s, const char *id, cog_store_owner_t expected,
    cog_store_owner_guard_t **out) { return acquire(s, id, expected, true, out); }
/** @brief Borrow one normal execution constructor admission. */
cog_result_t cog_store_owner_guard_borrow(cog_store_owner_guard_t *g, const char *id)
{
    if (g == NULL || id == NULL) return COG_ERROR_NULL_PTR;
    if (g->recovery || g->blocked || strcmp(g->id, id) != 0) return COG_ERROR_INVALID_STATE;
    if (g->borrowers == SIZE_MAX) return COG_ERROR_LIMIT;
    ++g->borrowers; return COG_OK;
}
/** @brief Return one completed execution borrow. */
cog_result_t cog_store_owner_guard_return(cog_store_owner_guard_t *g)
{
    if (g == NULL) return COG_ERROR_NULL_PTR;
    if (g->borrowers == 0U) return COG_ERROR_INVALID_STATE;
    --g->borrowers; return COG_OK;
}
/** @brief Destroy a drained guard and release its independently opened lock. */
cog_result_t cog_store_owner_release(cog_store_owner_guard_t *g)
{
    if (g == NULL) return COG_OK;
    if (g->borrowers != 0U) return COG_ERROR_INVALID_STATE;
    cog_store_t *s = g->store;
    if (s->backend == &cog_store_file_backend) return file_release(s, g);
    cog_result_t rc = s->backend->owner_release(s->context, g->backend_guard);
    if (rc == COG_ERROR_INVALID_STATE) return rc;
    cog_store_owner_guard_t **p = &s->guards;
    while (*p != NULL && *p != g) p = &(*p)->next;
    assert(*p == g); *p = g->next; free(g); return rc;
}

#define _GNU_SOURCE
#include "internal.h"
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#ifdef __linux__
#include <linux/types.h>
#endif
/* Directory metadata has fixed reservation; content bytes have an independent quantum. */
#define CI_DIRENTS 4096U
typedef struct ci_root_fd {
    char *id;
    int fd;
} ci_root_fd;
typedef enum ci_phase {
    CI_IDLE,
    CI_ROOT,
    CI_WALK,
    CI_SCOPE_ROOT,
    CI_SCOPE_PROBE,
    CI_SCOPE_NEXT,
    CI_READ,
    CI_SCAN,
    CI_SORT,
    CI_DONE
} ci_phase;
struct cog_instruction_source_reader {
    cog_instruction_source_limits_t limits;
    ci_root_fd *roots;
    size_t root_count, root;
    ci_arena config, arena;
    cog_instruction_source_item_t *items;
    size_t count;
    char *request, *walk, *current, *component;
    size_t position, scope_position;
    unsigned char *directory;
    size_t directory_used, directory_at;
    int dirfd, filefd;
    struct stat initial;
    size_t file_expected, file_used;
    char *file_text;
    cog_instruction_source_operation_t operation;
    bool optional;
    ci_phase phase;
    ci_sha hash;
    ci_utf8 utf8;
    cog_result_t result;
    cog_instruction_diagnostic_t error;
    size_t sort_i, sort_j;
};
static cog_result_t io_error(void) {
    return errno == ENOENT ? COG_ERROR_NOT_FOUND
                           : (errno == ELOOP || errno == ENOTDIR ? COG_ERROR_DENIED : COG_ERROR_IO);
}
static bool close_one(int *fd) {
    if (*fd < 0)
        return true;
    int n = *fd;
    *fd = -1;
    return close(n) == 0;
}
static cog_result_t finish(cog_instruction_source_reader_t *r, cog_result_t rc) {
    bool ok = close_one(&r->filefd);
    if (!close_one(&r->dirfd))
        ok = false;
    if (!ok && rc == COG_OK)
        rc = COG_ERROR_IO;
    r->result = rc;
    r->phase = CI_DONE;
    r->error = (cog_instruction_diagnostic_t){.result = rc,
                                              .root_id = r->roots[r->root].id,
                                              .path = r->current[0] ? r->current : r->request};
    return rc;
}
static bool append_path(cog_instruction_source_reader_t *r, const char *prefix, const char *name,
                        const char *suffix) {
    size_t a = strlen(prefix), b = strlen(name), c = strlen(suffix), n;
    if (!ci_add(a, b, &n) || !ci_add(n, c, &n) || !ci_add(n, a ? 1U : 0U, &n) ||
        n >= r->limits.max_path_bytes)
        return false;
    size_t at = 0;
    if (a) {
        memcpy(r->current, prefix, a);
        r->current[a] = '/';
        at = a + 1U;
    }
    memcpy(r->current + at, name, b);
    memcpy(r->current + at + b, suffix, c + 1U);
    size_t depth = 0;
    return ci_path(r->current, false, &depth) && depth <= r->limits.max_depth;
}
static cog_result_t add_item(cog_instruction_source_reader_t *r, bool present, bool loaded,
                             const char *text, size_t bytes, const unsigned char *hash) {
    if (r->count == r->limits.max_items)
        return COG_ERROR_LIMIT;
    const char *id = ci_string(&r->arena, r->roots[r->root].id),
               *path = ci_string(&r->arena, r->current);
    if (!id || !path)
        return COG_ERROR_LIMIT;
    cog_instruction_source_item_t *i = &r->items[r->count++];
    *i = (cog_instruction_source_item_t){id, path, present, loaded, text, bytes, {0}};
    if (hash)
        memcpy(i->sha256, hash, 32);
    return COG_OK;
}
static cog_result_t start_file(cog_instruction_source_reader_t *r, const char *name,
                               bool optional) {
    r->filefd = openat(r->dirfd, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (r->filefd < 0) {
        cog_result_t rc = io_error();
        if (optional && rc == COG_ERROR_NOT_FOUND) {
            if (r->operation == COG_INSTRUCTION_SOURCE_READ)
                return add_item(r, false, false, NULL, 0, NULL);
            return COG_OK;
        }
        return rc;
    }
    if (fstat(r->filefd, &r->initial) != 0)
        return COG_ERROR_IO;
    if (!S_ISREG(r->initial.st_mode))
        return COG_ERROR_DENIED;
    if (r->initial.st_size < 0 || (uintmax_t)r->initial.st_size > r->limits.max_file_bytes)
        return COG_ERROR_LIMIT;
    r->file_expected = (size_t)r->initial.st_size;
    if (r->file_expected == SIZE_MAX || r->file_expected + 1U > r->arena.capacity - r->arena.used)
        return COG_ERROR_LIMIT;
    r->file_text = r->arena.data + r->arena.used;
    r->arena.used += r->file_expected + 1U;
    r->file_used = 0;
    r->utf8 = (ci_utf8){0};
    ci_sha_init(&r->hash);
    r->phase = CI_READ;
    return COG_OK;
}
static bool unchanged(const struct stat *a, const struct stat *b) {
    return a->st_dev == b->st_dev && a->st_ino == b->st_ino && a->st_size == b->st_size &&
           a->st_mtim.tv_sec == b->st_mtim.tv_sec && a->st_mtim.tv_nsec == b->st_mtim.tv_nsec;
}
static cog_result_t read_unit(cog_instruction_source_reader_t *r, size_t *budget) {
    if (*budget == 0U)
        return COG_OK;
    size_t want = r->file_expected - r->file_used;
    unsigned char extra;
    void *target = r->file_text + r->file_used;
    if (want == 0U) {
        want = 1;
        target = &extra;
    }
    if (want > *budget)
        want = *budget;
    ssize_t got = read(r->filefd, target, want);
    if (got < 0)
        return errno == EINTR ? COG_OK : COG_ERROR_IO;
    if (got > 0) {
        size_t n = (size_t)got;
        *budget -= n;
        if (target == &extra)
            return COG_ERROR_IO;
        if (!ci_utf8_feed(&r->utf8, target, n))
            return COG_ERROR_PROTOCOL;
        ci_sha_update(&r->hash, target, n);
        r->file_used += n;
        return COG_OK;
    }
    struct stat now;
    if (fstat(r->filefd, &now) != 0 || !unchanged(&r->initial, &now) ||
        r->file_used != r->file_expected)
        return COG_ERROR_IO;
    if (r->utf8.remaining)
        return COG_ERROR_PROTOCOL;
    if (!close_one(&r->filefd))
        return COG_ERROR_IO;
    r->file_text[r->file_used] = '\0';
    unsigned char hash[32];
    ci_sha_finish(&r->hash, hash);
    cog_result_t rc = add_item(r, true, true, r->file_text, r->file_used, hash);
    if (rc != COG_OK)
        return rc;
    if (r->operation == COG_INSTRUCTION_SOURCE_READ)
        return finish(r, COG_OK);
    r->phase = CI_SCOPE_NEXT;
    return COG_OK;
}
/* Opens absolute configured roots without following any component symlink. */
static int root_open(const char *path) {
    int fd = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    size_t n = strlen(path), at = 1;
    while (at < n) {
        size_t end = at;
        while (end < n && path[end] != '/')
            ++end;
        if (end == at) {
            ++at;
            continue;
        }
        size_t k = end - at;
        char *part = malloc(k + 1U);
        if (!part) {
            close(fd);
            errno = ENOMEM;
            return -1;
        }
        memcpy(part, path + at, k);
        part[k] = '\0';
        if (strcmp(part, ".") == 0 || strcmp(part, "..") == 0) {
            free(part);
            close(fd);
            errno = EINVAL;
            return -1;
        }
        int next = openat(fd, part, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        free(part);
        int saved = errno;
        if (close(fd) != 0 && next >= 0) {
            close(next);
            errno = EIO;
            return -1;
        }
        if (next < 0) {
            errno = saved;
            return -1;
        }
        fd = next;
        at = end + 1U;
    }
    return fd;
}
cog_result_t cog_instruction_source_reader_free(cog_instruction_source_reader_t *r) {
    if (!r)
        return COG_OK;
    bool ok = close_one(&r->filefd);
    if (!close_one(&r->dirfd))
        ok = false;
    for (size_t i = 0; i < r->root_count; ++i)
        if (!close_one(&r->roots[i].fd))
            ok = false;
    free(r->roots);
    free(r->config.data);
    free(r->arena.data);
    free(r->items);
    free(r->request);
    free(r->walk);
    free(r->current);
    free(r->component);
    free(r->directory);
    free(r);
    return ok ? COG_OK : COG_ERROR_IO;
}
cog_result_t cog_instruction_source_reader_new(cog_instruction_source_reader_t **out,
                                               const cog_instruction_source_config_t *cfg,
                                               cog_instruction_diagnostic_t *err) {
    if (err)
        *err = (cog_instruction_diagnostic_t){0};
    if (!out) {
        if (err)
            err->result = COG_ERROR_NULL_PTR;
        return COG_ERROR_NULL_PTR;
    }
    *out = NULL;
    cog_result_t rc = COG_OK;
    const char *bad = NULL;
    cog_instruction_source_reader_t *r = NULL;
#ifndef __linux__
    (void)cfg;
    rc = COG_ERROR_UNSUPPORTED;
    goto done;
#else
    if (!cfg || (!cfg->roots && cfg->root_count)) {
        rc = COG_ERROR_NULL_PTR;
        goto done;
    }
    const cog_instruction_source_limits_t *l = &cfg->limits;
    if (!l->max_roots || !l->max_path_bytes || !l->max_file_bytes || !l->max_total_bytes ||
        !l->max_items || !l->max_depth || !l->work_quantum || !l->byte_quantum ||
        !cfg->root_count) {
        rc = COG_ERROR_INVALID_ARG;
        goto done;
    }
    if (cfg->root_count > l->max_roots || cfg->root_count > SIZE_MAX / sizeof(ci_root_fd) ||
        l->max_items > SIZE_MAX / sizeof(cog_instruction_source_item_t) ||
        cfg->root_count > SIZE_MAX / l->max_path_bytes) {
        rc = COG_ERROR_LIMIT;
        goto done;
    }
    r = calloc(1, sizeof(*r));
    if (!r) {
        rc = COG_ERROR_NOMEM;
        goto done;
    }
    r->dirfd = r->filefd = -1;
    r->limits = *l;
    r->roots = ci_array(cfg->root_count, sizeof(*r->roots));
    r->config.capacity = cfg->root_count * l->max_path_bytes;
    r->config.data = malloc(r->config.capacity);
    r->arena.capacity = l->max_total_bytes;
    r->arena.data = malloc(l->max_total_bytes);
    r->items = ci_array(l->max_items, sizeof(*r->items));
    r->request = malloc(l->max_path_bytes);
    r->walk = malloc(l->max_path_bytes);
    r->current = malloc(l->max_path_bytes);
    r->component = malloc(l->max_path_bytes);
    r->directory = malloc(CI_DIRENTS);
    if (!r->roots || !r->config.data || !r->arena.data || !r->items || !r->request || !r->walk ||
        !r->current || !r->component || !r->directory) {
        rc = COG_ERROR_NOMEM;
        goto done;
    }
    for (size_t i = 0; i < cfg->root_count; ++i)
        r->roots[i].fd = -1;
    for (size_t i = 0; i < cfg->root_count; ++i) {
        const cog_instruction_source_root_t *root = &cfg->roots[i];
        bad = ci_root(root->id) ? root->id : NULL;
        if (!ci_root(root->id) || !root->directory || root->directory[0] != '/' ||
            !ci_text(root->directory, strlen(root->directory), NULL)) {
            rc = COG_ERROR_INVALID_ARG;
            goto done;
        }
        if (strlen(root->id) >= l->max_path_bytes || strlen(root->directory) >= l->max_path_bytes) {
            rc = COG_ERROR_LIMIT;
            goto done;
        }
        for (size_t j = 0; j < i; ++j)
            if (strcmp(root->id, r->roots[j].id) == 0) {
                rc = COG_ERROR_INVALID_ARG;
                goto done;
            }
        r->roots[i].id = ci_string(&r->config, root->id);
        r->roots[i].fd = root_open(root->directory);
        if (r->roots[i].fd < 0) {
            rc = errno == ENOMEM   ? COG_ERROR_NOMEM
                 : errno == ENOENT ? COG_ERROR_NOT_FOUND
                                   : COG_ERROR_IO;
            goto done;
        }
        ++r->root_count;
    }
    *out = r;
    return COG_OK;
#endif
done:
    if (err)
        *err = (cog_instruction_diagnostic_t){.result = rc, .root_id = bad};
    cog_instruction_source_reader_free(r);
    return rc;
}
cog_result_t cog_instruction_source_begin(cog_instruction_source_reader_t *r,
                                          const cog_instruction_source_request_t *q) {
    if (!r || !q || !q->root_id || !q->path)
        return COG_ERROR_NULL_PTR;
    if (r->phase != CI_IDLE && r->phase != CI_DONE)
        return COG_ERROR_INVALID_STATE;
    if (q->operation < COG_INSTRUCTION_SOURCE_READ || q->operation > COG_INSTRUCTION_SOURCE_SCOPE ||
        (q->optional && q->operation != COG_INSTRUCTION_SOURCE_READ))
        return COG_ERROR_INVALID_ARG;
    size_t depth = 0;
    if (!ci_path(q->path, q->operation != COG_INSTRUCTION_SOURCE_READ, &depth))
        return COG_ERROR_INVALID_ARG;
    if (strlen(q->path) >= r->limits.max_path_bytes || depth > r->limits.max_depth)
        return COG_ERROR_LIMIT;
    size_t root = 0;
    for (; root < r->root_count; ++root)
        if (strcmp(q->root_id, r->roots[root].id) == 0)
            break;
    if (root == r->root_count)
        return COG_ERROR_NOT_FOUND;
    r->root = root;
    memmove(r->request, q->path, strlen(q->path) + 1U);
    memmove(r->current, r->request, strlen(r->request) + 1U);
    r->walk[0] = '\0';
    r->operation = q->operation;
    r->optional = q->optional;
    r->position = r->scope_position = 0;
    r->count = r->arena.used = 0;
    r->directory_used = r->directory_at = 0;
    r->result = COG_OK;
    r->error = (cog_instruction_diagnostic_t){0};
    r->phase = CI_ROOT;
    return COG_OK;
}
static cog_result_t walk_unit(cog_instruction_source_reader_t *r) {
    size_t n = strlen(r->request);
    if (r->position == n) {
        if (r->operation == COG_INSTRUCTION_SOURCE_SCOPE) {
            if (!close_one(&r->dirfd))
                return COG_ERROR_IO;
            r->phase = CI_SCOPE_ROOT;
        } else
            r->phase = CI_SCAN;
        return COG_OK;
    }
    size_t end = r->position;
    while (end < n && r->request[end] != '/')
        ++end;
    size_t k = end - r->position;
    memcpy(r->component, r->request + r->position, k);
    r->component[k] = '\0';
    if (end == n && r->operation == COG_INSTRUCTION_SOURCE_READ) {
        cog_result_t rc = start_file(r, r->component, r->optional);
        if (rc == COG_OK && r->phase != CI_READ)
            return finish(r, COG_OK);
        return rc;
    }
    int fd = openat(r->dirfd, r->component, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0)
        return io_error();
    if (!close_one(&r->dirfd)) {
        close(fd);
        return COG_ERROR_IO;
    }
    r->dirfd = fd;
    r->position = end < n ? end + 1U : end;
    return COG_OK;
}
#ifdef __linux__
struct ci_dirent {
    uint64_t ino;
    int64_t off;
    unsigned short reclen;
    unsigned char type;
    char name[];
};
#endif
static cog_result_t scan_unit(cog_instruction_source_reader_t *r) {
#ifndef __linux__
    (void)r;
    return COG_ERROR_UNSUPPORTED;
#else
    if (r->directory_at == r->directory_used) {
        long got = syscall(SYS_getdents64, r->dirfd, r->directory, CI_DIRENTS);
        if (got < 0)
            return errno == EINTR ? COG_OK : COG_ERROR_IO;
        if (got == 0) {
            r->phase = CI_SORT;
            r->sort_i = r->sort_j = 1;
            return COG_OK;
        }
        r->directory_at = 0;
        r->directory_used = (size_t)got;
        return COG_OK;
    }
    size_t remain = r->directory_used - r->directory_at;
    if (remain < offsetof(struct ci_dirent, name) + 1U)
        return COG_ERROR_IO;
    unsigned short length;
    memcpy(&length, r->directory + r->directory_at + offsetof(struct ci_dirent, reclen),
           sizeof(length));
    if (length > remain || length < offsetof(struct ci_dirent, name) + 1U)
        return COG_ERROR_IO;
    const char *name =
        (const char *)(r->directory + r->directory_at + offsetof(struct ci_dirent, name));
    size_t max = (size_t)length - offsetof(struct ci_dirent, name);
    const char *zero = memchr(name, 0, max);
    if (!zero)
        return COG_ERROR_IO;
    size_t n = (size_t)(zero - name);
    r->directory_at += (size_t)length;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
        return COG_OK;
    bool agent = r->operation == COG_INSTRUCTION_SOURCE_AGENT_SCAN;
    if (agent && (n < 3U || memcmp(name + n - 3U, ".md", 3U) != 0))
        return COG_OK;
    struct stat st;
    if (fstatat(r->dirfd, name, &st, AT_SYMLINK_NOFOLLOW) != 0)
        return io_error();
    if (S_ISLNK(st.st_mode))
        return COG_ERROR_DENIED;
    if (agent) {
        if (!ci_text(name, n, NULL))
            return COG_ERROR_PROTOCOL;
        if (!S_ISREG(st.st_mode))
            return COG_ERROR_DENIED;
        if (!append_path(r, r->request, name, ""))
            return COG_ERROR_LIMIT;
        return add_item(r, true, false, NULL, 0, NULL);
    }
    if (!S_ISDIR(st.st_mode))
        return COG_OK;
    int fd = openat(r->dirfd, name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0)
        return io_error();
    int result = fstatat(fd, "SKILL.md", &st, AT_SYMLINK_NOFOLLOW);
    int error = errno;
    bool closed = close(fd) == 0;
    if (!closed)
        return COG_ERROR_IO;
    if (result != 0) {
        if (error == ENOENT)
            return COG_OK;
        errno = error;
        return io_error();
    }
    if (!S_ISREG(st.st_mode))
        return COG_ERROR_DENIED;
    if (!ci_text(name, n, NULL))
        return COG_ERROR_PROTOCOL;
    if (!append_path(r, r->request, name, "/SKILL.md"))
        return COG_ERROR_LIMIT;
    return add_item(r, true, false, NULL, 0, NULL);
#endif
}
cog_result_t cog_instruction_source_poll(cog_instruction_source_reader_t *r, bool *done) {
    if (!r || !done)
        return COG_ERROR_NULL_PTR;
    if (r->phase == CI_IDLE)
        return COG_ERROR_INVALID_STATE;
    if (r->phase == CI_DONE) {
        *done = true;
        return r->result;
    }
    size_t bytes = r->limits.byte_quantum;
    cog_result_t rc = COG_OK;
    for (size_t unit = 0; unit < r->limits.work_quantum && r->phase != CI_DONE; ++unit) {
        switch (r->phase) {
        case CI_ROOT:
            r->dirfd =
                openat(r->roots[r->root].fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
            if (r->dirfd < 0)
                rc = COG_ERROR_IO;
            else
                r->phase = CI_WALK;
            break;
        case CI_WALK:
            rc = walk_unit(r);
            break;
        case CI_SCOPE_ROOT:
            r->dirfd =
                openat(r->roots[r->root].fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
            if (r->dirfd < 0)
                rc = COG_ERROR_IO;
            else {
                r->walk[0] = '\0';
                r->phase = CI_SCOPE_PROBE;
            }
            break;
        case CI_SCOPE_PROBE:
            if (!append_path(r, r->walk, "AGENTS.md", "")) {
                rc = COG_ERROR_LIMIT;
                break;
            }
            rc = start_file(r, "AGENTS.md", true);
            if (rc == COG_OK && r->phase != CI_READ)
                r->phase = CI_SCOPE_NEXT;
            break;
        case CI_SCOPE_NEXT: {
            size_t n = strlen(r->request);
            if (r->scope_position == n) {
                rc = finish(r, COG_OK);
                break;
            }
            size_t end = r->scope_position;
            while (end < n && r->request[end] != '/')
                ++end;
            size_t k = end - r->scope_position;
            memcpy(r->component, r->request + r->scope_position, k);
            r->component[k] = '\0';
            int fd =
                openat(r->dirfd, r->component, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
            if (fd < 0) {
                rc = io_error();
                break;
            }
            if (!close_one(&r->dirfd)) {
                close(fd);
                rc = COG_ERROR_IO;
                break;
            }
            r->dirfd = fd;
            memcpy(r->walk, r->request, end);
            r->walk[end] = '\0';
            r->scope_position = end < n ? end + 1U : end;
            r->phase = CI_SCOPE_PROBE;
            break;
        }
        case CI_READ:
            if (bytes == 0U) {
                *done = false;
                return COG_OK;
            }
            rc = read_unit(r, &bytes);
            break;
        case CI_SCAN:
            rc = scan_unit(r);
            break;
        case CI_SORT:
            if (r->sort_i >= r->count) {
                rc = finish(r, COG_OK);
                break;
            }
            if (r->sort_j > 0U &&
                strcmp(r->items[r->sort_j - 1U].path, r->items[r->sort_j].path) > 0) {
                cog_instruction_source_item_t temp = r->items[r->sort_j];
                r->items[r->sort_j] = r->items[r->sort_j - 1U];
                r->items[r->sort_j - 1U] = temp;
                --r->sort_j;
            } else {
                ++r->sort_i;
                r->sort_j = r->sort_i;
            }
            break;
        case CI_IDLE:
        case CI_DONE:
            break;
        }
        if (rc != COG_OK) {
            (void)finish(r, rc);
            break;
        }
    }
    *done = r->phase == CI_DONE;
    return rc;
}
cog_result_t cog_instruction_source_view(const cog_instruction_source_reader_t *r,
                                         const cog_instruction_source_item_t **items,
                                         size_t *count) {
    if (!r || !items || !count)
        return COG_ERROR_NULL_PTR;
    if (r->phase == CI_IDLE)
        return COG_ERROR_INVALID_STATE;
    if (r->phase != CI_DONE)
        return COG_ERROR_AGAIN;
    if (r->result != COG_OK)
        return r->result;
    *items = r->count ? r->items : NULL;
    *count = r->count;
    return COG_OK;
}
cog_result_t cog_instruction_source_error(const cog_instruction_source_reader_t *r,
                                          cog_instruction_diagnostic_t *error) {
    if (!r || !error)
        return COG_ERROR_NULL_PTR;
    if (r->phase == CI_IDLE)
        return COG_ERROR_INVALID_STATE;
    if (r->phase != CI_DONE)
        return COG_ERROR_AGAIN;
    if (r->result == COG_OK)
        return COG_ERROR_NOT_FOUND;
    *error = r->error;
    return COG_OK;
}
cog_result_t cog_instruction_source_cancel(cog_instruction_source_reader_t *r) {
    if (!r)
        return COG_ERROR_NULL_PTR;
    if (r->phase == CI_IDLE)
        return COG_ERROR_INVALID_STATE;
    if (r->phase == CI_DONE)
        return COG_OK;
    bool ok = close_one(&r->filefd);
    if (!close_one(&r->dirfd))
        ok = false;
    (void)finish(r, COG_ERROR_ABORTED);
    return ok ? COG_OK : COG_ERROR_IO;
}

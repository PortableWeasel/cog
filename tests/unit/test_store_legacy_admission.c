/** @file test_store_legacy_admission.c @brief Incomplete legacy data cannot become empty authority. */
#define _POSIX_C_SOURCE 200809L
#include <cog/session_store.h>
#include "session_store_file.h"
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *const names[] = {"meta.json", "nodes.jsonl", "index.bin",
    "meta.json.tmp", "nodes.jsonl.tmp", "index.bin.tmp"};
static const char metadata[] = "{\"version\":1,\"id\":\"Y2FzZQ==\",\"parent_id\":null,\"max_nodes\":8,\"node_count\":0,\"next_node_id\":1,\"active_node_id\":0}";
static const char *fault_name;
static int fault_errno;
static unsigned fault_hits;
int __real_fstatat(int dir, const char *path, struct stat *info, int flags);
int __wrap_fstatat(int dir, const char *path, struct stat *info, int flags);
int __wrap_fstatat(int dir, const char *path, struct stat *info, int flags)
{
    if (fault_name != NULL && strcmp(path, fault_name) == 0) {
        assert(flags == AT_SYMLINK_NOFOLLOW);
        fault_hits++; errno = fault_errno; return -1;
    }
    return __real_fstatat(dir, path, info, flags);
}
static void remove_tree(const char *path)
{
    DIR *dir = opendir(path); assert(dir != NULL); struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        char child[1024]; assert(snprintf(child, sizeof(child), "%s/%s", path, entry->d_name) > 0);
        struct stat info; assert(lstat(child, &info) == 0);
        if (S_ISDIR(info.st_mode)) remove_tree(child); else assert(unlink(child) == 0);
    }
    assert(closedir(dir) == 0); assert(rmdir(path) == 0);
}
static cog_store_t *new_store(const char *base, bool owned)
{
    cog_store_t *store = NULL;
    cog_store_config_t config = {.base_dir = base, .require_owner = owned, .max_record_bytes = 1048576U};
    assert(cog_store_new(&store, &config) == COG_OK); return store;
}
static cog_session_t *new_session(void)
{
    cog_session_t *session = NULL;
    cog_session_config_t config = {.id = "case", .max_nodes = 8U, .max_payload_bytes = 4096U};
    assert(cog_session_new(&session, &config) == COG_OK); return session;
}
static void path_for(char *out, size_t capacity, const char *base, const char *name)
{
    int size = snprintf(out, capacity, "%s/case/%s", base, name);
    assert(size > 0 && (size_t)size < capacity);
}
static void put(const char *base, const char *name, const char *text)
{
    char path[1024]; path_for(path, sizeof(path), base, name);
    FILE *stream = fopen(path, "w"); assert(stream != NULL);
    assert(fwrite(text, 1U, strlen(text), stream) == strlen(text)); assert(fclose(stream) == 0);
}
static void expect_owner(cog_store_t *store, cog_store_owner_t expected, uint64_t generation)
{
    cog_store_owner_t owner; cog_store_checkpoint_t checkpoint;
    assert(cog_store_owner_read(store, "case", &owner, &checkpoint) == COG_OK);
    assert(owner.epoch == expected.epoch && owner.instance_id == expected.instance_id);
    assert(checkpoint.generation == generation);
}
static void artifact_case(size_t name, unsigned kind, bool owned)
{
    char base[] = "/tmp/cog-legacy-artifact-XXXXXX"; assert(mkdtemp(base) != NULL);
    char path[1024]; path_for(path, sizeof(path), base, ""); assert(mkdir(path, 0700) == 0);
    cog_store_t *store = new_store(base, owned); cog_session_t *session = new_session();
    cog_store_owner_t old = {0}, next = {1U, 11U}; cog_store_checkpoint_t zero = {0};
    cog_store_owner_guard_t *guard = NULL;
    if (owned) {
        assert(cog_store_owner_compare_exchange(store, "case", old, next, zero) == COG_OK);
        old = next; next = (cog_store_owner_t){2U, 22U};
        assert(cog_store_owner_acquire(store, "case", old, &guard) == COG_OK);
    }
    path_for(path, sizeof(path), base, names[name]);
    if (kind == 0U) put(base, names[name], name == 0U ? metadata : "unidentified legacy effect");
    else if (kind == 1U) assert(mkdir(path, 0700) == 0);
    else assert(symlink("absent-target", path) == 0);
    struct stat before, after; assert(lstat(path, &before) == 0);
    cog_result_t read_error = name == 0U ? COG_ERROR_IO : COG_ERROR_UNSUPPORTED;
    cog_session_t *loaded = NULL; cog_store_load_report_t report = {.source_version = 99U};
    assert(cog_store_load_ex(store, "case", COG_STORE_LOAD_REPLAY, &loaded, &report) == read_error);
    assert(loaded == NULL && report.source_version == 99U);
    assert(cog_store_save(store, session) == read_error);
    if (owned) {
        assert(cog_store_owner_compare_exchange(store, "case", old, next, zero) == COG_ERROR_AGAIN);
        assert(cog_store_owner_release(guard) == COG_OK); guard = NULL;
        assert(cog_store_owner_acquire(store, "case", old, &guard) == COG_ERROR_UNSUPPORTED);
        assert(guard == NULL);
    }
    assert(cog_store_owner_compare_exchange(store, "case", old, next, zero) == COG_ERROR_UNSUPPORTED);
    expect_owner(store, old, 0U);
    assert(lstat(path, &after) == 0 && before.st_ino == after.st_ino && before.st_size == after.st_size);
    if (kind == 0U) {
        FILE *file = fopen(path, "r"); assert(file != NULL); char data[256] = {0};
        size_t count = fread(data, 1U, sizeof(data), file); assert(fclose(file) == 0);
        const char *expected = name == 0U ? metadata : "unidentified legacy effect";
        assert(count == strlen(expected) && memcmp(data, expected, count) == 0);
    }
    cog_session_free(session); cog_store_free(store); remove_tree(base);
}
static void io_case(const char *name, int error)
{
    char base[] = "/tmp/cog-legacy-io-XXXXXX"; assert(mkdtemp(base) != NULL);
    cog_store_t *store = new_store(base, true); cog_session_t *session = new_session();
    cog_store_owner_t one = {1U, 11U}, two = {2U, 22U}; cog_store_checkpoint_t zero = {0};
    fault_name = name; fault_errno = error; fault_hits = 0U;
    assert(cog_store_owner_compare_exchange(store, "case", (cog_store_owner_t){0}, one, zero) == COG_ERROR_IO);
    cog_store_t *unowned = new_store(base, false);
    assert(cog_store_save(unowned, session) == COG_ERROR_IO && fault_hits == 2U);
    fault_name = NULL; expect_owner(store, (cog_store_owner_t){0}, 0U); cog_store_free(unowned);
    assert(cog_store_owner_compare_exchange(store, "case", (cog_store_owner_t){0}, one, zero) == COG_OK);
    fault_name = name; fault_errno = error; fault_hits = 0U;
    assert(cog_store_owner_compare_exchange(store, "case", one, two, zero) == COG_ERROR_IO);
    cog_store_owner_guard_t *guard = NULL;
    assert(cog_store_owner_acquire(store, "case", one, &guard) == COG_ERROR_IO && guard == NULL);
    cog_session_t *loaded = NULL; cog_store_load_report_t report = {.source_version = 99U};
    assert(cog_store_load_ex(store, "case", COG_STORE_LOAD_REPLAY, &loaded, &report) == COG_ERROR_IO);
    assert(loaded == NULL && report.source_version == 99U);
    assert(fault_hits == 3U); fault_name = NULL;
    assert(cog_store_owner_acquire(store, "case", one, &guard) == COG_OK);
    fault_name = name;
    assert(cog_store_save(store, session) == COG_ERROR_IO && fault_hits == 4U);
    fault_name = NULL;
    assert(cog_store_owner_release(guard) == COG_OK); expect_owner(store, one, 0U);
    cog_session_free(session); cog_store_free(store); remove_tree(base);
}
static void absence_and_orphans(void)
{
    char base[] = "/tmp/cog-legacy-absent-XXXXXX"; assert(mkdtemp(base) != NULL);
    cog_store_t *store = new_store(base, true); cog_session_t *session = new_session();
    cog_session_t *loaded = NULL; cog_store_load_report_t report = {0};
    assert(cog_store_load_ex(store, "case", COG_STORE_LOAD_REPLAY, &loaded, &report) == COG_ERROR_NOT_FOUND);
    char path[1024]; path_for(path, sizeof(path), base, ""); assert(mkdir(path, 0700) == 0);
    put(base, ".generation.tmp", "uncommitted"); put(base, ".manifest.tmp", "uncommitted");
    put(base, "generation-00000000000000000001.bin", "uncommitted"); put(base, "unrelated", "unrelated");
    assert(cog_store_load_ex(store, "case", COG_STORE_LOAD_REPLAY, &loaded, &report) == COG_ERROR_NOT_FOUND);
    cog_store_owner_t one = {1U, 11U}, two = {2U, 22U}; cog_store_checkpoint_t zero = {0};
    assert(cog_store_owner_compare_exchange(store, "case", (cog_store_owner_t){0}, one, zero) == COG_OK);
    assert(cog_store_owner_compare_exchange(store, "case", one, two, zero) == COG_OK);
    cog_store_owner_guard_t *guard = NULL; assert(cog_store_owner_acquire(store, "case", two, &guard) == COG_OK);
    cog_store_t *other = new_store(base, true); cog_store_owner_guard_t *blocked = NULL;
    assert(cog_store_owner_acquire(other, "case", two, &blocked) == COG_ERROR_AGAIN && blocked == NULL);
    assert(cog_store_save(store, session) == COG_OK); expect_owner(store, two, 2U);
    assert(cog_store_owner_release(guard) == COG_OK);
    assert(cog_store_load_ex(store, "case", COG_STORE_LOAD_REPLAY, &loaded, &report) == COG_OK);
    assert(report.source_version == 2U && report.checkpoint_generation == 2U);
    cog_session_free(loaded); cog_session_free(session); cog_store_free(other); cog_store_free(store); remove_tree(base);
}
static void migration_and_v2_priority(void)
{
    char base[] = "/tmp/cog-legacy-migrate-XXXXXX"; assert(mkdtemp(base) != NULL);
    char path[1024]; path_for(path, sizeof(path), base, ""); assert(mkdir(path, 0700) == 0);
    put(base, "meta.json", metadata); put(base, "nodes.jsonl", "");
    for (size_t i = 2U; i < sizeof(names) / sizeof(names[0]); ++i) put(base, names[i], "old derived/staged bytes");
    cog_store_t *unowned = new_store(base, false), *owned = new_store(base, true);
    cog_session_t *loaded = NULL; cog_store_load_report_t report = {0};
    assert(cog_store_load_ex(unowned, "case", COG_STORE_LOAD_REPLAY, &loaded, &report) == COG_OK);
    assert(report.source_version == 1U && report.migrated_in_memory);
    cog_store_owner_t one = {1U, 11U}; cog_store_checkpoint_t zero = {0};
    assert(cog_store_owner_compare_exchange(owned, "case", (cog_store_owner_t){0}, one, zero) == COG_ERROR_UNSUPPORTED);
    assert(cog_store_save(unowned, loaded) == COG_OK); cog_session_free(loaded); loaded = NULL;
    cog_store_owner_t old; cog_store_checkpoint_t checkpoint;
    assert(cog_store_owner_read(owned, "case", &old, &checkpoint) == COG_OK && checkpoint.generation == 1U);
    /* Recognized legacy inspection would fail, so these successful operations
       also prove nonzero v2 does not accidentally consult legacy remnants. */
    fault_name = "meta.json"; fault_errno = EIO; fault_hits = 0U;
    assert(cog_store_load_ex(owned, "case", COG_STORE_LOAD_REPLAY, &loaded, &report) == COG_OK);
    assert(report.source_version == 2U);
    assert(cog_store_owner_compare_exchange(owned, "case", old, one, checkpoint) == COG_OK);
    cog_store_owner_guard_t *guard = NULL; assert(cog_store_owner_acquire(owned, "case", one, &guard) == COG_OK);
    assert(cog_store_save(owned, loaded) == COG_OK && fault_hits == 0U);
    fault_name = NULL; assert(cog_store_owner_release(guard) == COG_OK);
    cog_session_free(loaded); cog_store_free(unowned); cog_store_free(owned); remove_tree(base);
}
int main(void)
{
    cog_store_legacy_files_t output = {.present = true, .metadata = true};
    assert(cog_store_file_legacy_probe(-1, NULL) == COG_ERROR_NULL_PTR);
    assert(cog_store_file_legacy_probe(-1, &output) == COG_ERROR_IO && output.present && output.metadata);
    for (size_t i = 0U; i < sizeof(names) / sizeof(names[0]); ++i) {
        for (unsigned kind = 0U; kind < 3U; ++kind) { artifact_case(i, kind, false); artifact_case(i, kind, true); }
        io_case(names[i], EIO); io_case(names[i], EACCES); io_case(names[i], ELOOP);
    }
    absence_and_orphans(); migration_and_v2_priority(); return 0;
}

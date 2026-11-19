/** @file workspace.c @brief Descriptor-relative bounded workspace tools. */
#define _GNU_SOURCE
#include "workspace_internal.h"
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

static bool valid_path(const char *path, size_t length)
{
    if (length == 0U || length >= COG_WORKSPACE_PATH_BYTES || path[0] == '/' ||
        memchr(path, '\0', length) != NULL) { return false; }
    size_t start = 0U;
    for (size_t i = 0U; i <= length; i++) {
        if (i == length || path[i] == '/') {
            size_t count = i - start;
            if (count == 0U || (count == 2U && path[start] == '.' && path[start + 1U] == '.')) { return false; }
            start = i + 1U;
        }
    }
    return true;
}

int cog_workspace_open_parent(cog_workspace_t *workspace, const char *path, char *leaf)
{
    char copy[COG_WORKSPACE_PATH_BYTES];
    size_t length = strlen(path);
    if (!valid_path(path, length)) { errno = EINVAL; return -1; }
    memcpy(copy, path, length + 1U);
    int directory = fcntl(workspace->root_fd, F_DUPFD_CLOEXEC, 3);
    if (directory < 0) { return -1; }
    char *component = copy;
    for (;;) {
        char *slash = strchr(component, '/');
        if (slash == NULL) { memcpy(leaf, component, strlen(component) + 1U); return directory; }
        *slash = '\0';
        int next = openat(directory, component, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        int error = errno;
        (void)close(directory);
        if (next < 0) { errno = error; return -1; }
        directory = next;
        component = slash + 1;
    }
}

int cog_workspace_open_path(cog_workspace_t *workspace, const char *path, int flags)
{
    char leaf[COG_WORKSPACE_PATH_BYTES];
    int parent = cog_workspace_open_parent(workspace, path, leaf);
    if (parent < 0) { return -1; }
    int fd = openat(parent, leaf, flags | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    int error = errno;
    (void)close(parent);
    errno = error;
    return fd;
}

static int open_root(const char *path)
{
    if (*path == '\0' || strlen(path) >= COG_WORKSPACE_PATH_BYTES) { errno = EINVAL; return -1; }
    char copy[COG_WORKSPACE_PATH_BYTES];
    memcpy(copy, path, strlen(path) + 1U);
    int directory = open(path[0] == '/' ? "/" : ".", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (directory < 0) { return -1; }
    char *component = copy;
    while (*component != '\0') {
        while (*component == '/') { component++; }
        if (*component == '\0') { break; }
        char *end = strchr(component, '/');
        if (end != NULL) { *end = '\0'; }
        if (strcmp(component, "..") == 0) { (void)close(directory); errno = EINVAL; return -1; }
        int next = openat(directory, component, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        int error = errno;
        (void)close(directory);
        if (next < 0) { errno = error; return -1; }
        directory = next;
        if (end == NULL) { break; }
        component = end + 1;
    }
    return directory;
}

static cog_result_t path_error(void)
{
    return errno == ELOOP || errno == ENOTDIR || errno == EINVAL ? COG_ERROR_INVALID_ARG :
           errno == ENOENT ? COG_ERROR_NOT_FOUND : COG_ERROR_IO;
}

static bool environment_name(const char *entry, size_t *name_len)
{
    const char *equals = strchr(entry, '=');
    if (equals == NULL || equals == entry) { return false; }
    size_t n = (size_t)(equals - entry);
    for (size_t i = 0U; i < n; i++) {
        unsigned char ch = (unsigned char)entry[i];
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_' ||
              (i != 0U && ch >= '0' && ch <= '9'))) { return false; }
    }
    *name_len = n;
    return true;
}

cog_result_t cog_workspace_new(cog_workspace_t **out, const cog_workspace_config_t *config)
{
    static const char *const defaults[] = { "PATH=/usr/bin:/bin", "LC_ALL=C.UTF-8" };
    if (out == NULL) { return COG_ERROR_NULL_PTR; }
    *out = NULL;
    if (config == NULL || config->root == NULL) { return COG_ERROR_NULL_PTR; }
#if !defined(__linux__) || !defined(O_NOFOLLOW) || !defined(SYS_getdents64)
    return COG_ERROR_UNSUPPORTED;
#else
    if (config->max_file_bytes == 0U || config->max_file_bytes > SIZE_MAX / 2U - 1U ||
        config->max_search_entries == 0U || config->service_quantum == 0U ||
        config->max_search_entries > SIZE_MAX / sizeof(workspace_directory_t) ||
        (config->environment == NULL && config->environment_count != 0U)) { return COG_ERROR_INVALID_ARG; }
    const char *const *environment = config->environment_count == 0U ? defaults : config->environment;
    size_t count = config->environment_count == 0U ? 2U : config->environment_count;
    if (count >= SIZE_MAX / sizeof(char *)) { return COG_ERROR_INVALID_ARG; }
    size_t strings = strlen(config->root) + 1U;
    for (size_t i = 0U; i < count; i++) {
        size_t name_len;
        if (environment[i] == NULL || !environment_name(environment[i], &name_len)) { return COG_ERROR_INVALID_ARG; }
        for (size_t j = 0U; j < i; j++) {
            if (strchr(environment[j], '=') - environment[j] == (ptrdiff_t)name_len &&
                memcmp(environment[i], environment[j], name_len) == 0) { return COG_ERROR_INVALID_ARG; }
        }
        size_t length = strlen(environment[i]) + 1U;
        if (length > SIZE_MAX - strings) { return COG_ERROR_INVALID_ARG; }
        strings += length;
    }
    cog_workspace_t *workspace = calloc(1U, sizeof(*workspace));
    if (workspace == NULL) { return COG_ERROR_NOMEM; }
    workspace->root_fd = -1;
    workspace->environment = calloc(count + 1U, sizeof(char *));
    workspace->strings = malloc(strings);
    workspace->file_workspace = malloc(config->max_file_bytes + 1U);
    workspace->edit_workspace = malloc(config->max_file_bytes + 1U);
    workspace->traversal_workspace = calloc(config->max_search_entries, sizeof(workspace_directory_t));
    if (workspace->environment == NULL || workspace->strings == NULL ||
        workspace->file_workspace == NULL || workspace->edit_workspace == NULL || workspace->traversal_workspace == NULL) {
        (void)cog_workspace_free(workspace); return COG_ERROR_NOMEM;
    }
    char *storage = workspace->strings;
    size_t length = strlen(config->root) + 1U;
    memcpy(storage, config->root, length);
    storage += length;
    for (size_t i = 0U; i < count; i++) {
        workspace->environment[i] = storage;
        length = strlen(environment[i]) + 1U;
        memcpy(storage, environment[i], length);
        storage += length;
    }
    workspace->root_fd = open_root(config->root);
    if (workspace->root_fd < 0) { (void)cog_workspace_free(workspace); return COG_ERROR_IO; }
    workspace->max_file = config->max_file_bytes;
    workspace->max_entries = config->max_search_entries;
    workspace->quantum = config->service_quantum;
    workspace->commands = config->enable_commands;
    *out = workspace;
    return COG_OK;
#endif
}

cog_result_t cog_workspace_commands_enabled(const cog_workspace_t *workspace, bool *enabled)
{
    if (workspace == NULL || enabled == NULL) { return COG_ERROR_NULL_PTR; }
    *enabled = workspace->commands;
    return COG_OK;
}

cog_result_t cog_workspace_free(cog_workspace_t *workspace)
{
    if (workspace == NULL) { return COG_OK; }
    if (workspace->borrowers != 0U) { return COG_ERROR_INVALID_STATE; }
    if (workspace->root_fd >= 0) { (void)close(workspace->root_fd); }
    free(workspace->environment);
    free(workspace->strings);
    free(workspace->file_workspace);
    free(workspace->edit_workspace);
    free(workspace->traversal_workspace);
    free(workspace);
    return COG_OK;
}

void cog_workspace_capture(workspace_execution_t *execution, const char *bytes, size_t length)
{
    size_t left = execution->config.max_output_bytes - execution->output.output_len;
    size_t copy = length < left ? length : left;
    memcpy(execution->capture + execution->output.output_len, bytes, copy);
    execution->output.output_len += copy;
    execution->capture[execution->output.output_len] = '\0';
    if (copy != length) { execution->output.truncated = true; }
}

/* Keep the public result a valid UTF-8 C string, including at the capture edge.
   Binary/invalid process bytes are represented by '?'; no hidden codec storage. */
static void sanitize_capture(workspace_execution_t *execution)
{
    unsigned char *text = (unsigned char *)execution->capture;
    size_t length = execution->output.output_len;
    size_t i = 0U;
    while (i < length) {
        unsigned char ch = text[i];
        if (ch != 0U && ch < 0x80U) { i++; continue; }
        size_t count = ch >= 0xC2U && ch <= 0xDFU ? 2U :
                       ch >= 0xE0U && ch <= 0xEFU ? 3U :
                       ch >= 0xF0U && ch <= 0xF4U ? 4U : 0U;
        if (count != 0U && count > length - i && execution->output.truncated) {
            execution->output.output_len = i; text[i] = 0U; break;
        }
        bool valid = count != 0U && count <= length - i;
        for (size_t j = 1U; valid && j < count; j++) { valid = (text[i + j] & 0xC0U) == 0x80U; }
        if (valid && count == 3U) { valid = !(ch == 0xE0U && text[i + 1U] < 0xA0U) && !(ch == 0xEDU && text[i + 1U] >= 0xA0U); }
        if (valid && count == 4U) { valid = !(ch == 0xF0U && text[i + 1U] < 0x90U) && !(ch == 0xF4U && text[i + 1U] >= 0x90U); }
        if (valid) { i += count; } else { text[i++] = '?'; }
    }
}

void cog_workspace_finish(workspace_execution_t *execution, cog_result_t status)
{
    if (execution->fd >= 0) { (void)close(execution->fd); execution->fd = -1; }
    if (execution->temporary_fd >= 0) { (void)close(execution->temporary_fd); execution->temporary_fd = -1; }
    if (execution->temporary[0] != '\0' && execution->parent_fd >= 0) {
        if (unlinkat(execution->parent_fd, execution->temporary, 0) != 0) { status = COG_ERROR_IO; }
        execution->temporary[0] = '\0';
    }
    if (execution->parent_fd >= 0) { (void)close(execution->parent_fd); execution->parent_fd = -1; }
    for (size_t i = 0U; i < execution->directory_count; i++) {
        if (execution->directories[i].fd >= 0) { (void)close(execution->directories[i].fd); execution->directories[i].fd = -1; }
    }
    execution->output.status = status;
    execution->done = true;
    sanitize_capture(execution);
}

static cog_result_t workspace_destroy(void *handle)
{
    workspace_execution_t *execution = handle;
    if (execution == NULL) { return COG_OK; }
    if (execution->started && !execution->done) { return COG_ERROR_INVALID_STATE; }
    cog_workspace_finish(execution, execution->output.status);
    if (execution->control_fd >= 0) { (void)close(execution->control_fd); }
    if (execution->output_fd >= 0) { (void)close(execution->output_fd); }
    if (execution->report_fd >= 0) { (void)close(execution->report_fd); }
    cog_toolkit_json_free(&execution->json);
    execution->workspace->borrowers--;
    free(execution->capture);
    if (execution->borrowed_scratch) { execution->workspace->scratch_borrowed = false; }
    else { free(execution->file); free(execution->replacement); free(execution->directories); }
    free(execution);
    return COG_OK;
}

static const char *string_argument(yyjson_val *root, const char *name, size_t *length)
{
    yyjson_val *value = yyjson_obj_get(root, name);
    if (value == NULL) { *length = 0U; return NULL; }
    *length = yyjson_get_len(value);
    return yyjson_get_str(value);
}

static cog_result_t workspace_new(cog_core_t *core, const char *request, size_t request_len,
    const cog_tool_async_config_t *config, void **out_handle, workspace_operation_t operation)
{
    (void)core;
    if (out_handle == NULL) { return COG_ERROR_NULL_PTR; }
    *out_handle = NULL;
    if (core == NULL || request == NULL || config == NULL || config->user == NULL) { return COG_ERROR_NULL_PTR; }
    cog_workspace_t *workspace = config->user;
    if (config->max_output_bytes == 0U || config->max_output_bytes == SIZE_MAX || config->timeout_ms == 0U) { return COG_ERROR_INVALID_ARG; }
    if (operation == WORKSPACE_COMMAND && !workspace->commands) { return COG_ERROR_UNSUPPORTED; }
    if (workspace->borrowers == SIZE_MAX) { return COG_ERROR_LIMIT; }
    workspace_execution_t *execution = calloc(1U, sizeof(*execution));
    if (execution == NULL) { return COG_ERROR_NOMEM; }
    execution->workspace = workspace;
    workspace->borrowers++;
    execution->operation = operation;
    execution->helper_pidfd = -1;
    execution->config = *config;
    execution->fd = -1; execution->parent_fd = -1; execution->temporary_fd = -1;
    execution->control_fd = -1; execution->output_fd = -1; execution->report_fd = -1;
    cog_result_t r = cog_toolkit_json_new(&execution->json, request, request_len,
        cog_workspace_tools.tools[(size_t)operation].input_schema, config);
    if (r != COG_OK) { (void)workspace_destroy(execution); return r; }
    yyjson_val *root = yyjson_doc_get_root(execution->json.arguments);
    size_t length;
    execution->path = string_argument(root, operation == WORKSPACE_COMMAND ? "cwd" : "path", &length);
    if (execution->path == NULL) { execution->path = "."; length = 1U; }
    if (!valid_path(execution->path, length)) { (void)workspace_destroy(execution); return COG_ERROR_INVALID_ARG; }
    execution->pattern = string_argument(root, "pattern", &execution->pattern_len);
    execution->old_text = string_argument(root, "old", &execution->old_len);
    execution->new_text = string_argument(root, "new", &execution->new_len);
    execution->command = string_argument(root, "command", &length);
    if ((execution->command != NULL && memchr(execution->command, '\0', length) != NULL) ||
        (execution->old_text != NULL && memchr(execution->old_text, '\0', execution->old_len) != NULL) ||
        (execution->new_text != NULL && memchr(execution->new_text, '\0', execution->new_len) != NULL) ||
        (execution->pattern != NULL && memchr(execution->pattern, '\0', execution->pattern_len) != NULL)) {
        (void)workspace_destroy(execution); return COG_ERROR_INVALID_ARG;
    }
    execution->capture = calloc(config->max_output_bytes + 1U, 1U);
    execution->output.output = execution->capture;
    if (operation != WORKSPACE_COMMAND && !workspace->scratch_borrowed) {
        workspace->scratch_borrowed = true;
        execution->borrowed_scratch = true;
        execution->file = workspace->file_workspace;
        execution->replacement = workspace->edit_workspace;
        execution->directories = workspace->traversal_workspace;
    } else if (operation != WORKSPACE_COMMAND) {
        execution->file = malloc(workspace->max_file + 1U);
        if (operation == WORKSPACE_EDIT) { execution->replacement = malloc(workspace->max_file + 1U); }
        if (operation == WORKSPACE_SEARCH) {
            execution->directories = calloc(workspace->max_entries, sizeof(*execution->directories));
        }
    }
    if (execution->capture == NULL ||
        (operation != WORKSPACE_COMMAND && execution->file == NULL) ||
        (operation == WORKSPACE_EDIT && execution->replacement == NULL) ||
        (operation == WORKSPACE_SEARCH && execution->directories == NULL)) {
        (void)workspace_destroy(execution); return COG_ERROR_NOMEM;
    }
    *out_handle = execution;
    return COG_OK;
}

#define WORKSPACE_NEW(name, kind) \
static cog_result_t name(cog_core_t *core, const char *request, size_t length, \
    const cog_tool_async_config_t *config, void **out) \
{ return workspace_new(core, request, length, config, out, kind); }
WORKSPACE_NEW(read_new, WORKSPACE_READ)
WORKSPACE_NEW(search_new, WORKSPACE_SEARCH)
WORKSPACE_NEW(edit_new, WORKSPACE_EDIT)
WORKSPACE_NEW(command_new, WORKSPACE_COMMAND)

static cog_result_t background_new(cog_core_t *core, const char *request, size_t length,
    const cog_tool_async_config_t *config, void **out)
{
    cog_result_t r = command_new(core, request, length, config, out);
    if (r == COG_OK) {
        workspace_execution_t *execution = *out;
        execution->background = true;
    }
    return r;
}

static cog_result_t clock_update(workspace_execution_t *execution, uint64_t now)
{
    if (execution->clock_set && now < execution->now) { return COG_ERROR_INVALID_ARG; }
    execution->now = now; execution->clock_set = true;
    return COG_OK;
}

static cog_result_t workspace_start(void *handle, uint64_t now)
{
    workspace_execution_t *execution = handle;
    if (execution == NULL) { return COG_ERROR_NULL_PTR; }
    if (execution->started || execution->done) { return COG_ERROR_INVALID_STATE; }
    if (UINT64_MAX - now < execution->config.timeout_ms) { return COG_ERROR_INVALID_ARG; }
    cog_result_t r = clock_update(execution, now);
    if (r != COG_OK) { return r; }
    execution->deadline = now + execution->config.timeout_ms;
    execution->started = true;
    if (execution->operation == WORKSPACE_COMMAND) {
        r = cog_workspace_process_start(execution);
    } else if (execution->operation == WORKSPACE_EDIT) {
        execution->parent_fd = cog_workspace_open_parent(execution->workspace, execution->path, execution->leaf);
        if (execution->parent_fd < 0) { r = path_error(); }
        else {
            execution->fd = openat(execution->parent_fd, execution->leaf, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
            if (execution->fd < 0) { r = path_error(); }
        }
    } else {
        execution->fd = cog_workspace_open_path(execution->workspace, execution->path, O_RDONLY);
        if (execution->fd < 0) { r = path_error(); }
    }
    if (r == COG_OK && execution->operation != WORKSPACE_COMMAND) {
        struct stat st;
        if (fstat(execution->fd, &st) != 0) { r = COG_ERROR_IO; }
        else if (execution->operation == WORKSPACE_SEARCH && S_ISDIR(st.st_mode)) {
            execution->directories[0].fd = execution->fd;
            memcpy(execution->directories[0].path, execution->path, strlen(execution->path) + 1U);
            execution->directory_count = 1U;
            execution->fd = -1;
        } else if (!S_ISREG(st.st_mode)) { r = COG_ERROR_INVALID_ARG; }
        else if (st.st_size < 0 || (uint64_t)st.st_size > execution->workspace->max_file) { r = COG_ERROR_LIMIT; }
        else {
            execution->mode = st.st_mode;
            execution->file_device = st.st_dev; execution->file_inode = st.st_ino; execution->file_size = st.st_size;
            execution->file_mtime_seconds = (int64_t)st.st_mtim.tv_sec;
            execution->file_mtime_nanoseconds = st.st_mtim.tv_nsec;
            execution->file_ctime_seconds = (int64_t)st.st_ctim.tv_sec;
            execution->file_ctime_nanoseconds = st.st_ctim.tv_nsec;
            memcpy(execution->current_path, execution->path, strlen(execution->path) + 1U);
        }
    }
    if (r != COG_OK) {
        cog_workspace_finish(execution, r);
        if (execution->background) { return r; }
    }
    return COG_OK;
}

static cog_result_t prepare_edit(workspace_execution_t *execution)
{
    if (memchr(execution->file, '\0', execution->file_len) != NULL) { return COG_ERROR_INVALID_ARG; }
    size_t found = SIZE_MAX;
    for (size_t i = 0U; i <= execution->file_len; i++) {
        if (execution->old_len <= execution->file_len - i &&
            memcmp(execution->file + i, execution->old_text, execution->old_len) == 0) {
            if (found != SIZE_MAX) { return COG_ERROR_INVALID_ARG; }
            found = i;
        }
    }
    if (found == SIZE_MAX) { return COG_ERROR_NOT_FOUND; }
    if (execution->new_len > execution->workspace->max_file - (execution->file_len - execution->old_len)) { return COG_ERROR_LIMIT; }
    execution->replacement_len = execution->file_len - execution->old_len + execution->new_len;
    memcpy(execution->replacement, execution->file, found);
    memcpy(execution->replacement + found, execution->new_text, execution->new_len);
    memcpy(execution->replacement + found + execution->new_len,
        execution->file + found + execution->old_len, execution->file_len - found - execution->old_len);
    if (execution->workspace->temp_serial == SIZE_MAX) { return COG_ERROR_LIMIT; }
    size_t serial = execution->workspace->temp_serial++;
    int count = snprintf(execution->temporary, sizeof(execution->temporary), ".cog-edit-%ld-%zu", (long)getpid(), serial);
    if (count < 0 || (size_t)count >= sizeof(execution->temporary)) { return COG_ERROR_LIMIT; }
    execution->temporary_fd = openat(execution->parent_fd, execution->temporary,
        O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (execution->temporary_fd < 0) {
        /* Never unlink a colliding name we did not create. */
        execution->temporary[0] = '\0';
        return COG_ERROR_IO;
    }
    execution->output.side_effects_possible = true;
    if (fchmod(execution->temporary_fd, execution->mode & (mode_t)0777) != 0) { return COG_ERROR_IO; }
    execution->phase = 1U;
    return COG_OK;
}

static void search_file(workspace_execution_t *execution)
{
    size_t line = 1U;
    size_t start = 0U;
    for (size_t i = 0U; i <= execution->file_len; i++) {
        if (i == execution->file_len || execution->file[i] == '\n') {
            bool found = false;
            for (size_t j = start; execution->pattern_len <= i - j; j++) {
                if (memcmp(execution->file + j, execution->pattern, execution->pattern_len) == 0) { found = true; break; }
            }
            if (found) {
                char number[48];
                int n = snprintf(number, sizeof(number), ":%zu:", line);
                cog_workspace_capture(execution, execution->current_path, strlen(execution->current_path));
                if (n > 0 && (size_t)n < sizeof(number)) { cog_workspace_capture(execution, number, (size_t)n); }
                cog_workspace_capture(execution, execution->file + start, i - start);
                cog_workspace_capture(execution, "\n", 1U);
            }
            start = i + 1U; line++;
        }
    }
}

struct workspace_dirent {
    uint64_t inode;
    int64_t offset;
    unsigned short record_length;
    unsigned char type;
    char name[];
};

static cog_result_t search_next(workspace_execution_t *execution)
{
    if (execution->directory_index == execution->directory_count) { cog_workspace_finish(execution, COG_OK); return COG_OK; }
    workspace_directory_t *directory = &execution->directories[execution->directory_index];
    if (execution->directory_offset == execution->directory_bytes) {
        long bytes = syscall(SYS_getdents64, directory->fd, execution->directory_buffer, sizeof(execution->directory_buffer));
        if (bytes < 0) { return errno == EINTR ? COG_OK : COG_ERROR_IO; }
        if (bytes == 0) {
            (void)close(directory->fd); directory->fd = -1;
            execution->directory_index++;
            execution->directory_offset = 0U; execution->directory_bytes = 0U;
            return COG_OK;
        }
        execution->directory_offset = 0U; execution->directory_bytes = (size_t)bytes;
    }
    /* memcpy avoids unaligned access to the Linux getdents64 records. */
    size_t remaining = execution->directory_bytes - execution->directory_offset;
    size_t base = offsetof(struct workspace_dirent, name);
    unsigned short record;
    if (remaining < base + 1U) { return COG_ERROR_IO; }
    memcpy(&record, execution->directory_buffer + execution->directory_offset + offsetof(struct workspace_dirent, record_length), sizeof(record));
    if ((size_t)record < base + 1U || (size_t)record > remaining) { return COG_ERROR_IO; }
    const char *name = (const char *)execution->directory_buffer + execution->directory_offset + base;
    if (memchr(name, '\0', (size_t)record - base) == NULL) { return COG_ERROR_IO; }
    execution->directory_offset += record;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) { return COG_OK; }
    if (++execution->examined > execution->workspace->max_entries) { return COG_ERROR_LIMIT; }
    int n = snprintf(execution->current_path, sizeof(execution->current_path), "%s/%s", directory->path, name);
    if (n < 0 || (size_t)n >= sizeof(execution->current_path)) { return COG_ERROR_LIMIT; }
    int fd = openat(directory->fd, name, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) { return path_error(); }
    struct stat st;
    if (fstat(fd, &st) != 0) { (void)close(fd); return COG_ERROR_IO; }
    if (S_ISDIR(st.st_mode)) {
        if (execution->directory_count == execution->workspace->max_entries) { (void)close(fd); return COG_ERROR_LIMIT; }
        workspace_directory_t *next = &execution->directories[execution->directory_count++];
        next->fd = fd;
        memcpy(next->path, execution->current_path, (size_t)n + 1U);
    } else if (S_ISREG(st.st_mode)) {
        if (st.st_size < 0 || (uint64_t)st.st_size > execution->workspace->max_file) { (void)close(fd); return COG_ERROR_LIMIT; }
        execution->fd = fd; execution->file_len = 0U;
    } else { (void)close(fd); return COG_ERROR_INVALID_ARG; }
    return COG_OK;
}

static cog_result_t file_step(workspace_execution_t *execution)
{
    if (execution->operation == WORKSPACE_EDIT && execution->phase == 1U) {
        size_t left = execution->replacement_len - execution->write_offset;
        size_t count = left < COG_WORKSPACE_IO_BYTES ? left : COG_WORKSPACE_IO_BYTES;
        if (count != 0U) {
            ssize_t written = write(execution->temporary_fd, execution->replacement + execution->write_offset, count);
            if (written < 0) { return errno == EINTR ? COG_OK : COG_ERROR_IO; }
            if (written == 0) { return COG_ERROR_IO; }
            execution->write_offset += (size_t)written;
            return COG_OK;
        }
        if (fsync(execution->temporary_fd) != 0) { return COG_ERROR_IO; }
        /* Refuse a final symlink/different target inserted since the read. */
        struct stat current;
        if (fstatat(execution->parent_fd, execution->leaf, &current, AT_SYMLINK_NOFOLLOW) != 0) { return COG_ERROR_IO; }
        if (!S_ISREG(current.st_mode) || current.st_ino != execution->file_inode || current.st_dev != execution->file_device ||
            current.st_size != execution->file_size || (int64_t)current.st_mtim.tv_sec != execution->file_mtime_seconds ||
            current.st_mtim.tv_nsec != execution->file_mtime_nanoseconds ||
            (int64_t)current.st_ctim.tv_sec != execution->file_ctime_seconds || current.st_ctim.tv_nsec != execution->file_ctime_nanoseconds) { return COG_ERROR_INVALID_STATE; }
        if (renameat(execution->parent_fd, execution->temporary, execution->parent_fd, execution->leaf) != 0) { return COG_ERROR_IO; }
        execution->temporary[0] = '\0';
        if (fsync(execution->parent_fd) != 0) { return COG_ERROR_INDETERMINATE; }
        cog_workspace_capture(execution, "edited\n", 7U);
        cog_workspace_finish(execution, COG_OK);
        return COG_OK;
    }
    if (execution->fd < 0) { return search_next(execution); }
    size_t left = execution->workspace->max_file - execution->file_len;
    size_t count = left < COG_WORKSPACE_IO_BYTES ? left + 1U : COG_WORKSPACE_IO_BYTES;
    ssize_t bytes = read(execution->fd, execution->file + execution->file_len, count);
    if (bytes < 0) { return errno == EINTR ? COG_OK : COG_ERROR_IO; }
    if ((size_t)bytes > left) { return COG_ERROR_LIMIT; }
    execution->file_len += (size_t)bytes;
    if (bytes != 0) { return COG_OK; }
    execution->file[execution->file_len] = '\0';
    if (execution->operation == WORKSPACE_READ) {
        cog_workspace_capture(execution, execution->file, execution->file_len);
        cog_workspace_finish(execution, COG_OK);
    } else if (execution->operation == WORKSPACE_EDIT) { return prepare_edit(execution); }
    else {
        search_file(execution);
        (void)close(execution->fd); execution->fd = -1;
        if (execution->directory_count == 0U) { cog_workspace_finish(execution, COG_OK); }
    }
    return COG_OK;
}

static cog_result_t workspace_cancel(void *handle, uint64_t now)
{
    workspace_execution_t *execution = handle;
    if (execution == NULL) { return COG_ERROR_NULL_PTR; }
    cog_result_t r = clock_update(execution, now);
    if (r != COG_OK || execution->done) { return r; }
    if (!execution->started) { cog_workspace_finish(execution, COG_ERROR_ABORTED); return COG_OK; }
    if (execution->operation != WORKSPACE_COMMAND) { cog_workspace_finish(execution, COG_ERROR_ABORTED); return COG_OK; }
    if (!execution->cancelling) {
        execution->output.status = COG_ERROR_ABORTED;
        return cog_workspace_process_cancel(execution);
    }
    return COG_OK;
}

static cog_result_t workspace_poll(void *handle, uint64_t now, bool *done, uint64_t *deadline)
{
    workspace_execution_t *execution = handle;
    if (execution == NULL || done == NULL || deadline == NULL) { return COG_ERROR_NULL_PTR; }
    if (!execution->started && !execution->done) { return COG_ERROR_INVALID_STATE; }
    cog_result_t r = clock_update(execution, now);
    if (r != COG_OK) { return r; }
    if (!execution->done && now >= execution->deadline && !execution->cancelling) {
        if (execution->operation == WORKSPACE_COMMAND) {
            execution->output.status = COG_ERROR_TIMEOUT;
            r = cog_workspace_process_cancel(execution);
        } else { cog_workspace_finish(execution, COG_ERROR_TIMEOUT); }
    }
    if (!execution->done && r == COG_OK) {
        if (execution->operation == WORKSPACE_COMMAND) { r = cog_workspace_process_poll(execution); }
        else {
            for (size_t i = 0U; i < execution->workspace->quantum && !execution->done; i++) {
                r = file_step(execution);
                if (r != COG_OK) { cog_workspace_finish(execution, r); break; }
            }
            r = COG_OK; /* Execution failures are retained outcomes. */
        }
    }
    *done = execution->done;
    *deadline = execution->done ? UINT64_MAX : execution->cancelling ? execution->kill_deadline : execution->deadline;
    return r;
}

static cog_result_t workspace_result(const void *handle, cog_tool_output_t *out)
{
    const workspace_execution_t *execution = handle;
    if (execution == NULL || out == NULL) { return COG_ERROR_NULL_PTR; }
    if (!execution->done) { return COG_ERROR_AGAIN; }
    *out = execution->output;
    return COG_OK;
}

#define WORKSPACE_OPS(name, constructor) \
static const cog_tool_async_vtable_t name = { constructor, workspace_start, workspace_poll, workspace_cancel, workspace_result, workspace_destroy }
WORKSPACE_OPS(read_ops, read_new);
WORKSPACE_OPS(search_ops, search_new);
WORKSPACE_OPS(edit_ops, edit_new);
WORKSPACE_OPS(command_ops, command_new);
WORKSPACE_OPS(background_ops, background_new);
#define OBJECT_BEGIN "{\"type\":\"object\",\"additionalProperties\":false,\"properties\":{"
#define STRING_SCHEMA "{\"type\":\"string\"}"
#define PATH_SCHEMA "{\"type\":\"string\",\"minLength\":1,\"maxLength\":4095}"
static const char command_schema[] = OBJECT_BEGIN "\"command\":{\"type\":\"string\",\"minLength\":1},\"cwd\":" PATH_SCHEMA "},\"required\":[\"command\"]}";
static const cog_tool_t workspace_descriptors[] = {
    { .name = "read_file", .description = "Read bounded text from a workspace file.",
      .input_schema = OBJECT_BEGIN "\"path\":" PATH_SCHEMA "},\"required\":[\"path\"]}", .async = &read_ops },
    { .name = "search", .description = "Search literal text in bounded workspace traversal.",
      .input_schema = OBJECT_BEGIN "\"pattern\":{\"type\":\"string\",\"minLength\":1},\"path\":" PATH_SCHEMA "},\"required\":[\"pattern\"]}", .async = &search_ops },
    { .name = "edit_file", .description = "Atomically replace exactly one literal match.",
      .input_schema = OBJECT_BEGIN "\"path\":" PATH_SCHEMA ",\"old\":{\"type\":\"string\",\"minLength\":1},\"new\":" STRING_SCHEMA "},\"required\":[\"path\",\"old\",\"new\"]}",
      .async = &edit_ops, .flags = COG_TOOL_SIDE_EFFECTING },
    { .name = "run_command", .description = "Run an explicitly allowed shell command; this is not OS confinement.",
      .input_schema = command_schema,
      .async = &command_ops, .flags = COG_TOOL_SIDE_EFFECTING }
};
const cog_tool_table_t cog_workspace_tools = { workspace_descriptors, sizeof(workspace_descriptors) / sizeof(workspace_descriptors[0]) };
const cog_tool_t cog_workspace_start_background_tool = {
    .name = "start_background",
    .description = "Start a workspace command in the background and return its job ID.",
    .input_schema = command_schema, .async = &background_ops, .flags = COG_TOOL_SIDE_EFFECTING
};

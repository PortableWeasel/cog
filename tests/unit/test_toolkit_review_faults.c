/** @file test_toolkit_review_faults.c @brief Independent I.4 local failure regressions. */
#define _POSIX_C_SOURCE 200809L
#include <cog/toolkit.h>
#include "fixtures/toolkit_core.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

ssize_t __real_send(int fd, const void *bytes, size_t length, int flags);
ssize_t __wrap_send(int fd, const void *bytes, size_t length, int flags);
int __real_fsync(int fd);
int __wrap_fsync(int fd);
static bool fail_term;
static bool fail_sync;
static size_t term_attempts;
ssize_t __wrap_send(int fd, const void *bytes, size_t length, int flags)
{
    if (length == 1U && *(const char *)bytes == 'T') {
        term_attempts++;
        if (fail_term) { fail_term = false; errno = EINTR; return -1; }
    }
    return __real_send(fd, bytes, length, flags);
}
int __wrap_fsync(int fd)
{
    if (fail_sync) { fail_sync = false; errno = EIO; return -1; }
    return __real_fsync(fd);
}
static uint64_t now_ms(void)
{
    struct timespec t;
    assert(clock_gettime(CLOCK_MONOTONIC, &t) == 0);
    return (uint64_t)t.tv_sec * 1000U + (uint64_t)t.tv_nsec / 1000000U;
}
static void pause_poll(void)
{
    const struct timespec delay = {0, 1000000};
    (void)nanosleep(&delay, NULL);
}
static cog_toolkit_execution_t *prepare(cog_workspace_t *w, const char *name, const char *args)
{
    cog_tool_async_config_t cfg = {.max_output_bytes=256U, .timeout_ms=3000U,
        .terminate_grace_ms=150U, .user=w};
    cog_toolkit_execution_t *e = NULL;
    assert(cog_toolkit_execution_new(&e, toolkit_test_core, &cog_workspace_tools,
        "review", name, args, strlen(args), &cfg) == COG_OK);
    assert(cog_toolkit_execution_start(e, COG_TOOLKIT_POLICY_ALLOW, now_ms()) == COG_OK);
    return e;
}
static cog_toolkit_result_t finish(cog_toolkit_execution_t *e)
{
    bool done = false;
    uint64_t limit = now_ms() + 5000U;
    while (!done) {
        uint64_t deadline;
        assert(cog_toolkit_execution_poll(e, now_ms(), &done, &deadline) == COG_OK);
        assert(now_ms() < limit);
        if (!done) { pause_poll(); }
    }
    char output[257];
    size_t required;
    cog_toolkit_result_t r;
    assert(cog_toolkit_execution_result(e, &r, output, sizeof(output), &required) == COG_OK);
    return r;
}
int main(void)
{
    char root[] = "/tmp/cog-i-review-XXXXXX";
    assert(mkdtemp(root) != NULL);
    int dir = open(root, O_RDONLY | O_DIRECTORY);
    assert(dir >= 0);
    int fd = openat(dir, "file", O_WRONLY | O_CREAT | O_EXCL, 0600);
    assert(fd >= 0 && write(fd, "original", 8U) == 8 && close(fd) == 0);
    toolkit_core_create();
    cog_workspace_config_t cfg = {.root=root, .max_file_bytes=4096U,
        .max_search_entries=16U, .service_quantum=1U, .enable_commands=true};
    cog_workspace_t *w = NULL;
    assert(cog_workspace_new(&w, &cfg) == COG_OK);
    cog_toolkit_execution_t *e = prepare(w, "edit_file",
        "{\"path\":\"file\",\"old\":\"original\",\"new\":\"replacement\"}");
    fail_sync = true;
    cog_toolkit_result_t r = finish(e);
    assert(r.status == COG_ERROR_IO && r.side_effects_possible);
    assert(cog_toolkit_execution_free(e) == COG_OK);
    fd = openat(dir, "file", O_RDONLY);
    char original[9] = {0};
    assert(fd >= 0 && read(fd, original, 8U) == 8 && close(fd) == 0);
    assert(strcmp(original, "original") == 0);
    char temporary[96];
    int n = snprintf(temporary, sizeof(temporary), ".cog-edit-%ld-0", (long)getpid());
    assert(n > 0 && (size_t)n < sizeof(temporary));
    assert(faccessat(dir, temporary, F_OK, 0) == -1 && errno == ENOENT);
    e = prepare(w, "run_command", "{\"command\":\"trap 'printf term > term; exit 0' TERM; printf ready > ready; while :; do sleep .01; done\"}");
    uint64_t ready_limit = now_ms() + 1500U;
    while (faccessat(dir, "ready", F_OK, 0) != 0) {
        assert(now_ms() < ready_limit);
        pause_poll();
    }
    fail_term = true;
    assert(cog_toolkit_execution_cancel(e, now_ms()) == COG_OK);
    r = finish(e);
    assert(r.status == COG_ERROR_ABORTED && r.side_effects_possible && r.exit_status_known);
    bool saw_term = faccessat(dir, "term", F_OK, 0) == 0;
    fprintf(stderr, "TERM attempts=%zu; TERM handler ran=%d; process exit=%d\n",
        term_attempts, (int)saw_term, r.exit_status);
    assert(cog_toolkit_execution_free(e) == COG_OK);
    assert(cog_workspace_free(w) == COG_OK);
    toolkit_core_destroy();
    assert(unlinkat(dir, "file", 0) == 0);
    assert(unlinkat(dir, "ready", 0) == 0);
    if (saw_term) { assert(unlinkat(dir, "term", 0) == 0); }
    assert(close(dir) == 0 && rmdir(root) == 0);
    if (term_attempts < 2U || !saw_term) {
        fputs("FAIL: transient TERM send must be retried before KILL grace expires\n", stderr);
        return 1;
    }
    return 0;
}

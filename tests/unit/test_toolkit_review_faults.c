/** @file test_toolkit_review_faults.c @brief Independent I.4 failure regressions. */
#define _POSIX_C_SOURCE 200809L
#include <cog/toolkit.h>
#include "fixtures/toolkit_core.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

ssize_t __real_send(int fd, const void *bytes, size_t length, int flags);
ssize_t __wrap_send(int fd, const void *bytes, size_t length, int flags);
int __real_fsync(int fd);
int __wrap_fsync(int fd);
static int term_error;
static int kill_error;
static size_t term_failures;
static size_t kill_failures;
static bool persistent_term_backpressure;
static bool fail_sync;
static size_t term_attempts;
static size_t kill_attempts;
static size_t term_accepted;
static size_t kill_accepted;
ssize_t __wrap_send(int fd, const void *bytes, size_t length, int flags)
{
    char command = length == 1U ? *(const char *)bytes : '\0';
    if (command == 'T') {
        term_attempts++;
        if (persistent_term_backpressure || term_failures != 0U) {
            if (term_failures != 0U) { term_failures--; }
            errno = term_error;
            return -1;
        }
    } else if (command == 'K') {
        kill_attempts++;
        if (kill_failures != 0U) { kill_failures--; errno = kill_error; return -1; }
    }
    ssize_t count = __real_send(fd, bytes, length, flags);
    if (count == 1) {
        if (command == 'T') { term_accepted++; }
        else if (command == 'K') { kill_accepted++; }
    }
    return count;
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
static cog_toolkit_execution_t *prepare(cog_workspace_t *workspace, const char *name,
    const char *arguments, uint32_t timeout, uint32_t grace)
{
    cog_tool_async_config_t config = { .max_output_bytes = 256U, .timeout_ms = timeout,
        .terminate_grace_ms = grace, .user = workspace };
    cog_toolkit_execution_t *execution = NULL;
    assert(cog_toolkit_execution_new(&execution, toolkit_test_core, &cog_workspace_tools,
        "review", name, arguments, strlen(arguments), &config) == COG_OK);
    assert(cog_toolkit_execution_start(execution, COG_TOOLKIT_POLICY_ALLOW, now_ms()) == COG_OK);
    return execution;
}
static cog_toolkit_result_t finish(cog_toolkit_execution_t *execution)
{
    bool done = false;
    uint64_t limit = now_ms() + 5000U;
    while (!done) {
        uint64_t deadline;
        size_t before = term_attempts + kill_attempts;
        assert(cog_toolkit_execution_poll(execution, now_ms(), &done, &deadline) == COG_OK);
        /* Transient errors must yield to the caller, not spin on send. */
        assert(term_attempts + kill_attempts - before <= 1U);
        assert(now_ms() < limit);
        if (!done) { pause_poll(); }
    }
    char output[257];
    size_t required;
    cog_toolkit_result_t result;
    assert(cog_toolkit_execution_result(execution, &result, output, sizeof(output), &required) == COG_OK);
    return result;
}
static void edit_fsync_failure(cog_workspace_t *workspace, int directory)
{
    cog_toolkit_execution_t *execution = prepare(workspace, "edit_file",
        "{\"path\":\"file\",\"old\":\"original\",\"new\":\"replacement\"}", 3000U, 150U);
    fail_sync = true;
    cog_toolkit_result_t result = finish(execution);
    assert(result.status == COG_ERROR_IO && result.side_effects_possible);
    assert(cog_toolkit_execution_free(execution) == COG_OK);
    int fd = openat(directory, "file", O_RDONLY);
    char original[9] = {0};
    assert(fd >= 0 && read(fd, original, 8U) == 8 && close(fd) == 0);
    assert(strcmp(original, "original") == 0);
    char temporary[96];
    int n = snprintf(temporary, sizeof(temporary), ".cog-edit-%ld-0", (long)getpid());
    assert(n > 0 && (size_t)n < sizeof(temporary));
    assert(faccessat(directory, temporary, F_OK, 0) == -1 && errno == ENOENT);
}
static long read_pid(int directory, const char *name)
{
    int fd = openat(directory, name, O_RDONLY);
    char text[64] = {0};
    assert(fd >= 0);
    ssize_t count = read(fd, text, sizeof(text) - 1U);
    assert(count > 0 && close(fd) == 0);
    long pid = strtol(text, NULL, 10);
    assert(pid > 1L);
    return pid;
}
static void cancellation_case(cog_workspace_t *workspace, int directory, int error,
    size_t failures, bool persistent, bool timeout, uint32_t grace)
{
    term_error = error;
    term_failures = failures;
    persistent_term_backpressure = persistent;
    kill_error = EAGAIN;
    kill_failures = persistent ? 2U : 0U;
    term_attempts = 0U; kill_attempts = 0U;
    term_accepted = 0U; kill_accepted = 0U;
    static const char command[] =
        "{\"command\":\"trap 'printf term > term; exit 0' TERM; "
        "printf start >> starts; printf '%s' $$ > shell; "
        "sleep 30 & printf '%s' $! > child; printf ready > ready; wait\"}";
    cog_toolkit_execution_t *execution = prepare(workspace, "run_command", command,
        timeout ? 80U : 3000U, grace);
    uint64_t ready_limit = now_ms() + 1500U;
    while (faccessat(directory, "ready", F_OK, 0) != 0) {
        assert(now_ms() < ready_limit);
        pause_poll();
    }
    long shell = read_pid(directory, "shell");
    long child = read_pid(directory, "child");
    if (!timeout) {
        assert(cog_toolkit_execution_cancel(execution, now_ms()) == COG_OK);
        assert(cog_toolkit_execution_cancel(execution, now_ms()) == COG_OK);
        /* Cancellation admission queues the command; it never busy-retries. */
        assert(term_attempts == 0U && kill_attempts == 0U);
    }
    cog_toolkit_result_t result = finish(execution);
    assert(result.status == (timeout ? COG_ERROR_TIMEOUT : COG_ERROR_ABORTED));
    assert(result.side_effects_possible && result.exit_status_known);
    bool saw_term = faccessat(directory, "term", F_OK, 0) == 0;
    if (persistent || grace == 0U) {
        assert(!saw_term && result.exit_status == 137 && term_accepted == 0U);
        assert(kill_accepted == 1U && kill_attempts >= (persistent ? 3U : 1U));
        if (grace == 0U) { assert(term_attempts == 0U); }
        else { assert(term_attempts >= 2U); }
    } else {
        assert(saw_term && result.exit_status == 0);
        assert(term_attempts >= failures + 1U && term_accepted == 1U);
        assert(kill_attempts == 0U && kill_accepted == 0U);
    }
    /* Copying the same terminal result cannot resubmit control or execute again. */
    size_t sends = term_attempts + kill_attempts;
    for (size_t i = 0U; i < 3U; i++) {
        char output[257];
        size_t required;
        cog_toolkit_result_t copy;
        assert(cog_toolkit_execution_result(execution, &copy, output, sizeof(output), &required) == COG_OK);
        assert(copy.status == result.status && copy.exit_status == result.exit_status);
    }
    assert(cog_toolkit_execution_cancel(execution, now_ms()) == COG_OK);
    assert(term_attempts + kill_attempts == sends);
    assert(cog_toolkit_execution_free(execution) == COG_OK);
    assert(kill((pid_t)shell, 0) == -1 && errno == ESRCH);
    assert(kill((pid_t)child, 0) == -1 && errno == ESRCH);
    int status;
    assert(waitpid(-1, &status, WNOHANG) == -1 && errno == ECHILD);
    int fd = openat(directory, "starts", O_RDONLY);
    char starts[16] = {0};
    assert(fd >= 0 && read(fd, starts, sizeof(starts)) == 5 && close(fd) == 0);
    assert(strcmp(starts, "start") == 0);
    fprintf(stderr, "cancel fault=%d/%zu persistent=%d timeout=%d grace=%u: TERM=%zu/%zu KILL=%zu/%zu exit=%d\n",
        error, failures, (int)persistent, (int)timeout, (unsigned)grace,
        term_accepted, term_attempts, kill_accepted, kill_attempts, result.exit_status);
    static const char *const files[] = { "ready", "shell", "child", "starts" };
    for (size_t i = 0U; i < sizeof(files) / sizeof(files[0]); i++) { assert(unlinkat(directory, files[i], 0) == 0); }
    if (saw_term) { assert(unlinkat(directory, "term", 0) == 0); }
    persistent_term_backpressure = false;
}
int main(void)
{
    char root[] = "/tmp/cog-i-review-XXXXXX";
    assert(mkdtemp(root) != NULL);
    int directory = open(root, O_RDONLY | O_DIRECTORY);
    assert(directory >= 0);
    int fd = openat(directory, "file", O_WRONLY | O_CREAT | O_EXCL, 0600);
    assert(fd >= 0 && write(fd, "original", 8U) == 8 && close(fd) == 0);
    toolkit_core_create();
    cog_workspace_config_t config = { .root = root, .max_file_bytes = 4096U,
        .max_search_entries = 16U, .service_quantum = 1U, .enable_commands = true };
    cog_workspace_t *workspace = NULL;
    assert(cog_workspace_new(&workspace, &config) == COG_OK);
    edit_fsync_failure(workspace, directory);
    cancellation_case(workspace, directory, EINTR, 1U, false, false, 150U);
    cancellation_case(workspace, directory, EAGAIN, 1U, false, false, 150U);
    cancellation_case(workspace, directory, EINTR, 3U, false, false, 150U);
    cancellation_case(workspace, directory, EAGAIN, 3U, false, true, 150U);
    cancellation_case(workspace, directory, EAGAIN, 0U, true, false, 50U);
    cancellation_case(workspace, directory, EAGAIN, 0U, true, true, 50U);
    cancellation_case(workspace, directory, EAGAIN, 0U, false, false, 0U);
    assert(cog_workspace_free(workspace) == COG_OK);
    toolkit_core_destroy();
    assert(unlinkat(directory, "file", 0) == 0);
    assert(close(directory) == 0 && rmdir(root) == 0);
    return 0;
}

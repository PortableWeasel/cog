/** @file test_workspace.c @brief Disposable workspace/process contract tests. */
#define _XOPEN_SOURCE 700
#include <cog/toolkit.h>
#include "fixtures/toolkit_core.h"
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static uint64_t monotonic_ms(void)
{
    struct timespec ts;
    assert(clock_gettime(CLOCK_MONOTONIC, &ts) == 0);
    return (uint64_t)ts.tv_sec * 1000U + (uint64_t)ts.tv_nsec / 1000000U;
}
static void write_fixture(const char *root, const char *name, const char *text)
{
    char path[4096];
    int n = snprintf(path, sizeof(path), "%s/%s", root, name);
    assert(n > 0 && (size_t)n < sizeof(path));
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    assert(fd >= 0);
    size_t length = strlen(text);
    assert(write(fd, text, length) == (ssize_t)length);
    assert(close(fd) == 0);
}
static cog_toolkit_execution_t *prepare(cog_workspace_t *workspace, const char *name,
    const char *arguments, size_t max_output, uint32_t timeout)
{
    cog_toolkit_execution_t *execution = NULL;
    cog_tool_async_config_t config = { .max_output_bytes = max_output, .timeout_ms = timeout,
        .terminate_grace_ms = 20U, .user = workspace };
    static const cog_tool_table_t background = { &cog_workspace_start_background_tool, 1U };
    const cog_tool_table_t *table = strcmp(name, "start_background") == 0 ? &background : &cog_workspace_tools;
    cog_result_t r = cog_toolkit_execution_new(&execution, toolkit_test_core, table,
        "workspace-call", name, arguments, strlen(arguments), &config);
    if (r != COG_OK) { fprintf(stderr, "prepare %s failed %d: %s\n", name, (int)r, arguments); abort(); }
    return execution;
}
static cog_toolkit_result_t finish(cog_toolkit_execution_t *execution, char *output, size_t capacity)
{
    uint64_t ceiling = monotonic_ms() + 5000U;
    bool done = false;
    while (!done) {
        uint64_t deadline;
        assert(cog_toolkit_execution_poll(execution, monotonic_ms(), &done, &deadline) == COG_OK);
        assert(monotonic_ms() < ceiling);
        if (!done) {
            struct timespec delay = {0, 1000000};
            (void)nanosleep(&delay, NULL);
        }
    }
    cog_toolkit_result_t result = {0};
    size_t required;
    assert(cog_toolkit_execution_result(execution, &result, output, capacity, &required) == COG_OK);
    assert(required == result.output_len + 1U);
    return result;
}
static cog_toolkit_result_t run(cog_workspace_t *workspace, const char *name,
    const char *arguments, char *output, size_t capacity)
{
    cog_toolkit_execution_t *execution = prepare(workspace, name, arguments, capacity - 1U, 1000U);
    assert(cog_toolkit_execution_start(execution, COG_TOOLKIT_POLICY_ALLOW, monotonic_ms()) == COG_OK);
    cog_toolkit_result_t result = finish(execution, output, capacity);
    assert(cog_toolkit_execution_free(execution) == COG_OK);
    return result;
}
static int remove_fixture(const char *path, const struct stat *st, int type, struct FTW *state)
{
    (void)st; (void)type; (void)state;
    return remove(path);
}
static void file_cases(cog_workspace_t *workspace, const char *root)
{
    char output[4096];
    cog_toolkit_result_t result = run(workspace, "read_file", "{\"path\":\"file.txt\"}", output, sizeof(output));
    assert(result.status == COG_OK && strcmp(output, "alpha\nbeta\n") == 0 && !result.side_effects_possible);
    cog_toolkit_execution_t *execution = prepare(workspace, "edit_file", "{\"path\":\"file.txt\",\"old\":\"alpha\",\"new\":\"changed\"}", 64U, 1000U);
    assert(cog_workspace_free(workspace) == COG_ERROR_INVALID_STATE);
    assert(cog_toolkit_execution_start(execution, COG_TOOLKIT_POLICY_UNDECIDED, monotonic_ms()) == COG_ERROR_AGAIN);
    assert(cog_toolkit_execution_start(execution, COG_TOOLKIT_POLICY_DENY, monotonic_ms()) == COG_OK);
    result = finish(execution, output, sizeof(output));
    assert(result.status == COG_ERROR_DENIED && !result.side_effects_possible);
    assert(cog_toolkit_execution_free(execution) == COG_OK);
    result = run(workspace, "read_file", "{\"path\":\"file.txt\"}", output, sizeof(output));
    assert(result.status == COG_OK && strcmp(output, "alpha\nbeta\n") == 0);
    execution = prepare(workspace, "edit_file", "{\"path\":\"file.txt\",\"old\":\"alpha\",\"new\":\"changed\"}", 64U, 1000U);
    assert(cog_toolkit_execution_start(execution, COG_TOOLKIT_POLICY_ALLOW, monotonic_ms()) == COG_OK);
    result = finish(execution, output, sizeof(output));
    assert(result.status == COG_OK && result.side_effects_possible && strcmp(output, "edited\n") == 0);
    strcpy(output, "preserved");
    size_t required;
    assert(cog_toolkit_execution_result(execution, &result, output, 2U, &required) == COG_ERROR_OVERFLOW);
    assert(strcmp(output, "preserved") == 0 && required == 8U);
    assert(cog_toolkit_execution_result(execution, &result, output, sizeof(output), &required) == COG_OK);
    assert(cog_toolkit_execution_start(execution, COG_TOOLKIT_POLICY_ALLOW, monotonic_ms()) == COG_ERROR_INVALID_STATE);
    assert(cog_toolkit_execution_free(execution) == COG_OK);
    result = run(workspace, "read_file", "{\"path\":\"file.txt\"}", output, sizeof(output));
    assert(result.status == COG_OK && strcmp(output, "changed\nbeta\n") == 0);
    result = run(workspace, "edit_file", "{\"path\":\"file.txt\",\"old\":\"missing\",\"new\":\"x\"}", output, sizeof(output));
    assert(result.status == COG_ERROR_NOT_FOUND && !result.side_effects_possible);
    write_fixture(root, "repeated.txt", "aa aa");
    result = run(workspace, "edit_file", "{\"path\":\"repeated.txt\",\"old\":\"aa\",\"new\":\"x\"}", output, sizeof(output));
    assert(result.status == COG_ERROR_INVALID_ARG && !result.side_effects_possible);
    result = run(workspace, "search", "{\"path\":\".\",\"pattern\":\"beta\"}", output, sizeof(output));
    assert(result.status == COG_OK && strstr(output, "file.txt:2:beta") != NULL);
    execution = prepare(workspace, "read_file", "{\"path\":\"file.txt\"}", 5U, 1000U);
    assert(cog_toolkit_execution_start(execution, COG_TOOLKIT_POLICY_ALLOW, monotonic_ms()) == COG_OK);
    result = finish(execution, output, sizeof(output));
    assert(result.status == COG_OK && result.truncated && result.output_len == 5U && strcmp(output, "chang") == 0);
    assert(cog_toolkit_execution_free(execution) == COG_OK);
    execution = prepare(workspace, "edit_file", "{\"path\":\"file.txt\",\"old\":\"changed\",\"new\":\"cancelled\"}", 64U, 1000U);
    assert(cog_toolkit_execution_start(execution, COG_TOOLKIT_POLICY_ALLOW, monotonic_ms()) == COG_OK);
    assert(cog_toolkit_execution_cancel(execution, monotonic_ms()) == COG_OK);
    result = finish(execution, output, sizeof(output));
    assert(result.status == COG_ERROR_ABORTED && !result.side_effects_possible);
    assert(cog_toolkit_execution_free(execution) == COG_OK);
}
static void refusal_cases(cog_workspace_t *workspace, const char *root)
{
    static const char *const malformed[] = {
        "{\"path\":\"../escape\"}", "{\"path\":\"/tmp/escape\"}",
        "{\"path\":\"a/../escape\"}", "{\"path\":\"file\\u0000.txt\"}",
        "{\"path\":\"file.txt\",\"extra\":true}", "{\"path\":3}", "{}"
    };
    cog_tool_async_config_t config = { .max_output_bytes = 128U, .timeout_ms = 1000U, .user = workspace };
    for (size_t i = 0U; i < sizeof(malformed) / sizeof(malformed[0]); i++) {
        cog_toolkit_execution_t *execution = NULL;
        assert(cog_toolkit_execution_new(&execution, toolkit_test_core, &cog_workspace_tools, "id", "read_file",
            malformed[i], strlen(malformed[i]), &config) == COG_ERROR_INVALID_ARG);
        assert(execution == NULL);
    }
    char link_path[4096];
    int n = snprintf(link_path, sizeof(link_path), "%s/link", root);
    assert(n > 0 && (size_t)n < sizeof(link_path));
    assert(symlink("file.txt", link_path) == 0);
    char output[4096];
    cog_toolkit_result_t result = run(workspace, "read_file", "{\"path\":\"link\"}", output, sizeof(output));
    assert(result.status == COG_ERROR_INVALID_ARG && !result.side_effects_possible);
    result = run(workspace, "edit_file", "{\"path\":\"link\",\"old\":\"changed\",\"new\":\"escape\"}", output, sizeof(output));
    assert(result.status == COG_ERROR_INVALID_ARG && !result.side_effects_possible);
    assert(unlink(link_path) == 0);
    assert(symlink("sub", link_path) == 0);
    result = run(workspace, "read_file", "{\"path\":\"link/child.txt\"}", output, sizeof(output));
    assert(result.status == COG_ERROR_INVALID_ARG && !result.side_effects_possible);
    result = run(workspace, "run_command", "{\"command\":\"touch forbidden\",\"cwd\":\"link\"}", output, sizeof(output));
    assert(result.status == COG_ERROR_INVALID_ARG && !result.side_effects_possible);
    assert(unlink(link_path) == 0);
    result = run(workspace, "read_file", "{\"path\":\"missing\"}", output, sizeof(output));
    assert(result.status == COG_ERROR_NOT_FOUND);
}
static void command_cases(cog_workspace_t *workspace, const char *root)
{
    char output[4096];
    assert(setenv("COG_INHERITED_SECRET", "should-not-inherit", 1) == 0);
    cog_toolkit_result_t result = run(workspace, "run_command",
        "{\"command\":\"printf '%s/%s' \\\"$COG_EXPLICIT_VALUE\\\" \\\"${COG_INHERITED_SECRET-unset}\\\"\",\"cwd\":\"sub\"}", output, sizeof(output));
    assert(result.status == COG_OK && strcmp(output, "copied/unset") == 0 && result.exit_status_known && result.exit_status == 0);
    result = run(workspace, "run_command", "{\"command\":\"printf stderr >&2; exit 7\"}", output, sizeof(output));
    assert(result.status == COG_ERROR_IO && result.exit_status_known && result.exit_status == 7 && strcmp(output, "stderr") == 0);
    cog_toolkit_execution_t *execution = prepare(workspace, "run_command", "{\"command\":\"printf effect > effect; printf 12345678901234567890\"}", 5U, 1000U);
    assert(cog_toolkit_execution_start(execution, COG_TOOLKIT_POLICY_ALLOW, monotonic_ms()) == COG_OK);
    assert(cog_toolkit_execution_free(execution) == COG_ERROR_INVALID_STATE);
    result = finish(execution, output, sizeof(output));
    assert(result.status == COG_OK && result.truncated && result.side_effects_possible && strcmp(output, "12345") == 0);
    for (size_t i = 0U; i < 5U; i++) {
        size_t required;
        assert(cog_toolkit_execution_result(execution, &result, output, sizeof(output), &required) == COG_OK);
        assert(strcmp(output, "12345") == 0);
    }
    assert(cog_toolkit_execution_free(execution) == COG_OK);
    result = run(workspace, "read_file", "{\"path\":\"effect\"}", output, sizeof(output));
    assert(result.status == COG_OK && strcmp(output, "effect") == 0);
    execution = prepare(workspace, "run_command", "{\"command\":\"touch denied-effect\"}", 64U, 1000U);
    assert(cog_toolkit_execution_start(execution, COG_TOOLKIT_POLICY_DENY, monotonic_ms()) == COG_OK);
    result = finish(execution, output, sizeof(output));
    assert(result.status == COG_ERROR_DENIED && !result.side_effects_possible);
    assert(cog_toolkit_execution_free(execution) == COG_OK);
    char path[4096];
    int n = snprintf(path, sizeof(path), "%s/denied-effect", root);
    assert(n > 0 && (size_t)n < sizeof(path));
    assert(access(path, F_OK) != 0 && errno == ENOENT);
    assert(unsetenv("COG_INHERITED_SECRET") == 0);
}
static void wait_ready(const char *root)
{
    char path[4096];
    int n = snprintf(path, sizeof(path), "%s/ready", root);
    assert(n > 0 && (size_t)n < sizeof(path));
    uint64_t limit = monotonic_ms() + 3000U;
    while (access(path, F_OK) != 0) {
        assert(errno == ENOENT && monotonic_ms() < limit);
        struct timespec delay = {0, 1000000};
        (void)nanosleep(&delay, NULL);
    }
    assert(unlink(path) == 0);
}
static void wait_shell_reaped(const char *root)
{
    char path[4096];
    int n = snprintf(path, sizeof(path), "%s/shell-pid", root);
    assert(n > 0 && (size_t)n < sizeof(path));
    int fd = open(path, O_RDONLY);
    char text[64] = {0};
    assert(fd >= 0 && read(fd, text, sizeof(text) - 1U) > 0 && close(fd) == 0);
    long shell = strtol(text, NULL, 10);
    assert(shell > 1L && unlink(path) == 0);
    uint64_t limit = monotonic_ms() + 3000U;
    while (kill((pid_t)shell, 0) == 0) {
        assert(monotonic_ms() < limit);
        struct timespec delay = {0, 1000000};
        (void)nanosleep(&delay, NULL);
    }
    assert(errno == ESRCH);
}
static void process_cleanup(cog_workspace_t *workspace, const char *root)
{
    /* The shell exits immediately, its inherited-group grandchild keeps running.
       The helper must adopt/reap it; parent must remain responsive and cancellable. */
    static const char command[] = "{\"command\":\"sleep 30 & echo $!; printf %s $$ > shell-pid; printf ready > ready; exit 0\"}";
    char output[4096];
    for (size_t mode = 0U; mode < 4U; mode++) {
        const char *tool = mode == 3U ? "start_background" : "run_command";
        cog_toolkit_execution_t *execution = prepare(workspace, tool, command, 128U, mode == 0U ? 40U : 5000U);
        /* Hold polling until readiness, then exercise an expired deadline. */
        uint64_t start = monotonic_ms();
        assert(cog_toolkit_execution_start(execution, COG_TOOLKIT_POLICY_ALLOW,
            start - (mode == 0U ? 40U : 0U)) == COG_OK);
        wait_ready(root);
        wait_shell_reaped(root);
        bool done = false;
        if (mode != 0U) {
            uint64_t deadline;
            assert(cog_toolkit_execution_poll(execution, monotonic_ms(), &done, &deadline) == COG_OK && !done);
            assert(cog_toolkit_execution_cancel(execution, monotonic_ms()) == COG_OK);
            assert(cog_toolkit_execution_cancel(execution, monotonic_ms()) == COG_OK);
        }
        cog_toolkit_result_t result = finish(execution, output, sizeof(output));
        assert(result.status == (mode == 0U ? COG_ERROR_TIMEOUT : COG_ERROR_ABORTED));
        assert(result.side_effects_possible && result.exit_status_known);
        long grandchild = strtol(output, NULL, 10);
        assert(grandchild > 1L);
        assert(kill((pid_t)grandchild, 0) == -1 && errno == ESRCH);
        assert(cog_toolkit_execution_free(execution) == COG_OK);
    }
    int status;
    assert(waitpid(-1, &status, WNOHANG) == -1 && errno == ECHILD);
    /* TERM-resistant descendant forces the configured KILL deadline. */
    cog_toolkit_execution_t *execution = prepare(workspace, "run_command",
        "{\"command\":\"trap '' TERM; printf ready > ready; while :; do sleep 1; done\"}", 64U, 30U);
    assert(cog_toolkit_execution_start(execution, COG_TOOLKIT_POLICY_ALLOW, monotonic_ms()) == COG_OK);
    wait_ready(root);
    cog_toolkit_result_t result = finish(execution, output, sizeof(output));
    assert(result.status == COG_ERROR_TIMEOUT && result.side_effects_possible);
    assert(cog_toolkit_execution_free(execution) == COG_OK);
    assert(waitpid(-1, &status, WNOHANG) == -1 && errno == ECHILD);
}
static void limit_cases(const char *root)
{
    cog_workspace_config_t config = { .root = root, .max_file_bytes = 4U,
        .max_search_entries = 1U, .service_quantum = 1U };
    cog_workspace_t *workspace = NULL;
    assert(cog_workspace_new(&workspace, &config) == COG_OK);
    char output[128];
    cog_toolkit_result_t result = run(workspace, "read_file", "{\"path\":\"file.txt\"}", output, sizeof(output));
    assert(result.status == COG_ERROR_LIMIT);
    cog_tool_async_config_t limits = { .max_output_bytes = 64U, .timeout_ms = 100U, .user = workspace };
    cog_toolkit_execution_t *execution = NULL;
    assert(cog_toolkit_execution_new(&execution, toolkit_test_core, &cog_workspace_tools, "id", "run_command",
        "{\"command\":\"true\"}", 18U, &limits) == COG_ERROR_UNSUPPORTED);
    assert(execution == NULL);
    assert(cog_workspace_free(workspace) == COG_OK);
    config.max_file_bytes = 65536U;
    assert(cog_workspace_new(&workspace, &config) == COG_OK);
    result = run(workspace, "search", "{\"pattern\":\"beta\"}", output, sizeof(output));
    assert(result.status == COG_ERROR_LIMIT);
    assert(cog_workspace_free(workspace) == COG_OK);
}
int main(void)
{
    toolkit_core_create();
    char root[] = "/tmp/cog-toolkit-workspace-XXXXXX";
    assert(mkdtemp(root) != NULL);
    char sub[4096];
    int n = snprintf(sub, sizeof(sub), "%s/sub", root);
    assert(n > 0 && (size_t)n < sizeof(sub) && mkdir(sub, 0700) == 0);
    write_fixture(root, "file.txt", "alpha\nbeta\n");
    write_fixture(sub, "child.txt", "child\n");
    char environment_value[] = "COG_EXPLICIT_VALUE=copied";
    const char *const environment[] = {"PATH=/usr/bin:/bin", environment_value};
    cog_workspace_config_t config = { .root = root, .max_file_bytes = 65536U,
        .max_search_entries = 64U, .service_quantum = 2U, .environment = environment,
        .environment_count = 2U, .enable_commands = true };
    cog_workspace_t *workspace = NULL;
    assert(cog_workspace_new(&workspace, &config) == COG_OK);
    memset(environment_value, 'x', sizeof(environment_value) - 1U);
    file_cases(workspace, root); refusal_cases(workspace, root); command_cases(workspace, root);
    char answer[64];
    cog_toolkit_result_t background = run(workspace, "start_background",
        "{\"command\":\"printf completed\"}", answer, sizeof(answer));
    assert(background.status == COG_OK && strcmp(answer, "completed") == 0);
    assert(background.exit_status_known && background.exit_status == 0);
    cog_toolkit_execution_t *refused = prepare(workspace, "start_background",
        "{\"command\":\"true\",\"cwd\":\"missing-dir\"}", 64U, 1000U);
    assert(cog_toolkit_execution_start(refused, COG_TOOLKIT_POLICY_ALLOW, monotonic_ms()) == COG_ERROR_IO);
    assert(cog_toolkit_execution_free(refused) == COG_OK);
    process_cleanup(workspace, root);
    assert(cog_workspace_free(workspace) == COG_OK);
    limit_cases(root);
    assert(nftw(root, remove_fixture, 16, FTW_DEPTH | FTW_PHYS) == 0);
    toolkit_core_destroy();
    puts("test_workspace: ok");
    return 0;
}

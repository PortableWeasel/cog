/** @file workspace_process.c @brief Helper-local descendant ownership and reap. */
#define _GNU_SOURCE
#include "workspace_internal.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

/* No embedding-process signal/subreaper/cwd changes. All such state below
   belongs to this explicit helper, which never calls back into Cog. */
static bool helper_close_descriptors(const int *keep, size_t count)
{
    unsigned int first = 0U;
    for (;;) {
        unsigned int next = UINT_MAX;
        for (size_t i = 0U; i < count; i++) {
            if (keep[i] >= 0 && (unsigned int)keep[i] >= first && (unsigned int)keep[i] < next) {
                next = (unsigned int)keep[i];
            }
        }
        if (next > first && syscall(SYS_close_range, first, next - 1U, 0U) != 0) { return false; }
        if (next == UINT_MAX) { return true; }
        first = next + 1U;
    }
}

static void helper_report(int fd, const workspace_report_t *report)
{
    ssize_t sent;
    do { sent = write(fd, report, sizeof(*report)); } while (sent < 0 && errno == EINTR);
    (void)sent;
}

static void process_helper(workspace_execution_t *execution, int control, int output,
                           int report_fd, int cwd)
{
    const int keep[] = {control, output, report_fd, cwd};
    workspace_report_t report = { .status = COG_ERROR_IO };
    if (!helper_close_descriptors(keep, sizeof(keep) / sizeof(keep[0]))) {
        report.status = COG_ERROR_UNSUPPORTED; helper_report(report_fd, &report); _exit(0);
    }
    struct sigaction disposition;
    memset(&disposition, 0, sizeof(disposition));
    disposition.sa_handler = SIG_DFL;
    (void)sigemptyset(&disposition.sa_mask);
    /* Inherited SIGCHLD=IGN/NOCLDWAIT would discard wait status; inherited
       application handlers must never run in the supervisor. */
    for (int sig = 1; sig < NSIG; sig++) {
        if (sig != SIGKILL && sig != SIGSTOP) { (void)sigaction(sig, &disposition, NULL); }
    }
    sigset_t empty;
    (void)sigemptyset(&empty);
    (void)sigprocmask(SIG_SETMASK, &empty, NULL);
    if (prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0) != 0 || setsid() < 0) {
        report.status = COG_ERROR_UNSUPPORTED;
        helper_report(report_fd, &report); _exit(0);
    }
    pid_t shell = fork();
    if (shell < 0) { helper_report(report_fd, &report); _exit(0); }
    if (shell == 0) {
        if (setpgid(0, 0) != 0 || fchdir(cwd) != 0 ||
            dup2(output, STDOUT_FILENO) < 0 || dup2(output, STDERR_FILENO) < 0 ||
            fcntl(STDOUT_FILENO, F_SETFD, 0) != 0 || fcntl(STDERR_FILENO, F_SETFD, 0) != 0) { _exit(126); }
        int null_fd = open("/dev/null", O_RDONLY);
        if (null_fd < 0 || dup2(null_fd, STDIN_FILENO) < 0) { _exit(126); }
        const int shell_keep[] = { STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO };
        if (!helper_close_descriptors(shell_keep, sizeof(shell_keep) / sizeof(shell_keep[0]))) { _exit(126); }
        char *const argv[] = { (char *)"sh", (char *)"-c", (char *)(void *)execution->command, NULL };
        execve("/bin/sh", argv, execution->workspace->environment);
        _exit(127);
    }
    (void)setpgid(shell, shell); /* Child also sets the group before exec. */
    (void)close(output);
    (void)close(cwd);
    report.status = COG_OK;
    for (;;) {
        /* Every descendant in the supported inherited group is either still
           parented to its live ancestor or adopted here after that parent dies. */
        int status;
        pid_t child = waitpid(-1, &status, WNOHANG);
        if (child > 0) {
            if (child == shell) { report.shell_status = status; report.shell_known = true; }
            continue;
        }
        if (child < 0 && errno == ECHILD) { break; }
        if (child < 0 && errno != EINTR) { report.status = COG_ERROR_INDETERMINATE; break; }
        struct pollfd descriptor = { .fd = control, .events = POLLIN | POLLHUP };
        int polled = poll(&descriptor, 1U, 10);
        if (polled < 0 && errno != EINTR) { report.status = COG_ERROR_INDETERMINATE; (void)kill(-shell, SIGKILL); }
        if (polled > 0 && (descriptor.revents & (POLLIN | POLLHUP)) != 0) {
            char commands[16];
            ssize_t count = read(control, commands, sizeof(commands));
            if (count == 0) { (void)kill(-shell, SIGKILL); }
            else if (count > 0) {
                for (ssize_t i = 0; i < count; i++) {
                    if (commands[i] == 'T') { (void)kill(-shell, SIGTERM); }
                    else if (commands[i] == 'K') { (void)kill(-shell, SIGKILL); }
                }
            }
        }

    }
    if (!report.shell_known && report.status == COG_OK) { report.status = COG_ERROR_INDETERMINATE; }
    helper_report(report_fd, &report);
    _exit(0);
}

static bool nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

cog_result_t cog_workspace_process_start(workspace_execution_t *execution)
{
#if !defined(__linux__) || !defined(SYS_close_range)
    (void)execution;
    return COG_ERROR_UNSUPPORTED;
#else
    int cwd = cog_workspace_open_path(execution->workspace, execution->path, O_RDONLY | O_DIRECTORY);
    if (cwd < 0) { return errno == ELOOP || errno == ENOTDIR ? COG_ERROR_INVALID_ARG : COG_ERROR_IO; }
    int control[2] = {-1, -1};
    int output[2] = {-1, -1};
    int report[2] = {-1, -1};
    cog_result_t result = COG_ERROR_IO;
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, control) != 0 ||
        pipe2(output, O_CLOEXEC) != 0 || pipe2(report, O_CLOEXEC) != 0 ||
        !nonblocking(control[0]) || !nonblocking(output[0]) || !nonblocking(report[0])) { goto cleanup; }
    /* Fail before launching the helper if descriptor-safe cleanup is absent.
       Probe an empty range: a supported syscall returns EINVAL, not ENOSYS. */
    errno = 0;
    if (syscall(SYS_close_range, 4U, 3U, 0U) < 0 && errno != EINVAL) {
        result = COG_ERROR_UNSUPPORTED; goto cleanup;
    }
    pid_t helper = fork();
    if (helper < 0) { goto cleanup; }
    if (helper == 0) { process_helper(execution, control[1], output[1], report[1], cwd); }
    execution->helper = helper;
    execution->control_fd = control[0]; control[0] = -1;
    execution->output_fd = output[0]; output[0] = -1;
    execution->report_fd = report[0]; report[0] = -1;
    execution->output.side_effects_possible = true;
    result = COG_OK;
cleanup:
    (void)close(cwd);
    for (size_t i = 0U; i < 2U; i++) {
        if (control[i] >= 0) { (void)close(control[i]); }
        if (output[i] >= 0) { (void)close(output[i]); }
        if (report[i] >= 0) { (void)close(report[i]); }
    }
    return result;
#endif
}

static cog_result_t send_control(workspace_execution_t *execution, char command)
{
    ssize_t count = send(execution->control_fd, &command, 1U, MSG_NOSIGNAL);
    if (count == 1) { return COG_OK; }
    if (count < 0 && (errno == EAGAIN || errno == EINTR)) { return COG_ERROR_AGAIN; }
    /* Completion may close its control socket before the parent observes it.
       A missing report remains uncertain; never claim successful reaping. */
    return COG_ERROR_IO;
}

cog_result_t cog_workspace_process_cancel(workspace_execution_t *execution)
{
    if (execution->cancelling) { return COG_OK; }
    execution->cancelling = true;
    execution->kill_deadline = UINT64_MAX - execution->now < execution->config.terminate_grace_ms ?
        UINT64_MAX : execution->now + execution->config.terminate_grace_ms;
    cog_result_t r = send_control(execution, execution->config.terminate_grace_ms == 0U ? 'K' : 'T');
    if (r == COG_OK && execution->config.terminate_grace_ms == 0U) { execution->kill_sent = true; }
    /* A transient send is retried by poll. An exited helper is handled there. */
    return COG_OK;
}

cog_result_t cog_workspace_process_poll(workspace_execution_t *execution)
{
    if (execution->cancelling && !execution->kill_sent && execution->now >= execution->kill_deadline) {
        cog_result_t r = send_control(execution, 'K');
        if (r == COG_OK) { execution->kill_sent = true; }
    }
    char bytes[COG_WORKSPACE_IO_BYTES];
    for (size_t i = 0U; i < execution->workspace->quantum && !execution->output_eof; i++) {
        ssize_t count = read(execution->output_fd, bytes, sizeof(bytes));
        if (count > 0) { cog_workspace_capture(execution, bytes, (size_t)count); }
        else if (count == 0) { execution->output_eof = true; }
        else if (errno == EINTR) { continue; }
        else if (errno == EAGAIN) { break; }
        else { execution->output.status = COG_ERROR_IO; execution->output_eof = true; (void)cog_workspace_process_cancel(execution); }
    }
    if (execution->report_bytes < sizeof(execution->report) && !execution->report_eof) {
        ssize_t count = read(execution->report_fd, (char *)&execution->report + execution->report_bytes,
                             sizeof(execution->report) - execution->report_bytes);
        if (count > 0) { execution->report_bytes += (size_t)count; }
        else if (count == 0) { execution->report_eof = true; }
        else if (errno != EINTR && errno != EAGAIN) { execution->report_eof = true; }
    }
    if (!execution->helper_reaped) {
        int status;
        pid_t reaped = waitpid(execution->helper, &status, WNOHANG);
        if (reaped == execution->helper) {
            execution->helper_reaped = true;
            if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
                execution->output.status = COG_ERROR_INDETERMINATE;
            }
        } else if (reaped < 0 && errno != EINTR) {
            /* An embedding handler that stole our explicit child makes ownership
               unverifiable. Retain that fact instead of asserting success. */
            execution->helper_reaped = true;
            execution->output.status = COG_ERROR_INDETERMINATE;
        }
    }
    if (execution->helper_reaped && execution->output_eof &&
        (execution->report_bytes == sizeof(execution->report) || execution->report_eof)) {
        cog_result_t status = execution->output.status;
        if (execution->report_bytes != sizeof(execution->report)) { status = COG_ERROR_INDETERMINATE; }
        else if (execution->report.status != COG_OK) { status = (cog_result_t)execution->report.status; }
        else {
            execution->output.exit_status_known = execution->report.shell_known;
            int shell_status = execution->report.shell_status;
            execution->output.exit_status = WIFEXITED(shell_status) ? WEXITSTATUS(shell_status) :
                WIFSIGNALED(shell_status) ? 128 + WTERMSIG(shell_status) : shell_status;
            if (status == COG_OK && execution->output.exit_status != 0) { status = COG_ERROR_IO; }
        }
        cog_workspace_finish(execution, status);
    }
    return COG_OK;
}

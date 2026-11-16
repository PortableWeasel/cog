/** @file test_host_output_critic.c @brief Independent host adapter fault oracles. */
#define _GNU_SOURCE
#include "host_output.h"
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

static int fault_fd = -1;
static unsigned writes, constructors, fail_constructor;
static bool interrupt_write;
ssize_t __real_write(int fd, const void *bytes, size_t size);
ssize_t __wrap_write(int fd, const void *bytes, size_t size);
ssize_t __wrap_write(int fd, const void *bytes, size_t size)
{
    if (fd != fault_fd) return __real_write(fd, bytes, size);
    writes++;
    if (interrupt_write) { errno = EINTR; return -1; }
    assert(size >= 3U);
    return __real_write(fd, bytes, 3U);
}
FILE *__real_fopencookie(void *cookie, const char *mode, cookie_io_functions_t functions);
FILE *__wrap_fopencookie(void *cookie, const char *mode, cookie_io_functions_t functions);
FILE *__wrap_fopencookie(void *cookie, const char *mode, cookie_io_functions_t functions)
{
    constructors++;
    if (constructors == fail_constructor) { errno = ENOMEM; return NULL; }
    return __real_fopencookie(cookie, mode, functions);
}
static int fd_count(void)
{
    DIR *dir = opendir("/proc/self/fd"); assert(dir != NULL);
    int count = 0; struct dirent *item;
    while ((item = readdir(dir)) != NULL) if (item->d_name[0] != '.') count++;
    assert(closedir(dir) == 0); return count;
}
static int temporary_output(void)
{
    char path[] = "/tmp/cog-output-critic-XXXXXX";
    int fd = mkstemp(path); assert(fd >= 0); assert(unlink(path) == 0);
    assert(dup2(fd, STDOUT_FILENO) == STDOUT_FILENO); return fd;
}
static void zero_and_unwind(void)
{
    cog_cli_host_output_t output = {0};
    FILE *out = stdout, *err = stderr;
    int before = fd_count();
    assert(cog_cli_host_output_close_stdout(&output) == COG_OK);
    assert(cog_cli_host_output_close(&output, COG_ERROR_LIMIT) == COG_ERROR_LIMIT);
    assert(fcntl(STDIN_FILENO, F_GETFD) >= 0);
    fail_constructor = 2U;
    assert(cog_cli_host_output_open(&output) == COG_ERROR_NOMEM);
    assert(constructors == 2U && stdout == out && stderr == err);
    assert(fd_count() == before);
    assert(cog_cli_host_output_close(&output, COG_OK) == COG_OK);
    assert(fd_count() == before);
}
static void regular_offset(void)
{
    int fd = temporary_output();
    assert(write(fd, "prefix", 6U) == 6);
    int flags = fcntl(fd, F_GETFL); assert(flags >= 0);
    cog_cli_host_output_t output = {0}; FILE *original = stdout;
    assert(cog_cli_host_output_open(&output) == COG_OK);
    assert(cog_cli_host_output_open(&output) == COG_ERROR_INVALID_STATE);
    assert(fwrite("suffix", 1U, 6U, stdout) == 6U);
    assert(cog_cli_host_output_close_stdout(&output) == COG_OK);
    assert(stdout == original && lseek(fd, 0, SEEK_CUR) == 12);
    assert(cog_cli_host_output_close(&output, COG_OK) == COG_OK);
    assert(cog_cli_host_output_close(&output, COG_OK) == COG_OK);
    assert(fcntl(fd, F_GETFL) == flags);
    char bytes[12]; assert(pread(fd, bytes, sizeof(bytes), 0) == 12);
    assert(memcmp(bytes, "prefixsuffix", sizeof(bytes)) == 0);
    assert(close(fd) == 0);
}
static void partial_or_interrupt(bool interrupted)
{
    int fd = temporary_output(); cog_cli_host_output_t output = {0};
    assert(cog_cli_host_output_open(&output) == COG_OK);
    fault_fd = output.slots[0].descriptor; interrupt_write = interrupted;
    assert(fwrite("abcdef", 1U, 6U, stdout) == 6U);
    assert(fflush(stdout) != 0 && writes == 1U);
    clearerr(stdout);
    (void)fwrite("another", 1U, 7U, stdout);
    (void)fflush(stdout);
    assert(cog_cli_host_output_close_stdout(&output) == COG_ERROR_IO);
    assert(cog_cli_host_output_close(&output, COG_ERROR_LIMIT) == COG_ERROR_LIMIT);
    assert(cog_cli_host_output_close(&output, COG_OK) == COG_ERROR_IO);
    assert(writes == 1U);
    char bytes[16] = {0}; ssize_t count = pread(fd, bytes, sizeof(bytes), 0);
    assert(count == (interrupted ? 0 : 3));
    if (!interrupted) assert(memcmp(bytes, "abc", 3U) == 0);
    assert(close(fd) == 0);
}
static void socket_saturation(void)
{
    int pair[2]; assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    int flags = fcntl(pair[0], F_GETFL); assert(flags >= 0 && (flags & O_NONBLOCK) == 0);
    char bytes[4096] = {0};
    while (send(pair[0], bytes, sizeof(bytes), MSG_DONTWAIT | MSG_NOSIGNAL) > 0) {}
    assert(errno == EAGAIN || errno == EWOULDBLOCK);
    assert(dup2(pair[0], STDOUT_FILENO) == STDOUT_FILENO);
    cog_cli_host_output_t output = {0};
    assert(cog_cli_host_output_open(&output) == COG_OK);
    assert(fwrite("full", 1U, 4U, stdout) == 4U);
    assert(fflush(stdout) != 0);
    assert(cog_cli_host_output_close(&output, COG_OK) == COG_ERROR_IO);
    assert(fcntl(pair[0], F_GETFL) == flags);
    assert(close(pair[0]) == 0 && close(pair[1]) == 0);
}
static void partial(void) { partial_or_interrupt(false); }
static void interrupted(void) { partial_or_interrupt(true); }
int main(void)
{
    static void (*const cases[])(void) = {zero_and_unwind, regular_offset, partial, interrupted, socket_saturation};
    for (size_t i = 0U; i < sizeof(cases) / sizeof(cases[0]); i++) {
        pid_t pid = fork(); assert(pid >= 0);
        if (pid == 0) { alarm(5U); cases[i](); _exit(0); }
        int status; assert(waitpid(pid, &status, 0) == pid);
        assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }
    return 0;
}

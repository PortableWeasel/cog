/** @file host_output.c @brief Checked executable output without shared flag mutation. */
#define _GNU_SOURCE
#include "host_output.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

static ssize_t output_write(void *cookie, const char *bytes, size_t length)
{
    cog_cli_host_output_slot_t *slot = cookie;
    if (slot->error != COG_OK || length > (size_t)SSIZE_MAX) {
        slot->error = COG_ERROR_IO; errno = EIO; return 0;
    }
    ssize_t written = slot->socket ? send(slot->descriptor, bytes, length, MSG_DONTWAIT | MSG_NOSIGNAL) :
        write(slot->descriptor, bytes, length);
    if (written < 0 || (size_t)written != length) {
        /* The accepted prefix is not replayable. Cookie writes report zero on
           error; sticky state prevents libc flush/close from retrying it. */
        slot->error = COG_ERROR_IO;
        if (written >= 0) errno = EIO;
        return 0;
    }
    return written;
}
static int output_descriptor_close(void *cookie)
{
    cog_cli_host_output_slot_t *slot = cookie;
    int descriptor = slot->descriptor;
    slot->descriptor = -1;
    if (descriptor >= 0 && close(descriptor) != 0) {
        slot->error = COG_ERROR_IO; return -1;
    }
    return 0;
}
static const cookie_io_functions_t output_functions = {
    .read = NULL, .write = output_write, .seek = NULL, .close = output_descriptor_close
};
static cog_result_t output_descriptor(int inherited, cog_cli_host_output_slot_t *slot)
{
    struct stat info;
    int flags = fcntl(inherited, F_GETFL);
    if (flags < 0 || (flags & O_ACCMODE) == O_RDONLY || fstat(inherited, &info) != 0)
        return COG_ERROR_IO;
    slot->socket = S_ISSOCK(info.st_mode);
    if (S_ISREG(info.st_mode) || slot->socket)
        slot->descriptor = fcntl(inherited, F_DUPFD_CLOEXEC, 3);
    else if (S_ISFIFO(info.st_mode) || S_ISCHR(info.st_mode)) {
        const char *path = inherited == STDOUT_FILENO ? "/proc/self/fd/1" : "/proc/self/fd/2";
        int descriptor = open(path, O_WRONLY | O_NONBLOCK | O_CLOEXEC | O_NOCTTY);
        if (descriptor >= 0 && descriptor < 3) {
            /* Do not temporarily occupy an inherited closed stdin. */
            int replacement = fcntl(descriptor, F_DUPFD_CLOEXEC, 3);
            (void)close(descriptor); descriptor = replacement;
        }
        slot->descriptor = descriptor;
    } else return COG_ERROR_IO;
    return slot->descriptor < 0 ? COG_ERROR_IO : COG_OK;
}
static cog_result_t close_slot(cog_cli_host_output_slot_t *slot)
{
    if (slot->stream != NULL) {
        FILE *stream = slot->stream;
        slot->stream = NULL;
        if (stdout == stream) stdout = slot->original;
        if (stderr == stream) stderr = slot->original;
        if (fclose(stream) != 0) slot->error = COG_ERROR_IO;
    } else if (slot->descriptor >= 0) {
        (void)output_descriptor_close(slot);
    }
    return slot->error;
}
cog_result_t cog_cli_host_output_open(cog_cli_host_output_t *output)
{
    if (output == NULL) return COG_ERROR_NULL_PTR;
    if (output->installed || output->slots[0].stream != NULL || output->slots[1].stream != NULL)
        return COG_ERROR_INVALID_STATE;
    *output = (cog_cli_host_output_t){0};
    output->slots[0].descriptor = -1; output->slots[1].descriptor = -1;
    output->slots[0].original = stdout; output->slots[1].original = stderr;
    cog_result_t result = COG_OK;
    for (size_t i = 0U; i < 2U; i++) {
        cog_cli_host_output_slot_t *slot = &output->slots[i];
        result = output_descriptor(i == 0U ? STDOUT_FILENO : STDERR_FILENO, slot);
        if (result != COG_OK) break;
        slot->stream = fopencookie(slot, "w", output_functions);
        if (slot->stream == NULL) { result = errno == ENOMEM ? COG_ERROR_NOMEM : COG_ERROR_IO; break; }
        if (setvbuf(slot->stream, i == 0U ? output->stdout_buffer : NULL,
                i == 0U ? _IOFBF : _IONBF, i == 0U ? sizeof(output->stdout_buffer) : 0U) != 0) {
            result = COG_ERROR_IO; break;
        }
    }
    if (result != COG_OK) return cog_cli_host_output_close(output, result);
    stdout = output->slots[0].stream; stderr = output->slots[1].stream;
    output->installed = true;
    return COG_OK;
}
cog_result_t cog_cli_host_output_close_stdout(cog_cli_host_output_t *output)
{
    if (output == NULL) return COG_ERROR_NULL_PTR;
    if (output->slots[0].original == NULL) return COG_OK;
    return close_slot(&output->slots[0]);
}
cog_result_t cog_cli_host_output_close(cog_cli_host_output_t *output, cog_result_t primary)
{
    if (output == NULL) return COG_ERROR_NULL_PTR;
    /* A zero-initialized unopened object owns no descriptors. */
    if (output->slots[0].original == NULL && output->slots[1].original == NULL) return primary;
    cog_result_t first = close_slot(&output->slots[0]);
    cog_result_t second = close_slot(&output->slots[1]);
    output->installed = false;
    return primary != COG_OK ? primary : first != COG_OK ? first : second;
}

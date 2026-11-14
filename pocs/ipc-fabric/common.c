/**
 * @file common.c
 * @brief Shared implementation for the IPC fabric PoC.
 */

#include "common.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void
ipc_frame_encode_header(uint8_t header[IPC_HEADER_SIZE], uint32_t len)
{
    header[0] = (uint8_t)((len >> 24) & 0xFFU);
    header[1] = (uint8_t)((len >> 16) & 0xFFU);
    header[2] = (uint8_t)((len >> 8) & 0xFFU);
    header[3] = (uint8_t)(len & 0xFFU);
}

uint32_t
ipc_frame_decode_header(const uint8_t header[IPC_HEADER_SIZE])
{
    uint32_t len = 0U;

    len |= ((uint32_t)header[0]) << 24;
    len |= ((uint32_t)header[1]) << 16;
    len |= ((uint32_t)header[2]) << 8;
    len |= ((uint32_t)header[3]);

    return len;
}

ssize_t
ipc_read_exact(int fd, void *buf, size_t n)
{
    size_t total = 0U;
    char *cursor = buf;

    while (total < n) {
        ssize_t rc = read(fd, cursor + total, n - total);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (rc == 0) {
            break;
        }
        total += (size_t)rc;
    }

    return (ssize_t)total;
}

int
ipc_write_exact(int fd, const void *buf, size_t n)
{
    size_t total = 0U;
    const char *cursor = buf;

    while (total < n) {
        ssize_t rc = write(fd, cursor + total, n - total);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        total += (size_t)rc;
    }

    return 0;
}

const char *
ipc_socket_path(void)
{
    const char *env = getenv(COG_IPC_SOCKET_ENV);

    if ((env != NULL) && (env[0] != '\0')) {
        return env;
    }

    return COG_IPC_SOCKET_PATH;
}

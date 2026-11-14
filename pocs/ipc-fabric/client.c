/**
 * @file client.c
 * @brief IPC fabric client for the Cog PoC.
 *
 * The client connects to the Unix domain socket, sends a small sequence of
 * framed messages, prints the replies, then disconnects and reconnects to
 * demonstrate recovery.
 */

#define _POSIX_C_SOURCE 200809L

#include "common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

/** Number of messages to send before reconnecting. */
#define MESSAGE_COUNT 3

/** Number of reconnect cycles to perform. */
#define RECONNECT_COUNT 2

/**
 * @brief Connect to the IPC server.
 *
 * @param[in] path Unix domain socket path.
 * @return Connected socket fd, or -1 on error.
 */
static int connect_to_server(const char *path)
{
    int fd;
    struct sockaddr_un addr;
    size_t path_len;

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }

    (void)memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    path_len = strlen(path);
    if (path_len >= sizeof(addr.sun_path)) {
        (void)fprintf(stderr, "socket path too long\n");
        (void)close(fd);
        return -1;
    }
    (void)strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1U);

    if (connect(fd, (const struct sockaddr *)&addr, sizeof(addr)) < 0) {
        (void)close(fd);
        return -1;
    }

    return fd;
}

/**
 * @brief Send a framed message.
 *
 * @param[in] fd Socket fd.
 * @param[in] msg Message payload.
 * @param[in] len Payload length.
 * @return 0 on success, -1 on error.
 */
static int send_message(int fd, const char *msg, size_t len)
{
    uint8_t header[IPC_HEADER_SIZE];

    ipc_frame_encode_header(header, (uint32_t)len);
    if (ipc_write_exact(fd, header, IPC_HEADER_SIZE) < 0) {
        return -1;
    }
    if (ipc_write_exact(fd, msg, len) < 0) {
        return -1;
    }
    return 0;
}

/**
 * @brief Receive one framed message and print it.
 *
 * @param[in] fd Socket fd.
 * @return 0 on success, -1 on error or disconnect.
 */
static int receive_and_print(int fd)
{
    uint8_t header[IPC_HEADER_SIZE];
    uint32_t len;
    char *buf;
    ssize_t rc;

    rc = ipc_read_exact(fd, header, IPC_HEADER_SIZE);
    if (rc < 0) {
        return -1;
    }
    if (rc != (ssize_t)IPC_HEADER_SIZE) {
        return -1;
    }

    len = ipc_frame_decode_header(header);
    if (len > IPC_MAX_PAYLOAD) {
        return -1;
    }

    buf = malloc((size_t)len + 1U);
    if (buf == NULL) {
        return -1;
    }

    if (len > 0U) {
        rc = ipc_read_exact(fd, buf, (size_t)len);
        if (rc < 0) {
            free(buf);
            return -1;
        }
        if ((size_t)rc != (size_t)len) {
            free(buf);
            return -1;
        }
    }

    buf[len] = '\0';
    (void)printf("%s\n", buf);
    free(buf);
    return 0;
}

/**
 * @brief Run one connection cycle: connect, send messages, receive replies.
 *
 * @param[in] path Socket path.
 * @param[in] cycle Cycle number (for messages).
 * @return 0 on success, -1 on error.
 */
static int run_cycle(const char *path, int cycle)
{
    int fd;
    int n;
    char msg[128];

    fd = connect_to_server(path);
    if (fd < 0) {
        (void)perror("connect");
        return -1;
    }

    (void)printf("--- connected (cycle %d) ---\n", cycle);

    for (int i = 0; i < MESSAGE_COUNT; i++) {
        size_t len;

        n = snprintf(msg, sizeof(msg), "hello from cycle %d message %d", cycle, i);
        if (n < 0) {
            (void)close(fd);
            return -1;
        }
        len = (size_t)n;

        if (send_message(fd, msg, len) < 0) {
            (void)perror("send_message");
            (void)close(fd);
            return -1;
        }

        if (receive_and_print(fd) < 0) {
            (void)fprintf(stderr, "server closed connection\n");
            (void)close(fd);
            return -1;
        }
    }

    (void)close(fd);
    (void)printf("--- disconnected (cycle %d) ---\n", cycle);
    return 0;
}

int
main(int argc, char **argv)
{
    const char *path;

    (void)argc;
    (void)argv;

    path = ipc_socket_path();

    for (int cycle = 0; cycle < RECONNECT_COUNT; cycle++) {
        if (run_cycle(path, cycle) < 0) {
            return EXIT_FAILURE;
        }
    }

    return EXIT_SUCCESS;
}

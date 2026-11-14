/**
 * @file edge-test.c
 * @brief Edge-case tests for the IPC fabric PoC.
 *
 * This program exercises boundary conditions that the smoke test does
 * not cover:
 *
 * - Empty payload (length 0) frames.
 * - Maximum-size payload frames.
 * - Oversized frame headers are rejected by closing the connection.
 */

#define _POSIX_C_SOURCE 200809L

#include "common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

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
 * @param[in] payload Message payload (may be NULL if len is 0).
 * @param[in] len Payload length.
 * @return 0 on success, -1 on error.
 */
static int send_frame(int fd, const uint8_t *payload, uint32_t len)
{
    uint8_t header[IPC_HEADER_SIZE];

    ipc_frame_encode_header(header, len);
    if (ipc_write_exact(fd, header, IPC_HEADER_SIZE) < 0) {
        return -1;
    }
    if ((len > 0U) && (payload != NULL)) {
        if (ipc_write_exact(fd, payload, (size_t)len) < 0) {
            return -1;
        }
    }
    return 0;
}

/**
 * @brief Receive one framed message into a buffer.
 *
 * @param[in] fd Socket fd.
 * @param[out] payload Buffer to receive payload.
 * @param[in] payload_size Capacity of @p payload.
 * @param[out] out_len Received payload length.
 * @return 0 on success, -1 on error or disconnect.
 */
static int recv_frame(int fd, uint8_t *payload, size_t payload_size,
                      uint32_t *out_len)
{
    uint8_t header[IPC_HEADER_SIZE];
    uint32_t len;
    ssize_t rc;

    rc = ipc_read_exact(fd, header, IPC_HEADER_SIZE);
    if (rc < 0) {
        return -1;
    }
    if ((size_t)rc != IPC_HEADER_SIZE) {
        return -1;
    }

    len = ipc_frame_decode_header(header);
    if (len > payload_size) {
        return -1;
    }

    if (len > 0U) {
        rc = ipc_read_exact(fd, payload, (size_t)len);
        if (rc < 0) {
            return -1;
        }
        if ((size_t)rc != (size_t)len) {
            return -1;
        }
    }

    *out_len = len;
    return 0;
}

/**
 * @brief Test that an empty payload frame is echoed back.
 *
 * @param[in] path Socket path.
 * @return 0 on success, -1 on error.
 */
static int test_empty_payload(const char *path)
{
    int fd;
    uint8_t payload[64];
    uint32_t len;

    fd = connect_to_server(path);
    if (fd < 0) {
        (void)perror("connect");
        return -1;
    }

    if (send_frame(fd, NULL, 0U) < 0) {
        (void)close(fd);
        return -1;
    }

    if (recv_frame(fd, payload, sizeof(payload), &len) < 0) {
        (void)fprintf(stderr, "empty payload: no reply\n");
        (void)close(fd);
        return -1;
    }

    if (len != 4U) {
        (void)fprintf(stderr, "empty payload: expected 4-byte reply, got %u\n",
                      len);
        (void)close(fd);
        return -1;
    }

    if (memcmp(payload, "[1] ", 4U) != 0) {
        (void)fprintf(stderr, "empty payload: unexpected reply\n");
        (void)close(fd);
        return -1;
    }

    (void)close(fd);
    (void)printf("empty payload test passed\n");
    return 0;
}

/**
 * @brief Test that a near-maximum payload frame is echoed back.
 *
 * The payload is sized so the reply ("[N] " prefix + payload) still
 * fits in a single frame.
 *
 * @param[in] path Socket path.
 * @return 0 on success, -1 on error.
 */
static int test_max_payload(const char *path)
{
    int fd;
    uint8_t *send_buf;
    uint8_t *recv_buf;
    uint32_t payload_len = IPC_MAX_PAYLOAD - 4U;
    uint32_t len;
    int status = 0;

    send_buf = malloc(payload_len);
    recv_buf = malloc(IPC_MAX_PAYLOAD);
    if ((send_buf == NULL) || (recv_buf == NULL)) {
        free(send_buf);
        free(recv_buf);
        return -1;
    }

    (void)memset(send_buf, 'A', payload_len);

    fd = connect_to_server(path);
    if (fd < 0) {
        (void)perror("connect");
        free(send_buf);
        free(recv_buf);
        return -1;
    }

    if (send_frame(fd, send_buf, payload_len) < 0) {
        status = -1;
    } else if (recv_frame(fd, recv_buf, IPC_MAX_PAYLOAD, &len) < 0) {
        status = -1;
    } else if (len != IPC_MAX_PAYLOAD) {
        (void)fprintf(stderr, "max payload: expected %u bytes, got %u\n",
                      IPC_MAX_PAYLOAD, len);
        status = -1;
    } else if (memcmp(recv_buf, "[2] ", 4U) != 0) {
        (void)fprintf(stderr, "max payload: missing client id prefix\n");
        status = -1;
    } else if (memcmp(recv_buf + 4U, send_buf, payload_len) != 0) {
        (void)fprintf(stderr, "max payload: echoed data mismatch\n");
        status = -1;
    }

    (void)close(fd);
    free(send_buf);
    free(recv_buf);

    if (status == 0) {
        (void)printf("max payload test passed\n");
    }
    return status;
}

/**
 * @brief Test that an oversized frame header causes the server to close.
 *
 * @param[in] path Socket path.
 * @return 0 on success, -1 on error.
 */
static int test_oversized_frame(const char *path)
{
    int fd;
    uint8_t header[IPC_HEADER_SIZE];
    ssize_t rc;

    fd = connect_to_server(path);
    if (fd < 0) {
        (void)perror("connect");
        return -1;
    }

    ipc_frame_encode_header(header, IPC_MAX_PAYLOAD + 1U);
    if (ipc_write_exact(fd, header, IPC_HEADER_SIZE) < 0) {
        (void)close(fd);
        return -1;
    }

    /* The server should close the connection. A subsequent read should
     * return 0 bytes. */
    rc = ipc_read_exact(fd, header, IPC_HEADER_SIZE);
    if (rc != 0) {
        (void)fprintf(stderr,
                      "oversized frame: expected disconnect, got %zd\n", rc);
        (void)close(fd);
        return -1;
    }

    (void)close(fd);
    (void)printf("oversized frame test passed\n");
    return 0;
}

int
main(int argc, char **argv)
{
    const char *path;

    (void)argc;
    (void)argv;

    path = ipc_socket_path();

    if (test_empty_payload(path) < 0) {
        return EXIT_FAILURE;
    }
    if (test_max_payload(path) < 0) {
        return EXIT_FAILURE;
    }
    if (test_oversized_frame(path) < 0) {
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}

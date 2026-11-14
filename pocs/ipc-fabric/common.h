/**
 * @file common.h
 * @brief Shared definitions for the IPC fabric PoC.
 *
 * This header defines the framing protocol, socket path, and helper
 * functions used by both the server and client executables.
 */

#ifndef COG_POC_IPC_FABRIC_COMMON_H
#define COG_POC_IPC_FABRIC_COMMON_H

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Default Unix domain socket path. */
#define COG_IPC_SOCKET_PATH "/tmp/cog-ipc-poc.sock"

/** Environment variable that can override ::COG_IPC_SOCKET_PATH. */
#define COG_IPC_SOCKET_ENV "COG_IPC_SOCKET"

/** Maximum payload size in a single frame (65535 bytes). */
#define IPC_MAX_PAYLOAD (UINT16_MAX)

/** Size of the frame header, in bytes. */
#define IPC_HEADER_SIZE 4U

/**
 * @brief Encode a payload length into a 4-byte big-endian header.
 *
 * @param[out] header 4-byte buffer to receive the encoded length.
 * @param[in]  len    Payload length; must be <= ::IPC_MAX_PAYLOAD.
 */
void ipc_frame_encode_header(uint8_t header[IPC_HEADER_SIZE], uint32_t len);

/**
 * @brief Decode a 4-byte big-endian header into a host payload length.
 *
 * @param[in] header 4-byte buffer containing the encoded length.
 * @return Decoded payload length.
 */
uint32_t ipc_frame_decode_header(const uint8_t header[IPC_HEADER_SIZE]);

/**
 * @brief Read exactly @p n bytes from a file descriptor.
 *
 * Blocks until @p n bytes have been read, the peer closes the connection,
 * or an error occurs.
 *
 * @param[in] fd File descriptor to read from.
 * @param[out] buf Buffer to read into.
 * @param[in] n Number of bytes to read.
 * @return Number of bytes read, or -1 on error. A return value less than
 *         @p n indicates the peer closed the connection.
 */
ssize_t ipc_read_exact(int fd, void *buf, size_t n);

/**
 * @brief Write exactly @p n bytes to a file descriptor.
 *
 * Loops until all bytes are written or an unrecoverable error occurs.
 *
 * @param[in] fd File descriptor to write to.
 * @param[in] buf Buffer to write from.
 * @param[in] n Number of bytes to write.
 * @return 0 on success, or -1 on error.
 */
int ipc_write_exact(int fd, const void *buf, size_t n);

/**
 * @brief Resolve the socket path to use.
 *
 * Returns the value of the ::COG_IPC_SOCKET_ENV environment variable if it
 * is set and non-empty, otherwise ::COG_IPC_SOCKET_PATH.
 *
 * @return Pointer to a static string; must not be freed.
 */
const char *ipc_socket_path(void);

#ifdef __cplusplus
}
#endif

#endif /* COG_POC_IPC_FABRIC_COMMON_H */

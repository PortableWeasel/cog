/**
 * @file common.h
 * @brief Shared definitions for the process-orchestration PoC.
 *
 * This header defines the framing protocol, message vocabulary, and
 * helpers used by both the orchestrator (main) mode and the worker
 * (auxiliary core) mode of the `proc-orch` executable.
 *
 * Framing reuses the PoC 1 scheme: a 4-byte big-endian payload length
 * followed by the payload. Messages in this PoC are small control
 * messages, so the payload is capped at ::ORCH_MAX_MESSAGE bytes.
 */

#ifndef COG_POC_PROC_ORCH_COMMON_H
#define COG_POC_PROC_ORCH_COMMON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Size of the frame header, in bytes. */
#define ORCH_HEADER_SIZE 4U

/** Maximum payload size for a single message (control messages are small). */
#define ORCH_MAX_MESSAGE 1024U

/** Default number of children to spawn. */
#define ORCH_DEFAULT_CHILDREN 3U

/** Minimum and maximum number of children accepted. */
#define ORCH_MIN_CHILDREN 1U
#define ORCH_MAX_CHILDREN 16U

/** Default heartbeat interval in milliseconds. */
#define ORCH_DEFAULT_INTERVAL_MS 250U

/** Heartbeat timeout is this multiple of the interval. */
#define ORCH_TIMEOUT_MULTIPLIER 3U

/** Environment variable overrides for the demo. */
#define ORCH_CHILDREN_ENV "COG_ORCH_CHILDREN"
#define ORCH_INTERVAL_ENV "COG_ORCH_INTERVAL_MS"

/** Message-mode argv markers. */
#define ORCH_MODE_ORCHESTRATOR "orchestrator"
#define ORCH_MODE_WORKER "worker"

/**
 * @brief Encode a payload length into a 4-byte big-endian header.
 *
 * @param[out] header 4-byte buffer to receive the encoded length.
 * @param[in]  len    Payload length; must be <= ::ORCH_MAX_MESSAGE.
 */
void orch_frame_encode_header(uint8_t header[ORCH_HEADER_SIZE], uint32_t len);

/**
 * @brief Decode a 4-byte big-endian header into a host payload length.
 *
 * @param[in] header 4-byte buffer containing the encoded length.
 * @return Decoded payload length.
 */
uint32_t orch_frame_decode_header(const uint8_t header[ORCH_HEADER_SIZE]);

/**
 * @brief Send one framed message over a stream socket.
 *
 * @param[in] fd    Connected stream socket.
 * @param[in] msg   Message payload (may be NULL if len is 0).
 * @param[in] len   Payload length; must be <= ::ORCH_MAX_MESSAGE.
 * @return 0 on success, -1 on error (including oversized messages).
 */
int orch_send_message(int fd, const char *msg, size_t len);

/**
 * @brief Per-connection frame parser state.
 *
 * Stream sockets deliver bytes with no message boundaries, so each
 * receiver keeps one of these to reassemble complete frames from
 * arbitrary read chunk boundaries.
 */
struct orch_frame_reader {
    uint8_t header[ORCH_HEADER_SIZE];
    size_t header_len;
    /* One extra byte so a max-size (ORCH_MAX_MESSAGE) payload can hold the
     * NUL terminator written at payload[payload_len] without overflowing. */
    uint8_t payload[ORCH_MAX_MESSAGE + 1U];
    size_t payload_len;
    size_t payload_received;
    bool in_payload;
};

/**
 * @brief Callback invoked for each complete message found by the reader.
 *
 * @param[in] ctx Opaque context passed to ::orch_frame_reader_feed.
 * @param[in] msg NUL-terminated message payload (payload never contains
 *                NUL bytes in this PoC).
 * @param[in] len Length of @p msg.
 * @return 0 to continue, nonzero to abort parsing.
 */
typedef int (*orch_message_fn)(void *ctx, const char *msg, size_t len);

/**
 * @brief Reset a frame reader to the header-reading state.
 *
 * @param[out] r Reader to reset.
 */
void orch_frame_reader_reset(struct orch_frame_reader *r);

/**
 * @brief Feed raw bytes into a frame reader.
 *
 * Parses as many complete frames as @p data contains and invokes @p fn
 * for each. Incomplete frames are retained in @p r for the next call.
 *
 * @param[in,out] r    Reader state.
 * @param[in]     data Raw bytes just read from the socket.
 * @param[in]     n    Number of bytes in @p data.
 * @param[in]     fn   Callback for complete messages.
 * @param[in]     ctx  Context passed to @p fn.
 * @return 0 on success, -1 on a protocol violation (oversized frame or a
 *         nonzero callback return). After -1 the stream is unusable.
 */
int orch_frame_reader_feed(struct orch_frame_reader *r, const uint8_t *data,
                           size_t n, orch_message_fn fn, void *ctx);

/**
 * @brief Current monotonic time in milliseconds.
 *
 * @return Milliseconds since an unspecified epoch (CLOCK_MONOTONIC).
 */
uint64_t orch_now_ms(void);

/**
 * @brief Entry point for orchestrator (main process) mode.
 *
 * @param[in] argc Argument count.
 * @param[in] argv Argument vector.
 * @return Exit status.
 */
int orchestrator_run(int argc, char **argv);

/**
 * @brief Entry point for auxiliary-core (worker) mode.
 *
 * @param[in] argc Argument count.
 * @param[in] argv Argument vector.
 * @return Exit status.
 */
int worker_run(int argc, char **argv);

#ifdef __cplusplus
}
#endif

#endif /* COG_POC_PROC_ORCH_COMMON_H */

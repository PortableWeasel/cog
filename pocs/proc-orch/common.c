/**
 * @file common.c
 * @brief Shared implementation for the process-orchestration PoC.
 */

#define _POSIX_C_SOURCE 200809L

#include "common.h"

#include <errno.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

void
orch_frame_encode_header(uint8_t header[ORCH_HEADER_SIZE], uint32_t len)
{
    header[0] = (uint8_t)((len >> 24) & 0xFFU);
    header[1] = (uint8_t)((len >> 16) & 0xFFU);
    header[2] = (uint8_t)((len >> 8) & 0xFFU);
    header[3] = (uint8_t)(len & 0xFFU);
}

uint32_t
orch_frame_decode_header(const uint8_t header[ORCH_HEADER_SIZE])
{
    uint32_t len = 0U;

    len |= ((uint32_t)header[0]) << 24;
    len |= ((uint32_t)header[1]) << 16;
    len |= ((uint32_t)header[2]) << 8;
    len |= ((uint32_t)header[3]);

    return len;
}

/**
 * @brief Write all bytes to a descriptor, retrying on partial writes.
 *
 * @param[in] fd  Descriptor to write to.
 * @param[in] buf Data to write.
 * @param[in] n   Number of bytes to write.
 * @return 0 on success, -1 on error.
 */
static int
write_all(int fd, const void *buf, size_t n)
{
    const char *cursor = buf;
    size_t remaining = n;

    while (remaining > 0U) {
        ssize_t rc = write(fd, cursor, remaining);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        cursor += (size_t)rc;
        remaining -= (size_t)rc;
    }

    return 0;
}

int
orch_send_message(int fd, const char *msg, size_t len)
{
    uint8_t header[ORCH_HEADER_SIZE];

    if (len > ORCH_MAX_MESSAGE) {
        return -1;
    }

    orch_frame_encode_header(header, (uint32_t)len);

    /* Write header and payload back-to-back; the receiver re-assembles
     * partial reads via orch_frame_reader regardless of chunking. */
    if (write_all(fd, header, ORCH_HEADER_SIZE) < 0) {
        return -1;
    }
    if ((len > 0U) && (write_all(fd, msg, len) < 0)) {
        return -1;
    }

    return 0;
}

void
orch_frame_reader_reset(struct orch_frame_reader *r)
{
    r->header_len = 0U;
    r->payload_len = 0U;
    r->payload_received = 0U;
    r->in_payload = false;
}

int
orch_frame_reader_feed(struct orch_frame_reader *r, const uint8_t *data,
                       size_t n, orch_message_fn fn, void *ctx)
{
    for (size_t i = 0U; i < n; i++) {
        uint8_t byte = data[i];

        if (!r->in_payload) {
            r->header[r->header_len++] = byte;
            if (r->header_len == ORCH_HEADER_SIZE) {
                uint32_t len = orch_frame_decode_header(r->header);

                if ((len == 0U) || (len > ORCH_MAX_MESSAGE)) {
                    /* Empty frames are not used by this protocol, and a
                     * length beyond the cap is a protocol violation. */
                    return -1;
                }
                r->payload_len = (size_t)len;
                r->payload_received = 0U;
                r->in_payload = true;
            }
        } else {
            r->payload[r->payload_received++] = byte;
            if (r->payload_received == r->payload_len) {
                r->payload[r->payload_len] = '\0';
                if (fn(ctx, (const char *)r->payload, r->payload_len) != 0) {
                    return -1;
                }
                orch_frame_reader_reset(r);
            }
        }
    }

    return 0;
}

uint64_t
orch_now_ms(void)
{
    struct timespec ts;

    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((uint64_t)ts.tv_sec * 1000U) + ((uint64_t)ts.tv_nsec / 1000000U);
}

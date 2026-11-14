/**
 * @file reader-overflow-test.c
 * @brief Regression test: feeding a max-size (ORCH_MAX_MESSAGE) frame
 *        through the frame reader must not write past the payload buffer.
 *
 * The reader's payload buffer is ORCH_MAX_MESSAGE bytes and the NUL
 * terminator is written at payload[payload_len]. A payload of exactly
 * ORCH_MAX_MESSAGE bytes would write one byte past the end. This test
 * drives that path and relies on ASAN to flag the overflow.
 */

#define _POSIX_C_SOURCE 200809L

#include "common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int
count_messages(void *ctx, const char *msg, size_t len)
{
    size_t *n = ctx;
    (void)msg;
    (void)len;
    (*n)++;
    return 0;
}

int
main(void)
{
    struct orch_frame_reader r;
    uint8_t *frame;
    size_t payload_len = ORCH_MAX_MESSAGE;
    size_t frame_len = ORCH_HEADER_SIZE + payload_len;
    size_t messages = 0U;
    int rc;

    orch_frame_reader_reset(&r);

    frame = malloc(frame_len);
    if (frame == NULL) {
        return 2;
    }

    orch_frame_encode_header(frame, (uint32_t)payload_len);
    (void)memset(frame + ORCH_HEADER_SIZE, 'x', payload_len);

    rc = orch_frame_reader_feed(&r, frame, frame_len, count_messages,
                                &messages);

    free(frame);

    if (rc != 0) {
        (void)fprintf(stderr, "reader returned error %d\n", rc);
        return 1;
    }
    if (messages != 1U) {
        (void)fprintf(stderr, "expected 1 message, got %zu\n", messages);
        return 1;
    }

    (void)printf("reader-overflow-test passed\n");
    return 0;
}

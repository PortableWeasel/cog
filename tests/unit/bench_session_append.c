/**
 * @file bench_session_append.c
 * @brief Repeatable scaling benchmark for the primary session append path.
 *
 * Ticket 0f055f5 / review 14519f8 revision 1.  This is deliberately not a
 * timing assertion in the unit suite: it prints three repetitions at three
 * sizes so super-linear behavior is visible without making CI host-speed
 * dependent.  Session destruction is outside the measured region.
 *
 * Build and run from the repository root:
 *   gcc -std=c11 -O2 -DNDEBUG -I libraries/cog-core/include \
 *       -I libraries/cog-ipc/include libraries/cog-core/src/session.c \
 *       tests/unit/bench_session_append.c -o /tmp/bench_session_append
 *   /tmp/bench_session_append
 */
#define _POSIX_C_SOURCE 200809L

#include "cog/session.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#define BENCH_REPEATS 3U

static uint64_t monotonic_ns(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0U;
    }
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}

static int run_append(size_t count, size_t max_nodes, uint64_t *elapsed_ns)
{
    static const cog_message_t msg = {
        COG_MESSAGE_ROLE_USER, "x", 1U, NULL
    };
    cog_session_config_t cfg = {NULL, max_nodes};
    cog_session_t *s = NULL;
    cog_session_node_t *active;
    uint64_t start;
    uint64_t end;
    size_t i;
    int rc = 1;

    if (cog_session_new(&s, &cfg) != COG_OK) {
        return 1;
    }
    start = monotonic_ns();
    if (start == 0U) {
        goto done;
    }
    for (i = 0U; i < count; ++i) {
        if (cog_session_append(s, &msg, NULL) != COG_OK) {
            goto done;
        }
    }
    end = monotonic_ns();
    if (end == 0U || end < start) {
        goto done;
    }
    active = cog_session_active(s);
    if (active == NULL || cog_node_data(active)->node_id != (uint64_t)count) {
        goto done;
    }
    *elapsed_ns = end - start;
    rc = 0;

done:
    cog_session_free(s);
    return rc;
}

int main(void)
{
    static const size_t sizes[] = {10000U, 20000U, 40000U};
    static const size_t caps[] = {0U, 64U}; /* default cap, small cap */
    size_t ci;
    size_t si;
    size_t repeat;

    /* Warm up allocator and instruction paths before reporting samples. */
    {
        uint64_t ignored = 0U;
        if (run_append(1000U, 0U, &ignored) != 0) {
            (void)fprintf(stderr, "benchmark warm-up failed\n");
            return 1;
        }
    }

    (void)puts("max_nodes appends repeat elapsed_ms ns_per_append");
    for (ci = 0U; ci < sizeof(caps) / sizeof(caps[0]); ++ci) {
        for (si = 0U; si < sizeof(sizes) / sizeof(sizes[0]); ++si) {
            for (repeat = 0U; repeat < BENCH_REPEATS; ++repeat) {
                uint64_t elapsed = 0U;
                double elapsed_ms;
                double per_append;

                if (run_append(sizes[si], caps[ci], &elapsed) != 0) {
                    (void)fprintf(stderr, "benchmark run failed\n");
                    return 1;
                }
                elapsed_ms = (double)elapsed / 1000000.0;
                per_append = (double)elapsed / (double)sizes[si];
                (void)printf("%9zu %8zu %6zu %10.3f %13.1f\n",
                             caps[ci], sizes[si], repeat + 1U,
                             elapsed_ms, per_append);
            }
        }
    }
    return 0;
}

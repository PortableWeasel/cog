/**
 * @file worker.c
 * @brief Auxiliary-core (worker) mode for the process-orchestration PoC.
 *
 * The worker is exec'd by the orchestrator with a single inherited
 * socketpair descriptor. It sends a heartbeat frame to the orchestrator
 * every ::ORCH_DEFAULT_INTERVAL_MS milliseconds and responds to a
 * `spawn subagent <n>` request with an `ack <n>` frame, proving that a
 * spawned auxiliary core can both report health and acknowledge workload
 * assignments.
 *
 * Usage (invoked by the orchestrator):
 *
 *     proc-orch worker --id <id> --fd <fd> --interval <ms>
 *                      [--lazy-after <count>]
 *
 * `--lazy-after count` sends exactly @p count heartbeats and then stops
 * (while remaining responsive to requests), which lets the orchestrator
 * demo heartbeat-timeout detection deterministically.
 */

#define _POSIX_C_SOURCE 200809L

#include "common.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/** Maximum number of messages read in one poll wake-up. */
#define WORKER_READ_CHUNK 1024U

/** Working state of one worker process. */
struct worker {
    uint32_t id;
    int fd;
    uint64_t heartbeat_interval_ms;
    uint64_t next_heartbeat_ms;
    uint64_t seq;
    long lazy_after;
    bool lazy_reached;
    struct orch_frame_reader reader;
};

/**
 * @brief Parse a worker-mode argument vector.
 *
 * Accepts `--id <n>`, `--fd <n>`, `--interval <n>`, and `--lazy-after <n>`.
 *
 * @param[in] argc Argument count (excluding the mode word).
 * @param[in] argv Argument vector.
 * @param[out] id Worker id.
 * @param[out] fd Inherited socket descriptor.
 * @param[out] interval_ms Heartbeat interval.
 * @param[out] lazy_after Heartbeats before going quiet (-1 = never).
 * @return 0 on success, -1 on a missing or malformed option.
 */
static int
parse_args(int argc, char **argv, uint32_t *id, int *fd,
           uint64_t *interval_ms, long *lazy_after)
{
    int i = 0;

    *lazy_after = -1L;

    while (i < argc) {
        const char *arg = argv[i];

        if ((strcmp(arg, "--id") == 0) && (i + 1 < argc)) {
            char *end = NULL;
            long v = strtol(argv[i + 1], &end, 10);
            if ((end == argv[i + 1]) || (*end != '\0') || (v < 1L)) {
                return -1;
            }
            *id = (uint32_t)v;
            i += 2;
        } else if ((strcmp(arg, "--fd") == 0) && (i + 1 < argc)) {
            char *end = NULL;
            long v = strtol(argv[i + 1], &end, 10);
            if ((end == argv[i + 1]) || (*end != '\0') || (v < 0L)) {
                return -1;
            }
            *fd = (int)v;
            i += 2;
        } else if ((strcmp(arg, "--interval") == 0) && (i + 1 < argc)) {
            char *end = NULL;
            long v = strtol(argv[i + 1], &end, 10);
            if ((end == argv[i + 1]) || (*end != '\0') || (v <= 0L)) {
                return -1;
            }
            *interval_ms = (uint64_t)v;
            i += 2;
        } else if ((strcmp(arg, "--lazy-after") == 0) && (i + 1 < argc)) {
            char *end = NULL;
            long v = strtol(argv[i + 1], &end, 10);
            if ((end == argv[i + 1]) || (*end != '\0') || (v < 0L)) {
                return -1;
            }
            *lazy_after = v;
            i += 2;
        } else {
            (void)fprintf(stderr, "worker %u: unknown argument '%s'\n", *id,
                          arg);
            return -1;
        }
    }

    return 0;
}

/**
 * @brief Handle a complete message received from the orchestrator.
 *
 * Responds to `spawn subagent <n>` with `ack <n>`.
 *
 * @param[in] ctx Worker state.
 * @param[in] msg Message payload.
 * @param[in] len Message length.
 * @return 0 on success, -1 on error.
 */
static int
worker_on_message(void *ctx, const char *msg, size_t len)
{
    struct worker *w = ctx;

    (void)len;

    if (strncmp(msg, "spawn subagent ", 15U) == 0) {
        char *end = NULL;
        long n = strtol(msg + 15, &end, 10);
        char ack[64];
        int written;

        if ((end == msg + 15) || (*end != '\0') || (n < 0L)) {
            (void)fprintf(stderr, "worker %u: malformed spawn request '%s'\n",
                          w->id, msg);
            return -1;
        }

        written = snprintf(ack, sizeof(ack), "ack %ld", n);
        if (written < 0) {
            return -1;
        }
        if (orch_send_message(w->fd, ack, (size_t)written) < 0) {
            return -1;
        }
    }
    /* Unknown messages are ignored; the protocol is intentionally small. */

    return 0;
}

/**
 * @brief Send the next heartbeat if one is due.
 *
 * @param[in,out] w Worker state.
 * @param[in] now Current monotonic time in milliseconds.
 * @return 0 on success, -1 on error.
 */
static int
worker_maybe_heartbeat(struct worker *w, uint64_t now)
{
    if (now < w->next_heartbeat_ms) {
        return 0;
    }
    if (w->lazy_reached) {
        /* Quiet mode: stay alive and responsive, but go silent. */
        w->next_heartbeat_ms = now + w->heartbeat_interval_ms;
        return 0;
    }

    {
        char hb[64];
        int written;

        written = snprintf(hb, sizeof(hb), "heartbeat %u %llu", w->id,
                           (unsigned long long)(w->seq + 1U));
        if (written < 0) {
            return -1;
        }
        w->seq++;
        if (orch_send_message(w->fd, hb, (size_t)written) < 0) {
            return -1;
        }

        if ((w->lazy_after >= 0L) &&
            ((long)w->seq >= w->lazy_after)) {
            w->lazy_reached = true;
        }
    }

    w->next_heartbeat_ms = now + w->heartbeat_interval_ms;
    return 0;
}

int
worker_run(int argc, char **argv)
{
    struct worker w;
    uint64_t now;

    (void)memset(&w, 0, sizeof(w));
    w.fd = -1;
    w.heartbeat_interval_ms = ORCH_DEFAULT_INTERVAL_MS;

    if (parse_args(argc, argv, &w.id, &w.fd, &w.heartbeat_interval_ms,
                   &w.lazy_after) < 0) {
        (void)fprintf(stderr, "worker: bad arguments\n");
        return EXIT_FAILURE;
    }
    if (w.fd < 0) {
        (void)fprintf(stderr, "worker: missing --fd\n");
        return EXIT_FAILURE;
    }

    orch_frame_reader_reset(&w.reader);
    now = orch_now_ms();
    w.next_heartbeat_ms = now + w.heartbeat_interval_ms;

    for (;;) {
        struct pollfd pfd;
        uint8_t buf[WORKER_READ_CHUNK];
        ssize_t rc;

        now = orch_now_ms();

        pfd.fd = w.fd;
        pfd.events = POLLIN;
        pfd.revents = 0;

        if (now < w.next_heartbeat_ms) {
            int timeout_ms = (int)(w.next_heartbeat_ms - now);
            rc = poll(&pfd, 1, timeout_ms);
        } else {
            rc = poll(&pfd, 1, 0);
        }
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            (void)perror("worker poll");
            return EXIT_FAILURE;
        }

        now = orch_now_ms();

        if (worker_maybe_heartbeat(&w, now) < 0) {
            return EXIT_FAILURE;
        }

        if ((pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            /* Orchestrator closed the connection or went away. */
            break;
        }

        if ((pfd.revents & POLLIN) != 0) {
            rc = read(w.fd, buf, sizeof(buf));
            if (rc < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return EXIT_FAILURE;
            }
            if (rc == 0) {
                /* Clean EOF from the orchestrator. */
                break;
            }
            if (orch_frame_reader_feed(&w.reader, buf, (size_t)rc,
                                        worker_on_message, &w) < 0) {
                return EXIT_FAILURE;
            }
        }
    }

    (void)close(w.fd);
    return EXIT_SUCCESS;
}

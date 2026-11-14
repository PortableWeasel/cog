/**
 * @file orchestrator.c
 * @brief Main-process (orchestrator) mode for the process-orchestration PoC.
 *
 * The orchestrator:
 *
 *  1. spawns N auxiliary-core processes with fork(2)+execv(2), each child
 *     inheriting one end of a socketpair;
 *  2. monitors the children with poll(2), tracking per-child heartbeat
 *     deadlines on a monotonic clock;
 *  3. detects a child exit (socket EOF plus waitpid(2) reaping) and a
 *     heartbeat timeout (no heartbeat within 3x the interval);
 *  4. sends a `spawn subagent <id>` request to child 1 and waits for its
 *     `ack <id>`, proving two-way request/acknowledge flow.
 *
 * The run doubles as a deterministic demo: child 2 is SIGTERM'd to prove
 * exit detection, and child 3 runs with `--lazy-after 4` to prove
 * heartbeat-timeout detection while the process stays alive.
 */

#define _POSIX_C_SOURCE 200809L

#include "common.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

/** Read chunk size per child per poll wake-up. */
#define ORCH_READ_CHUNK 1024U

/** Abort the demo after this much wall time (defensive only). */
#define ORCH_SAFETY_DEADLINE_MS 30000U

/** One-sided poll timeout (ms) when no heartbeat deadline exists yet. */
#define ORCH_IDLE_POLL_MS 1000

/** Subagent id used by the demo spawn request. */
#define ORCH_DEMO_SUBAGENT_ID 1001

/** Child 3 goes quiet after this many heartbeats (demo of timeout). */
#define ORCH_LAZY_HEARTBEATS 4L

/** Per-child bookkeeping in the orchestrator. */
struct child {
    uint32_t id;
    pid_t pid;
    int fd;
    uint64_t last_hb_ms;       /**< Monotonic ms of the last heartbeat. */
    bool got_heartbeat;
    bool timed_out;
    bool reaped;
    bool socket_closed;
    struct orch_frame_reader reader;
    struct orchestrator *owner;
};

/** Orchestrator-wide state. */
struct orchestrator {
    struct child *children;
    size_t count;
    size_t capacity;
    uint64_t interval_ms;
    uint64_t timeout_ms;
    uint64_t start_ms;
    bool spawn_sent;
    bool ack_received;
    bool kill_sent;
    bool shutdown;
    bool spawn_failed;
};

/**
 * @brief Print one machine-readable event line to stdout.
 *
 * @param[in] fmt printf-style format.
 * @param[in] ... Format arguments.
 */
static void
log_event(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    (void)vprintf(fmt, ap);
    va_end(ap);
    (void)printf("\n");
    (void)fflush(stdout);
}

/**
 * @brief Parse the child count from the environment.
 *
 * Uses ::ORCH_CHILDREN_ENV if set and valid (1..16), otherwise
 * ::ORCH_DEFAULT_CHILDREN.
 *
 * @return Number of children to spawn.
 */
static size_t
parse_children_count(void)
{
    const char *env = getenv(ORCH_CHILDREN_ENV);
    size_t n = ORCH_DEFAULT_CHILDREN;

    if ((env != NULL) && (env[0] != '\0')) {
        char *end = NULL;
        long v = strtol(env, &end, 10);
        if ((end != env) && (*end == '\0') &&
            (v >= (long)ORCH_MIN_CHILDREN) &&
            (v <= (long)ORCH_MAX_CHILDREN)) {
            n = (size_t)v;
        } else {
            (void)fprintf(stderr,
                          "orchestrator: invalid %s='%s', using %u\n",
                          ORCH_CHILDREN_ENV, env, ORCH_DEFAULT_CHILDREN);
        }
    }

    return n;
}

/**
 * @brief Parse the heartbeat interval from the environment.
 *
 * Uses ::ORCH_INTERVAL_ENV if set and valid (50..10000 ms), otherwise
 * ::ORCH_DEFAULT_INTERVAL_MS.
 *
 * @return Heartbeat interval in milliseconds.
 */
static uint64_t
parse_interval_ms(void)
{
    const char *env = getenv(ORCH_INTERVAL_ENV);
    uint64_t ms = ORCH_DEFAULT_INTERVAL_MS;

    if ((env != NULL) && (env[0] != '\0')) {
        char *end = NULL;
        long v = strtol(env, &end, 10);
        if ((end != env) && (*end == '\0') && (v >= 50L) && (v <= 10000L)) {
            ms = (uint64_t)v;
        } else {
            (void)fprintf(stderr,
                          "orchestrator: invalid %s='%s', using %u\n",
                          ORCH_INTERVAL_ENV, env, ORCH_DEFAULT_INTERVAL_MS);
        }
    }

    return ms;
}

/**
 * @brief Grow the children table if full.
 *
 * @param[in,out] o Orchestrator.
 * @return 0 on success, -1 on allocation failure.
 */
static int
children_grow(struct orchestrator *o)
{
    size_t new_capacity;
    struct child *new_children;

    if (o->count < o->capacity) {
        return 0;
    }

    new_capacity = (o->capacity == 0U) ? 4U : o->capacity * 2U;
    new_children = realloc(o->children, new_capacity * sizeof(new_children[0]));
    if (new_children == NULL) {
        return -1;
    }

    (void)memset(&new_children[o->capacity], 0,
                 (new_capacity - o->capacity) * sizeof(new_children[0]));
    o->children = new_children;
    o->capacity = new_capacity;
    return 0;
}

/**
 * @brief Spawn one auxiliary-core child.
 *
 * Creates a socketpair, forks, and execs the current executable in worker
 * mode with the child's socket fd inherited (no CLOEXEC on that fd).
 *
 * @param[in,out] o      Orchestrator.
 * @param[in]     id     Child id (1-based).
 * @param[in]     lazy   If true, pass `--lazy-after N` so the child stops
 *                       heartbeating after N beats (for timeout demo).
 * @return 0 on success, -1 on failure.
 */
static int
spawn_child(struct orchestrator *o, uint32_t id, bool lazy)
{
    int sv[2];
    pid_t pid;
    char exe[512];
    ssize_t exe_len;
    char id_s[16];
    char fd_s[16];
    char iv_s[16];
    char lazy_s[16];
    char *args[12];
    int argn = 0;

    exe_len = readlink("/proc/self/exe", exe, sizeof(exe) - 1U);
    if (exe_len < 0) {
        (void)perror("readlink /proc/self/exe");
        return -1;
    }
    exe[(size_t)exe_len] = '\0';

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
        (void)perror("socketpair");
        return -1;
    }

    pid = fork();
    if (pid < 0) {
        (void)perror("fork");
        (void)close(sv[0]);
        (void)close(sv[1]);
        return -1;
    }

    if (pid == 0) {
        /* Child: keep only its own socket fd, then exec worker mode. */
        (void)close(sv[0]);
        for (size_t i = 0U; i < o->count; i++) {
            if (o->children[i].fd >= 0) {
                (void)close(o->children[i].fd);
            }
        }

        (void)snprintf(id_s, sizeof(id_s), "%u", id);
        (void)snprintf(fd_s, sizeof(fd_s), "%d", sv[1]);
        (void)snprintf(iv_s, sizeof(iv_s), "%llu",
                       (unsigned long long)o->interval_ms);

        args[argn++] = exe;
        args[argn++] = (char *)ORCH_MODE_WORKER;
        args[argn++] = (char *)"--id";
        args[argn++] = id_s;
        args[argn++] = (char *)"--fd";
        args[argn++] = fd_s;
        args[argn++] = (char *)"--interval";
        args[argn++] = iv_s;
        if (lazy) {
            (void)snprintf(lazy_s, sizeof(lazy_s), "%ld", ORCH_LAZY_HEARTBEATS);
            args[argn++] = (char *)"--lazy-after";
            args[argn++] = lazy_s;
        }
        args[argn] = NULL;

        (void)execv(exe, args);
        (void)fprintf(stderr, "child %u: execv failed: %s\n", id,
                      strerror(errno));
        _exit(127);
    }

    /* Parent: keep the listening end of the socket. */
    (void)close(sv[1]);

    if (children_grow(o) < 0) {
        (void)kill(pid, SIGKILL);
        (void)close(sv[0]);
        return -1;
    }

    {
        struct child *c = &o->children[o->count];
        c->id = id;
        c->pid = pid;
        c->fd = sv[0];
        c->last_hb_ms = 0U;
        c->got_heartbeat = false;
        c->timed_out = false;
        c->reaped = false;
        c->socket_closed = false;
        c->owner = o;
        orch_frame_reader_reset(&c->reader);
        o->count++;
    }

    log_event("spawned child=%u pid=%d", id, (int)pid);
    return 0;
}

/**
 * @brief Handle a complete message from a child.
 *
 * Recognises `heartbeat <id> <seq>` and `ack <subagent>` messages.
 *
 * @param[in] ctx Child that sent the message.
 * @param[in] msg Message payload.
 * @param[in] len Message length.
 * @return 0 on success, -1 on malformed input.
 */
static int
on_message(void *ctx, const char *msg, size_t len)
{
    struct child *c = ctx;
    struct orchestrator *o = c->owner;

    (void)len;

    if (strncmp(msg, "heartbeat ", 10U) == 0) {
        const char *rest = msg + 10U;
        char *end = NULL;
        unsigned long cid = strtoul(rest, &end, 10);

        if ((end == rest) || (*end != ' ')) {
            return -1;
        }

        {
            char *end2 = NULL;
            unsigned long seq = strtoul(end + 1U, &end2, 10);
            if ((end2 == end + 1U) || (*end2 != '\0')) {
                return -1;
            }
            log_event("heartbeat child=%lu seq=%lu", cid, seq);
        }

        c->got_heartbeat = true;
        c->last_hb_ms = orch_now_ms();
    } else if (strncmp(msg, "ack ", 4U) == 0) {
        char *end = NULL;
        unsigned long n = strtoul(msg + 4U, &end, 10);
        if ((end == msg + 4U) || (*end != '\0')) {
            return -1;
        }
        log_event("ack child=%u subagent=%lu", c->id, n);
        if (c->id == 1U) {
            o->ack_received = true;
        }
    }
    /* Unrecognised messages are ignored; the protocol is intentionally
     * small and grows with the real core. */

    return 0;
}

/**
 * @brief Read and parse all pending data from one child.
 *
 * @param[in,out] o Orchestrator.
 * @param[in,out] c Child to read from.
 * @return 0 if the child is still healthy, -1 if it should be dropped.
 */
static int
handle_child_data(struct orchestrator *o, struct child *c)
{
    uint8_t buf[ORCH_READ_CHUNK];
    ssize_t rc;

    (void)o;

    /* Drain everything currently buffered without ever blocking on a
     * read: poll() already told us data is available, and recv with
     * MSG_DONTWAIT returns EAGAIN once the socket is drained. */
    for (;;) {
        rc = recv(c->fd, buf, sizeof(buf), MSG_DONTWAIT);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            /* EWOULDBLOCK == EAGAIN on Linux. */
            if (errno == EAGAIN) {
                break;
            }
            return -1;
        }
        if (rc == 0) {
            return -1;
        }
        if (orch_frame_reader_feed(&c->reader, buf, (size_t)rc, on_message,
                                   c) < 0) {
            return -1;
        }
    }

    return 0;
}

/**
 * @brief Drop a child's socket after EOF/error and note the close.
 *
 * @param[in,out] c Child whose socket closed.
 */
static void
close_child_socket(struct child *c)
{
    if (c->fd >= 0) {
        (void)close(c->fd);
        c->fd = -1;
        c->socket_closed = true;
        log_event("closed child=%u", c->id);
    }
}

/**
 * @brief Reap any exited children that have not been reaped yet.
 *
 * @param[in,out] o Orchestrator.
 */
static void
reap_pending(struct orchestrator *o)
{
    for (size_t i = 0U; i < o->count; i++) {
        struct child *c = &o->children[i];
        int status;

        if (c->reaped) {
            continue;
        }

        {
            pid_t r = waitpid(c->pid, &status, WNOHANG);
            if (r == c->pid) {
                c->reaped = true;
                if (WIFEXITED(status)) {
                    log_event("exit child=%u code=%d", c->id,
                              WEXITSTATUS(status));
                } else if (WIFSIGNALED(status)) {
                    log_event("exit child=%u signal=%d", c->id,
                              WTERMSIG(status));
                } else {
                    log_event("exit child=%u", c->id);
                }
                if (!c->got_heartbeat) {
                    o->spawn_failed = true;
                    log_event("spawn-failed child=%u", c->id);
                }
            } else if (r < 0) {
                /* ECHILD should not happen for our own children. */
                c->reaped = true;
            }
        }
    }
}

/**
 * @brief Build the pollfd array from children with open sockets.
 *
 * @param[in]  o       Orchestrator.
 * @param[out] fds     Poll array (size must be >= count).
 * @param[out] mapping Maps pollfd index to child index.
 * @return Number of pollfds filled.
 */
static size_t
build_poll_fds(const struct orchestrator *o, struct pollfd *fds,
               size_t *mapping)
{
    size_t n = 0U;

    for (size_t i = 0U; i < o->count; i++) {
        if (o->children[i].fd >= 0) {
            fds[n].fd = o->children[i].fd;
            fds[n].events = POLLIN;
            fds[n].revents = 0;
            mapping[n] = i;
            n++;
        }
    }

    return n;
}

/**
 * @brief Check whether every scenario step for this child count is done.
 *
 * @param[in] o Orchestrator.
 * @return True if the demo has exercised everything required for its
 *         child count (spawn/ack always; kill/exit for count >= 2;
 *         heartbeat timeout for count >= 3).
 */
static bool
scenario_complete(const struct orchestrator *o)
{
    if (!o->spawn_sent || !o->ack_received) {
        return false;
    }
    if ((o->count >= 2U) && (!o->kill_sent || !o->children[1].reaped)) {
        return false;
    }
    if ((o->count >= 3U) && !o->children[2].timed_out) {
        return false;
    }
    return true;
}

/**
 * @brief Advance the demo scenario state machine.
 *
 * Sends the spawn-subagent request after every child has heartbeated at
 * least once, and SIGTERMs child 2 once child 1 acknowledged.
 *
 * @param[in,out] o Orchestrator.
 */
static void
run_scenario_steps(struct orchestrator *o)
{
    bool all_heartbeated = true;

    for (size_t i = 0U; i < o->count; i++) {
        if (!o->children[i].got_heartbeat) {
            all_heartbeated = false;
            break;
        }
    }

    if (!o->spawn_sent && all_heartbeated && (o->count >= 1U)) {
        char req[64];
        int written = snprintf(req, sizeof(req), "spawn subagent %d",
                               ORCH_DEMO_SUBAGENT_ID);

        if ((written > 0) &&
            (orch_send_message(o->children[0].fd, req, (size_t)written) >= 0)) {
            o->spawn_sent = true;
            log_event("spawn-sent child=%u subagent=%d", o->children[0].id,
                      ORCH_DEMO_SUBAGENT_ID);
        }
    }

    if (!o->kill_sent && o->spawn_sent && o->ack_received &&
        (o->count >= 2U)) {
        if (kill(o->children[1].pid, SIGTERM) == 0) {
            o->kill_sent = true;
            log_event("kill child=%u", o->children[1].id);
        }
    }
}

/**
 * @brief Send SIGTERM to every child still running and close sockets.
 *
 * @param[in,out] o Orchestrator.
 */
static void
shutdown_children(struct orchestrator *o)
{
    for (size_t i = 0U; i < o->count; i++) {
        struct child *c = &o->children[i];

        if (!c->reaped) {
            (void)kill(c->pid, SIGTERM);
            if (c->fd >= 0) {
                log_event("stopping child=%u", c->id);
                (void)close(c->fd);
                c->fd = -1;
                c->socket_closed = true;
            }
        }
    }
    o->shutdown = true;
}

int
orchestrator_run(int argc, char **argv)
{
    struct orchestrator o;
    struct pollfd *fds;
    size_t *mapping;
    bool scenario_ok;
    int rc = EXIT_SUCCESS;

    (void)argc;
    (void)argv;

    (void)memset(&o, 0, sizeof(o));
    o.count = 0U;
    o.capacity = 0U;
    o.interval_ms = parse_interval_ms();
    o.timeout_ms = o.interval_ms * ORCH_TIMEOUT_MULTIPLIER;
    o.start_ms = orch_now_ms();

    /* Closed sockets at exit are not errors here. */
    (void)signal(SIGPIPE, SIG_IGN);

    log_event("orchestrator children=%llu interval=%llums timeout=%llums",
              (unsigned long long)parse_children_count(),
              (unsigned long long)o.interval_ms,
              (unsigned long long)o.timeout_ms);

    {
        size_t n = parse_children_count();

        for (size_t i = 0U; i < n; i++) {
            uint32_t id = (uint32_t)(i + 1U);
            /* Child 3 (demo) goes quiet after a few heartbeats so the
             * heartbeat-timeout path can be observed deterministically. */
            bool lazy = (n >= 3U) && (id == 3U);

            if (spawn_child(&o, id, lazy) < 0) {
                o.spawn_failed = true;
                break;
            }
        }
    }

    fds = calloc(o.count, sizeof(fds[0]));
    mapping = calloc(o.count, sizeof(mapping[0]));
    if ((fds == NULL) || (mapping == NULL)) {
        (void)fprintf(stderr, "orchestrator: out of memory\n");
        shutdown_children(&o);
        rc = EXIT_FAILURE;
        goto done;
    }

    for (;;) {
        uint64_t now;
        uint64_t deadline = 0U;
        bool have_deadline = false;
        int poll_timeout;
        size_t nfds;
        int prc;

        now = orch_now_ms();

        if (now - o.start_ms > ORCH_SAFETY_DEADLINE_MS) {
            (void)fprintf(stderr, "orchestrator: safety deadline reached\n");
            shutdown_children(&o);
            rc = EXIT_FAILURE;
            goto done;
        }

        reap_pending(&o);

        if (o.count > 0U) {
            bool all_done = true;
            for (size_t i = 0U; i < o.count; i++) {
                if (!o.children[i].reaped) {
                    all_done = false;
                    break;
                }
            }
            if (all_done) {
                break;
            }
        }

        run_scenario_steps(&o);

        /* Trigger shutdown once every required scenario step is done
         * (for N >= 3 this includes the lazy child's heartbeat timeout). */
        if (!o.shutdown && scenario_complete(&o)) {
            shutdown_children(&o);
            continue;
        }

        /* Compute the next heartbeat deadline across live children. */
        for (size_t i = 0U; i < o.count; i++) {
            struct child *c = &o.children[i];

            if ((c->fd >= 0) && c->got_heartbeat && !c->timed_out) {
                uint64_t d = c->last_hb_ms + o.timeout_ms;
                if (!have_deadline || (d < deadline)) {
                    deadline = d;
                    have_deadline = true;
                }
            }
        }

        if (have_deadline) {
            uint64_t diff = (deadline > now) ? (deadline - now) : 0U;
            poll_timeout = (diff > (uint64_t)ORCH_IDLE_POLL_MS)
                               ? (int)ORCH_IDLE_POLL_MS
                               : (int)diff;
        } else {
            poll_timeout = ORCH_IDLE_POLL_MS;
        }

        nfds = build_poll_fds(&o, fds, mapping);
        if (nfds == 0U) {
            /* Everything is closed; let reaping finish the loop. */
            continue;
        }

        prc = poll(fds, (nfds_t)nfds, poll_timeout);
        if (prc < 0) {
            if (errno == EINTR) {
                continue;
            }
            (void)perror("orchestrator poll");
            shutdown_children(&o);
            rc = EXIT_FAILURE;
            goto done;
        }

        now = orch_now_ms();

        /* Heartbeat timeouts first, so dead children are pruned before
         * any new data is attributed to them. */
        for (size_t i = 0U; i < o.count; i++) {
            struct child *c = &o.children[i];

            if ((c->fd >= 0) && c->got_heartbeat && !c->timed_out &&
                (now >= c->last_hb_ms + o.timeout_ms)) {
                c->timed_out = true;
                log_event("timeout child=%u", c->id);
                close_child_socket(c);
            }
        }

        /* Then drain ready sockets. */
        for (size_t k = 0U; k < nfds; k++) {
            struct child *c = &o.children[mapping[k]];
            short revents = fds[k].revents;

            if ((revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
                close_child_socket(c);
                continue;
            }

            if ((revents & POLLIN) != 0) {
                if (handle_child_data(&o, c) < 0) {
                    close_child_socket(c);
                }
            }
        }
    }

    scenario_ok = scenario_complete(&o);

    if (o.spawn_failed) {
        rc = EXIT_FAILURE;
    } else if (!scenario_ok) {
        rc = EXIT_FAILURE;
    } else {
        rc = EXIT_SUCCESS;
    }

    log_event("orchestrator done spawned=%zu spawn_failed=%d scenario_ok=%d",
              o.count, o.spawn_failed ? 1 : 0, scenario_ok ? 1 : 0);

done:
    free(mapping);
    free(fds);
    free(o.children);
    return rc;
}

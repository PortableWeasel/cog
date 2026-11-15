/**
 * @file orchestrator.c
 * @brief Process orchestrator: spawn, monitor, and place auxiliary cores.
 *
 * Implements the public contract declared in <cog/orchestrator.h>.  The
 * orchestrator lives only in the main core process.  It owns the IPC
 * listener socket, tracks auxiliary-core health, and decides where to
 * place new subagent workloads.
 *
 * Design notes
 * ------------
 * - The orchestrator binds its own listener socket (config->socket_path).
 *   Auxiliary cores connect back to it.  The main core's run loop drives
 *   the orchestrator through the private hooks in orchestrator_internal.h
 *   (cog_orchestrator_poll / cog_orchestrator_accept).
 * - Health is computed lazily from the last heartbeat timestamp and the
 *   configured interval/threshold; no timer thread is required.  A core
 *   that has never heartbeated is UNKNOWN; one that missed one interval
 *   is DEGRADED; one that missed the threshold is DEAD.  A reaped or
 *   disconnected child is always DEAD.
 * - Process ownership: every child spawned by cog_orchestrator_spawn is
 *   reaped with waitpid(WNOHANG) inside cog_orchestrator_poll.  Dead
 *   cores stay in the managed table (so cog_orchestrator_sessions can
 *   still report affected subagents during crash recovery) until they are
 *   replaced (auto_replace) or the orchestrator is freed.
 * - The wire protocol is newline-terminated text frames:
 *     aux -> orch : "hello <pid>", "heartbeat <active>", "session <id>",
 *                   "ack"
 *     orch -> aux : "shutdown"
 *   The "hello" message lets a spontaneously-connected core announce its
 *   OS pid (spawned cores already know theirs from fork(2)).
 *
 * Threading: not thread-safe.  All functions are called from the thread
 * that runs the main core's event loop.
 */

#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "cog/orchestrator.h"
#include "orchestrator_internal.h"

#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/** Maximum length of the stored socket path, including the NUL. */
#define COG_ORCH_PATH_MAX 128U

/** Maximum size of a control message frame. */
#define COG_ORCH_MSG_MAX 256U

/** Maximum sessions tracked per core (crash-recovery table). */
#define COG_ORCH_SESSIONS_PER_CORE 64U

/** How long spawn waits for the child to connect, in ms. */
#define COG_ORCH_SPAWN_TIMEOUT_MS 5000

/** How long stop waits for a shutdown acknowledgement, in ms. */
#define COG_ORCH_ACK_TIMEOUT_MS 2000

/** Defaults applied when a config field is zero. */
#define COG_ORCH_DEFAULT_MAX_AUX ((size_t)4)
#define COG_ORCH_DEFAULT_HEALTH_INTERVAL_MS ((uint32_t)1000)
#define COG_ORCH_DEFAULT_UNHEALTHY_THRESHOLD ((uint32_t)3)
#define COG_ORCH_DEFAULT_CAPACITY ((size_t)8)

/** Initial capacity of the managed-core table. */
#define COG_ORCH_CORES_INITIAL ((size_t)4)

/** Initial capacity of a core's session table. */
#define COG_ORCH_SESSIONS_INITIAL ((size_t)4)

/**
 * @brief One managed auxiliary-core record.
 */
struct cog_core_record {
    cog_peer_id_t peer_id;        /**< IPC peer id assigned by accept. */
    pid_t pid;                    /**< OS pid; -1 if unknown. */
    cog_health_t health;          /**< Cached health (refreshed lazily). */
    size_t active_subagents;      /**< Reported active subagents. */
    size_t capacity;              /**< Maximum subagent capacity. */
    uint64_t last_heartbeat_ms;   /**< Monotonic ms of last heartbeat. */
    bool has_heartbeat;           /**< True once a heartbeat is seen. */
    bool reaped;                  /**< Child process was reaped. */
    bool disconnected;            /**< Socket closed / recv failed. */
    bool acked;                   /**< Acknowledged a shutdown command. */
    bool shutting_down;           /**< Shutdown command sent, awaiting ack. */
    bool spawned;                 /**< True if created via spawn (owned pid). */
    bool replacing;               /**< Auto-replace in progress (guard). */
    char *binary_path;            /**< Owned copy; used for auto-replace. */
    cog_ipc_endpoint_t *conn;     /**< Accepted endpoint for this core. */
    char **sessions;              /**< Owned session-id strings. */
    size_t session_count;         /**< Live session entries. */
    size_t session_capacity;      /**< Allocated session slots. */
};

/**
 * @brief Orchestrator context.
 */
struct cog_orchestrator {
    cog_orchestrator_config_t config; /**< Effective configuration. */
    cog_ipc_endpoint_t *listener;     /**< Listener socket (owned). */
    struct cog_core_record *cores;    /**< Managed cores (owned array). */
    size_t core_count;                /**< Live core records. */
    size_t core_capacity;             /**< Allocated core slots. */
    uint64_t now_ms;                  /**< Cached monotonic clock. */
    bool clock_overridden;            /**< Test hook: fixed clock. */
    uint32_t spawn_timeout_ms;        /**< Spawn connect timeout. */
    uint32_t ack_timeout_ms;          /**< Shutdown-ack timeout. */
    char socket_path[COG_ORCH_PATH_MAX]; /**< Bound socket path. */
};

/* Forward declarations (defined at the bottom of this file). */
static cog_result_t wait_for_ack(cog_orchestrator_t *o,
                                 struct cog_core_record *c, int timeout_ms);
static void accept_pending(cog_orchestrator_t *o, int timeout_ms);

/* ------------------------------------------------------------------ */
/* Clock                                                               */
/* ------------------------------------------------------------------ */

/**
 * @brief Return the current monotonic time in milliseconds.
 */
static uint64_t
orch_now_ms(void)
{
    struct timespec ts;

    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000U +
           (uint64_t)(ts.tv_nsec / 1000000L);
}

/* ------------------------------------------------------------------ */
/* Core lookup and health                                              */
/* ------------------------------------------------------------------ */

/**
 * @brief Find a managed core by peer id.
 */
static struct cog_core_record *
find_core(cog_orchestrator_t *o, cog_peer_id_t peer_id)
{
    size_t i;

    for (i = 0U; i < o->core_count; i++) {
        if (o->cores[i].peer_id == peer_id) {
            return &o->cores[i];
        }
    }
    return NULL;
}

/**
 * @brief Compute a core's health from its heartbeat history.
 */
static cog_health_t
compute_health(const struct cog_core_record *c, uint64_t now_ms,
               uint32_t interval_ms, uint32_t threshold)
{
    uint64_t elapsed;
    uint64_t limit;

    if (c->reaped || c->disconnected) {
        return COG_HEALTH_DEAD;
    }
    if (!c->has_heartbeat) {
        return COG_HEALTH_UNKNOWN;
    }
    elapsed = now_ms - c->last_heartbeat_ms;
    if (elapsed < (uint64_t)interval_ms) {
        return COG_HEALTH_HEALTHY;
    }
    limit = (uint64_t)interval_ms * (uint64_t)threshold;
    if (elapsed < limit) {
        return COG_HEALTH_DEGRADED;
    }
    return COG_HEALTH_DEAD;
}

/* ------------------------------------------------------------------ */
/* Session table helpers                                              */
/* ------------------------------------------------------------------ */

/**
 * @brief Release a core's session table.
 */
static void
free_sessions(struct cog_core_record *c)
{
    size_t i;

    for (i = 0U; i < c->session_count; i++) {
        free(c->sessions[i]);
    }
    free(c->sessions);
    c->sessions = NULL;
    c->session_count = 0U;
    c->session_capacity = 0U;
}

/* ------------------------------------------------------------------ */
/* Message helpers                                                     */
/* ------------------------------------------------------------------ */

/**
 * @brief Send a NUL-terminated text frame on an endpoint.
 */
static cog_result_t
send_text(cog_ipc_endpoint_t *ep, const char *text)
{
    cog_ipc_message_t msg;
    cog_result_t rc;
    size_t len = strlen(text);

    rc = cog_ipc_message_new(&msg, len + 1U);
    if (rc != COG_OK) {
        return rc;
    }
    (void)memcpy(msg.data, text, len);
    msg.len = len;
    rc = cog_ipc_endpoint_send(ep, &msg);
    (void)cog_ipc_message_free(&msg);
    return rc;
}

/**
 * @brief Receive one text frame without blocking (poll first).
 *
 * @return COG_OK on success; COG_ERROR_AGAIN if nothing is ready;
 *         COG_ERROR_IO / COG_ERROR_OVERFLOW if the link failed.
 */
static cog_result_t
recv_text(cog_ipc_endpoint_t *ep, char *buf, size_t bufsize, size_t *out_len)
{
    cog_ipc_message_t msg;
    cog_result_t rc;
    int revents = 0;
    size_t n;

    rc = cog_ipc_endpoint_poll(ep, COG_IPC_POLL_IN, 0, &revents);
    if (rc != COG_OK) {
        return rc;
    }
    /* Do not bail on POLL_ERR: a peer that sent a frame and then closed
     * reports EOF while the buffered frame is still readable.  recv
     * returns COG_ERROR_IO once the buffer is exhausted, which the caller
     * maps to "disconnected". */
    rc = cog_ipc_message_new(&msg, bufsize);
    if (rc != COG_OK) {
        return rc;
    }
    rc = cog_ipc_endpoint_recv(ep, &msg);
    if (rc == COG_OK) {
        n = msg.len;
        if (n >= bufsize) {
            n = bufsize - 1U;
        }
        (void)memcpy(buf, msg.data, n);
        buf[n] = '\0';
        if (out_len != NULL) {
            *out_len = n;
        }
    }
    (void)cog_ipc_message_free(&msg);
    return rc;
}

/**
 * @brief Dispatch one received control message.
 */
static void
process_message(cog_orchestrator_t *o, struct cog_core_record *c, char *msg)
{
    size_t len = strlen(msg);
    const char *p;

    /* Trim trailing whitespace / newline so exact matches ("ack") and
     * value parsing (session ids) see a clean token. */
    while (len > 0U && (msg[len - 1U] == '\n' || msg[len - 1U] == '\r' ||
                        msg[len - 1U] == ' ' || msg[len - 1U] == '\t')) {
        msg[len - 1U] = '\0';
        len--;
    }

    if (strncmp(msg, "heartbeat", 9U) == 0) {
        unsigned long long v;
        size_t active;

        p = msg + 9U;
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        v = strtoull(p, NULL, 10);
        active = (size_t)v;
        (void)cog_orchestrator_heartbeat(o, c->peer_id, active);
    } else if (strncmp(msg, "session", 7U) == 0) {
        p = msg + 7U;
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (*p != '\0') {
            (void)cog_orchestrator_register_session(o, c->peer_id, p);
        }
    } else if (strncmp(msg, "hello", 5U) == 0) {
        long pid;

        p = msg + 5U;
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        pid = strtol(p, NULL, 10);
        if (pid > 0L) {
            c->pid = (pid_t)pid;
        }
    } else if (strcmp(msg, "ack") == 0) {
        c->acked = true;
    }
}

/* ------------------------------------------------------------------ */
/* Process helpers                                                     */
/* ------------------------------------------------------------------ */

/**
 * @brief SIGKILL and reap a child process.
 */
static void
kill_child(pid_t pid)
{
    if (pid > 0) {
        (void)kill(pid, SIGKILL);
        (void)waitpid(pid, NULL, 0);
    }
}

/**
 * @brief Force-kill a managed core and mark it dead.
 */
static void
force_kill_core(cog_orchestrator_t *o, struct cog_core_record *c)
{
    (void)o;
    if (c->spawned && c->pid > 0 && !c->reaped) {
        (void)kill(c->pid, SIGKILL);
        (void)waitpid(c->pid, NULL, 0);
    }
    c->reaped = true;
    c->disconnected = true;
    c->health = COG_HEALTH_DEAD;
}

/**
 * @brief Reap any exited spawned children.
 */
static void
reap_children(cog_orchestrator_t *o)
{
    size_t i;

    for (i = 0U; i < o->core_count; i++) {
        struct cog_core_record *c = &o->cores[i];
        int status = 0;

        if (!c->spawned || c->reaped || c->pid <= 0) {
            continue;
        }
        if (waitpid(c->pid, &status, WNOHANG) == c->pid) {
            c->reaped = true;
            c->health = COG_HEALTH_DEAD;
        }
    }
}

/**
 * @brief Reap a spawned child if it has already exited (non-blocking).
 *
 * @param[in,out] c Core record.
 * @return true if the child was reaped (it had exited); false if the
 *         child is still running or is not a spawned child.
 *
 * A still-running child is left alone for reap_children / normal cleanup;
 * this never blocks on a non-exiting process.  Used after a graceful
 * shutdown acknowledgement, where the peer may ack and then take a long
 * time (or never) to terminate.
 */
static bool
reap_if_exited(struct cog_core_record *c)
{
    int status = 0;

    if (c->spawned && c->pid > 0 && !c->reaped) {
        if (waitpid(c->pid, &status, WNOHANG) == c->pid) {
            c->reaped = true;
            c->health = COG_HEALTH_DEAD;
            return true;
        }
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* Core registration                                                   */
/* ------------------------------------------------------------------ */

/**
 * @brief Grow the managed-core table if full.
 */
static cog_result_t
grow_cores(cog_orchestrator_t *o)
{
    size_t nc;
    struct cog_core_record *na;

    if (o->core_count < o->core_capacity) {
        return COG_OK;
    }
    nc = (o->core_capacity == 0U) ? COG_ORCH_CORES_INITIAL
                                  : o->core_capacity * 2U;
    na = realloc(o->cores, nc * sizeof(*na));
    if (na == NULL) {
        return COG_ERROR_NOMEM;
    }
    (void)memset(&na[o->core_capacity], 0,
                 (nc - o->core_capacity) * sizeof(*na));
    o->cores = na;
    o->core_capacity = nc;
    return COG_OK;
}

/**
 * @brief Append a new managed core record.
 *
 * Takes ownership of @p conn on success.  On failure @p conn is left
 * owned by the caller.
 */
static cog_result_t
register_core(cog_orchestrator_t *o, cog_peer_id_t peer_id, pid_t pid,
              bool spawned, const char *binary_path,
              cog_ipc_endpoint_t *conn)
{
    struct cog_core_record *c;
    char *bp = NULL;
    cog_result_t rc;

    if (binary_path != NULL) {
        bp = strdup(binary_path);
        if (bp == NULL) {
            return COG_ERROR_NOMEM;
        }
    }
    rc = grow_cores(o);
    if (rc != COG_OK) {
        free(bp);
        return rc;
    }
    c = &o->cores[o->core_count];
    (void)memset(c, 0, sizeof(*c));
    c->peer_id = peer_id;
    c->pid = pid;
    c->spawned = spawned;
    c->health = COG_HEALTH_UNKNOWN;
    c->capacity = o->config.default_core_capacity;
    c->conn = conn;
    c->binary_path = bp;
    o->core_count++;
    return COG_OK;
}

/**
 * @brief Wait for a child to connect and fill a reserved core slot.
 *
 * Polls the listener, accepts the child's connection, and writes the
 * record into @p slot.  On failure the slot is left untouched (the caller
 * rolls back the reservation).
 */
static cog_result_t
accept_into_slot(cog_orchestrator_t *o, size_t slot, pid_t pid,
                 const char *binary_path)
{
    uint64_t deadline = o->now_ms + (uint64_t)o->spawn_timeout_ms;

    for (;;) {
        int revents = 0;
        cog_result_t rc;

        if (!o->clock_overridden) {
            o->now_ms = orch_now_ms();
        }
        rc = cog_ipc_endpoint_poll(o->listener, COG_IPC_POLL_IN, 50,
                                   &revents);
        if (rc == COG_OK && (revents & COG_IPC_POLL_IN) != 0) {
            cog_ipc_endpoint_t *client = NULL;
            cog_peer_id_t peer_id = COG_PEER_ID_INVALID;
            char *bp;

            rc = cog_ipc_endpoint_accept(o->listener, &client, &peer_id);
            if (rc == COG_OK) {
                struct cog_core_record *c;

                bp = strdup(binary_path);
                if (bp == NULL) {
                    (void)cog_ipc_endpoint_free(client);
                    return COG_ERROR_NOMEM;
                }
                c = &o->cores[slot];
                if (c->conn != NULL) {
                    (void)cog_ipc_endpoint_free(c->conn);
                }
                free_sessions(c);
                free(c->binary_path);
                (void)memset(c, 0, sizeof(*c));
                c->peer_id = peer_id;
                c->pid = pid;
                c->spawned = true;
                c->health = COG_HEALTH_UNKNOWN;
                c->capacity = o->config.default_core_capacity;
                c->conn = client;
                c->binary_path = bp;
                return COG_OK;
            }
            if (rc == COG_ERROR_OVERFLOW) {
                return COG_ERROR_OVERFLOW;
            }
        }
        if (o->now_ms >= deadline) {
            return COG_ERROR_IO;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Auto-replace                                                        */
/* ------------------------------------------------------------------ */

/**
 * @brief Replace dead cores when auto_replace is enabled.
 */
static void
auto_replace(cog_orchestrator_t *o)
{
    size_t i;

    for (i = 0U; i < o->core_count; i++) {
        struct cog_core_record *c = &o->cores[i];
        pid_t pid;

        if (c->binary_path == NULL) {
            continue;
        }
        /* Replacement eligibility depends on process lifetime, not on
         * acknowledgement.  auto_replace is recovery after actual process
         * death, never termination of a draining core:
         *   - a spawned core is eligible only once its process has actually
         *     exited and been reaped (reap_children).  An acked-but-alive
         *     child (disconnected, still running) is skipped while it is
         *     alive: its pid/binary/connection records stay owned for
         *     reap_children / normal cleanup, and its slot is never reused
         *     (which would memset the record and orphan the live pid).
         *   - a non-spawned core is eligible once it disconnects. */
        if (c->spawned) {
            if (!c->reaped) {
                continue;
            }
        } else if (!c->disconnected) {
            continue;
        }
        if (c->replacing) {
            continue;
        }
        c->replacing = true;
        pid = fork();
        if (pid < 0) {
            c->replacing = false;
            continue;
        }
        if (pid == 0) {
            char *argv[] = { c->binary_path, o->socket_path, NULL };

            (void)execv(c->binary_path, argv);
            _exit(127);
        }
        if (accept_into_slot(o, i, pid, c->binary_path) != COG_OK) {
            kill_child(pid);
            c->replacing = false;
            continue;
        }
        c->replacing = false;
    }
}

/* ------------------------------------------------------------------ */
/* Public lifecycle                                                    */
/* ------------------------------------------------------------------ */

cog_result_t
cog_orchestrator_new(cog_orchestrator_t **out,
                     const cog_orchestrator_config_t *config)
{
    cog_orchestrator_t *o;
    cog_result_t rc;
    size_t plen;

    if (out == NULL || config == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    if (config->socket_path == NULL) {
        return COG_ERROR_INVALID_ARG;
    }
    plen = strlen(config->socket_path);
    if (plen >= COG_ORCH_PATH_MAX) {
        return COG_ERROR_INVALID_ARG;
    }

    o = calloc(1U, sizeof(*o));
    if (o == NULL) {
        return COG_ERROR_NOMEM;
    }
    o->config = *config;
    if (o->config.max_aux_cores == 0U) {
        o->config.max_aux_cores = COG_ORCH_DEFAULT_MAX_AUX;
    }
    if (o->config.health_interval_ms == 0U) {
        o->config.health_interval_ms = COG_ORCH_DEFAULT_HEALTH_INTERVAL_MS;
    }
    if (o->config.unhealthy_threshold == 0U) {
        o->config.unhealthy_threshold = COG_ORCH_DEFAULT_UNHEALTHY_THRESHOLD;
    }
    if (o->config.default_core_capacity == 0U) {
        o->config.default_core_capacity = COG_ORCH_DEFAULT_CAPACITY;
    }
    (void)memcpy(o->socket_path, config->socket_path, plen + 1U);

    rc = cog_ipc_endpoint_new(&o->listener);
    if (rc != COG_OK) {
        free(o);
        return rc;
    }
    rc = cog_ipc_endpoint_listen(o->listener, o->socket_path);
    if (rc != COG_OK) {
        (void)cog_ipc_endpoint_free(o->listener);
        free(o);
        return rc;
    }
    o->spawn_timeout_ms = COG_ORCH_SPAWN_TIMEOUT_MS;
    o->ack_timeout_ms = COG_ORCH_ACK_TIMEOUT_MS;
    o->now_ms = orch_now_ms();
    *out = o;
    return COG_OK;
}

void
cog_orchestrator_free(cog_orchestrator_t *o)
{
    size_t i;

    if (o == NULL) {
        return;
    }
    for (i = 0U; i < o->core_count; i++) {
        struct cog_core_record *c = &o->cores[i];

        if (c->spawned && c->pid > 0 && !c->reaped) {
            (void)kill(c->pid, SIGKILL);
            (void)waitpid(c->pid, NULL, 0);
        }
        if (c->conn != NULL) {
            (void)cog_ipc_endpoint_free(c->conn);
        }
        free_sessions(c);
        free(c->binary_path);
    }
    free(o->cores);
    if (o->listener != NULL) {
        (void)cog_ipc_endpoint_free(o->listener);
    }
    (void)unlink(o->socket_path);
    free(o);
}

/* ------------------------------------------------------------------ */
/* Public process management                                           */
/* ------------------------------------------------------------------ */

cog_result_t
cog_orchestrator_spawn(cog_orchestrator_t *o, const char *binary_path,
                       cog_peer_id_t *out_peer_id)
{
    pid_t pid;
    cog_result_t rc;
    size_t slot;

    if (o == NULL || binary_path == NULL || out_peer_id == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    if (o->core_count >= o->config.max_aux_cores) {
        return COG_ERROR_OVERFLOW;
    }
    rc = grow_cores(o);
    if (rc != COG_OK) {
        return rc;
    }
    slot = o->core_count;
    o->core_count++;

    pid = fork();
    if (pid < 0) {
        o->core_count--;
        return COG_ERROR_IO;
    }
    if (pid == 0) {
        char *argv[] = { (char *)binary_path, o->socket_path, NULL };

        (void)execv(binary_path, argv);
        _exit(127);
    }

    rc = accept_into_slot(o, slot, pid, binary_path);
    if (rc != COG_OK) {
        o->core_count--;
        kill_child(pid);
        return rc;
    }
    *out_peer_id = o->cores[slot].peer_id;
    return COG_OK;
}

cog_result_t
cog_orchestrator_stop(cog_orchestrator_t *o, cog_peer_id_t peer_id)
{
    struct cog_core_record *c;
    cog_result_t rc;

    if (o == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    c = find_core(o, peer_id);
    if (c == NULL) {
        return COG_ERROR_NOT_FOUND;
    }
    if (c->reaped || c->disconnected || c->conn == NULL) {
        return COG_OK;
    }
    rc = send_text(c->conn, "shutdown\n");
    if (rc != COG_OK) {
        force_kill_core(o, c);
        return COG_OK;
    }
    rc = wait_for_ack(o, c, (int)o->ack_timeout_ms);
    if (rc == COG_OK) {
        /* The core acknowledged shutdown: it is logically stopped.  Mark
         * it disconnected so health reports DEAD.  If it has already
         * exited, reap it; if it is still alive (e.g. it acked but has not
         * yet terminated), leave it for reap_children / normal cleanup so
         * we never block on a non-exiting child. */
        (void)reap_if_exited(c);
        c->disconnected = true;
        return COG_OK;
    }
    force_kill_core(o, c);
    return COG_OK;
}

cog_result_t
cog_orchestrator_kill(cog_orchestrator_t *o, cog_peer_id_t peer_id)
{
    struct cog_core_record *c;

    if (o == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    c = find_core(o, peer_id);
    if (c == NULL) {
        return COG_ERROR_NOT_FOUND;
    }
    force_kill_core(o, c);
    return COG_OK;
}

/* ------------------------------------------------------------------ */
/* Public health                                                       */
/* ------------------------------------------------------------------ */

cog_health_t
cog_orchestrator_health(const cog_orchestrator_t *o, cog_peer_id_t peer_id)
{
    struct cog_core_record *c;

    if (o == NULL) {
        return COG_HEALTH_UNKNOWN;
    }
    c = find_core((cog_orchestrator_t *)o, peer_id);
    if (c == NULL) {
        return COG_HEALTH_UNKNOWN;
    }
    return compute_health(c, o->now_ms, o->config.health_interval_ms,
                          o->config.unhealthy_threshold);
}

cog_result_t
cog_orchestrator_list(const cog_orchestrator_t *o, cog_core_info_t *out_infos,
                      size_t max_infos, size_t *out_count)
{
    size_t i;
    size_t copy;

    if (o == NULL || out_count == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    *out_count = o->core_count;
    if (out_infos == NULL) {
        return COG_OK;
    }
    copy = (o->core_count < max_infos) ? o->core_count : max_infos;
    for (i = 0U; i < copy; i++) {
        const struct cog_core_record *c = &o->cores[i];

        out_infos[i].peer_id = c->peer_id;
        out_infos[i].pid = (int)c->pid;
        out_infos[i].health =
            compute_health(c, o->now_ms, o->config.health_interval_ms,
                           o->config.unhealthy_threshold);
        out_infos[i].active_subagents = c->active_subagents;
        out_infos[i].capacity = c->capacity;
        out_infos[i].last_heartbeat_ms = c->last_heartbeat_ms;
    }
    return COG_OK;
}

cog_result_t
cog_orchestrator_heartbeat(cog_orchestrator_t *o, cog_peer_id_t peer_id,
                           size_t active_subagents)
{
    struct cog_core_record *c;

    if (o == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    c = find_core(o, peer_id);
    if (c == NULL) {
        return COG_ERROR_NOT_FOUND;
    }
    c->active_subagents = active_subagents;
    c->has_heartbeat = true;
    c->last_heartbeat_ms = o->now_ms;
    c->health = COG_HEALTH_HEALTHY;
    return COG_OK;
}

/* ------------------------------------------------------------------ */
/* Public placement                                                    */
/* ------------------------------------------------------------------ */

cog_peer_id_t
cog_orchestrator_place(const cog_orchestrator_t *o, size_t estimated_load)
{
    cog_peer_id_t best = COG_PEER_ID_INVALID;
    size_t best_free = 0U;
    int best_score = 0;
    size_t i;

    if (o == NULL) {
        return COG_PEER_ID_INVALID;
    }
    for (i = 0U; i < o->core_count; i++) {
        const struct cog_core_record *c = &o->cores[i];
        cog_health_t h;
        int score;
        size_t free;

        h = compute_health(c, o->now_ms, o->config.health_interval_ms,
                           o->config.unhealthy_threshold);
        if (h == COG_HEALTH_HEALTHY) {
            score = 2;
        } else if (h == COG_HEALTH_DEGRADED) {
            score = 1;
        } else {
            continue;
        }
        if (c->active_subagents + estimated_load > c->capacity) {
            continue;
        }
        free = c->capacity - c->active_subagents;
        if (score > best_score ||
            (score == best_score && free > best_free)) {
            best = c->peer_id;
            best_free = free;
            best_score = score;
        }
    }
    return best;
}

/* ------------------------------------------------------------------ */
/* Public shutdown                                                     */
/* ------------------------------------------------------------------ */

cog_result_t
cog_orchestrator_shutdown(cog_orchestrator_t *o, uint32_t timeout_ms)
{
    size_t i;
    uint64_t deadline;
    bool all_acked;

    if (o == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    for (i = 0U; i < o->core_count; i++) {
        struct cog_core_record *c = &o->cores[i];

        if (c->reaped || c->disconnected || c->conn == NULL) {
            continue;
        }
        c->acked = false;
        c->shutting_down = true;
        if (send_text(c->conn, "shutdown\n") != COG_OK) {
            force_kill_core(o, c);
            continue;
        }
    }

    deadline = o->now_ms + (uint64_t)timeout_ms;
    for (;;) {
        bool progress = false;

        if (!o->clock_overridden) {
            o->now_ms = orch_now_ms();
        }
        for (i = 0U; i < o->core_count; i++) {
            struct cog_core_record *c = &o->cores[i];
            int revents = 0;
            cog_result_t rc;

            if (!c->shutting_down || c->acked || c->reaped ||
                c->disconnected) {
                continue;
            }
            rc = cog_ipc_endpoint_poll(c->conn, COG_IPC_POLL_IN, 0,
                                       &revents);
            if (rc == COG_OK &&
                (revents & (COG_IPC_POLL_IN | COG_IPC_POLL_ERR)) != 0) {
                char buf[COG_ORCH_MSG_MAX];
                size_t len = 0U;

                rc = recv_text(c->conn, buf, sizeof(buf), &len);
                if (rc == COG_OK) {
                    process_message(o, c, buf);
                    if (c->acked) {
                        progress = true;
                    }
                } else if (rc == COG_ERROR_IO ||
                           rc == COG_ERROR_OVERFLOW) {
                    c->disconnected = true;
                    progress = true;
                }
            }
        }

        all_acked = true;
        for (i = 0U; i < o->core_count; i++) {
            const struct cog_core_record *c = &o->cores[i];

            /* A graceful shutdown requires an explicit acknowledgement.
             * A core that disconnects or is reaped without acking is a
             * timeout, not a graceful stop. */
            if (c->shutting_down && !c->acked) {
                all_acked = false;
                break;
            }
        }
        if (all_acked) {
            break;
        }
        if (o->now_ms >= deadline) {
            break;
        }
        if (!progress) {
            struct timespec ts;

            ts.tv_sec = 0;
            ts.tv_nsec = 1000000L;
            (void)nanosleep(&ts, NULL);
        }
    }

    all_acked = true;
    for (i = 0U; i < o->core_count; i++) {
        struct cog_core_record *c = &o->cores[i];

        if (c->shutting_down) {
            if (c->acked) {
                /* Graceful stop: the core acknowledged.  Mark it
                 * disconnected so health reports DEAD.  If it has exited,
                 * reap it; if it is still alive (e.g. it acked but has not
                 * yet terminated), leave it for reap_children / normal
                 * cleanup so we never block on a non-exiting child. */
                (void)reap_if_exited(c);
                c->disconnected = true;
            } else {
                force_kill_core(o, c);
                all_acked = false;
            }
            c->shutting_down = false;
        }
    }
    return all_acked ? COG_OK : COG_ERROR_TIMEOUT;
}

/* ------------------------------------------------------------------ */
/* Public session tracking                                             */
/* ------------------------------------------------------------------ */

cog_result_t
cog_orchestrator_sessions(const cog_orchestrator_t *o, cog_peer_id_t peer_id,
                          const char **out_sessions, size_t max_sessions,
                          size_t *out_count)
{
    struct cog_core_record *c;
    size_t i;
    size_t copy;

    if (o == NULL || out_sessions == NULL || out_count == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    c = find_core((cog_orchestrator_t *)o, peer_id);
    if (c == NULL) {
        return COG_ERROR_NOT_FOUND;
    }
    *out_count = c->session_count;
    copy = (c->session_count < max_sessions) ? c->session_count
                                             : max_sessions;
    for (i = 0U; i < copy; i++) {
        out_sessions[i] = c->sessions[i];
    }
    return COG_OK;
}

/* ------------------------------------------------------------------ */
/* Private drive hooks (orchestrator_internal.h)                       */
/* ------------------------------------------------------------------ */

cog_result_t
cog_orchestrator_poll(cog_orchestrator_t *o, int timeout_ms)
{
    size_t i;

    if (o == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    if (!o->clock_overridden) {
        o->now_ms = orch_now_ms();
    }
    reap_children(o);
    accept_pending(o, timeout_ms);
    for (i = 0U; i < o->core_count; i++) {
        struct cog_core_record *c = &o->cores[i];
        int revents = 0;
        cog_result_t rc;

        if (c->conn == NULL) {
            continue;
        }
        rc = cog_ipc_endpoint_poll(c->conn, COG_IPC_POLL_IN, 0, &revents);
        if (rc != COG_OK) {
            continue;
        }
        /* Drain on POLL_ERR too: a peer that sent data and then closed
         * reports EOF (POLL_ERR) while buffered frames are still readable.
         * recv_text returns IO once the buffer is exhausted. */
        if ((revents & (COG_IPC_POLL_IN | COG_IPC_POLL_ERR)) == 0) {
            continue;
        }
        {
            char buf[COG_ORCH_MSG_MAX];
            size_t len = 0U;

            rc = recv_text(c->conn, buf, sizeof(buf), &len);
            if (rc == COG_OK) {
                process_message(o, c, buf);
            } else if (rc == COG_ERROR_IO || rc == COG_ERROR_OVERFLOW) {
                c->disconnected = true;
            }
        }
    }
    if (o->config.auto_replace) {
        auto_replace(o);
    }
    return COG_OK;
}

cog_result_t
cog_orchestrator_accept(cog_orchestrator_t *o, cog_peer_id_t *out_peer_id)
{
    cog_ipc_endpoint_t *client = NULL;
    cog_peer_id_t peer_id = COG_PEER_ID_INVALID;
    cog_result_t rc;

    if (o == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    if (o->core_count >= o->config.max_aux_cores) {
        return COG_ERROR_OVERFLOW;
    }
    rc = cog_ipc_endpoint_accept(o->listener, &client, &peer_id);
    if (rc != COG_OK) {
        return rc;
    }
    rc = register_core(o, peer_id, (pid_t)-1, false, NULL, client);
    if (rc != COG_OK) {
        (void)cog_ipc_endpoint_free(client);
        return rc;
    }
    if (out_peer_id != NULL) {
        *out_peer_id = peer_id;
    }
    return COG_OK;
}

cog_result_t
cog_orchestrator_register_session(cog_orchestrator_t *o,
                                  cog_peer_id_t peer_id,
                                  const char *session_id)
{
    struct cog_core_record *c;
    size_t i;

    if (o == NULL || session_id == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    c = find_core(o, peer_id);
    if (c == NULL) {
        return COG_ERROR_NOT_FOUND;
    }
    for (i = 0U; i < c->session_count; i++) {
        if (strcmp(c->sessions[i], session_id) == 0) {
            return COG_OK;
        }
    }
    if (c->session_count >= COG_ORCH_SESSIONS_PER_CORE) {
        return COG_ERROR_OVERFLOW;
    }
    if (c->session_count >= c->session_capacity) {
        size_t nc;
        char **na;

        nc = (c->session_capacity == 0U) ? COG_ORCH_SESSIONS_INITIAL
                                         : c->session_capacity * 2U;
        na = realloc(c->sessions, nc * sizeof(*na));
        if (na == NULL) {
            return COG_ERROR_NOMEM;
        }
        c->sessions = na;
        c->session_capacity = nc;
    }
    {
        char *copy = strdup(session_id);

        if (copy == NULL) {
            return COG_ERROR_NOMEM;
        }
        c->sessions[c->session_count++] = copy;
    }
    return COG_OK;
}

cog_result_t
cog_orchestrator_unregister_session(cog_orchestrator_t *o,
                                    cog_peer_id_t peer_id,
                                    const char *session_id)
{
    struct cog_core_record *c;
    size_t i;

    if (o == NULL || session_id == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    c = find_core(o, peer_id);
    if (c == NULL) {
        return COG_ERROR_NOT_FOUND;
    }
    for (i = 0U; i < c->session_count; i++) {
        if (strcmp(c->sessions[i], session_id) == 0) {
            free(c->sessions[i]);
            for (; i + 1U < c->session_count; i++) {
                c->sessions[i] = c->sessions[i + 1U];
            }
            c->session_count--;
            return COG_OK;
        }
    }
    return COG_ERROR_NOT_FOUND;
}

void
cog_orchestrator_set_now(cog_orchestrator_t *o, uint64_t now_ms)
{
    if (o == NULL) {
        return;
    }
    o->now_ms = now_ms;
    o->clock_overridden = true;
}

void
cog_orchestrator_set_spawn_timeout(cog_orchestrator_t *o, uint32_t timeout_ms)
{
    if (o == NULL) {
        return;
    }
    o->spawn_timeout_ms = timeout_ms;
}

void
cog_orchestrator_set_ack_timeout(cog_orchestrator_t *o, uint32_t timeout_ms)
{
    if (o == NULL) {
        return;
    }
    o->ack_timeout_ms = timeout_ms;
}

uint64_t
cog_orchestrator_now(const cog_orchestrator_t *o)
{
    if (o == NULL) {
        return 0U;
    }
    return o->now_ms;
}

/* ------------------------------------------------------------------ */
/* Static helpers used above (defined after use for readability).      */
/* ------------------------------------------------------------------ */

/**
 * @brief Wait for a shutdown acknowledgement from a core.
 */
static cog_result_t
wait_for_ack(cog_orchestrator_t *o, struct cog_core_record *c,
             int timeout_ms)
{
    uint64_t deadline = o->now_ms + (uint64_t)timeout_ms;

    for (;;) {
        int revents = 0;
        cog_result_t rc;

        if (!o->clock_overridden) {
            o->now_ms = orch_now_ms();
        }
        rc = cog_ipc_endpoint_poll(c->conn, COG_IPC_POLL_IN, 50, &revents);
        if (rc == COG_OK &&
            (revents & (COG_IPC_POLL_IN | COG_IPC_POLL_ERR)) != 0) {
            char buf[COG_ORCH_MSG_MAX];
            size_t len = 0U;

            rc = recv_text(c->conn, buf, sizeof(buf), &len);
            if (rc == COG_OK) {
                process_message(o, c, buf);
                if (c->acked) {
                    return COG_OK;
                }
            } else if (rc == COG_ERROR_IO || rc == COG_ERROR_OVERFLOW) {
                c->disconnected = true;
                return COG_ERROR_IO;
            }
        }
        if (o->now_ms >= deadline) {
            return COG_ERROR_TIMEOUT;
        }
    }
}

/**
 * @brief Accept all pending listener connections.
 */
static void
accept_pending(cog_orchestrator_t *o, int timeout_ms)
{
    int revents = 0;
    cog_result_t rc;

    rc = cog_ipc_endpoint_poll(o->listener, COG_IPC_POLL_IN, timeout_ms,
                               &revents);
    if (rc != COG_OK || (revents & COG_IPC_POLL_IN) == 0) {
        return;
    }
    for (;;) {
        cog_peer_id_t peer_id = COG_PEER_ID_INVALID;

        rc = cog_orchestrator_accept(o, &peer_id);
        if (rc != COG_OK) {
            break;
        }
    }
}

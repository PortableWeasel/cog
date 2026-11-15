/**
 * @file aux_core.c
 * @brief Test fixture: a minimal auxiliary-core process for the
 *        orchestrator integration tests.
 *
 * Connects to the orchestrator's listener socket (argv[1]) and speaks the
 * orchestrator control protocol:
 *
 *   aux -> orch : "hello <pid>", "heartbeat <active>", "session <id>",
 *                 "ack"
 *   orch -> aux : "shutdown"
 *
 * Behaviour is selected by environment variables so the same binary can
 * play every role the tests need:
 *
 *   COG_AUX_MODE         hb (default) | silent | crash | noack
 *   COG_AUX_SESSION      optional session id to register at startup
 *   COG_AUX_INTERVAL_MS  heartbeat interval in ms (default 50)
 *
 * Modes:
 *   - hb:     send heartbeats every interval; ack shutdown and exit.
 *   - silent: connect but never heartbeat; ack shutdown and exit.
 *   - crash:  send one heartbeat then exit(1) without acking (crash).
 *   - noack:  send heartbeats; on shutdown exit without acking (so the
 *             orchestrator must force-kill / time out).
 *
 * This fixture is owned exclusively by the orchestrator integration test;
 * it never touches processes outside the test.
 */

#define _POSIX_C_SOURCE 200809L

#include "cog/ipc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/** Send a NUL-terminated string as a framed message. */
static int
send_str(cog_ipc_endpoint_t *ep, const char *text)
{
    cog_ipc_message_t msg;
    size_t len = strlen(text);
    cog_result_t rc;

    rc = cog_ipc_message_new(&msg, len + 1U);
    if (rc != COG_OK) {
        return -1;
    }
    (void)memcpy(msg.data, text, len);
    msg.len = len;
    rc = cog_ipc_endpoint_send(ep, &msg);
    (void)cog_ipc_message_free(&msg);
    return (rc == COG_OK) ? 0 : -1;
}

int
main(int argc, char **argv)
{
    const char *path;
    const char *mode;
    const char *session;
    const char *interval_env;
    int interval;
    cog_ipc_endpoint_t *ep = NULL;
    cog_peer_id_t peer_id = COG_PEER_ID_INVALID;
    char hello[64];

    if (argc < 2) {
        return 2;
    }
    path = argv[1];
    mode = getenv("COG_AUX_MODE");
    if (mode == NULL || mode[0] == '\0') {
        mode = "hb";
    }
    session = getenv("COG_AUX_SESSION");
    interval_env = getenv("COG_AUX_INTERVAL_MS");
    interval = 50;
    if (interval_env != NULL && interval_env[0] != '\0') {
        interval = atoi(interval_env);
    }
    if (interval < 1) {
        interval = 1;
    }

    if (cog_ipc_endpoint_new(&ep) != COG_OK) {
        return 2;
    }
    if (cog_ipc_endpoint_connect(ep, path, &peer_id) != COG_OK) {
        (void)cog_ipc_endpoint_free(ep);
        return 2;
    }

    (void)snprintf(hello, sizeof(hello), "hello %ld\n", (long)getpid());
    (void)send_str(ep, hello);
    if (session != NULL && session[0] != '\0') {
        char buf[256];

        (void)snprintf(buf, sizeof(buf), "session %s\n", session);
        (void)send_str(ep, buf);
    }

    if (strcmp(mode, "crash") == 0) {
        (void)send_str(ep, "heartbeat 0\n");
        (void)cog_ipc_endpoint_free(ep);
        return 1;
    }

    for (;;) {
        int revents = 0;
        cog_result_t rc;

        rc = cog_ipc_endpoint_poll(ep, COG_IPC_POLL_IN, interval, &revents);
        if (rc == COG_OK && (revents & COG_IPC_POLL_IN) != 0) {
            cog_ipc_message_t msg;

            if (cog_ipc_message_new(&msg, 256U) == COG_OK) {
                rc = cog_ipc_endpoint_recv(ep, &msg);
                if (rc == COG_OK && msg.len >= 8U &&
                    memcmp(msg.data, "shutdown", 8U) == 0) {
                    if (strcmp(mode, "noack") != 0) {
                        (void)send_str(ep, "ack\n");
                    }
                    (void)cog_ipc_message_free(&msg);
                    break;
                }
                (void)cog_ipc_message_free(&msg);
            }
        }
        if (strcmp(mode, "hb") == 0 || strcmp(mode, "noack") == 0) {
            (void)send_str(ep, "heartbeat 0\n");
        }
    }

    (void)cog_ipc_endpoint_free(ep);
    return 0;
}

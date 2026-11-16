/** @file session_scenario.h @brief Test-owned actual UI client driving helpers. */
#ifndef COG_TEST_SESSION_SCENARIO_H
#define COG_TEST_SESSION_SCENARIO_H
#include "headless_fixture.h"
#include "view_internal.h"
#include "../../../libraries/cog-ipc/src/ipc_internal.h"
#include <sys/socket.h>
#include <unistd.h>
#include <string.h>

typedef struct session_client {
    cog_cli_host_client_t *client;
    FILE *output;
    int input[2];
} session_client_t;
static cog_result_t session_client_open(session_client_t *c,const char *socket,
    const char *output,bool watch);
static cog_result_t session_tick(headless_fixture_t *f,session_client_t *a,
    session_client_t *b);
static bool session_online(const headless_fixture_t *f,const session_client_t *c);
static cog_result_t session_write(session_client_t *c,const char *text);
static cog_result_t session_disconnect(session_client_t *c);
static bool session_contains(const session_client_t *c,const char *text);
static cog_result_t session_client_close(session_client_t *c,cog_result_t primary);
#endif

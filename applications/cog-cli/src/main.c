/**
 * @file main.c
 * @brief cog-cli entry point.
 *
 * Parses options, builds the CLI application, installs SIGINT/SIGTERM
 * handlers that request a graceful stop, runs the agent loop, and reports
 * a clean exit code.  All real work lives in cli.c so it is testable
 * without spawning the process.
 */
#define _POSIX_C_SOURCE 200809L

#include "cog_cli/cli.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

/** App currently running, for the signal handler. */
static cog_cli_app_t *g_app = NULL;

/** Signal handler: request a graceful stop (async-signal-safe). */
static void cli_signal_handler(int sig)
{
    (void)sig;
    if (g_app != NULL) {
        cog_cli_app_stop(g_app);
    }
}

int main(int argc, char **argv)
{
    cog_cli_options_t opts;
    cog_cli_app_t *app = NULL;
    cog_result_t rc;
    int exit_code = 0;

    rc = cog_cli_options_parse(&opts, argc, argv, &exit_code);
    if (rc == COG_ERROR_ABORTED) {
        return exit_code; /* --help / --version / bad args already printed */
    }
    if (rc != COG_OK) {
        (void)fprintf(stderr, "cog-cli: option parsing failed: %s\n",
                      cog_cli_result_name(rc));
        return 2;
    }

    rc = cog_cli_app_new(&app, &opts);
    if (rc != COG_OK) {
        (void)fprintf(stderr, "cog-cli: startup failed: %s\n",
                      cog_cli_result_name(rc));
        return 1;
    }

    g_app = app;
    (void)signal(SIGINT, cli_signal_handler);
    (void)signal(SIGTERM, cli_signal_handler);

    rc = cog_cli_app_run(app);
    if (rc != COG_OK) {
        (void)fprintf(stderr, "cog-cli: run failed: %s\n",
                      cog_cli_result_name(rc));
        exit_code = 1;
    }

    g_app = NULL;
    cog_cli_app_free(app);
    return exit_code;
}

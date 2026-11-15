/** @file main.c @brief Linux CLI host: explicit transport and signal lifecycle. */
#define _GNU_SOURCE
#include "app_internal.h"
#include <curl/curl.h>
#include <signal.h>
#include <stdio.h>
#include <sys/signalfd.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    cog_cli_options_t options;
    int status = 0, signal_fd = -1;
    bool runtime = false, masked = false;
    cog_cli_app_t *app = NULL;
    cog_net_ctx_t *net = NULL;
    cog_cli_observer_t *observer = NULL;
    cog_cli_configuration_t *candidate = NULL;
    sigset_t signals, old_mask;
    struct sigaction old_pipe, ignore_pipe = {0};
    ignore_pipe.sa_handler = SIG_IGN;
    (void)sigemptyset(&ignore_pipe.sa_mask);
    if (sigaction(SIGPIPE, &ignore_pipe, &old_pipe) != 0) return 1;
    cog_result_t rc = cog_cli_options_parse(&options, argc, argv, &status);
    if (rc == COG_ERROR_ABORTED) { rc = COG_OK; goto done; }
    if (rc != COG_OK) { status = 2; goto done; }
    /* Resolve backend before creating a real transport: explicit/configured
       MOCK remains usable on hosts without the audited resolver profile. */
    rc = cog_cli_configuration_new(&candidate, &options, 1U);
    if (rc != COG_OK) goto done;
    const cog_cli_options_t *effective = cog_cli_configuration_options(candidate);
    bool execution = effective->session_action != COG_CLI_SESSION_LIST &&
        effective->session_action != COG_CLI_SESSION_RECOVER;
    bool real = effective->backend == COG_CLI_BACKEND_REAL && execution;
    size_t context = cog_cli_configuration_options(candidate)->max_context_bytes;
    cog_cli_configuration_free(candidate); candidate = NULL;
    if (execution) {
        rc = cog_cli_observer_new(&observer, 4U);
        if (rc != COG_OK) goto done;
    }
    if (real) {
        if (context > SIZE_MAX / 8U) { rc = COG_ERROR_LIMIT; goto done; }
        if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) { rc = COG_ERROR_NOMEM; goto done; }
        runtime = true;
        cog_net_config_t config = {.host_runtime_initialized = true,
            .dependency_memory = COG_NET_DEPENDENCY_MEMORY_DYNAMIC,
            .max_request_bytes = context * 8U, .max_connections = 4U,
            .observer = cog_cli_observer_capture, .observer_user = observer};
        rc = cog_net_new(&net, &config);
        if (rc != COG_OK) goto done;
        options.net = net;
    }
    (void)sigemptyset(&signals);
    (void)sigaddset(&signals, SIGINT); (void)sigaddset(&signals, SIGTERM);
    if (sigprocmask(SIG_BLOCK, &signals, &old_mask) != 0) { rc = COG_ERROR_IO; goto done; }
    masked = true;
    signal_fd = signalfd(-1, &signals, SFD_CLOEXEC | SFD_NONBLOCK);
    if (signal_fd < 0) { rc = COG_ERROR_IO; goto done; }
    rc = cog_cli_app_new(&app, &options);
    if (rc != COG_OK) goto done;
    if (observer != NULL) {
        rc = cog_cli_observer_bind(observer, app->obs);
        if (rc != COG_OK) goto done;
        app->observer = observer;
    }
    app->signal_fd = signal_fd; /* Borrowed host descriptor, never global. */
    if (!cog_cli_metadata(app) && app->opts.interactive && app->store != NULL &&
        (fprintf(stderr, "Session: %s\n", cog_session_id(app->session)) < 0 || fflush(stderr) != 0)) {
        rc = COG_ERROR_IO; goto done;
    }
    rc = cog_cli_app_run(app);
done:
    if (fclose(stdout) != 0 && rc == COG_OK) rc = COG_ERROR_IO;
    cog_cli_configuration_free(candidate);
    cog_cli_app_free(app);
    if (observer != NULL) {
        cog_result_t observed = cog_cli_observer_status(observer);
        if (rc == COG_OK) rc = observed;
    }
    cog_net_free(net);
    cog_cli_observer_free(observer);
    if (runtime) curl_global_cleanup();
    if (signal_fd >= 0) {
        struct signalfd_siginfo info;
        while (read(signal_fd, &info, sizeof(info)) == (ssize_t)sizeof(info)) {}
        (void)close(signal_fd);
    }
    if (masked) (void)sigprocmask(SIG_SETMASK, &old_mask, NULL);
    (void)sigaction(SIGPIPE, &old_pipe, NULL);
    if (rc != COG_OK) {
        (void)fprintf(stderr, "cog-cli: %s\n", cog_cli_result_name(rc)); status = 1;
    }
    return status;
}

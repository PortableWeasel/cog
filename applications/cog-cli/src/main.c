/** @file main.c @brief Linux CLI host: explicit transport and signal lifecycle. */
#define _GNU_SOURCE
#include "app_internal.h"
#include "host_output.h"
#include "host_options.h"
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <curl/curl.h>
#include <signal.h>
#include <stdio.h>
#include <sys/signalfd.h>
#include <unistd.h>

/* Installed role ownership remains explicit; only the host waits between slices. */
static cog_result_t host_wait(int signal_fd, uint32_t max_wait_ms)
{
    int wait_ms = max_wait_ms > (uint32_t)INT_MAX ? INT_MAX : (int)max_wait_ms;
    struct pollfd ready = {signal_fd, POLLIN, 0};
    int result = poll(signal_fd < 0 ? NULL : &ready, signal_fd < 0 ? 0U : 1U, wait_ms);
    return result < 0 && errno != EINTR ? COG_ERROR_IO : COG_OK;
}
static cog_result_t run_coordinator(cog_cli_host_options_t *options, int signal_fd,
    const cog_net_config_t *net_config)
{
    char executable[4096];
    if (options->runtime.workers != 0U) {
        ssize_t count = readlink("/proc/self/exe", executable, sizeof(executable) - 1U);
        if (count <= 0 || (size_t)count >= sizeof(executable) - 1U) return COG_ERROR_IO;
        executable[(size_t)count] = '\0'; options->runtime.executable = executable;
    }
    options->runtime.signal_fd = signal_fd; options->runtime.net_config = net_config;
    options->runtime.diagnostic = stderr;
    cog_cli_host_runtime_t *host = NULL;
    cog_result_t primary = cog_cli_host_runtime_new(&host, &options->runtime);
    if (primary == COG_OK) {
        primary = cog_cli_host_runtime_begin(host);
        /* Begin may own a successfully spawned prefix even when a later
           startup operation fails. Keep that owner through bounded drain. */
        if (primary != COG_OK) (void)cog_cli_host_runtime_request_shutdown(host);
        bool done = false;
        while (!done) {
            cog_cli_host_runtime_step_t step = {0};
            cog_result_t rc = cog_cli_host_runtime_step(host, &step);
            if (rc != COG_OK && rc != COG_ERROR_AGAIN) {
                if (primary == COG_OK) primary = rc;
                (void)cog_cli_host_runtime_request_shutdown(host);
            }
            done = step.done;
            if (!done) {
                rc = host_wait(signal_fd, step.max_wait_ms);
                if (rc != COG_OK) {
                    if (primary == COG_OK) primary = rc;
                    (void)cog_cli_host_runtime_request_shutdown(host);
                }
            }
        }
    }
    return cog_cli_host_runtime_close(&host, primary);
}
static cog_result_t run_worker(cog_cli_host_options_t *options, int signal_fd,
    const cog_net_config_t *net_config)
{
    options->worker.signal_fd = signal_fd; options->worker.net_config = net_config;
    cog_cli_worker_host_t *host = NULL;
    cog_result_t primary = cog_cli_worker_host_new(&host, &options->worker);
    if (primary == COG_OK) {
        bool done = false;
        while (!done) {
            cog_cli_host_runtime_step_t step = {0};
            cog_result_t rc = cog_cli_worker_host_step(host, &step);
            if (rc != COG_OK && rc != COG_ERROR_AGAIN) {
                if (primary == COG_OK) primary = rc;
                (void)cog_cli_worker_host_request_shutdown(host);
            }
            done = step.done;
            if (!done) {
                rc = host_wait(signal_fd, step.max_wait_ms);
                if (rc != COG_OK) {
                    if (primary == COG_OK) primary = rc;
                    (void)cog_cli_worker_host_request_shutdown(host);
                }
            }
        }
    }
    return cog_cli_worker_host_close(&host, primary);
}
static cog_result_t run_client(cog_cli_host_options_t *options, int signal_fd,
    const cog_net_config_t *net_config)
{
    (void)net_config;
    options->client.signal_fd = signal_fd; options->client.output = stdout;
    options->client.diagnostic = stderr;
    options->client.terminal = isatty(STDOUT_FILENO) == 1;
    cog_cli_host_client_t *host = NULL;
    cog_result_t primary = cog_cli_host_client_new(&host, &options->client);
    if (primary == COG_OK) {
        bool done = false;
        while (!done) {
            cog_cli_host_runtime_step_t step = {0};
            cog_result_t rc = cog_cli_host_client_step(host, &step);
            if (rc != COG_OK && rc != COG_ERROR_AGAIN && primary == COG_OK) primary = rc;
            done = step.done;
            if (!done) {
                rc = host_wait(signal_fd, step.max_wait_ms);
                if (rc != COG_OK) { if (primary == COG_OK) primary = rc; break; }
            }
        }
    }
    return cog_cli_host_client_close(&host, primary);
}
typedef cog_result_t (*host_role_run_fn)(cog_cli_host_options_t *, int, const cog_net_config_t *);
static const host_role_run_fn host_roles[] = {
    NULL, run_coordinator, run_client, run_client, run_worker
};

int main(int argc, char **argv)
{
    cog_cli_options_t options;
    cog_cli_host_options_t host_options = {0};
    int status = 0, signal_fd = -1;
    bool runtime = false, masked = false;
    cog_cli_app_t *app = NULL;
    cog_net_ctx_t *net = NULL;
    cog_cli_observer_t *observer = NULL;
    cog_cli_configuration_t *candidate = NULL;
    cog_cli_host_output_t output = {0};
    cog_result_t written;
    sigset_t signals, old_mask;
    struct sigaction old_pipe, ignore_pipe = {0};
    ignore_pipe.sa_handler = SIG_IGN;
    (void)sigemptyset(&ignore_pipe.sa_mask);
    if (sigaction(SIGPIPE, &ignore_pipe, &old_pipe) != 0) return 1;
    cog_result_t rc = cog_cli_host_output_open(&output);
    if (rc != COG_OK) { (void)sigaction(SIGPIPE, &old_pipe, NULL); return 1; }
    rc = cog_cli_host_options_open(&host_options, argc, argv, &status);
    if (rc == COG_ERROR_ABORTED) { rc = COG_OK; goto done; }
    if (rc != COG_OK) { status = 2; goto done; }
    options = host_options.application;
    if (host_options.role != COG_CLI_HOST_STANDALONE) {
        (void)sigemptyset(&signals);
        (void)sigaddset(&signals, SIGINT); (void)sigaddset(&signals, SIGTERM);
        if (sigprocmask(SIG_BLOCK, &signals, &old_mask) != 0) { rc = COG_ERROR_IO; goto done; }
        masked = true;
        signal_fd = signalfd(-1, &signals, SFD_CLOEXEC | SFD_NONBLOCK);
        if (signal_fd < 0) { rc = COG_ERROR_IO; goto done; }
        bool transport = host_options.role == COG_CLI_HOST_HEADLESS || host_options.role == COG_CLI_HOST_WORKER;
        if (transport) {
            if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) { rc = COG_ERROR_NOMEM; goto done; }
            runtime = true;
        }
        const cog_net_config_t template = {.host_runtime_initialized = transport,
            .dependency_memory = COG_NET_DEPENDENCY_MEMORY_DYNAMIC,
            .max_request_bytes = 8U * 16U * 1024U * 1024U, .max_connections = 4U};
        size_t role = (size_t)host_options.role;
        if (role >= sizeof(host_roles) / sizeof(host_roles[0]) || host_roles[role] == NULL)
            rc = COG_ERROR_INVALID_ARG;
        else rc = host_roles[role](&host_options, signal_fd, transport ? &template : NULL);
        goto done;
    }
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
    written = cog_cli_host_output_close_stdout(&output);
    if (rc == COG_OK) rc = written;
    cog_cli_configuration_free(candidate);
    cog_cli_app_free(app);
    cog_cli_host_options_close(&host_options);
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
    if (rc != COG_OK) {
        (void)fprintf(stderr, "cog-cli: %s\n", cog_cli_result_name(rc)); status = 1;
    }
    rc = cog_cli_host_output_close(&output, rc);
    if (rc != COG_OK) status = 1;
    if (masked) (void)sigprocmask(SIG_SETMASK, &old_mask, NULL);
    (void)sigaction(SIGPIPE, &old_pipe, NULL);
    return status;
}

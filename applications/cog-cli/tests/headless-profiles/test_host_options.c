/** @file test_host_options.c @brief Actual fixed host grammar and borrowed argv. */
#define _POSIX_C_SOURCE 200809L
#include "host_options.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); return 1; } } while (0)
#define ARG(s) ((char *)(s))
#define PARSE(array) cog_cli_host_options_open(&o, (int)(sizeof(array) / sizeof(array[0])), array, &code)
static int roles(void)
{
    cog_cli_host_options_t o = {0}; int code = -1;
    char *standalone[] = {ARG("cog"), ARG("--model"), ARG("--headless"), ARG("--prompt"), ARG("--worker")};
    CHECK(PARSE(standalone) == COG_OK && code == 0 && o.role == COG_CLI_HOST_STANDALONE);
    CHECK(strcmp(o.application.model_id, "--headless") == 0 && strcmp(o.application.prompt, "--worker") == 0);
    CHECK(o.application.model_id == standalone[2] && o.application.prompt == standalone[4]);
    cog_cli_host_options_close(&o); cog_cli_host_options_close(&o);
    char *headless[] = {ARG("cog"), ARG("--headless"), ARG("--socket=/ui"), ARG("--workers=8"), ARG("--worker-socket"), ARG("/workers"),
        ARG("--children=16"), ARG("--child-requests=18446744073709551615"), ARG("--child-tools=read,command"), ARG("--child-policy=ask"),
        ARG("--prompt"), ARG("--attach"), ARG("--model"), ARG("synthetic/model")};
    CHECK(PARSE(headless) == COG_OK && o.role == COG_CLI_HOST_HEADLESS && !o.application.interactive);
    CHECK(o.runtime.root_options == &o.application && o.application.socket_path == NULL && o.application.net == NULL);
    CHECK(strcmp(o.runtime.ui_socket, "/ui") == 0 && strcmp(o.runtime.worker_socket, "/workers") == 0 && o.runtime.workers == 8U);
    CHECK(o.runtime.children == 16U && o.runtime.child_requests == UINT64_MAX && o.runtime.child_profile.tools == 9U && o.runtime.child_profile.policy == COG_CLI_CHILD_POLICY_ASK);
    CHECK(strcmp(o.application.prompt, "--attach") == 0 && o.runtime.signal_fd == -1);
    cog_cli_host_options_close(&o);
    char *attach[] = {ARG("cog"), ARG("--attach"), ARG("/ui"), ARG("--projection-bytes=16777216"), ARG("--reconnect-ms=1")};
    CHECK(PARSE(attach) == COG_OK && o.role == COG_CLI_HOST_ATTACH && !o.client.watch);
    CHECK(o.client.projection_bytes == 16777216U && o.client.reconnect_ms == 1U && o.client.output == stdout && o.client.input_fd == 0 && o.client.signal_fd == -1);
    CHECK(o.application.model_id == NULL && o.application.credential_read == NULL && o.runtime.root_options == NULL);
    cog_cli_host_options_close(&o);
    char *watch[] = {ARG("cog"), ARG("--watch=/ui")};
    CHECK(PARSE(watch) == COG_OK && o.role == COG_CLI_HOST_WATCH && o.client.watch && o.client.reconnect_ms == 3000U);
    cog_cli_host_options_close(&o);
    char *worker[] = {ARG("cog"), ARG("--worker"), ARG("--control-socket=/workers"), ARG("--worker-incarnation=18446744073709551615"),
        ARG("--worker-number=2"), ARG("--parent-pid=3"), ARG("--executor-slots=18"), ARG("--frame-bytes=4294967295")};
    CHECK(PARSE(worker) == COG_OK && o.role == COG_CLI_HOST_WORKER);
    CHECK(o.worker.worker.incarnation == UINT64_MAX && o.worker.worker.number == 2U && o.worker.parent_pid == 3U && o.worker.executor_slots == 18U && o.worker.frame_bytes == UINT32_MAX && o.worker.signal_fd == -1);
    cog_cli_host_options_close(&o);
    return 0;
}
static int invalid(void)
{
    static const char *const cases[][5] = {
        {"--socket=/ui", NULL}, {"--headless", NULL},
        {"--headless", "--socket=/ui", "--interactive", NULL},
        {"--headless", "--socket=/ui", "--no-store", NULL},
        {"--headless", "--socket=/ui", "--sessions", NULL},
        {"--headless", "--socket=/ui", "--recover=id", "--fenced", NULL},
        {"--headless", "--socket=/ui", "--workers=1", NULL},
        {"--headless", "--socket=/ui", "--workers=9", NULL},
        {"--headless", "--socket=/ui", "--worker-socket=/ui", "--workers=1", NULL},
        {"--headless", "--socket=/ui", "--children=17", NULL},
        {"--headless", "--socket=/ui", "--child-policy=allow", NULL},
        {"--headless", "--socket=/ui", "--child-tools=read,read", NULL},
        {"--headless", "--socket=/ui", "--child-tools=read,", NULL},
        {"--headless", "--socket=/ui", "--child-requests=18446744073709551616", NULL},
        {"--headless", "--socket=/ui", "--children=-1", NULL},
        {"--headless", "--socket=/ui", "--workers=1", "--workers=2", NULL},
        {"--attach=/ui", "--model=secret", NULL}, {"--watch=/ui", "--config=/missing", NULL},
        {"--attach=/ui", "--watch=/ui", NULL}, {"--headless", "--attach=/ui", NULL},
        {"--attach=/ui", "--reconnect-ms=0", NULL}, {"--attach=/ui", "--projection-bytes=16777217", NULL},
        {"--worker", NULL}, {"--worker", "--prompt=forbidden", NULL}, {"--headless=yes", NULL},
        {"--workers=0", NULL}, {"--watch=", NULL}
    };
    FILE *capture = tmpfile(); int saved = dup(STDERR_FILENO);
    CHECK(capture != NULL && saved >= 0 && fflush(stderr) == 0 && dup2(fileno(capture), STDERR_FILENO) >= 0);
    for (size_t i = 0U; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        cog_cli_host_options_t o = {0}; int code = 0;
        char *argv[6] = {ARG("cog")}; int argc = 1;
        for (size_t j = 0U; cases[i][j] != NULL; ++j) argv[argc++] = (char *)cases[i][j];
        CHECK(cog_cli_host_options_open(&o, argc, argv, &code) == COG_ERROR_ABORTED && code == 2);
        cog_cli_host_options_close(&o);
    }
    CHECK(fflush(stderr) == 0 && dup2(saved, STDERR_FILENO) >= 0 && close(saved) == 0);
    CHECK(fseek(capture, 0L, SEEK_SET) == 0);
    char output[8192]; size_t length = fread(output, 1U, sizeof(output) - 1U, capture); output[length] = '\0';
    CHECK(strstr(output, "secret") == NULL && strstr(output, "/missing") == NULL);
    CHECK(fclose(capture) == 0);
    cog_cli_host_options_t o = {0}; int code = 0;
    CHECK(cog_cli_host_options_open(NULL, 0, NULL, &code) == COG_ERROR_NULL_PTR);
    CHECK(cog_cli_host_options_open(&o, 0, NULL, NULL) == COG_ERROR_NULL_PTR);
    CHECK(cog_cli_host_options_open(&o, -1, NULL, &code) == COG_ERROR_INVALID_ARG);
    char *many[1025] = {ARG("cog")};
    CHECK(cog_cli_host_options_open(&o, 1025, many, &code) == COG_ERROR_LIMIT);
    char huge[65537]; memset(huge, 'x', sizeof(huge)); huge[sizeof(huge) - 1U] = '\0';
    char *big[] = {ARG("cog"), huge}; CHECK(PARSE(big) == COG_ERROR_LIMIT);
    cog_cli_host_options_close(&o);
    return 0;
}
static int checked_help(void)
{
    cog_cli_host_options_t o = {0}; int code = 0;
    FILE *full = fopen("/dev/full", "w"); int saved = dup(STDOUT_FILENO);
    CHECK(full != NULL && saved >= 0 && fflush(stdout) == 0 && dup2(fileno(full), STDOUT_FILENO) >= 0);
    char *help[] = {ARG("cog"), ARG("--help")};
    CHECK(PARSE(help) == COG_ERROR_IO); cog_cli_host_options_close(&o);
    clearerr(stdout);
    CHECK(dup2(saved, STDOUT_FILENO) >= 0 && close(saved) == 0 && fclose(full) == 0);
    return 0;
}
int main(void)
{
    CHECK(roles() == 0); CHECK(invalid() == 0); CHECK(checked_help() == 0);
    puts("host role options: ok"); return 0;
}

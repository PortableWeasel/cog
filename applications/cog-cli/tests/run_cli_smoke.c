/**
 * @file run_cli_smoke.c
 * @brief Spawns the built cog-cli binary and checks its stdout and exit
 *        code for the stream, error-retry, and cancel mock modes.
 *
 * The binary path is injected at compile time via COG_CLI_BIN.
 */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef COG_CLI_BIN
#define COG_CLI_BIN "cog-cli"
#endif

static int failures = 0;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            (void)fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__,         \
                          __LINE__, #cond);                             \
            ++failures;                                                 \
        }                                                               \
    } while (0)

/** Run the binary with @p args and capture stdout into @p out. */
static int run_binary(const char *args, char *out, size_t out_cap)
{
    char cmd[4096];
    FILE *fp;
    size_t out_len = 0U;
    int rc;

    (void)snprintf(cmd, sizeof(cmd), "\"%s\" %s", COG_CLI_BIN, args);
    fp = popen(cmd, "r");
    if (fp == NULL) {
        (void)perror("popen");
        return -1;
    }
    while (fgets(cmd, (int)sizeof(cmd), fp) != NULL) {
        size_t n = strlen(cmd);

        if (out_len + n < out_cap) {
            (void)memcpy(out + out_len, cmd, n);
            out_len += n;
        }
    }
    out[out_len] = '\0';
    rc = pclose(fp);
    return rc; /* wait status; 0 means clean exit */
}

static void test_stream(void)
{
    char out[8192];

    CHECK(run_binary("--prompt hello", out, sizeof(out)) == 0);
    CHECK(strstr(out, "Hello from cog-cli!") != NULL);
}

static void test_error_retry(void)
{
    char out[8192];

    CHECK(run_binary("--prompt hello --mock-mode error", out, sizeof(out))
          == 0);
    CHECK(strstr(out, "retried ok") != NULL);
}

static void test_cancel(void)
{
    char out[8192];

    /* A cancelled stream produces no assistant output. */
    CHECK(run_binary("--prompt hello --mock-mode cancel", out, sizeof(out))
          == 0);
    CHECK(strstr(out, "Hello from cog-cli!") == NULL);
}

static void test_custom_chunks(void)
{
    char out[8192];

    CHECK(run_binary("--prompt hi --chunks \"a,b,c\"", out, sizeof(out))
          == 0);
    CHECK(strstr(out, "abc") != NULL);
}

static void test_help_exit(void)
{
    char out[8192];

    CHECK(run_binary("--help", out, sizeof(out)) == 0);
    CHECK(strstr(out, "Usage: cog-cli") != NULL);
}

static void test_bad_option_exit(void)
{
    char out[8192];

    /* Unknown option exits non-zero. */
    CHECK(run_binary("--bogus", out, sizeof(out)) != 0);
}

int main(void)
{
    test_stream();
    test_error_retry();
    test_cancel();
    test_custom_chunks();
    test_help_exit();
    test_bad_option_exit();

    if (failures != 0) {
        (void)fprintf(stderr, "run_cli_smoke: %d failure(s)\n", failures);
        return 1;
    }
    (void)printf("run_cli_smoke: all checks passed\n");
    return 0;
}

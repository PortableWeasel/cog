/** @file test_configuration.c @brief Offline configuration conformance. */
#define _POSIX_C_SOURCE 200809L
#include "configuration.h"
#include "options_internal.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#define CHECK(x)                                                                       \
    do {                                                                               \
        if (!(x)) {                                                                    \
            fprintf(stderr, "line %d: %s\n", __LINE__, #x);                            \
            return 1;                                                                  \
        }                                                                              \
    } while (0)
static const char synthetic_secret[] = "SYNTHETIC_TEST_VALUE_ONLY";
static const char *const variables[] = {"COG_MODEL",
                                        "COG_ENDPOINT",
                                        "COG_BASE_URL",
                                        "COG_CREDENTIAL_REF",
                                        "COG_WORKSPACE",
                                        "COG_TOOL_POLICY",
                                        "COG_MAX_OUTPUT_TOKENS",
                                        "COG_MAX_TOOL_ROUNDS",
                                        "COG_MAX_TURN_MS",
                                        "COG_MAX_TOOL_MS",
                                        "COG_MAX_TOOL_OUTPUT_BYTES",
                                        "COG_MAX_CONTEXT_BYTES",
                                        "COG_MAX_RETRIES",
                                        "COG_MAX_RETRY_ELAPSED_MS",
                                        "COG_STORE_MODE",
                                        "COG_STORE_DIR"};
static int clean(void)
{
    size_t i;
    for (i = 0U; i < sizeof(variables) / sizeof(variables[0]); ++i)
        CHECK(unsetenv(variables[i]) == 0);
    CHECK(setenv("COG_TEST_SECRET", synthetic_secret, 1) == 0);
    CHECK(setenv("HOME", "/synthetic/home", 1) == 0);
    CHECK(unsetenv("XDG_STATE_HOME") == 0);
    return 0;
}
static int fixture(char *path, const char *contents)
{
    int fd = mkstemp(path);
    size_t n = strlen(contents);
    FILE *f;
    CHECK(fd >= 0);
    f = fdopen(fd, "wb");
    CHECK(f != NULL);
    CHECK(fwrite(contents, 1U, n, f) == n);
    CHECK(fclose(f) == 0);
    return 0;
}
static void base(cog_cli_options_t *o)
{
    cog_cli_options_defaults(o);
    o->credential_ref = "env:COG_TEST_SECRET";
    o->explicit_options = COG_CLI_OPT_CREDENTIAL;
}
static int precedence(void)
{
    char path[] = "/tmp/cog-config-j-XXXXXX";
    cog_cli_options_t o;
    cog_cli_configuration_t *c = NULL;
    const cog_cli_options_t *e;
    const char *file =
        "models.default=file/"
        "model\nproviders.selected=openrouter\nproviders.openrouter.base-url=https://"
        "file.invalid/"
        "v1\ntools.policy=allow\nlimits.max-retries=7\nstore.mode=durable\n";
    CHECK(clean() == 0);
    CHECK(fixture(path, file) == 0);
    base(&o);
    o.config_path = path;
    CHECK(cog_cli_configuration_new(&c, &o, 1U) == COG_OK);
    e = cog_cli_configuration_options(c);
    CHECK(strcmp(e->model_id, "file/model") == 0 && e->max_retries == 7U &&
          e->tool_policy == COG_CLI_TOOL_POLICY_ALLOW);
    CHECK(strcmp(e->store_dir, "/synthetic/home/.local/state/cog") == 0);
    cog_cli_configuration_free(c);
    CHECK(setenv("COG_MODEL", "env/model", 1) == 0);
    CHECK(setenv("COG_MAX_RETRIES", "2", 1) == 0);
    CHECK(setenv("COG_TOOL_POLICY", "deny", 1) == 0);
    CHECK(setenv("COG_BASE_URL", "https://env.invalid/v2", 1) == 0);
    CHECK(cog_cli_configuration_new(&c, &o, 2U) == COG_OK);
    e = cog_cli_configuration_options(c);
    CHECK(strcmp(e->model_id, "env/model") == 0 && e->max_retries == 2U &&
          e->tool_policy == COG_CLI_TOOL_POLICY_DENY);
    CHECK(strcmp(e->base_url, "https://env.invalid/v2") == 0);
    cog_cli_configuration_free(c);
    o.model_id = "cli/model";
    o.max_retries = 0U;
    o.tool_policy = COG_CLI_TOOL_POLICY_ASK;
    o.store_mode = COG_CLI_STORE_DISABLED;
    o.explicit_options |= COG_CLI_OPT_MODEL | COG_CLI_OPT_RETRIES | COG_CLI_OPT_POLICY |
                          COG_CLI_OPT_STORE_MODE;
    CHECK(cog_cli_configuration_new(&c, &o, 3U) == COG_OK);
    e = cog_cli_configuration_options(c);
    CHECK(strcmp(e->model_id, "cli/model") == 0 && e->max_retries == 0U &&
          e->tool_policy == COG_CLI_TOOL_POLICY_ASK);
    CHECK(e->store_mode == COG_CLI_STORE_DISABLED);
    cog_cli_configuration_free(c);
    o.explicit_options =
        COG_CLI_OPT_CREDENTIAL; /* parse/default-filled values carry no authority */
    CHECK(cog_cli_configuration_new(&c, &o, 4U) == COG_OK);
    e = cog_cli_configuration_options(c);
    CHECK(strcmp(e->model_id, "env/model") == 0 && e->max_retries == 2U);
    cog_cli_configuration_free(c);
    CHECK(unlink(path) == 0);
    return 0;
}
static int field_matrix(void)
{
    size_t i;
    CHECK(clean() == 0);
    for (i = 0U; i < cog_cli_field_count; ++i) {
        const cli_field_t *f = &cog_cli_fields[i];
        cog_cli_options_t o;
        cog_cli_configuration_t *c = NULL;
        const unsigned char *field;
        const char *value = f->kind == CLI_U32 || f->kind == CLI_SIZE ? "23"
                            : f->kind == CLI_POLICY                   ? "deny"
                            : f->kind == CLI_STORE                    ? "disabled"
                            : f->bit == COG_CLI_OPT_CREDENTIAL ? "env:COG_TEST_SECRET"
                            : f->bit == COG_CLI_OPT_BASE_URL
                                ? "https://synthetic.invalid/v1"
                            : f->bit == COG_CLI_OPT_ENDPOINT ? "openrouter"
                                                             : "synthetic";
        if (f->environment == NULL)
            continue;
        base(&o);
        o.model_id = "base/model";
        o.explicit_options |= COG_CLI_OPT_MODEL;
        if (f->bit == COG_CLI_OPT_MODEL)
            o.explicit_options &= ~(uint32_t)COG_CLI_OPT_MODEL;
        if (f->bit == COG_CLI_OPT_CREDENTIAL)
            o.explicit_options &= ~(uint32_t)COG_CLI_OPT_CREDENTIAL;
        CHECK(setenv(f->environment, value, 1) == 0);
        CHECK(cog_cli_configuration_new(&c, &o, 10U + i) == COG_OK);
        field = (const unsigned char *)cog_cli_configuration_options(c) + f->offset;
        if (f->kind == CLI_STRING)
            CHECK(strcmp(*(const char *const *)field, value) == 0);
        else if (f->kind == CLI_U32)
            CHECK(*(const uint32_t *)field == 23U);
        else if (f->kind == CLI_SIZE)
            CHECK(*(const size_t *)field == 23U);
        else if (f->kind == CLI_POLICY)
            CHECK(*(const cog_cli_tool_policy_t *)field == COG_CLI_TOOL_POLICY_DENY);
        else if (f->kind == CLI_STORE)
            CHECK(*(const cog_cli_store_mode_t *)field == COG_CLI_STORE_DISABLED);
        cog_cli_configuration_free(c);
        CHECK(setenv(f->environment, "", 1) == 0);
        CHECK(cog_cli_configuration_new(&c, &o, 100U + i) == COG_ERROR_INVALID_ARG &&
              c == NULL);
        CHECK(unsetenv(f->environment) == 0);
    }
    return 0;
}
static cog_result_t resolve(void *arg, const char *ref, char *out, size_t cap,
                            size_t *required)
{
    const cog_result_t *verdict = arg;
    size_t n = sizeof(synthetic_secret);
    (void)ref;
    *required = n;
    if (cap < n)
        return COG_ERROR_OVERFLOW;
    memcpy(out, synthetic_secret, n);
    return *verdict;
}
static int generation(void)
{
    cog_cli_options_t o;
    cog_cli_configuration_t *old = NULL, *next = NULL, *bad = NULL;
    const cog_endpoint_t *ep;
    char text[160], small[2] = {'x', 'y'};
    size_t required = 0U;
    cog_result_t verdict = COG_OK;
    static const cog_provider_vtable_t descriptor = {
        0}; /* identity only, never dispatched */
    cog_provider_limits_t limits = {1U, 1U, 1U, 1U, 1U, 1U, 1U, 1U, 1U, 1U, 1U, 1U, 1U};
    char model[64] = "owned/model";
    CHECK(clean() == 0);
    base(&o);
    o.model_id = model;
    o.explicit_options |= COG_CLI_OPT_MODEL;
    o.credential_ref = "host:synthetic";
    o.credential_read = resolve;
    o.credential_arg = &verdict;
    o.net = (cog_net_ctx_t *)(void *)&o; /* opaque borrowed identity, no net calls */
    CHECK(cog_cli_configuration_new(&old, &o, 31U) == COG_OK);
    CHECK(cog_cli_configuration_bind(old, &descriptor, NULL, NULL, &limits) == COG_OK);
    ep = cog_cli_configuration_endpoint(old);
    CHECK(ep != NULL && ep->generation == 31U);
    CHECK(strcmp(ep->api_key, synthetic_secret) == 0 &&
          strcmp(ep->models->models[0].id, "owned/model") == 0);
    strcpy(model, "changed/model");
    CHECK(cog_cli_configuration_new(&next, &o, 32U) == COG_OK);
    CHECK(cog_cli_configuration_bind(next, &descriptor, NULL, NULL, &limits) == COG_OK);
    verdict = COG_ERROR_DENIED;
    CHECK(cog_cli_configuration_new(&bad, &o, 33U) == COG_ERROR_DENIED && bad == NULL);
    CHECK(strcmp(ep->models->models[0].id, "owned/model") == 0 &&
          strcmp(ep->api_key, synthetic_secret) == 0);
    CHECK(cog_cli_configuration_bind(old, &descriptor, NULL, NULL, &limits) ==
          COG_ERROR_INVALID_STATE);
    CHECK(cog_cli_configuration_describe(old, small, sizeof(small), &required) ==
          COG_ERROR_OVERFLOW);
    CHECK(small[0] == 'x' && small[1] == 'y');
    CHECK(cog_cli_configuration_describe(old, text, sizeof(text), &required) == COG_OK);
    CHECK(strstr(text, synthetic_secret) == NULL &&
          strstr(text, "host:synthetic") == NULL && strstr(text, "REDACTED") != NULL);
    cog_cli_configuration_free(next);
    CHECK(strcmp(ep->api_key, synthetic_secret) == 0); /* old borrower still valid */
    cog_cli_configuration_free(old);
    return 0;
}
static int failures_and_defaults(void)
{
    cog_cli_options_t o;
    cog_cli_configuration_t *c = NULL;
    const cog_cli_options_t *e;
    char path[] = "/tmp/cog-config-bad-j-XXXXXX";
    CHECK(clean() == 0);
    base(&o);
    CHECK(cog_cli_configuration_new(&c, &o, 1U) == COG_ERROR_NOT_FOUND &&
          c == NULL); /* no implicit model */
    o.model_id = "synthetic/model";
    o.explicit_options |= COG_CLI_OPT_MODEL;
    CHECK(setenv("XDG_STATE_HOME", "/synthetic/state", 1) == 0);
    CHECK(cog_cli_configuration_new(&c, &o, 1U) == COG_OK);
    e = cog_cli_configuration_options(c);
    CHECK(e->backend == COG_CLI_BACKEND_REAL && e->store_mode == COG_CLI_STORE_DURABLE);
    CHECK(strcmp(e->store_dir, "/synthetic/state/cog") == 0);
    cog_cli_configuration_free(c);
    CHECK(setenv("XDG_STATE_HOME", "relative", 1) == 0);
    CHECK(unsetenv("HOME") == 0);
    CHECK(cog_cli_configuration_new(&c, &o, 1U) == COG_ERROR_NOT_FOUND);
    o.store_mode = COG_CLI_STORE_DISABLED;
    o.explicit_options |= COG_CLI_OPT_STORE_MODE;
    CHECK(cog_cli_configuration_new(&c, &o, 1U) == COG_OK);
    cog_cli_configuration_free(c);
    CHECK(unsetenv("COG_TEST_SECRET") == 0);
    CHECK(cog_cli_configuration_new(&c, &o, 1U) == COG_ERROR_NOT_FOUND && c == NULL);
    o.backend = COG_CLI_BACKEND_MOCK;
    o.explicit_options |= COG_CLI_OPT_BACKEND;
    CHECK(cog_cli_configuration_new(&c, &o, 1U) == COG_OK);
    e = cog_cli_configuration_options(c);
    CHECK(e->backend == COG_CLI_BACKEND_MOCK && strcmp(e->endpoint_name, "mock") == 0);
    cog_cli_configuration_free(c);
    o.explicit_options |= 1U << 31;
    CHECK(cog_cli_configuration_new(&c, &o, 1U) == COG_ERROR_INVALID_ARG);
    o.explicit_options &= ~(1U << 31);
    o.max_output_tokens = 0U;
    o.explicit_options |= COG_CLI_OPT_OUTPUT_TOKENS;
    CHECK(cog_cli_configuration_new(&c, &o, 1U) == COG_ERROR_LIMIT);
    CHECK(fixture(path, "limits.max-retries=1\nlimits.max-retries=2\n") == 0);
    o.config_path = path;
    CHECK(cog_cli_configuration_new(&c, &o, 1U) == COG_ERROR_INVALID_ARG);
    CHECK(unlink(path) == 0);
    return 0;
}
static int malformed_and_capacity(void)
{
    const char *invalid[] = {"unknown.key=value\n",
                             "models.default=\n",
                             "models.default=x\nmodels.default=y\n",
                             "limits.max-retries=-1\n",
                             "limits.max-retries=4294967296\n",
                             "limits.max-turn-ms=0\n",
                             "store.mode=default\n",
                             "client.backend=automatic\n",
                             "tools.policy=ALLOW\n",
                             "models.default=quoted value\n",
                             "not-a-key-value\n"};
    cog_cli_options_t o;
    cog_cli_configuration_t *c = NULL;
    size_t i;
    char *oversized = malloc(65538U);
    CHECK(oversized != NULL);
    CHECK(clean() == 0);
    for (i = 0U; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        char path[] = "/tmp/cog-config-malformed-j-XXXXXX";
        CHECK(fixture(path, invalid[i]) == 0);
        base(&o);
        o.config_path = path;
        CHECK(cog_cli_configuration_new(&c, &o, 1U) != COG_OK && c == NULL);
        CHECK(unlink(path) == 0);
    }
    {
        char path[] = "/tmp/cog-config-large-j-XXXXXX";
        memset(oversized, '#', 65537U);
        oversized[65537U] = '\0';
        CHECK(fixture(path, oversized) == 0);
        base(&o);
        o.config_path = path;
        CHECK(cog_cli_configuration_new(&c, &o, 1U) == COG_ERROR_LIMIT && c == NULL);
        CHECK(unlink(path) == 0);
    }
    {
        char path[] = "/tmp/cog-config-nul-j-XXXXXX";
        int fd = mkstemp(path);
        static const char bytes[] = "models.default=a\0b\n";
        CHECK(fd >= 0);
        CHECK(write(fd, bytes, sizeof(bytes) - 1U) == (ssize_t)(sizeof(bytes) - 1U));
        CHECK(close(fd) == 0);
        base(&o);
        o.config_path = path;
        CHECK(cog_cli_configuration_new(&c, &o, 1U) == COG_ERROR_INVALID_ARG &&
              c == NULL);
        CHECK(unlink(path) == 0);
    }
    base(&o);
    o.model_id = "synthetic/model";
    o.explicit_options |= COG_CLI_OPT_MODEL | COG_CLI_OPT_BASE_URL;
    o.base_url = "https://secret@synthetic.invalid/v1";
    CHECK(cog_cli_configuration_new(&c, &o, 1U) == COG_ERROR_INVALID_ARG);
    o.base_url = "https://synthetic.invalid/v1?key=synthetic";
    CHECK(cog_cli_configuration_new(&c, &o, 1U) == COG_ERROR_INVALID_ARG);
    o.base_url = "http://synthetic.invalid/v1";
    CHECK(cog_cli_configuration_new(&c, &o, 1U) == COG_ERROR_INVALID_ARG);
    o.base_url = "https://synthetic.invalid/v1";
    o.credential_ref = "env:INVALID-NAME";
    CHECK(cog_cli_configuration_new(&c, &o, 1U) == COG_ERROR_INVALID_ARG);
    o.credential_ref = "pass:synthetic";
    CHECK(cog_cli_configuration_new(&c, &o, 1U) == COG_ERROR_UNSUPPORTED);
    o.credential_ref = "env:COG_TEST_SECRET";
    CHECK(setenv("COG_TEST_SECRET", "synthetic\r\nheader", 1) == 0);
    CHECK(cog_cli_configuration_new(&c, &o, 1U) == COG_ERROR_INVALID_ARG);
    CHECK(setenv("COG_TEST_SECRET", "", 1) == 0);
    CHECK(cog_cli_configuration_new(&c, &o, 1U) == COG_ERROR_NOT_FOUND);
    memset(oversized, 's', 16384U);
    oversized[16384U] = '\0';
    CHECK(setenv("COG_TEST_SECRET", oversized, 1) == 0);
    CHECK(cog_cli_configuration_new(&c, &o, 1U) == COG_ERROR_LIMIT);
    free(oversized);
    return 0;
}

static int error_redaction(void)
{
    FILE *capture = tmpfile();
    int saved = dup(STDERR_FILENO), code = 0;
    cog_cli_options_t o;
    char argument[128], bytes[512];
    size_t n;
    char *args[] = {argument, argument};
    CHECK(capture != NULL && saved >= 0);
    CHECK(snprintf(argument, sizeof(argument), "--unknown=%s", synthetic_secret) > 0);
    CHECK(fflush(stderr) == 0 && dup2(fileno(capture), STDERR_FILENO) >= 0);
    CHECK(cog_cli_options_parse(&o, 2, args, &code) == COG_ERROR_ABORTED);
    CHECK(fflush(stderr) == 0 && dup2(saved, STDERR_FILENO) >= 0 && close(saved) == 0);
    CHECK(fseek(capture, 0L, SEEK_SET) == 0);
    n = fread(bytes, 1U, sizeof(bytes) - 1U, capture);
    bytes[n] = '\0';
    CHECK(strstr(bytes, synthetic_secret) == NULL &&
          strstr(bytes, "invalid option") != NULL);
    CHECK(fclose(capture) == 0);
    return 0;
}

static int parsing(void)
{
#define S(x) ((char *)(x))
    char *args[] = {
        S("cog-cli"),       S("--mock"), S("--max-retries=0"),
        S("--tool-policy"), S("ask"),    S("--base-url=https://fixture.invalid"),
        S("--no-store")};
    char *bad[] = {S("cog-cli"), S("--no-store"), S("--store"), S("/tmp/synthetic")};
    char *overflow[] = {S("cog-cli"), S("--max-retries=4294967296")};
    cog_cli_options_t o;
    int code = 0;
    CHECK(cog_cli_options_parse(&o, 7, args, &code) == COG_OK);
    CHECK(o.max_retries == 0U && o.tool_policy == COG_CLI_TOOL_POLICY_ASK);
    CHECK((o.explicit_options & (COG_CLI_OPT_RETRIES | COG_CLI_OPT_POLICY |
                                 COG_CLI_OPT_STORE_MODE | COG_CLI_OPT_BASE_URL)) ==
          (COG_CLI_OPT_RETRIES | COG_CLI_OPT_POLICY | COG_CLI_OPT_STORE_MODE |
           COG_CLI_OPT_BASE_URL));
    CHECK(cog_cli_options_parse(&o, 4, bad, &code) == COG_ERROR_ABORTED && code == 2);
    CHECK(cog_cli_options_parse(&o, 2, overflow, &code) == COG_ERROR_ABORTED &&
          code == 2);
    return 0;
#undef S
}
int main(void)
{
    CHECK(malformed_and_capacity() == 0);
    CHECK(error_redaction() == 0);
    CHECK(parsing() == 0);
    CHECK(precedence() == 0);
    CHECK(field_matrix() == 0);
    CHECK(generation() == 0);
    CHECK(failures_and_defaults() == 0);
    puts("CLI effective configuration: ok");
    return 0;
}

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
        if (f->kind == CLI_STRING) {
            const char *string;
            memcpy(&string, field, sizeof(string));
            CHECK(strcmp(string, value) == 0);
        } else if (f->kind == CLI_U32) {
            uint32_t n;
            memcpy(&n, field, sizeof(n));
            CHECK(n == 23U);
        } else if (f->kind == CLI_SIZE) {
            size_t n;
            memcpy(&n, field, sizeof(n));
            CHECK(n == 23U);
        } else if (f->kind == CLI_POLICY) {
            cog_cli_tool_policy_t n;
            memcpy(&n, field, sizeof(n));
            CHECK(n == COG_CLI_TOOL_POLICY_DENY);
        } else if (f->kind == CLI_STORE) {
            cog_cli_store_mode_t n;
            memcpy(&n, field, sizeof(n));
            CHECK(n == COG_CLI_STORE_DISABLED);
        }
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
static cog_result_t descriptor_preflight(cog_provider_t *p,
                                         const cog_provider_request_t *r,
                                         cog_provider_preflight_stage_t stage)
{
    (void)p;
    (void)r;
    (void)stage;
    return COG_ERROR_UNSUPPORTED; /* Identity-only descriptor, never driven. */
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
        .preflight = descriptor_preflight}; /* identity only, never dispatched */
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
/* Independent critic regression: protocol fields must be valid before a
 * generation can be published. Filesystem strings remain arbitrary path bytes. */
static int protocol_inputs(void)
{
    static const char *const bad_models[] = {
        "synthetic/\xc0\xaf",         /* overlong UTF-8 */
        "synthetic/\x80",             /* isolated continuation */
        "synthetic/\xe2\x82",         /* incomplete scalar */
        "synthetic/\xed\xa0\x80",     /* surrogate */
        "synthetic/\xf4\x90\x80\x80", /* above U+10FFFF */
        "synthetic/\xc1\xbf",
        "synthetic/\xe0\x9f\xbf",
        "synthetic/\xf0\x8f\xbf\xbf",
        "synthetic/\xf5\x80\x80\x80",
        "synthetic/\xe2X\x83",
        "synthetic/\xf0\x90\x80"};
    static const char *const bad_urls[] = {
        "https://:443/v1",
        "https://[]/v1",
        "https://synthetic.invalid:bad/v1",
        "https://synthetic.invalid:65536/v1",
        "https://[::1/v1",
        "https:///v1",
        "https://synthetic.invalid:/v1",
        "https://synthetic.invalid:0/v1",
        "https://synthetic.invalid:18446744073709551616/v1",
        "https://::1/v1",
        "https://[not-ipv6]/v1",
        "https://[::1]suffix/v1",
        "https://[::1]:65536/v1",
        "https://[::1]:/v1",
        "https://[::1]]/v1"};
    static const char *const good_urls[] = {"https://synthetic.invalid/v1",
                                            "https://synthetic.invalid:443/v1",
                                            "https://[::1]:443/v1",
                                            "https://[::1]/v1",
                                            "https://127.0.0.1:1/v1",
                                            "https://localhost:65535/v1",
                                            "https://synthetic.invalid.:000443/v1",
                                            "https://[2001:db8::1]:65535/v1",
                                            "https://[::ffff:192.0.2.128]:1/v1"};
    static const char *const good_models[] = {
        "synthetic/\xc2\x80",         "synthetic/\xdf\xbf",
        "synthetic/\xe0\xa0\x80",     "synthetic/\xed\x9f\xbf",
        "synthetic/\xee\x80\x80",     "synthetic/\xef\xbf\xbf",
        "synthetic/\xf0\x90\x80\x80", "synthetic/\xf4\x8f\xbf\xbf"};
    cog_cli_options_t o;
    cog_cli_configuration_t *c = NULL;
    size_t i;
    CHECK(clean() == 0);
    base(&o);
    o.model_id = "synthetic/model";
    o.store_mode = COG_CLI_STORE_DISABLED;
    o.explicit_options |=
        COG_CLI_OPT_MODEL | COG_CLI_OPT_STORE_MODE | COG_CLI_OPT_BASE_URL;
    for (i = 0U; i < sizeof(good_urls) / sizeof(good_urls[0]); ++i) {
        o.base_url = good_urls[i];
        CHECK(cog_cli_configuration_new(&c, &o, 1U) == COG_OK);
        cog_cli_configuration_free(c);
    }
    for (i = 0U; i < sizeof(bad_urls) / sizeof(bad_urls[0]); ++i) {
        cog_result_t rc;
        bool empty;
        o.base_url = bad_urls[i];
        rc = cog_cli_configuration_new(&c, &o, 1U);
        empty = c == NULL;
        cog_cli_configuration_free(c);
        CHECK(rc == COG_ERROR_INVALID_ARG && empty);
        o.explicit_options &= ~(uint32_t)COG_CLI_OPT_BASE_URL;
        CHECK(setenv("COG_BASE_URL", bad_urls[i], 1) == 0);
        CHECK(cog_cli_configuration_new(&c, &o, 1U) == COG_ERROR_INVALID_ARG &&
              c == NULL);
        CHECK(unsetenv("COG_BASE_URL") == 0);
        {
            char path[] = "/tmp/cog-config-url-j-XXXXXX", contents[256];
            CHECK(snprintf(contents, sizeof(contents),
                           "providers.openrouter.base-url=%s\n", bad_urls[i]) > 0);
            CHECK(fixture(path, contents) == 0);
            o.config_path = path;
            CHECK(cog_cli_configuration_new(&c, &o, 1U) == COG_ERROR_INVALID_ARG &&
                  c == NULL);
            CHECK(unlink(path) == 0);
            o.config_path = NULL;
        }
        o.explicit_options |= COG_CLI_OPT_BASE_URL;
    }
    o.base_url = "https://synthetic.invalid/v1";
    for (i = 0U; i < sizeof(bad_models) / sizeof(bad_models[0]); ++i) {
        cog_result_t rc;
        bool empty;
        o.model_id = bad_models[i];
        rc = cog_cli_configuration_new(&c, &o, 1U);
        empty = c == NULL;
        cog_cli_configuration_free(c);
        CHECK(rc == COG_ERROR_INVALID_ARG && empty);
        o.explicit_options &= ~(uint32_t)COG_CLI_OPT_MODEL;
        CHECK(setenv("COG_MODEL", bad_models[i], 1) == 0);
        CHECK(cog_cli_configuration_new(&c, &o, 1U) == COG_ERROR_INVALID_ARG &&
              c == NULL);
        CHECK(unsetenv("COG_MODEL") == 0);
        {
            char path[] = "/tmp/cog-config-model-j-XXXXXX", contents[256];
            CHECK(snprintf(contents, sizeof(contents), "models.default=%s\n",
                           bad_models[i]) > 0);
            CHECK(fixture(path, contents) == 0);
            o.config_path = path;
            CHECK(cog_cli_configuration_new(&c, &o, 1U) == COG_ERROR_INVALID_ARG &&
                  c == NULL);
            CHECK(unlink(path) == 0);
            o.config_path = NULL;
        }
        o.explicit_options |= COG_CLI_OPT_MODEL;
    }
    for (i = 0U; i < sizeof(good_models) / sizeof(good_models[0]); ++i) {
        o.model_id = good_models[i];
        CHECK(cog_cli_configuration_new(&c, &o, 1U) == COG_OK);
        cog_cli_configuration_free(c);
    }
    o.workspace_root = "synthetic/\x80"; /* Paths retain native filesystem bytes. */
    o.explicit_options |= COG_CLI_OPT_WORKSPACE;
    o.model_id = "synthetic/\xe2\x98\x83"; /* valid multibyte scalar */
    CHECK(cog_cli_configuration_new(&c, &o, 1U) == COG_OK);
    cog_cli_configuration_free(c);
    return 0;
}

static cog_result_t offline_resolver(void *arg, const char *reference, char *output,
                                     size_t capacity, size_t *required)
{
    unsigned *calls = arg;
    (void)reference;
    (void)output;
    (void)capacity;
    (void)required;
    ++*calls;
    return COG_ERROR_DENIED;
}
static int session_configuration(void)
{
    static const cog_cli_session_action_t offline[] = {COG_CLI_SESSION_LIST,
                                                       COG_CLI_SESSION_RECOVER};
    cog_cli_options_t o;
    cog_cli_configuration_t *c = NULL;
    const cog_cli_options_t *effective;
    size_t i;
    unsigned calls = 0U;
    char source[128] = "session-owned-copy", text[160];
    size_t required;
    CHECK(clean() == 0);
    CHECK(setenv("COG_CREDENTIAL_REF", "env:COG_L1_MISSING_KEY", 1) == 0);
    CHECK(unsetenv("COG_L1_MISSING_KEY") == 0);
    for (i = 0U; i < sizeof(offline) / sizeof(offline[0]); ++i) {
        cog_cli_options_defaults(&o);
        o.session_action = offline[i];
        o.source_session_id =
            o.session_action == COG_CLI_SESSION_RECOVER ? source : NULL;
        o.previous_processes_fenced = o.session_action == COG_CLI_SESSION_RECOVER;
        o.show_usage = true;
        o.credential_read = offline_resolver;
        o.credential_arg = &calls;
        o.net = (cog_net_ctx_t *)(void *)&o;
        o.workspace_root = "/synthetic/workspace";
        o.tool_policy = COG_CLI_TOOL_POLICY_ALLOW;
        o.explicit_options = COG_CLI_OPT_WORKSPACE | COG_CLI_OPT_POLICY;
        CHECK(cog_cli_configuration_new(&c, &o, 100U + i) == COG_OK);
        effective = cog_cli_configuration_options(c);
        CHECK(effective->session_action == o.session_action && effective->show_usage);
        CHECK(effective->store_mode == COG_CLI_STORE_DURABLE);
        CHECK(strcmp(effective->store_dir, "/synthetic/home/.local/state/cog") == 0);
        CHECK(effective->model_id == NULL && effective->net == NULL);
        CHECK(effective->credential_read == NULL && effective->credential_arg == NULL &&
              calls == 0U);
        CHECK(cog_cli_configuration_endpoint(c) == NULL);
        CHECK(cog_cli_configuration_bind(c, NULL, NULL, NULL, NULL) ==
              COG_ERROR_INVALID_STATE);
        CHECK(cog_cli_configuration_endpoint(c) == NULL);
        CHECK(cog_cli_configuration_describe(c, text, sizeof(text), &required) ==
              COG_OK);
        CHECK(strstr(text, "tools=disabled") != NULL &&
              strstr(text, "credential=none") != NULL);
        if (o.session_action == COG_CLI_SESSION_RECOVER) {
            source[0] = 'X';
            CHECK(strcmp(effective->source_session_id, "session-owned-copy") == 0);
            CHECK(effective->previous_processes_fenced);
            source[0] = 's';
        }
        cog_cli_configuration_free(c);
        o.backend = COG_CLI_BACKEND_MOCK;
        o.explicit_options |= COG_CLI_OPT_BACKEND;
        CHECK(cog_cli_configuration_new(&c, &o, 200U + i) == COG_OK);
        CHECK(cog_cli_configuration_options(c)->store_mode == COG_CLI_STORE_DURABLE);
        CHECK(cog_cli_configuration_options(c)->model_id == NULL);
        cog_cli_configuration_free(c);
        o.store_mode = COG_CLI_STORE_DISABLED;
        o.explicit_options |= COG_CLI_OPT_STORE_MODE;
        CHECK(cog_cli_configuration_new(&c, &o, 300U + i) == COG_ERROR_INVALID_ARG &&
              c == NULL);
        o.explicit_options &= ~(uint32_t)COG_CLI_OPT_STORE_MODE;
        CHECK(setenv("COG_STORE_MODE", "disabled", 1) == 0);
        CHECK(cog_cli_configuration_new(&c, &o, 301U + i) == COG_ERROR_INVALID_ARG &&
              c == NULL);
        CHECK(unsetenv("COG_STORE_MODE") == 0);
        CHECK(setenv("COG_MODEL", "synthetic/\xc0\xaf", 1) == 0);
        CHECK(cog_cli_configuration_new(&c, &o, 302U + i) == COG_ERROR_INVALID_ARG &&
              c == NULL);
        CHECK(unsetenv("COG_MODEL") == 0);
        CHECK(setenv("COG_CREDENTIAL_REF", "env:INVALID-NAME", 1) == 0);
        CHECK(cog_cli_configuration_new(&c, &o, 303U + i) == COG_ERROR_INVALID_ARG &&
              c == NULL);
        CHECK(setenv("COG_CREDENTIAL_REF", "env:COG_L1_MISSING_KEY", 1) == 0);
        CHECK(setenv("COG_BASE_URL", "https://:443/v1", 1) == 0);
        CHECK(cog_cli_configuration_new(&c, &o, 304U + i) == COG_ERROR_INVALID_ARG &&
              c == NULL);
        CHECK(unsetenv("COG_BASE_URL") == 0);
        CHECK(setenv("COG_MAX_CONTEXT_BYTES", "0", 1) == 0);
        CHECK(cog_cli_configuration_new(&c, &o, 305U + i) == COG_ERROR_LIMIT &&
              c == NULL);
        CHECK(unsetenv("COG_MAX_CONTEXT_BYTES") == 0);
        CHECK(unsetenv("HOME") == 0);
        CHECK(cog_cli_configuration_new(&c, &o, 306U + i) == COG_ERROR_NOT_FOUND &&
              c == NULL);
        o.store_dir = "/synthetic/explicit";
        o.explicit_options |= COG_CLI_OPT_STORE_DIR;
        CHECK(cog_cli_configuration_new(&c, &o, 307U + i) == COG_OK);
        CHECK(strcmp(cog_cli_configuration_options(c)->store_dir,
                     "/synthetic/explicit") == 0);
        cog_cli_configuration_free(c);
        CHECK(setenv("HOME", "/synthetic/home", 1) == 0);
    }
    cog_cli_options_defaults(&o);
    o.session_action = COG_CLI_SESSION_RECOVER;
    o.source_session_id = source;
    CHECK(cog_cli_configuration_new(&c, &o, 1U) == COG_ERROR_DENIED && c == NULL);
    o.source_session_id = "session-\xe2\x98\x83";
    o.previous_processes_fenced = true;
    CHECK(cog_cli_configuration_new(&c, &o, 1U) == COG_OK);
    cog_cli_configuration_free(c);
    o.previous_processes_fenced = false;
    o.session_action = COG_CLI_SESSION_RESUME;
    o.store_mode = COG_CLI_STORE_DISABLED;
    o.explicit_options = COG_CLI_OPT_STORE_MODE;
    CHECK(cog_cli_configuration_new(&c, &o, 1U) == COG_ERROR_INVALID_ARG && c == NULL);
    o.explicit_options = 0U;
    CHECK(cog_cli_configuration_new(&c, &o, 1U) == COG_ERROR_NOT_FOUND && c == NULL);
    o.model_id = "synthetic/model";
    o.explicit_options = COG_CLI_OPT_MODEL;
    o.credential_read = offline_resolver;
    o.credential_arg = &calls;
    CHECK(cog_cli_configuration_new(&c, &o, 1U) == COG_ERROR_DENIED && c == NULL &&
          calls == 1U);
    o.backend = COG_CLI_BACKEND_MOCK;
    o.explicit_options |= COG_CLI_OPT_BACKEND;
    CHECK(cog_cli_configuration_new(&c, &o, 1U) == COG_OK);
    CHECK(cog_cli_configuration_options(c)->store_mode == COG_CLI_STORE_DURABLE);
    CHECK(cog_cli_configuration_options(c)->session_action == COG_CLI_SESSION_RESUME);
    cog_cli_configuration_free(c);
    o.session_action = COG_CLI_SESSION_FORK;
    o.fork_node_id = UINT64_MAX;
    o.previous_processes_fenced = true;
    CHECK(cog_cli_configuration_new(&c, &o, 1U) == COG_OK);
    effective = cog_cli_configuration_options(c);
    CHECK(effective->fork_node_id == UINT64_MAX &&
          effective->previous_processes_fenced);
    cog_cli_configuration_free(c);
    CHECK(calls == 1U);
    return 0;
}
static int session_selector_failures(void)
{
    static const char *const bad_ids[] = {
        "",         ".",    "..",     ".cog-domain.lock", "path/id",
        "path\\id", "id\n", "id\x7f", "id\x80",           "id\xed\xa0\x80"};
    cog_cli_options_t o;
    cog_cli_configuration_t *c = NULL;
    size_t i;
    char source[129];
    CHECK(clean() == 0);
    cog_cli_options_defaults(&o);
    o.session_action = COG_CLI_SESSION_RECOVER;
    o.previous_processes_fenced = true;
    for (i = 0U; i < sizeof(bad_ids) / sizeof(bad_ids[0]); ++i) {
        o.source_session_id = bad_ids[i];
        CHECK(cog_cli_configuration_new(&c, &o, 1U) == COG_ERROR_INVALID_ARG &&
              c == NULL);
    }
    memset(source, 's', 128U);
    source[128U] = '\0';
    o.source_session_id = source;
    CHECK(cog_cli_configuration_new(&c, &o, 1U) == COG_ERROR_LIMIT && c == NULL);
    source[127U] = '\0';
    CHECK(cog_cli_configuration_new(&c, &o, 1U) == COG_OK);
    cog_cli_configuration_free(c);
    o.source_session_id = "session/invalid";
    o.session_action = COG_CLI_SESSION_NEW;
    CHECK(cog_cli_configuration_new(&c, &o, 1U) == COG_ERROR_INVALID_ARG && c == NULL);
    o.source_session_id = NULL;
    o.previous_processes_fenced = false;
    o.session_action = (cog_cli_session_action_t)999;
    CHECK(cog_cli_configuration_new(&c, &o, 1U) == COG_ERROR_INVALID_ARG && c == NULL);
    o.session_action = COG_CLI_SESSION_LIST;
    o.prompt = "";
    CHECK(cog_cli_configuration_new(&c, &o, 1U) == COG_ERROR_INVALID_ARG && c == NULL);
    o.prompt = NULL;
    o.queue_text = "queued";
    CHECK(cog_cli_configuration_new(&c, &o, 1U) == COG_ERROR_INVALID_ARG && c == NULL);
    o.queue_text = NULL;
    o.session_id = "new-only";
    CHECK(cog_cli_configuration_new(&c, &o, 1U) == COG_ERROR_INVALID_ARG && c == NULL);
    o.session_id = NULL;
    o.source_session_id = "resume";
    o.session_action = COG_CLI_SESSION_RESUME;
    o.fork_node_id = 1U;
    CHECK(cog_cli_configuration_new(&c, &o, 1U) == COG_ERROR_INVALID_ARG && c == NULL);
    o.session_action = COG_CLI_SESSION_FORK;
    o.queue_text = "queued";
    CHECK(cog_cli_configuration_new(&c, &o, 1U) == COG_ERROR_INVALID_ARG && c == NULL);
    return 0;
}
static int session_inputs_are_direct(void)
{
    char path[] = "/tmp/cog-session-selectors-l1-XXXXXX";
    cog_cli_options_t o;
    cog_cli_configuration_t *c = NULL;
    CHECK(clean() == 0);
    CHECK(setenv("COG_SESSION_ACTION", "recover", 1) == 0);
    CHECK(setenv("COG_SOURCE_SESSION_ID", "ignored", 1) == 0);
    CHECK(setenv("COG_FORK_NODE_ID", "12", 1) == 0);
    CHECK(setenv("COG_PREVIOUS_PROCESSES_FENCED", "true", 1) == 0);
    CHECK(setenv("COG_SHOW_USAGE", "true", 1) == 0);
    base(&o);
    o.backend = COG_CLI_BACKEND_MOCK;
    o.explicit_options |= COG_CLI_OPT_BACKEND;
    CHECK(cog_cli_configuration_new(&c, &o, 1U) == COG_OK);
    CHECK(cog_cli_configuration_options(c)->session_action == COG_CLI_SESSION_NEW);
    CHECK(cog_cli_configuration_options(c)->source_session_id == NULL);
    CHECK(cog_cli_configuration_options(c)->fork_node_id == 0U);
    CHECK(!cog_cli_configuration_options(c)->previous_processes_fenced &&
          !cog_cli_configuration_options(c)->show_usage);
    cog_cli_configuration_free(c);
    CHECK(fixture(path, "client.session-action=list\n") == 0);
    o.config_path = path;
    CHECK(cog_cli_configuration_new(&c, &o, 1U) == COG_ERROR_INVALID_ARG && c == NULL);
    CHECK(unlink(path) == 0);
    CHECK(unsetenv("COG_SESSION_ACTION") == 0);
    CHECK(unsetenv("COG_SOURCE_SESSION_ID") == 0);
    CHECK(unsetenv("COG_FORK_NODE_ID") == 0);
    CHECK(unsetenv("COG_PREVIOUS_PROCESSES_FENCED") == 0);
    CHECK(unsetenv("COG_SHOW_USAGE") == 0);
    return 0;
}

/* The store ceiling is UTF-8 bytes, and every source-selecting action owns it. */
static int session_unicode_boundary(void)
{
    static const cog_cli_session_action_t actions[] = {
        COG_CLI_SESSION_RESUME, COG_CLI_SESSION_FORK, COG_CLI_SESSION_RECOVER};
    size_t i;
    CHECK(clean() == 0);
    for (i = 0U; i < sizeof(actions) / sizeof(actions[0]); ++i) {
        cog_cli_options_t o;
        cog_cli_configuration_t *c = NULL;
        const cog_cli_options_t *effective;
        char source[129], expected[128];
        memset(source, 's', 123U);
        memcpy(source + 123U, "\xf0\x9f\x98\x80", 5U);
        memcpy(expected, source, sizeof(expected));
        cog_cli_options_defaults(&o);
        o.backend = COG_CLI_BACKEND_MOCK;
        o.explicit_options = COG_CLI_OPT_BACKEND;
        o.session_action = actions[i];
        o.source_session_id = source;
        o.previous_processes_fenced = actions[i] == COG_CLI_SESSION_RECOVER;
        o.fork_node_id = actions[i] == COG_CLI_SESSION_FORK ? UINT64_MAX : 0U;
        CHECK(cog_cli_configuration_new(&c, &o, 1U) == COG_OK);
        effective = cog_cli_configuration_options(c);
        CHECK(effective->source_session_id != source);
        memset(source, 'x', 127U);
        CHECK(memcmp(effective->source_session_id, expected, sizeof(expected)) == 0);
        CHECK(effective->session_action == actions[i] &&
              effective->fork_node_id == o.fork_node_id &&
              effective->previous_processes_fenced == o.previous_processes_fenced);
        CHECK(effective->store_mode == COG_CLI_STORE_DURABLE);
        cog_cli_configuration_free(c);
        memcpy(source, expected, sizeof(expected));
        source[127] = 'x';
        source[128] = '\0';
        CHECK(cog_cli_configuration_new(&c, &o, 1U) == COG_ERROR_LIMIT && c == NULL);
        source[126] = '\0'; /* Truncated four-byte scalar, below byte ceiling. */
        CHECK(cog_cli_configuration_new(&c, &o, 1U) == COG_ERROR_INVALID_ARG &&
              c == NULL);
    }
    return 0;
}

typedef struct reload_resolver {
    const char *secret, *expected_reference;
    unsigned calls;
    cog_result_t result;
} reload_resolver_t;
static cog_result_t reload_resolve(void *arg, const char *reference, char *output,
                                   size_t capacity, size_t *required)
{
    reload_resolver_t *resolver = arg;
    ++resolver->calls;
    if (resolver->expected_reference != NULL &&
        strcmp(reference, resolver->expected_reference) != 0)
        return COG_ERROR_PROTOCOL;
    *required = strlen(resolver->secret) + 1U;
    if (*required > capacity)
        return COG_ERROR_OVERFLOW;
    memcpy(output, resolver->secret, *required);
    return resolver->result;
}
static int fixed_options_equal(const cog_cli_options_t *a, const cog_cli_options_t *b)
{
#define SAME(member) CHECK(a->member == b->member)
    SAME(backend);
    SAME(session_action);
    SAME(fork_node_id);
    SAME(previous_processes_fenced);
    SAME(show_usage);
    SAME(store_mode);
    SAME(mock_mode);
    SAME(script);
    SAME(mock_deltas);
    SAME(mock_delta_count);
    SAME(interactive);
    SAME(verbose);
    SAME(tool_policy);
    SAME(net);
    SAME(max_output_tokens);
    SAME(max_tool_rounds);
    SAME(max_turn_ms);
    SAME(max_tool_ms);
    SAME(max_tool_output_bytes);
    SAME(max_context_bytes);
    SAME(max_retries);
    SAME(max_retry_elapsed_ms);
    SAME(explicit_options);
#undef SAME
#define COPIED(member)                                                                 \
    do {                                                                               \
        CHECK((a->member == NULL) == (b->member == NULL));                             \
        if (a->member != NULL) {                                                       \
            CHECK(a->member != b->member);                                             \
            CHECK(strcmp(a->member, b->member) == 0);                                  \
        }                                                                              \
    } while (0)
    COPIED(prompt);
    COPIED(session_id);
    COPIED(source_session_id);
    COPIED(store_dir);
    COPIED(log_path);
    COPIED(socket_path);
    COPIED(endpoint_name);
    COPIED(chunks);
    COPIED(queue_text);
    COPIED(config_path);
    COPIED(workspace_root);
#undef COPIED
    CHECK(b->credential_read == NULL && b->credential_arg == NULL);
    return 0;
}
static int reload_generations(void)
{
    char path[] = "/tmp/cog-config-reload-XXXXXX", model[64] = "replacement/model";
    char url[64] = "https://replacement.invalid/v2", reference[64] = "host:replacement";
    cog_cli_options_t input;
    cog_cli_configuration_t *current = NULL, *next = NULL, *rotated = NULL,
                            *host = NULL, *copy = NULL;
    const cog_cli_options_t *old_options, *options;
    const cog_endpoint_t *old_endpoint, *endpoint;
    static const cog_provider_vtable_t identity = {
        0}; /* Never dispatched: ownership only. */
    const cog_provider_limits_t limits = {1U, 1U, 1U, 1U, 1U, 1U, 1U,
                                          1U, 1U, 1U, 1U, 1U, 1U};
    reload_resolver_t first = {"SYNTHETIC_INITIAL", "env:COG_TEST_SECRET", 0U, COG_OK};
    reload_resolver_t resolver = {"SYNTHETIC_ROTATED_HOST", reference, 0U, COG_OK};
    cog_cli_reload_options_t replacement = {.model_id = model, .base_url = url};
    size_t i;
    CHECK(clean() == 0);
    CHECK(
        fixture(
            path,
            "models.default=file/model\nproviders.openrouter.base-url=https://"
            "pinned.invalid/v1\nworkspace.root=/pinned/"
            "workspace\nlimits.max-output-tokens=17\nlimits.max-tool-rounds=3\nlimits."
            "max-turn-ms=8000\nlimits.max-tool-ms=4000\nlimits.max-tool-output-bytes="
            "71\nlimits.max-context-bytes=411\nlimits.max-retry-elapsed-ms=3000\n") ==
        0);
    base(&input);
    input.config_path = path;
    input.session_id = "pinned-session";
    input.prompt = "pinned prompt";
    input.queue_text = "pinned queue";
    input.log_path = "/pinned/log";
    input.socket_path = "/pinned/socket";
    input.max_retries = 0U;
    input.tool_policy = COG_CLI_TOOL_POLICY_ASK;
    input.explicit_options |= COG_CLI_OPT_RETRIES | COG_CLI_OPT_POLICY;
    input.interactive = false;
    input.verbose = true;
    input.show_usage = true;
    input.net = (cog_net_ctx_t *)(void *)&input;
    input.credential_read = reload_resolve;
    input.credential_arg = &first;
    CHECK(cog_cli_configuration_new(&current, &input, 1U) == COG_OK &&
          first.calls == 1U);
    CHECK(cog_cli_configuration_bind(current, &identity, NULL, NULL, &limits) ==
          COG_OK);
    old_options = cog_cli_configuration_options(current);
    old_endpoint = cog_cli_configuration_endpoint(current);
    CHECK(strcmp(old_options->model_id, "file/model") == 0);
    CHECK(strcmp(old_endpoint->api_key, "SYNTHETIC_INITIAL") == 0);
    CHECK(unlink(path) == 0); /* Reload must not reopen the old config file. */
    for (i = 0U; i < sizeof(variables) / sizeof(variables[0]); ++i)
        CHECK(setenv(variables[i], "INVALID_UNRELATED_SOURCE", 1) == 0);
    CHECK(setenv("HOME", "relative", 1) == 0 &&
          setenv("XDG_STATE_HOME", "relative", 1) == 0);
    CHECK(unsetenv("COG_TEST_SECRET") == 0);
    CHECK(cog_cli_configuration_reload_new(&next, current, &replacement, 2U) == COG_OK);
    options = cog_cli_configuration_options(next);
    CHECK(fixed_options_equal(old_options, options) == 0);
    CHECK(strcmp(options->model_id, model) == 0 && options->model_id != model);
    CHECK(strcmp(options->base_url, url) == 0 && options->base_url != url);
    CHECK(strcmp(options->credential_ref, old_options->credential_ref) == 0 &&
          options->credential_ref != old_options->credential_ref);
    CHECK(cog_cli_configuration_endpoint(next) == NULL && first.calls == 1U);
    memset(model, 'x', strlen(model));
    memset(url, 'x', strlen(url));
    CHECK(cog_cli_configuration_bind(next, &identity, NULL, NULL, &limits) == COG_OK);
    endpoint = cog_cli_configuration_endpoint(next);
    CHECK(endpoint->generation == 2U &&
          strcmp(endpoint->models->models[0].id, "replacement/model") == 0);
    CHECK(strcmp(endpoint->base_url, "https://replacement.invalid/v2") == 0);
    CHECK(endpoint->api_key != old_endpoint->api_key &&
          strcmp(endpoint->api_key, old_endpoint->api_key) == 0);
    CHECK(strcmp(old_endpoint->models->models[0].id, "file/model") == 0);
    CHECK(setenv("COG_TEST_SECRET", "SYNTHETIC_ROTATED_ENV", 1) == 0);
    replacement = (cog_cli_reload_options_t){.credential_ref = options->credential_ref};
    CHECK(cog_cli_configuration_reload_new(&rotated, next, &replacement, 3U) == COG_OK);
    CHECK(cog_cli_configuration_bind(rotated, &identity, NULL, NULL, &limits) ==
          COG_OK);
    CHECK(strcmp(cog_cli_configuration_endpoint(rotated)->api_key,
                 "SYNTHETIC_ROTATED_ENV") == 0);
    CHECK(strcmp(endpoint->api_key, "SYNTHETIC_INITIAL") == 0 && first.calls == 1U);
    CHECK(fixed_options_equal(options, cog_cli_configuration_options(rotated)) == 0);
    replacement = (cog_cli_reload_options_t){.credential_ref = reference,
                                             .credential_read = reload_resolve,
                                             .credential_arg = &resolver};
    CHECK(cog_cli_configuration_reload_new(&host, rotated, &replacement, 4U) ==
              COG_OK &&
          resolver.calls == 1U);
    CHECK(cog_cli_configuration_bind(host, &identity, NULL, NULL, &limits) == COG_OK);
    CHECK(strcmp(cog_cli_configuration_endpoint(host)->api_key,
                 "SYNTHETIC_ROTATED_HOST") == 0);
    CHECK(cog_cli_configuration_options(host)->credential_read == NULL &&
          cog_cli_configuration_options(host)->credential_arg == NULL);
    resolver.secret = "SYNTHETIC_SECOND_HOST";
    replacement.credential_ref = cog_cli_configuration_options(host)->credential_ref;
    CHECK(cog_cli_configuration_reload_new(&copy, host, &replacement, 5U) == COG_OK &&
          resolver.calls == 2U);
    CHECK(cog_cli_configuration_bind(copy, &identity, NULL, NULL, &limits) == COG_OK);
    CHECK(strcmp(cog_cli_configuration_endpoint(copy)->api_key,
                 "SYNTHETIC_SECOND_HOST") == 0);
    CHECK(strcmp(cog_cli_configuration_endpoint(host)->api_key,
                 "SYNTHETIC_ROTATED_HOST") == 0);
    cog_cli_configuration_free(host);
    host = copy;
    copy = NULL;
    memset(reference, 'x', strlen(reference));
    memset(&resolver, 0, sizeof(resolver));
    replacement = (cog_cli_reload_options_t){.model_id = "same-owner/new-model"};
    CHECK(cog_cli_configuration_reload_new(&copy, host, &replacement, 6U) == COG_OK);
    CHECK(fixed_options_equal(cog_cli_configuration_options(host),
                              cog_cli_configuration_options(copy)) == 0);
    cog_cli_configuration_free(current);
    cog_cli_configuration_free(next);
    cog_cli_configuration_free(rotated);
    cog_cli_configuration_free(host);
    CHECK(cog_cli_configuration_bind(copy, &identity, NULL, NULL, &limits) == COG_OK);
    endpoint = cog_cli_configuration_endpoint(copy);
    CHECK(endpoint->generation == 6U &&
          strcmp(endpoint->api_key, "SYNTHETIC_SECOND_HOST") == 0);
    CHECK(strcmp(endpoint->models->models[0].id, "same-owner/new-model") == 0);
    CHECK(strcmp(cog_cli_configuration_options(copy)->config_path, path) == 0);
    cog_cli_configuration_free(copy);
    CHECK(clean() == 0);
    return 0;
}
static int reload_validation(void)
{
    cog_cli_options_t input;
    cog_cli_configuration_t *current = NULL, *candidate = NULL;
    cog_cli_reload_options_t replacement = {.model_id = "valid/model"};
    reload_resolver_t resolver = {"SYNTHETIC_RELOAD_FAILURE", NULL, 0U,
                                  COG_ERROR_DENIED};
    char oversized[4097];
    static const char *const bad_models[] = {"", "bad model", "bad/\xc0\xaf",
                                             "bad/\xed\xa0\x80"};
    static const char *const bad_urls[] = {"", "http://synthetic.invalid",
                                           "https://:443/v1", "https://[]/v1",
                                           "https://synthetic.invalid:65536/v1"};
    size_t i;
    CHECK(clean() == 0);
    base(&input);
    input.model_id = "pinned/model";
    input.explicit_options |= COG_CLI_OPT_MODEL;
    CHECK(cog_cli_configuration_new(&current, &input, 1U) == COG_OK);
    CHECK(cog_cli_configuration_reload_new(NULL, current, &replacement, 2U) ==
          COG_ERROR_NULL_PTR);
    CHECK(cog_cli_configuration_reload_new(&candidate, NULL, &replacement, 2U) ==
              COG_ERROR_NULL_PTR &&
          candidate == NULL);
    CHECK(cog_cli_configuration_reload_new(&candidate, current, NULL, 2U) ==
              COG_ERROR_NULL_PTR &&
          candidate == NULL);
    CHECK(cog_cli_configuration_reload_new(&candidate, current, &replacement, 0U) ==
              COG_ERROR_INVALID_ARG &&
          candidate == NULL);
    CHECK(cog_cli_configuration_reload_new(&candidate, current, &replacement, 1U) ==
              COG_ERROR_INVALID_ARG &&
          candidate == NULL);
    replacement = (cog_cli_reload_options_t){0};
    CHECK(cog_cli_configuration_reload_new(&candidate, current, &replacement, 2U) ==
              COG_ERROR_INVALID_ARG &&
          candidate == NULL);
    replacement = (cog_cli_reload_options_t){.model_id = "next/model",
                                             .credential_read = reload_resolve,
                                             .credential_arg = &resolver};
    CHECK(cog_cli_configuration_reload_new(&candidate, current, &replacement, 2U) ==
              COG_ERROR_INVALID_ARG &&
          candidate == NULL && resolver.calls == 0U);
    replacement.credential_read = NULL;
    CHECK(cog_cli_configuration_reload_new(&candidate, current, &replacement, 2U) ==
              COG_ERROR_INVALID_ARG &&
          candidate == NULL);
    for (i = 0U; i < sizeof(bad_models) / sizeof(bad_models[0]); ++i) {
        replacement = (cog_cli_reload_options_t){.model_id = bad_models[i]};
        CHECK(cog_cli_configuration_reload_new(&candidate, current, &replacement, 2U) ==
                  COG_ERROR_INVALID_ARG &&
              candidate == NULL);
    }
    for (i = 0U; i < sizeof(bad_urls) / sizeof(bad_urls[0]); ++i) {
        replacement = (cog_cli_reload_options_t){.base_url = bad_urls[i]};
        CHECK(cog_cli_configuration_reload_new(&candidate, current, &replacement, 2U) ==
                  COG_ERROR_INVALID_ARG &&
              candidate == NULL);
    }
    memset(oversized, 'x', sizeof(oversized) - 1U);
    oversized[sizeof(oversized) - 1U] = '\0';
    replacement = (cog_cli_reload_options_t){.model_id = oversized};
    CHECK(cog_cli_configuration_reload_new(&candidate, current, &replacement, 2U) ==
              COG_ERROR_LIMIT &&
          candidate == NULL);
    replacement = (cog_cli_reload_options_t){.base_url = oversized};
    CHECK(cog_cli_configuration_reload_new(&candidate, current, &replacement, 2U) ==
              COG_ERROR_LIMIT &&
          candidate == NULL);
    replacement = (cog_cli_reload_options_t){.credential_ref = oversized,
                                             .credential_read = reload_resolve,
                                             .credential_arg = &resolver};
    CHECK(cog_cli_configuration_reload_new(&candidate, current, &replacement, 2U) ==
              COG_ERROR_LIMIT &&
          candidate == NULL && resolver.calls == 0U);
    replacement.credential_ref = "";
    CHECK(cog_cli_configuration_reload_new(&candidate, current, &replacement, 2U) ==
              COG_ERROR_INVALID_ARG &&
          candidate == NULL && resolver.calls == 0U);
    replacement.credential_ref = "host:failure";
    CHECK(cog_cli_configuration_reload_new(&candidate, current, &replacement, 2U) ==
              COG_ERROR_DENIED &&
          candidate == NULL && resolver.calls == 1U);
    CHECK(strcmp(cog_cli_configuration_options(current)->model_id, "pinned/model") ==
          0);
    replacement = (cog_cli_reload_options_t){.credential_ref = "env:INVALID-NAME"};
    CHECK(cog_cli_configuration_reload_new(&candidate, current, &replacement, 2U) ==
              COG_ERROR_INVALID_ARG &&
          candidate == NULL);
    replacement.credential_ref = "env:COG_RELOAD_MISSING";
    CHECK(unsetenv("COG_RELOAD_MISSING") == 0);
    CHECK(cog_cli_configuration_reload_new(&candidate, current, &replacement, 2U) ==
              COG_ERROR_NOT_FOUND &&
          candidate == NULL);
    replacement.credential_ref = "pass:synthetic";
    CHECK(cog_cli_configuration_reload_new(&candidate, current, &replacement, 2U) ==
              COG_ERROR_UNSUPPORTED &&
          candidate == NULL);
    oversized[4095U] = '\0';
    replacement = (cog_cli_reload_options_t){.model_id = oversized};
    CHECK(cog_cli_configuration_reload_new(&candidate, current, &replacement, 2U) ==
          COG_OK);
    CHECK(strlen(cog_cli_configuration_options(candidate)->model_id) == 4095U);
    cog_cli_configuration_free(candidate);
    replacement = (cog_cli_reload_options_t){.model_id = "pinned/model"};
    CHECK(cog_cli_configuration_reload_new(&candidate, current, &replacement,
                                           UINT64_MAX) == COG_OK);
    CHECK(fixed_options_equal(cog_cli_configuration_options(current),
                              cog_cli_configuration_options(candidate)) == 0);
    cog_cli_configuration_free(candidate);
    cog_cli_configuration_free(current);
    cog_cli_options_defaults(&input);
    input.session_action = COG_CLI_SESSION_LIST;
    CHECK(cog_cli_configuration_new(&current, &input, 1U) == COG_OK);
    CHECK(cog_cli_configuration_reload_new(&candidate, current, &replacement, 2U) ==
              COG_ERROR_INVALID_ARG &&
          candidate == NULL);
    cog_cli_configuration_free(current);
    return 0;
}
static int reload_mock_borrows(void)
{
    cog_cli_options_t input;
    cog_cli_configuration_t *current = NULL, *candidate = NULL;
    const cog_cli_options_t *before, *after;
    static const cog_mock_script_t script = {0};
    static const cog_provider_delta_t delta = {0};
    cog_cli_reload_options_t replacement = {.model_id = "mock/replacement"};
    CHECK(clean() == 0);
    base(&input);
    input.backend = COG_CLI_BACKEND_MOCK;
    input.explicit_options |= COG_CLI_OPT_BACKEND;
    input.session_action = COG_CLI_SESSION_FORK;
    input.source_session_id = "pinned-source";
    input.fork_node_id = 9U;
    input.previous_processes_fenced = true;
    input.script = &script;
    input.net =
        (cog_net_ctx_t *)(void *)&input; /* Borrow preservation, never dispatched. */
    CHECK(cog_cli_configuration_new(&current, &input, 1U) == COG_OK);
    before = cog_cli_configuration_options(current);
    CHECK(cog_cli_configuration_reload_new(&candidate, current, &replacement, 2U) ==
          COG_OK);
    after = cog_cli_configuration_options(candidate);
    CHECK(fixed_options_equal(before, after) == 0 &&
          strcmp(after->model_id, "mock/replacement") == 0);
    CHECK(after->script == &script && after->mock_deltas == NULL &&
          after->mock_delta_count == 0U && after->net == input.net);
    cog_cli_configuration_free(candidate);
    replacement.base_url = "https://synthetic.invalid";
    CHECK(cog_cli_configuration_reload_new(&candidate, current, &replacement, 3U) ==
              COG_ERROR_UNSUPPORTED &&
          candidate == NULL);
    replacement.base_url = NULL;
    replacement.credential_ref = "env:COG_TEST_SECRET";
    CHECK(cog_cli_configuration_reload_new(&candidate, current, &replacement, 4U) ==
              COG_ERROR_UNSUPPORTED &&
          candidate == NULL);
    CHECK(strcmp(before->model_id, "mock-model") == 0);
    cog_cli_configuration_free(current);
    input.script = NULL;
    input.mock_deltas = &delta;
    input.mock_delta_count = 1U;
    CHECK(cog_cli_configuration_new(&current, &input, 1U) == COG_OK);
    replacement = (cog_cli_reload_options_t){.model_id = "typed-fixture/model"};
    CHECK(cog_cli_configuration_reload_new(&candidate, current, &replacement, 2U) ==
          COG_OK);
    after = cog_cli_configuration_options(candidate);
    CHECK(after->script == NULL && after->mock_deltas == &delta &&
          after->mock_delta_count == 1U);
    CHECK(fixed_options_equal(cog_cli_configuration_options(current), after) == 0);
    cog_cli_configuration_free(candidate);
    cog_cli_configuration_free(current);
    return 0;
}

static int observation_labels(void)
{
    CHECK(clean()==0);cog_cli_options_t input; base(&input);
    reload_resolver_t resolver={"SYNTHETIC_LABEL_SECRET",NULL,0U,COG_OK};
    input.model_id="safe/model";input.session_id="safe-session";
    input.credential_ref="host:labels";input.credential_read=reload_resolve;input.credential_arg=&resolver;
    input.explicit_options|=COG_CLI_OPT_MODEL|COG_CLI_OPT_CREDENTIAL;
    cog_cli_configuration_t *current=NULL,*candidate=NULL;
    CHECK(cog_cli_configuration_new(&current,&input,1U)==COG_OK);
    const cog_cli_safe_labels_t *labels=cog_cli_configuration_labels(current);
    CHECK(labels!=NULL && labels->session_available && labels->model_available);
    CHECK(strcmp(labels->session,"safe-session")==0 && strcmp(labels->model,"safe/model")==0);
    resolver.secret="safe";
    const cog_cli_reload_options_t replacement={.credential_ref="host:labels",.credential_read=reload_resolve,.credential_arg=&resolver};
    CHECK(cog_cli_configuration_reload_new(&candidate,current,&replacement,2U)==COG_OK);
    const cog_cli_safe_labels_t *next=cog_cli_configuration_labels(candidate);
    CHECK(!next->session_available && !next->model_available && next->session==NULL && next->model==NULL);
    CHECK(labels->model_available && strcmp(labels->model,"safe/model")==0);
    CHECK(strcmp(cog_cli_configuration_options(candidate)->model_id,"safe/model")==0);
    cog_cli_configuration_free(candidate);cog_cli_configuration_free(current);
    resolver.secret="SYNTHETIC_LABEL_SECRET";
    const char *models[]={"prefix-SYNTHETIC_LABEL_SECRET-suffix","host:labels","safe/\"quote","safe/\xf0\x9f\x8c\xb1"};
    for(size_t i=0U;i<sizeof(models)/sizeof(models[0]);i++){
        input.model_id=models[i];CHECK(cog_cli_configuration_new(&current,&input,3U)==COG_OK);
        labels=cog_cli_configuration_labels(current);CHECK(!labels->model_available && labels->model==NULL);
        CHECK(strcmp(cog_cli_configuration_options(current)->model_id,models[i])==0);cog_cli_configuration_free(current);
    }
    char long_model[4096];memset(long_model,'a',sizeof(long_model)-1U);long_model[sizeof(long_model)-1U]='\0';
    input.model_id=long_model;input.session_id=NULL;
    CHECK(cog_cli_configuration_new(&current,&input,4U)==COG_OK);labels=cog_cli_configuration_labels(current);
    CHECK(!labels->model_available && !labels->session_available && strlen(cog_cli_configuration_options(current)->model_id)==4095U);
    cog_cli_configuration_free(current);
    char long_secret[129];memset(long_secret,'s',sizeof(long_secret)-1U);long_secret[sizeof(long_secret)-1U]='\0';
    resolver.secret=long_secret;input.model_id="safe/model";input.session_id="safe-session";
    CHECK(cog_cli_configuration_new(&current,&input,5U)==COG_OK);labels=cog_cli_configuration_labels(current);
    CHECK(!labels->model_available && !labels->session_available);cog_cli_configuration_free(current);
    CHECK(cog_cli_configuration_labels(NULL)==NULL);CHECK(clean()==0);return 0;
}
int main(void)
{
    CHECK(observation_labels()==0);
    CHECK(session_unicode_boundary() == 0);
    CHECK(reload_generations() == 0);
    CHECK(reload_validation() == 0);
    CHECK(reload_mock_borrows() == 0);
    CHECK(session_configuration() == 0);
    CHECK(session_selector_failures() == 0);
    CHECK(session_inputs_are_direct() == 0);
    CHECK(protocol_inputs() == 0);
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

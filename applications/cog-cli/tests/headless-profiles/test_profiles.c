/** @file test_profiles.c @brief Pure resolved profiles; never a fake child runtime. */
#define _POSIX_C_SOURCE 200809L
#include "child_profile.h"
#include "options_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); return 1; } } while (0)
static unsigned resolver_calls;
static cog_result_t resolve(void *arg, const char *reference, char *output, size_t cap, size_t *required)
{
    const char *secret = arg;
    (void)reference; ++resolver_calls; *required = strlen(secret) + 1U;
    if (cap < *required) return COG_ERROR_OVERFLOW;
    memcpy(output, secret, *required); return COG_OK;
}
static void options(cog_cli_options_t *o)
{
    cog_cli_options_defaults(o);
    o->model_id = "synthetic/model"; o->base_url = "https://fixture.invalid/v1";
    o->credential_ref = "host:synthetic"; o->credential_read = resolve;
    o->credential_arg = (void *)"SYNTHETIC_PROFILE_KEY";
    o->workspace_root = "relative/workspace"; o->store_dir = "relative/store";
    o->store_mode = COG_CLI_STORE_DURABLE;
    o->explicit_options = COG_CLI_OPT_MODEL | COG_CLI_OPT_BASE_URL | COG_CLI_OPT_CREDENTIAL |
        COG_CLI_OPT_WORKSPACE | COG_CLI_OPT_STORE_DIR | COG_CLI_OPT_STORE_MODE | COG_CLI_OPT_POLICY;
}
static int attachment(void)
{
    cog_cli_options_t o; options(&o);
    cog_cli_configuration_t *c = NULL, *reload = NULL;
    cog_cli_reload_options_t change = {.model_id = "other/model"};
    uint32_t mask = 99U;
    const char *bad[] = {"relative", "/a/", "/a//b", "/a/./b", "/a/../b", "/\xc0\xaf"};
    CHECK(cog_cli_configuration_new(&c, &o, 7U) == COG_OK);
    const cog_cli_options_t before = *cog_cli_configuration_options(c);
    CHECK(cog_cli_configuration_tool_mask(c, &mask) == COG_OK && mask == 15U);
    for (size_t i = 0U; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        CHECK(cog_cli_configuration_finish_host(c, (cog_net_ctx_t *)(void *)&o, bad[i], "/state") == COG_ERROR_INVALID_ARG);
        CHECK(memcmp(&before, cog_cli_configuration_options(c), sizeof(before)) == 0);
    }
    CHECK(cog_cli_configuration_finish_host(c, NULL, "/work", "/state") == COG_ERROR_INVALID_ARG);
    CHECK(cog_cli_configuration_finish_host(c, (cog_net_ctx_t *)(void *)&o, NULL, "/state") == COG_ERROR_NULL_PTR);
    char long_path[4097]; memset(long_path, 'x', sizeof(long_path)); long_path[0] = '/'; long_path[4096] = '\0';
    CHECK(cog_cli_configuration_finish_host(c, (cog_net_ctx_t *)(void *)&o, "/work", long_path) == COG_ERROR_LIMIT);
    CHECK(memcmp(&before, cog_cli_configuration_options(c), sizeof(before)) == 0);
    CHECK(cog_cli_configuration_finish_host(c, (cog_net_ctx_t *)(void *)&o, "/work space/\xf0\x9f\x8c\xb1", "/state") == COG_OK);
    CHECK(strcmp(cog_cli_configuration_options(c)->workspace_root, "/work space/\xf0\x9f\x8c\xb1") == 0);
    CHECK(cog_cli_configuration_options(c)->net == (cog_net_ctx_t *)(void *)&o);
    CHECK(cog_cli_configuration_finish_host(c, (cog_net_ctx_t *)(void *)&o, "/work", "/state") == COG_ERROR_INVALID_STATE);
    CHECK(cog_cli_configuration_reload_new(&reload, c, &change, 8U) == COG_OK);
    CHECK(cog_cli_configuration_tool_mask(reload, &mask) == COG_OK && mask == 15U);
    CHECK(cog_cli_configuration_finish_host(reload, (cog_net_ctx_t *)(void *)&o, "/work", "/state") == COG_ERROR_INVALID_STATE);
    cog_cli_configuration_free(reload); cog_cli_configuration_free(c);
    o.tool_policy = COG_CLI_TOOL_POLICY_DENY;
    CHECK(cog_cli_configuration_new(&c, &o, 7U) == COG_OK);
    CHECK(cog_cli_configuration_tool_mask(c, &mask) == COG_OK && mask == 15U);
    cog_cli_configuration_free(c);
    o.workspace_root = NULL; o.explicit_options &= ~(uint32_t)COG_CLI_OPT_WORKSPACE;
    CHECK(cog_cli_configuration_new(&c, &o, 7U) == COG_OK);
    CHECK(cog_cli_configuration_tool_mask(c, &mask) == COG_OK && mask == 0U);
    CHECK(cog_cli_configuration_finish_host(c, (cog_net_ctx_t *)(void *)&o, "/work", "/state") == COG_ERROR_INVALID_ARG);
    CHECK(cog_cli_configuration_finish_host(c, (cog_net_ctx_t *)(void *)&o, NULL, "/") == COG_OK);
    cog_cli_configuration_free(c);
    return 0;
}
static int child(void)
{
    cog_cli_options_t o; options(&o);
    cog_cli_configuration_t *parent = NULL, *c = NULL;
    cog_cli_child_profile_t profile = {.tools = COG_CLI_CHILD_READ | COG_CLI_CHILD_COMMAND,
        .timeout_ms = 350U, .max_output_tokens = 20U, .max_context_bytes = 1024U,
        .max_tool_output_bytes = 400U, .max_tool_rounds = 3U};
    uint64_t deadline = 99U;
    uint32_t mask = 99U;
    o.prompt = "not repeated"; o.queue_text = "never inherited"; o.socket_path = "/unused";
    o.session_id = "not-child"; o.show_usage = true; o.tool_policy = COG_CLI_TOOL_POLICY_ALLOW;
    CHECK(cog_cli_configuration_new(&parent, &o, 9U) == COG_OK);
    unsigned calls = resolver_calls;
    for (unsigned policy = 0U; policy < 3U; ++policy) {
        profile.policy = (cog_cli_child_policy_t)policy;
        CHECK(cog_cli_configuration_child_new(&c, parent, &profile, 9U, 100U, 1000U,
            (cog_net_ctx_t *)(void *)&o, &deadline) == COG_OK);
        const cog_cli_options_t *copied = cog_cli_configuration_options(c);
        CHECK(deadline == 450U && copied->max_turn_ms == 350U && copied->max_tool_ms == 350U && copied->max_retry_elapsed_ms == 350U);
        CHECK(copied->max_output_tokens == 20U && copied->max_tool_rounds == 3U && copied->max_retries == 0U);
        CHECK(copied->max_context_bytes == 1024U && copied->max_tool_output_bytes == 400U);
        CHECK(copied->tool_policy == (policy == 0U ? COG_CLI_TOOL_POLICY_ALLOW : policy == 1U ? COG_CLI_TOOL_POLICY_DENY : COG_CLI_TOOL_POLICY_ASK));
        CHECK(copied->prompt == NULL && copied->queue_text == NULL && copied->socket_path == NULL &&
            copied->session_id == NULL && copied->source_session_id == NULL && copied->log_path == NULL &&
            copied->config_path == NULL && !copied->show_usage && !copied->interactive && copied->credential_read == NULL && copied->credential_arg == NULL);
        CHECK(cog_cli_configuration_tool_mask(c, &mask) == COG_OK && mask == 9U);
        CHECK(cog_cli_configuration_finish_host(c, NULL, "/work", "/store") == COG_ERROR_INVALID_STATE);
        cog_cli_configuration_free(c);
    }
    deadline = 99U;
    CHECK(cog_cli_configuration_child_new(&c, parent, &profile, 8U, 100U, 1000U, (cog_net_ctx_t *)(void *)&o, &deadline) == COG_ERROR_INVALID_ARG && c == NULL && deadline == 99U);
    CHECK(cog_cli_configuration_child_new(&c, parent, &profile, 9U, 100U, 100U, (cog_net_ctx_t *)(void *)&o, &deadline) == COG_ERROR_TIMEOUT);
    CHECK(cog_cli_configuration_child_new(&c, parent, &profile, 9U, 100U, UINT64_MAX, (cog_net_ctx_t *)(void *)&o, &deadline) == COG_ERROR_INVALID_ARG);
    profile.timeout_ms = UINT32_MAX;
    CHECK(cog_cli_configuration_child_new(&c, parent, &profile, 9U, UINT64_MAX - 11U, UINT64_MAX - 1U,
        (cog_net_ctx_t *)(void *)&o, &deadline) == COG_OK && deadline == UINT64_MAX - 1U);
    cog_cli_configuration_free(c);
    CHECK(resolver_calls == calls);
    cog_cli_configuration_free(parent);
    o.prompt = NULL; o.queue_text = NULL; o.socket_path = NULL; o.session_id = NULL;
    o.tool_policy = COG_CLI_TOOL_POLICY_DENY;
    CHECK(cog_cli_configuration_new(&parent, &o, 9U) == COG_OK);
    CHECK(cog_cli_configuration_child_new(&c, parent, &profile, 9U, 100U, 1000U, (cog_net_ctx_t *)(void *)&o, &deadline) == COG_OK);
    CHECK(cog_cli_configuration_options(c)->tool_policy == COG_CLI_TOOL_POLICY_DENY);
    cog_cli_configuration_free(c); cog_cli_configuration_free(parent);
    o.backend = COG_CLI_BACKEND_MOCK; o.explicit_options |= COG_CLI_OPT_BACKEND;
    CHECK(cog_cli_configuration_new(&parent, &o, 9U) == COG_OK);
    CHECK(cog_cli_configuration_child_new(&c, parent, &profile, 9U, 100U, 1000U, (cog_net_ctx_t *)(void *)&o, &deadline) == COG_ERROR_UNSUPPORTED && c == NULL);
    cog_cli_configuration_free(parent);
    return 0;
}
static size_t field_offset(const unsigned char *wire, size_t index)
{
    size_t p = 54U;
    for (size_t i = 0U; i < index; ++i) {
        uint32_t len = 0U;
        for (size_t j = 0U; j < 4U; ++j) len = (len << 8U) | wire[p + j];
        p += 4U + len;
    }
    return p;
}
static size_t replace_session(const unsigned char *wire, size_t length,
    unsigned char *output, const char *id, size_t id_bytes)
{
    size_t at = field_offset(wire, 7U), old_bytes = 0U;
    for (size_t i = 0U; i < 4U; ++i) old_bytes = (old_bytes << 8U) | wire[at + i];
    memcpy(output, wire, at);
    for (size_t i = 0U; i < 4U; ++i) output[at + i] = (unsigned char)(id_bytes >> ((3U - i) * 8U));
    memcpy(output + at + 4U, id, id_bytes);
    memcpy(output + at + 4U + id_bytes, wire + at + 4U + old_bytes, length - at - 4U - old_bytes);
    return length + id_bytes - old_bytes;
}
static int probe_ids(cog_cli_configuration_t *parent, const cog_cli_bootstrap_probe_t *probe,
    const cog_net_config_t *transport, const unsigned char *wire, size_t length)
{
    static const char *const invalid[] = {"", ".", "..", "/", "a/b", "a\\b", "a\n", "a\r", "a\t", "a\x7f", ".cog-domain.lock", "\xc0\xaf"};
    unsigned char modified[32768], output[32768], before_output[32768];
    cog_cli_bootstrap_probe_t local = *probe, copied, original_probe;
    cog_cli_configuration_t *decoded = NULL;
    cog_net_config_t got, before_transport;
    char storage[64], before_storage[64];
    size_t required, used;
    for (size_t i = 0U; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        memcpy(local.session_id, invalid[i], strlen(invalid[i]) + 1U);
        memset(output, 0x5c, sizeof(output)); memcpy(before_output, output, sizeof(output)); required = 999U;
        CHECK(cog_cli_configuration_bootstrap_encode(parent, &local, transport, output, sizeof(output), &required) == COG_ERROR_INVALID_ARG);
        CHECK(required == 999U && memcmp(output, before_output, sizeof(output)) == 0);
        used = replace_session(wire, length, modified, invalid[i], strlen(invalid[i]));
        memset(&copied, 0x55, sizeof(copied)); original_probe = copied;
        memset(&got, 0x33, sizeof(got)); before_transport = got;
        memset(storage, 'x', sizeof(storage)); memcpy(before_storage, storage, sizeof(storage));
        CHECK(cog_cli_configuration_bootstrap_new(&decoded, modified, used,
            (cog_net_ctx_t *)(void *)&local, &copied) == COG_ERROR_PROTOCOL && decoded == NULL);
        CHECK(memcmp(&copied, &original_probe, sizeof(copied)) == 0);
        CHECK(cog_cli_bootstrap_transport_decode(modified, used, &got, storage, sizeof(storage), &required) == COG_ERROR_PROTOCOL);
        CHECK(required == 999U && memcmp(&got, &before_transport, sizeof(got)) == 0 && memcmp(storage, before_storage, sizeof(storage)) == 0);
    }
    const char embedded_nul[] = {'a', '\0', 'b'};
    used = replace_session(wire, length, modified, embedded_nul, sizeof(embedded_nul));
    CHECK(cog_cli_configuration_bootstrap_new(&decoded, modified, used,
        (cog_net_ctx_t *)(void *)&local, &copied) == COG_ERROR_PROTOCOL && decoded == NULL);
    CHECK(cog_cli_bootstrap_transport_decode(modified, used, &got, storage, sizeof(storage), &required) == COG_ERROR_PROTOCOL);
    char maximum[128], unicode[128];
    memset(maximum, 's', 127U); maximum[127] = '\0';
    for (size_t i = 0U; i < 63U; ++i) { unicode[i * 2U] = (char)0xc3; unicode[i * 2U + 1U] = (char)0xa9; }
    unicode[126] = 'x'; unicode[127] = '\0';
    const char *valid[] = {"a b:@literal", "\xf0\x9f\x8c\xb1-session", maximum, unicode};
    for (size_t i = 0U; i < sizeof(valid) / sizeof(valid[0]); ++i) {
        memcpy(local.session_id, valid[i], strlen(valid[i]) + 1U);
        CHECK(cog_cli_configuration_bootstrap_encode(parent, &local, transport, output, sizeof(output), &required) == COG_OK);
        used = replace_session(wire, length, modified, valid[i], strlen(valid[i]));
        CHECK(cog_cli_configuration_bootstrap_new(&decoded, modified, used,
            (cog_net_ctx_t *)(void *)&local, &copied) == COG_OK && strcmp(copied.session_id, valid[i]) == 0);
        cog_cli_configuration_free(decoded); decoded = NULL;
        CHECK(cog_cli_bootstrap_transport_decode(modified, used, &got, storage, sizeof(storage), &required) == COG_OK);
    }
    return 0;
}
static int bootstrap(void)
{
    cog_cli_options_t o; options(&o);
    cog_cli_configuration_t *parent = NULL, *decoded = NULL;
    cog_cli_bootstrap_probe_t probe = {.session_id = "fixture", .owner = {3U, 4U}, .checkpoint = {5U}}, copy, before;
    cog_net_config_t transport = {.host_runtime_initialized = true, .dependency_memory = COG_NET_DEPENDENCY_MEMORY_DYNAMIC,
        .ca_bundle = "/trust/synthetic.pem"}, got, original;
    unsigned char wire[65536], again[65536], modified[65536];
    char storage[64], canary[64];
    size_t required = 999U, length;
    CHECK(cog_cli_configuration_new(&parent, &o, 0x0102030405060708U) == COG_OK);
    CHECK(cog_cli_configuration_finish_host(parent, (cog_net_ctx_t *)(void *)&o, "/work", "/state") == COG_OK);
    memset(wire, 0xa5, sizeof(wire));
    CHECK(cog_cli_configuration_bootstrap_encode(parent, &probe, &transport, wire, 1U, &required) == COG_ERROR_OVERFLOW && required > 158U);
    for (size_t i = 0U; i < sizeof(wire); ++i) CHECK(wire[i] == 0xa5U);
    CHECK(cog_cli_configuration_bootstrap_encode(parent, &probe, &transport, wire, sizeof(wire), &length) == COG_OK && length == required);
    static const unsigned char golden[] = {'C','G','B','1',1,2,3,4,5,6,7,8,0,15,0,0,16,0,0,0,0,8};
    CHECK(memcmp(wire, golden, sizeof(golden)) == 0);
    unsigned calls = resolver_calls;
    memset(&got, 0x5a, sizeof(got)); original = got;
    memset(storage, 'x', sizeof(storage)); memcpy(canary, storage, sizeof(storage));
    required = 999U;
    CHECK(cog_cli_bootstrap_transport_decode(wire, length, &got, storage, 1U, &required) == COG_ERROR_OVERFLOW && required == strlen(transport.ca_bundle) + 1U);
    CHECK(memcmp(&got, &original, sizeof(got)) == 0 && memcmp(storage, canary, sizeof(storage)) == 0);
    CHECK(cog_cli_bootstrap_transport_decode(wire, length, &got, storage, sizeof(storage), &required) == COG_OK);
    CHECK(got.max_connections == 4U && got.connect_timeout_ms == 10000U && got.stall_timeout_ms == 30000U &&
        got.max_request_bytes == 1048576U && got.max_header_bytes == 65536U && got.chunk_bytes == 16384U &&
        got.chunks_per_request == 8U && got.service_quantum == 16U && !got.host_runtime_initialized &&
        got.dependency_memory == COG_NET_DEPENDENCY_MEMORY_UNSPECIFIED && got.observer == NULL && got.observer_user == NULL &&
        strcmp(got.ca_bundle, transport.ca_bundle) == 0);
    memset(&copy, 0x66, sizeof(copy)); before = copy;
    CHECK(cog_cli_configuration_bootstrap_new(&decoded, wire, length, (cog_net_ctx_t *)(void *)&o, &copy) == COG_OK);
    CHECK(strcmp(copy.session_id, probe.session_id) == 0 && copy.owner.epoch == 3U && copy.owner.instance_id == 4U && copy.checkpoint.generation == 5U);
    CHECK(cog_cli_configuration_options(decoded)->net == (cog_net_ctx_t *)(void *)&o);
    CHECK(cog_cli_configuration_bootstrap_encode(decoded, &probe, &transport, again, sizeof(again), &required) == COG_OK && required == length && memcmp(wire, again, length) == 0);
    cog_cli_configuration_free(decoded); decoded = NULL;
    for (size_t i = 0U; i < length; ++i) {
        copy = before; got = original; required = 999U;
        CHECK(cog_cli_bootstrap_transport_decode(wire, i, &got, storage, sizeof(storage), &required) != COG_OK);
        CHECK(memcmp(&got, &original, sizeof(got)) == 0 && required == 999U);
        CHECK(cog_cli_configuration_bootstrap_new(&decoded, wire, i, (cog_net_ctx_t *)(void *)&o, &copy) != COG_OK && decoded == NULL && memcmp(&copy, &before, sizeof(copy)) == 0);
    }
    memcpy(modified, wire, length); modified[length] = 0U;
    CHECK(cog_cli_configuration_bootstrap_new(&decoded, modified, length + 1U, (cog_net_ctx_t *)(void *)&o, &copy) == COG_ERROR_PROTOCOL);
    modified[3] = '2'; CHECK(cog_cli_configuration_bootstrap_new(&decoded, modified, length, (cog_net_ctx_t *)(void *)&o, &copy) == COG_ERROR_UNSUPPORTED);
    memcpy(modified, wire, length); modified[0] = 'X'; CHECK(cog_cli_configuration_bootstrap_new(&decoded, modified, length, (cog_net_ctx_t *)(void *)&o, &copy) == COG_ERROR_PROTOCOL);
    memcpy(modified, wire, length); modified[12] = 9U; CHECK(cog_cli_configuration_bootstrap_new(&decoded, modified, length, (cog_net_ctx_t *)(void *)&o, &copy) == COG_ERROR_PROTOCOL);
    memcpy(modified, wire, length); modified[13] = 16U; CHECK(cog_cli_configuration_bootstrap_new(&decoded, modified, length, (cog_net_ctx_t *)(void *)&o, &copy) == COG_ERROR_PROTOCOL);
    size_t secret_at = field_offset(wire, 3U) + 4U;
    for (unsigned byte = 0U; byte <= 32U; ++byte) {
        memcpy(modified, wire, length); modified[secret_at] = (unsigned char)byte;
        CHECK(cog_cli_configuration_bootstrap_new(&decoded, modified, length, (cog_net_ctx_t *)(void *)&o, &copy) == COG_ERROR_PROTOCOL);
    }
    memcpy(modified, wire, length); modified[secret_at] = 127U;
    CHECK(cog_cli_configuration_bootstrap_new(&decoded, modified, length, (cog_net_ctx_t *)(void *)&o, &copy) == COG_ERROR_PROTOCOL);
    size_t endpoint_at = field_offset(wire, 1U) + 4U;
    memcpy(modified, wire, length); modified[endpoint_at] = 'x';
    CHECK(cog_cli_configuration_bootstrap_new(&decoded, modified, length, (cog_net_ctx_t *)(void *)&o, &copy) == COG_ERROR_UNSUPPORTED);
    modified[secret_at] = 0U;
    CHECK(cog_cli_configuration_bootstrap_new(&decoded, modified, length, (cog_net_ctx_t *)(void *)&o, &copy) == COG_ERROR_PROTOCOL);
    /* Zero/overflowed numeric limits, malformed canonical paths/reference and
       oversized real fields are rejected before publication, not deferred to IO. */
    static const size_t numeric_offsets[] = {14U, 18U, 22U, 26U, 30U, 38U, 50U};
    for (size_t i = 0U; i < sizeof(numeric_offsets) / sizeof(numeric_offsets[0]); ++i) {
        size_t width = numeric_offsets[i] == 30U || numeric_offsets[i] == 38U ? 8U : 4U;
        memcpy(modified, wire, length); memset(modified + numeric_offsets[i], 0, width);
        CHECK(cog_cli_configuration_bootstrap_new(&decoded, modified, length, (cog_net_ctx_t *)(void *)&o, &copy) == COG_ERROR_PROTOCOL && decoded == NULL);
    }
    memcpy(modified, wire, length); memset(modified + 38U, 255, 8U);
    CHECK(cog_cli_configuration_bootstrap_new(&decoded, modified, length, (cog_net_ctx_t *)(void *)&o, &copy) == COG_ERROR_LIMIT);
    size_t store_at = field_offset(wire, 6U) + 4U;
    memcpy(modified, wire, length); modified[store_at] = 'x';
    CHECK(cog_cli_configuration_bootstrap_new(&decoded, modified, length, (cog_net_ctx_t *)(void *)&o, &copy) == COG_ERROR_PROTOCOL);
    size_t reference_at = field_offset(wire, 4U) + 4U;
    memcpy(modified, wire, length); memcpy(modified + reference_at, "env:!", 5U);
    CHECK(cog_cli_configuration_bootstrap_new(&decoded, modified, length, (cog_net_ctx_t *)(void *)&o, &copy) == COG_ERROR_PROTOCOL);
    size_t model_at = field_offset(wire, 0U), old_model_bytes = strlen(o.model_id);
    memcpy(modified, wire, model_at); modified[model_at] = 0U; modified[model_at+1U] = 0U;
    modified[model_at+2U] = 16U; modified[model_at+3U] = 0U;
    memset(modified + model_at + 4U, 'x', 4096U);
    memcpy(modified + model_at + 4U + 4096U, wire + model_at + 4U + old_model_bytes,
        length - model_at - 4U - old_model_bytes);
    CHECK(cog_cli_configuration_bootstrap_new(&decoded, modified, length + 4096U - old_model_bytes,
        (cog_net_ctx_t *)(void *)&o, &copy) == COG_ERROR_LIMIT && decoded == NULL);
    CHECK(probe_ids(parent, &probe, &transport, wire, length) == 0);
    CHECK(resolver_calls == calls);
    cog_cli_configuration_free(parent);
    return 0;
}
int main(void)
{
    /* Startup is explicit and synthetic. Clones below must not consult even a
       malformed replacement environment once the parent has been resolved. */
    for (size_t i = 0U; i < cog_cli_field_count; ++i)
        if (cog_cli_fields[i].environment != NULL) CHECK(unsetenv(cog_cli_fields[i].environment) == 0);
    CHECK(attachment() == 0); CHECK(child() == 0); CHECK(bootstrap() == 0);
    puts("resolved child/bootstrap profiles: ok"); return 0;
}

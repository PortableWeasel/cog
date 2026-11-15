/** @file test_openrouter.c @brief Actual adapter with controlled retained net
 * I/O. */
#include <assert.h>
#include <cog/openrouter.h>
#include <cog/provider.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Only transport calls are controlled. Real request encoder, framing, parser,
 * turn assembly, capacities and generic dispatch run unchanged. */
static const char *wire;
static size_t wire_len, offset, fragment = 1U;
static uint64_t net_sequence, fixture_now;
static bool released;
static int response_status = 200;
static cog_result_t terminal_result = COG_OK;
static char captured[131072];
static size_t allocations, fail_after;
static bool forbid_alloc;
void *__real_malloc(size_t n);
void *__real_calloc(size_t a, size_t b);
void *__real_realloc(void *p, size_t n);
void *__wrap_malloc(size_t n);
void *__wrap_calloc(size_t a, size_t b);
void *__wrap_realloc(void *p, size_t n);
void *__wrap_malloc(size_t n)
{
    ++allocations;
    assert(!forbid_alloc);
    if (fail_after != 0U && --fail_after == 0U)
        return NULL;
    return __real_malloc(n);
}
void *__wrap_calloc(size_t a, size_t b)
{
    ++allocations;
    assert(!forbid_alloc);
    if (fail_after != 0U && --fail_after == 0U)
        return NULL;
    return __real_calloc(a, b);
}
void *__wrap_realloc(void *p, size_t n)
{
    ++allocations;
    assert(!forbid_alloc);
    if (fail_after != 0U && --fail_after == 0U)
        return NULL;
    return __real_realloc(p, n);
}
cog_result_t __wrap_cog_net_request(cog_net_ctx_t *, const char *, const char *,
                                    const char *, const char *, size_t,
                                    cog_request_id_t *);
cog_result_t __wrap_cog_net_service(cog_net_ctx_t *, uint64_t, uint64_t *);
cog_result_t __wrap_cog_net_peek_request(cog_net_ctx_t *, cog_request_id_t,
                                         cog_net_response_t *);
cog_result_t __wrap_cog_net_ack(cog_net_ctx_t *, cog_request_id_t, uint64_t);
cog_result_t __wrap_cog_net_request_release(cog_net_ctx_t *, cog_request_id_t);
cog_result_t __wrap_cog_net_request(cog_net_ctx_t *ctx, const char *method,
                                    const char *url, const char *headers,
                                    const char *body, size_t n,
                                    cog_request_id_t *id)
{
    (void)ctx;
    assert(strcmp(method, "POST") == 0);
    assert(strcmp(url, "https://example.invalid/v1/chat/completions") == 0);
    assert(strstr(headers, "Authorization: Bearer synthetic") != NULL);
    assert(n < sizeof(captured));
    memcpy(captured, body, n);
    captured[n] = '\0';
    *id = 1U;
    offset = 0U;
    net_sequence = 1U;
    released = false;
    return COG_OK;
}
cog_result_t __wrap_cog_net_service(cog_net_ctx_t *ctx, uint64_t now,
                                    uint64_t *deadline)
{
    (void)ctx;
    (void)now;
    *deadline = UINT64_MAX;
    return COG_OK;
}
cog_result_t __wrap_cog_net_peek_request(cog_net_ctx_t *ctx,
                                         cog_request_id_t id,
                                         cog_net_response_t *out)
{
    (void)ctx;
    assert(id == 1U);
    assert(!released);
    size_t n = wire_len - offset;
    if (n > fragment)
        n = fragment;
    *out = (cog_net_response_t){.http_status = response_status,
                                .chunk = wire + offset,
                                .chunk_len = n,
                                .is_complete = n == 0U,
                                .request_id = id,
                                .sequence = net_sequence,
                                .outcome = terminal_result,
                                .error_kind = COG_NET_ERROR_TIMEOUT,
                                .retry_after_ms = 7000U};
    return COG_OK;
}
cog_result_t __wrap_cog_net_ack(cog_net_ctx_t *ctx, cog_request_id_t id,
                                uint64_t seq)
{
    (void)ctx;
    assert(id == 1U);
    assert(seq == net_sequence++);
    size_t n = wire_len - offset;
    if (n > fragment)
        n = fragment;
    offset += n;
    return COG_OK;
}
cog_result_t __wrap_cog_net_request_release(cog_net_ctx_t *ctx,
                                            cog_request_id_t id)
{
    (void)ctx;
    assert(id == 1U);
    assert(!released);
    released = true;
    return COG_OK;
}
static const cog_model_t model = {.id = "test",
                                  .context_window = 4096U,
                                  .capabilities =
                                      COG_MODEL_CAPABILITY_STREAMING |
                                      COG_MODEL_CAPABILITY_TOOLS};
static const cog_model_table_t models = {&model, 1U};
static cog_openrouter_config_t config = {
    .net = (cog_net_ctx_t *)(uintptr_t)1U,
    .limits = {.request_bytes = 131072U,
               .parser_bytes = 524288U,
               .writer_bytes = 1048576U,
               .max_json_depth = 32U,
               .max_frame_bytes = 32768U,
               .retained_delta_bytes = 16384U,
               .retained_delta_count = 1U,
               .max_text_bytes = 65536U,
               .max_tool_calls = 4U,
               .max_tool_id_bytes = 128U,
               .max_tool_name_bytes = 128U,
               .max_tool_arguments_bytes = 8192U,
               .max_opaque_bytes = 8192U},
    .service_quantum = 1U};
static cog_endpoint_t endpoint = {.name = "fixture",
                                  .base_url = "https://example.invalid/v1",
                                  .api_key = "synthetic",
                                  .vtable = &cog_openrouter_provider_vtable,
                                  .models = &models,
                                  .implementation_config = &config,
                                  .generation = 7U};
static cog_provider_request_t request = {.model_id = "test",
                                         .stream = true,
                                         .max_tokens = 512U,
                                         .configuration_generation = 7U};
static cog_provider_t *start(const char *s)
{
    wire = s;
    wire_len = strlen(s);
    fixture_now = 0U;
    cog_provider_t *p = NULL;
    assert(cog_provider_connect(&endpoint, &p) == COG_OK);
    assert(p != NULL);
    forbid_alloc = true;
    assert(cog_provider_send(p, &request) == COG_OK);
    return p;
}
static void stop(cog_provider_t *p)
{
    forbid_alloc = false;
    assert(cog_provider_disconnect(p) == COG_OK);
}
static cog_provider_delta_t obtain(cog_provider_t *p, char *storage, size_t cap)
{
    cog_provider_delta_t d;
    size_t required;
    for (uint64_t now = 0U; now < 100000U; ++now)
    {
        uint64_t deadline;
        assert(cog_provider_poll(p, fixture_now++, &deadline) == COG_OK);
        cog_result_t rc = cog_provider_next(p, &d, storage, cap, &required);
        if (rc == COG_OK)
            return d;
        assert(rc == COG_ERROR_AGAIN);
    }
    assert(false);
    return (cog_provider_delta_t){0};
}
static const char good[] =
    ": heartbeat\r\n\r\n"
    "data: {\r\ndata: \"choices\":[{\"index\":0,\"delta\":{\"content\":\"Hi "
    "café 😀\"}}]}\r\n\r\n"
    "data: "
    "{\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{\"index\":1,\"id\":"
    "\"call_\",\"type\":\"function\",\"function\":{\"name\":\"edit_\","
    "\"arguments\":\"{\\\"path\\\":\"}},{\"index\":0,\"id\":\"other\","
    "\"function\":{\"name\":\"echo\",\"arguments\":\"{}\"}}]}}]}\n\n"
    "data: "
    "{\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{\"index\":1,\"id\":"
    "\"7\",\"function\":{\"name\":\"file\",\"arguments\":\"\\\"demo.txt\\\"}\"}"
    "}],\"reasoning_details\":[{\"type\":\"reasoning.text\",\"text\":"
    "\"synthetic \",\"format\":\"test-v1\",\"index\":0}]}}]}\n\n"
    "data: "
    "{\"choices\":[{\"index\":0,\"delta\":{\"reasoning_details\":[{\"type\":"
    "\"reasoning.text\",\"text\":\"opaque "
    "fragments\",\"signature\":\"test-signature\",\"index\":0},{\"type\":"
    "\"reasoning.encrypted\",\"data\":\"opaque\"}]},\"finish_reason\":\"tool_"
    "calls\"}]}\n\n"
    "data: "
    "{\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"tool_calls\"}]"
    ",\"usage\":{\"prompt_tokens\":9,\"completion_tokens\":7,\"cost\":0.01,"
    "\"completion_tokens_details\":{\"reasoning_tokens\":0}}}\n\n"
    "data: [DONE]\n\n";
static void valid(void)
{
    for (size_t f = 1U; f <= strlen(good); f = f < 16U ? f + 1U : f * 2U)
    {
        fragment = f;
        cog_provider_t *p = start(good);
        char storage[20000];
        cog_provider_delta_t d = obtain(p, storage, sizeof(storage));
        assert(d.sequence == 1U && d.kind == COG_PROVIDER_DELTA_TEXT);
        assert(strcmp(d.data, "Hi café 😀") == 0);
        uint64_t seq = 1U;
        for (size_t tool = 0U; tool < 2U; ++tool)
        {
            d = obtain(p, storage, sizeof(storage));
            assert(d.sequence == ++seq &&
                   d.kind == COG_PROVIDER_DELTA_TOOL_BEGIN);
            assert(d.tool_index == tool);
            assert(strcmp(d.tool_call_id, tool == 0U ? "other" : "call_7") ==
                   0);
            assert(strcmp(d.tool_name, tool == 0U ? "echo" : "edit_file") == 0);
            d = obtain(p, storage, sizeof(storage));
            assert(d.sequence == ++seq &&
                   d.kind == COG_PROVIDER_DELTA_TOOL_ARGUMENTS);
            assert(strcmp(d.data,
                          tool == 0U ? "{}" : "{\"path\":\"demo.txt\"}") == 0);
            d = obtain(p, storage, sizeof(storage));
            assert(d.sequence == ++seq &&
                   d.kind == COG_PROVIDER_DELTA_TOOL_END);
        }
        d = obtain(p, storage, sizeof(storage));
        assert(d.sequence == ++seq && d.kind == COG_PROVIDER_DELTA_OPAQUE);
        char opaque[8192];
        assert(d.opaque.size < sizeof(opaque));
        memcpy(opaque, d.opaque.data, d.opaque.size);
        opaque[d.opaque.size] = '\0';
        assert(strstr(opaque, "synthetic opaque fragments") != NULL);
        assert(strstr(opaque, "test-signature") != NULL);
        d = obtain(p, storage, sizeof(storage));
        assert(d.sequence == ++seq && d.kind == COG_PROVIDER_DELTA_USAGE);
        assert(d.usage.tokens_known && d.usage.input_tokens == 9U &&
               d.usage.output_tokens == 7U);
        assert(d.usage.cost_known && d.usage.reasoning_tokens_known);
        d = obtain(p, storage, sizeof(storage));
        assert(d.sequence == ++seq && d.kind == COG_PROVIDER_DELTA_FINISH &&
               d.outcome == COG_OK &&
               d.finish_reason == COG_FINISH_REASON_TOOL_CALLS);
        stop(p);
    }
}
static const char plain[] =
    "data: "
    "{\"choices\":[{\"index\":0,\"delta\":{\"content\":\"hello\"},\"finish_"
    "reason\":\"stop\"}]}\n\ndata: [DONE]\n\n";
static void overflow_cancel(void)
{
    fragment = 4096U;
    cog_provider_t *p = start(plain);
    uint64_t deadline;
    assert(cog_provider_poll(p, 0U, &deadline) == COG_OK);
    cog_provider_delta_t d;
    memset(&d, 0x55, sizeof(d));
    cog_provider_delta_t old = d;
    size_t required = 0U;
    char bytes[32];
    memset(bytes, 0x66, sizeof(bytes));
    assert(cog_provider_next(p, &d, bytes, 1U, &required) ==
               COG_ERROR_OVERFLOW &&
           required == 6U);
    assert(memcmp(&d, &old, sizeof(d)) == 0 && bytes[0] == 0x66);
    assert(cog_provider_poll(p, 1U, &deadline) == COG_OK);
    assert(cog_provider_cancel(p) == COG_OK);
    assert(cog_provider_cancel(p) == COG_OK);
    assert(cog_provider_next(p, &d, bytes, sizeof(bytes), &required) ==
               COG_OK &&
           d.sequence == 1U && strcmp(d.data, "hello") == 0);
    assert(cog_provider_next(p, &d, NULL, 0U, &required) == COG_OK &&
           d.kind == COG_PROVIDER_DELTA_FINISH && d.sequence == 2U &&
           d.outcome == COG_ERROR_ABORTED);
    assert(cog_provider_next(p, &d, NULL, 0U, &required) ==
           COG_ERROR_INVALID_STATE);
    cog_provider_error_t error;
    assert(cog_provider_get_error(p, &error) == COG_OK &&
           error.output_accepted && !error.replay_safe);
    assert(strcmp(error.message, "provider turn cancelled") == 0);
    assert(cog_provider_poll(p, 0U, &deadline) == COG_ERROR_INVALID_ARG);
    stop(p);
}
static cog_result_t check(const char *s)
{
    fragment = 1U;
    cog_provider_t *p = start(s);
    char bytes[20000];
    cog_provider_delta_t d;
    do
    {
        d = obtain(p, bytes, sizeof(bytes));
    } while (d.kind != COG_PROVIDER_DELTA_FINISH);
    cog_result_t rc = d.outcome;
    stop(p);
    return rc;
}
static void failures(void)
{
    assert(check("data: [DONE]\n\n") == COG_ERROR_PROTOCOL);
    assert(check("data: {broken}\n\n") == COG_ERROR_PROTOCOL);
    assert(check("data: {\"choices\":[],\"choices\":[]}\n\n") ==
           COG_ERROR_PROTOCOL);
    assert(check("data: {\"choices\":[],\"x\":{\"a\":1,\"\\u0061\":2}}\n\n") ==
           COG_ERROR_PROTOCOL);
    assert(
        check("data: "
              "{\"choices\":[{\"index\":0,\"delta\":{\"content\":\"discard\","
              "\"tool_calls\":42}}]}\n\n") == COG_ERROR_PROTOCOL);
    assert(check("data: "
                 "{\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":"
                 "\"future\"}]}\n\n") == COG_ERROR_PROTOCOL);
    assert(check("data: "
                 "{\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":"
                 "\"stop\"}]}\n\n") == COG_ERROR_PROTOCOL);
    assert(
        check("data: "
              "{\"choices\":[{\"index\":0,\"delta\":{\"content\":\"\\uD800\"}}"
              "]}\n\n") == COG_ERROR_PROTOCOL);
    assert(
        check("data: "
              "{\"choices\":[{\"index\":0,\"delta\":{\"content\":\"\xc0\xaf\"}"
              "}]}\n\n") == COG_ERROR_PROTOCOL);
    assert(
        check("data: "
              "{\"choices\":[{\"index\":0,\"delta\":{\"reasoning_details\":[{"
              "\"type\":\"unknown\"}]}}]}\n\n") == COG_ERROR_UNSUPPORTED);
    assert(check("data: {\"error\":{\"message\":\"secret synthetic "
                 "response\"}}\n\n") == COG_ERROR_UNAVAILABLE);
    size_t saved = config.limits.retained_delta_bytes;
    config.limits.retained_delta_bytes = 2U;
    assert(check(plain) == COG_ERROR_LIMIT);
    config.limits.retained_delta_bytes = saved;
    saved = config.limits.parser_bytes;
    config.limits.parser_bytes = 96U;
    assert(check(plain) == COG_ERROR_LIMIT);
    config.limits.parser_bytes = saved;
    saved = config.limits.max_frame_bytes;
    config.limits.max_frame_bytes = 20U;
    assert(check(plain) == COG_ERROR_LIMIT);
    config.limits.max_frame_bytes = saved;
    saved = config.limits.max_json_depth;
    config.limits.max_json_depth = 2U;
    assert(check(plain) == COG_ERROR_LIMIT);
    config.limits.max_json_depth = saved;
}

static void request_copy_and_opaque(void)
{
    static const char opaque[] = "{\"reasoning_details\":[]}";
    char model_id[] = "test", text[] = "quote \" café";
    char id[] = "call_1", name[] = "echo", args[] = "{}";
    cog_message_tool_call_t call = {id, name, args, 2U};
    cog_message_opaque_t o = {"openrouter.chat-completions", "json-v1",
                              (const unsigned char *)opaque,
                              sizeof(opaque) - 1U};
    cog_message_t msg = {.role = COG_MESSAGE_ROLE_ASSISTANT,
                         .content = text,
                         .content_len = strlen(text),
                         .tool_calls = &call,
                         .tool_call_count = 1U,
                         .opaque = &o,
                         .opaque_count = 1U};
    cog_provider_request_t r = request;
    r.model_id = model_id;
    r.messages = &msg;
    r.message_count = 1U;
    wire = plain;
    wire_len = strlen(plain);
    fragment = 4096U;
    fixture_now = 0U;
    cog_provider_t *p = NULL;
    assert(cog_provider_connect(&endpoint, &p) == COG_OK);
    forbid_alloc = true;
    assert(cog_provider_send(p, &r) == COG_OK);
    memset(model_id, 'X', 4U);
    memset(text, 'X', strlen(text));
    memset(args, 'X', 2U);
    assert(strstr(captured, "\"model\":\"test\"") != NULL);
    assert(strstr(captured, "\"reasoning_details\":[]") != NULL);
    assert(strstr(captured, "quote \\\" café") != NULL);
    assert(strstr(captured, "\"arguments\":\"{}\"") != NULL);
    assert(cog_provider_cancel(p) == COG_OK);
    stop(p);
    /* Unsupported namespace/format and malformed argument JSON fail before
     * wire.
     */
    assert(cog_provider_connect(&endpoint, &p) == COG_OK);
    r.model_id = "test";
    args[0] = '{';
    args[1] = '}';
    o.format = "unknown";
    assert(cog_provider_send(p, &r) == COG_ERROR_UNSUPPORTED);
    o.format = "json-v1";
    args[0] = '[';
    args[1] = ']';
    assert(cog_provider_send(p, &r) == COG_ERROR_PROTOCOL);
    stop(p);
}
static void terminal_and_opaque(void)
{
    const char *cases[] = {
        "data: "
        "{\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"stop\"}]}"
        "\n\ndata: "
        "{\"choices\":[{\"index\":0,\"delta\":{\"content\":\"late\"},\"finish_"
        "reason\":\"stop\"}]}\n\ndata: [DONE]\n\n",
        "data: "
        "{\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"stop\"}]}"
        "\n\ndata: "
        "{\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"length\"}]"
        "}"
        "\n\n",
        "data: "
        "{\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{\"index\":0,"
        "\"id\":\"same\",\"function\":{\"name\":\"echo\",\"arguments\":\"{}\"}}"
        ",{"
        "\"index\":1,\"id\":\"same\",\"function\":{\"name\":\"echo\","
        "\"arguments\":\"{}\"}}]},\"finish_reason\":\"tool_calls\"}]}\n\ndata: "
        "[DONE]\n\n",
        "data: "
        "{\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{\"index\":0,"
        "\"id\":\"a\",\"function\":{\"name\":\"echo\",\"arguments\":\"{\"}}]},"
        "\"finish_reason\":\"tool_calls\"}]}\n\ndata: [DONE]\n\n"};
    for (size_t i = 0U; i < sizeof(cases) / sizeof(cases[0]); ++i)
        assert(check(cases[i]) == COG_ERROR_PROTOCOL);
    static const char conflict[] =
        "data: "
        "{\"choices\":[{\"index\":0,\"delta\":{\"reasoning_details\":[{"
        "\"type\":"
        "\"reasoning.text\",\"text\":\"a\",\"future\":{\"x\":1}}]}}]}\n\ndata: "
        "{\"choices\":[{\"index\":0,\"delta\":{\"reasoning_details\":[{"
        "\"type\":"
        "\"reasoning.text\",\"text\":\"b\",\"future\":{\"x\":2}}]}}]}\n\n";
    assert(check(conflict) == COG_ERROR_UNSUPPORTED);
    static const char empty[] =
        "data: "
        "{\"choices\":[{\"index\":0,\"delta\":{\"reasoning_details\":[]},"
        "\"finish_reason\":\"stop\"}]}\n\ndata: [DONE]\n\n";
    cog_provider_t *p = start(empty);
    char bytes[20000];
    cog_provider_delta_t d = obtain(p, bytes, sizeof(bytes));
    assert(d.kind == COG_PROVIDER_DELTA_OPAQUE && d.opaque.size == 24U &&
           memcmp(d.opaque.data, "{\"reasoning_details\":[]}", 24U) == 0);
    d = obtain(p, bytes, sizeof(bytes));
    assert(d.kind == COG_PROVIDER_DELTA_FINISH && d.outcome == COG_OK);
    stop(p);
    request.stream = false;
    p = start(
        "{\"choices\":[{\"index\":0,\"message\":{\"role\":\"assistant\","
        "\"content\":\"nonstream\"},\"finish_reason\":\"stop\"}],\"usage\":"
        "{\"prompt_tokens\":1,\"completion_tokens\":2}}");
    d = obtain(p, bytes, sizeof(bytes));
    assert(d.kind == COG_PROVIDER_DELTA_TEXT &&
           strcmp(d.data, "nonstream") == 0);
    d = obtain(p, bytes, sizeof(bytes));
    assert(d.kind == COG_PROVIDER_DELTA_USAGE);
    d = obtain(p, bytes, sizeof(bytes));
    assert(d.kind == COG_PROVIDER_DELTA_FINISH && d.outcome == COG_OK);
    stop(p);
    request.stream = true;
}
static void additional_capacity_checks(void)
{
    size_t saved = config.limits.max_text_bytes;
    config.limits.max_text_bytes = 1U;
    assert(check(plain) == COG_ERROR_LIMIT);
    config.limits.max_text_bytes = saved;
    saved = config.limits.writer_bytes;
    config.limits.writer_bytes = 96U;
    cog_provider_t *p = NULL;
    assert(cog_provider_connect(&endpoint, &p) == COG_OK);
    assert(cog_provider_send(p, &request) == COG_ERROR_LIMIT);
    stop(p);
    config.limits.writer_bytes = saved;
    assert(cog_provider_connect(&endpoint, &p) == COG_OK);
    cog_provider_request_t invalid = request;
    invalid.tools_json = "[1]";
    invalid.tools_json_len = 3U;
    assert(cog_provider_send(p, &invalid) == COG_ERROR_UNSUPPORTED);
    invalid.tools_json =
        "[{\"type\":\"function\",\"function\":{\"name\":\"\"}}]";
    invalid.tools_json_len = strlen(invalid.tools_json);
    assert(cog_provider_send(p, &invalid) == COG_ERROR_INVALID_ARG);
    stop(p);
}

/* A nonstream array contains final entries, not streaming fragments. */
static void nonstream_opaque_final_array(void)
{
    static const char expected[] =
        "{\"reasoning_details\":[{\"type\":\"reasoning.text\",\"text\":\"a\","
        "\"id\":\"one\"},{\"type\":\"reasoning.text\",\"text\":\"b\",\"id\":\"two\"}]}";
    request.stream = false;
    cog_provider_t *p = start(
        "{\"choices\":[{\"index\":0,\"message\":{\"role\":\"assistant\","
        "\"content\":\"\",\"reasoning_details\":[{\"type\":\"reasoning.text\","
        "\"text\":\"a\",\"id\":\"one\"},{\"type\":\"reasoning.text\",\"text\":\"b\","
        "\"id\":\"two\"}]},\"finish_reason\":\"stop\"}]}");
    char bytes[20000];
    cog_provider_delta_t d = obtain(p, bytes, sizeof(bytes));
    assert(d.kind == COG_PROVIDER_DELTA_OPAQUE);
    assert(d.opaque.size == sizeof(expected) - 1U);
    assert(memcmp(d.opaque.data, expected, sizeof(expected) - 1U) == 0);
    d = obtain(p, bytes, sizeof(bytes));
    assert(d.kind == COG_PROVIDER_DELTA_FINISH && d.outcome == COG_OK);
    stop(p);
    request.stream = true;
}
static void constructor_failures(void)
{
    size_t before = allocations;
    cog_provider_t *p = NULL;
    assert(cog_provider_connect(&endpoint, &p) == COG_OK);
    size_t count = allocations - before;
    stop(p);
    for (size_t i = 1U; i <= count; ++i)
    {
        fail_after = i;
        p = (cog_provider_t *)(uintptr_t)1U;
        assert(cog_provider_connect(&endpoint, &p) == COG_ERROR_NOMEM &&
               p == NULL);
        fail_after = 0U;
    }
    const char *url = endpoint.base_url;
    const char *bad[] = {"https://:0/v1",          "http://localhost/v1",
                         "https://user@host/v1",   "https://host:65536/v1",
                         "https://host/?secret=x", "https://host/\r\n"};
    for (size_t i = 0U; i < sizeof(bad) / sizeof(bad[0]); ++i)
    {
        endpoint.base_url = bad[i];
        assert(cog_provider_connect(&endpoint, &p) == COG_ERROR_INVALID_ARG &&
               p == NULL);
    }
    endpoint.base_url = "https://[::1]:443/v1";
    assert(cog_provider_connect(&endpoint, &p) == COG_OK);
    stop(p);
    endpoint.base_url = url;
    cog_provider_request_t invalid = request;
    invalid.model_id = "\xc0\xaf";
    assert(cog_provider_request_validate(&invalid) == COG_ERROR_INVALID_ARG);
}
static void transport_errors(void)
{
    fragment = 4096U;
    cog_provider_t *p = start(plain);
    response_status = 429;
    char bytes[20000];
    cog_provider_delta_t d = obtain(p, bytes, sizeof(bytes));
    assert(d.kind == COG_PROVIDER_DELTA_FINISH &&
           d.outcome == COG_ERROR_UNAVAILABLE);
    cog_provider_error_t e;
    assert(cog_provider_get_error(p, &e) == COG_OK && e.http_status == 429 &&
           e.retry_after_ms == 7000U && e.retryable && e.replay_safe &&
           !e.output_accepted);
    stop(p);
    response_status = 200;
    p = start("data: "
              "{\"choices\":[{\"index\":0,\"delta\":{\"content\":\"partial\"}}]"
              "}\n\n");
    terminal_result = COG_ERROR_TIMEOUT;
    d = obtain(p, bytes, sizeof(bytes));
    assert(d.kind == COG_PROVIDER_DELTA_TEXT);
    d = obtain(p, bytes, sizeof(bytes));
    assert(d.kind == COG_PROVIDER_DELTA_FINISH &&
           d.outcome == COG_ERROR_TIMEOUT);
    assert(cog_provider_get_error(p, &e) == COG_OK &&
           e.kind == COG_PROVIDER_ERROR_TIMEOUT && e.output_accepted &&
           !e.replay_safe);
    stop(p);
    terminal_result = COG_OK;
}

static void depth_boundary(void)
{
    size_t saved = config.limits.max_json_depth;
    config.limits.max_json_depth = 256U;
    char frame[8192];
    size_t at = 0U;
    for (unsigned turn = 0U; turn < 2U; ++turn)
    {
        const char *prefix =
            "data: "
            "{\"choices\":[{\"index\":0,\"delta\":{\"reasoning_details\":[{"
            "\"type\":\"reasoning.text\",\"text\":\"a\",\"future\":";
        strcpy(frame + at, prefix);
        at += strlen(prefix);
        for (unsigned i = 0U; i < 250U; ++i)
            frame[at++] = '[';
        frame[at++] = '0';
        for (unsigned i = 0U; i < 250U; ++i)
            frame[at++] = ']';
        const char *suffix =
            turn == 0U
                ? "}]}}]}\n\n"
                : "}]},\"finish_reason\":\"stop\"}]}\n\ndata: [DONE]\n\n";
        strcpy(frame + at, suffix);
        at += strlen(suffix);
    }
    assert(check(frame) == COG_OK);
    config.limits.max_json_depth = 257U;
    cog_provider_t *p = (cog_provider_t *)(uintptr_t)1U;
    assert(cog_provider_connect(&endpoint, &p) == COG_ERROR_INVALID_ARG &&
           p == NULL);
    config.limits.max_json_depth = 255U;
    assert(check(frame) == COG_ERROR_LIMIT);
    config.limits.max_json_depth = saved;
}
int main(void)
{
    additional_capacity_checks();
    depth_boundary();
    transport_errors();
    constructor_failures();
    request_copy_and_opaque();
    terminal_and_opaque();
    valid();
    overflow_cancel();
    failures();
    nonstream_opaque_final_array();
    printf("OpenRouter retained fixture checks passed (%zu constructor "
           "allocations)\n",
           allocations);
    return 0;
}

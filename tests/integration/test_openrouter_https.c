/** @file test_openrouter_https.c @brief Real adapter/net, two synthetic turns.
 * Credential enters through stdin only. Emits sanitized usage/outcome, never
 * credential headers, bodies, model text or opaque continuation bytes.
 */
#define _POSIX_C_SOURCE 200809L
#include <cog/openrouter.h>
#include <cog/provider.h>
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
static uint64_t monotonic_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0U;
    return (uint64_t)ts.tv_sec * 1000U + (uint64_t)ts.tv_nsec / 1000000U;
}
static cog_result_t drive(cog_provider_t *p, char *id, char *name,
                          char *arguments, char *text, unsigned char *opaque,
                          size_t *opaque_len, cog_usage_t *usage)
{
    uint64_t start = monotonic_ms();
    size_t text_len = 0U;
    char storage[65536];
    for (;;)
    {
        uint64_t now = monotonic_ms(), deadline;
        cog_result_t rc = cog_provider_poll(p, now, &deadline);
        if (rc != COG_OK)
            return rc;
        cog_provider_delta_t d;
        size_t required;
        rc = cog_provider_next(p, &d, storage, sizeof(storage), &required);
        if (rc == COG_ERROR_AGAIN)
        {
            if (now - start > 60000U)
            {
                (void)cog_provider_cancel(p);
                return COG_ERROR_TIMEOUT;
            }
            struct timespec pause = {0, 1000000L};
            (void)nanosleep(&pause, NULL);
            continue;
        }
        if (rc != COG_OK)
            return rc;
        if (d.kind == COG_PROVIDER_DELTA_TEXT)
        {
            if (d.data_len >= 8192U - text_len)
                return COG_ERROR_LIMIT;
            memcpy(text + text_len, d.data, d.data_len);
            text_len += d.data_len;
            text[text_len] = '\0';
        }
        if (d.kind == COG_PROVIDER_DELTA_TOOL_BEGIN)
        {
            if (d.tool_index != 0U || strlen(d.tool_call_id) >= 128U ||
                strlen(d.tool_name) >= 128U)
                return COG_ERROR_PROTOCOL;
            strcpy(id, d.tool_call_id);
            strcpy(name, d.tool_name);
        }
        if (d.kind == COG_PROVIDER_DELTA_TOOL_ARGUMENTS)
        {
            if (d.data_len >= 8192U)
                return COG_ERROR_LIMIT;
            memcpy(arguments, d.data, d.data_len);
            arguments[d.data_len] = '\0';
        }
        if (d.kind == COG_PROVIDER_DELTA_OPAQUE)
        {
            if (d.opaque.size > 32768U)
                return COG_ERROR_LIMIT;
            memcpy(opaque, d.opaque.data, d.opaque.size);
            *opaque_len = d.opaque.size;
        }
        if (d.kind == COG_PROVIDER_DELTA_USAGE)
            *usage = d.usage;
        if (d.kind == COG_PROVIDER_DELTA_FINISH)
            return d.outcome == COG_OK &&
                           (d.finish_reason == COG_FINISH_REASON_STOP ||
                            d.finish_reason == COG_FINISH_REASON_TOOL_CALLS)
                       ? COG_OK
                   : d.outcome == COG_OK ? COG_ERROR_PROTOCOL
                                         : d.outcome;
    }
}
int main(void)
{
    char key[4096];
    if (fgets(key, sizeof(key), stdin) == NULL)
        return 2;
    key[strcspn(key, "\r\n")] = '\0';
    const char *url = getenv("COG_PROVIDER_TEST_URL");
    if (url == NULL)
        return 2;
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
        return 2;
    cog_net_config_t nc = {.host_runtime_initialized = true,
                           .dependency_memory =
                               COG_NET_DEPENDENCY_MEMORY_DYNAMIC,
                           .ca_bundle = getenv("COG_NET_TEST_CA"),
                           .max_request_bytes = 262144U,
                           .chunk_bytes = 16384U,
                           .chunks_per_request = 8U};
    cog_net_ctx_t *net = NULL;
    cog_result_t rc = cog_net_new(&net, &nc);
    if (rc != COG_OK)
    {
        printf("transport-admission outcome=%d\n", (int)rc);
        curl_global_cleanup();
        return 3;
    }
    cog_openrouter_config_t config = {
        .net = net,
        .limits = {.request_bytes = 131072U,
                   .parser_bytes = 1048576U,
                   .writer_bytes = 2097152U,
                   .max_json_depth = 32U,
                   .max_frame_bytes = 65536U,
                   .retained_delta_bytes = 65536U,
                   .retained_delta_count = 1U,
                   .max_text_bytes = 65536U,
                   .max_tool_calls = 4U,
                   .max_tool_id_bytes = 128U,
                   .max_tool_name_bytes = 128U,
                   .max_tool_arguments_bytes = 8192U,
                   .max_opaque_bytes = 32768U},
        .service_quantum = 4U};
    cog_model_t model = {.id = "deepseek/deepseek-v4-flash-0731",
                         .context_window = 1048576U,
                         .capabilities = COG_MODEL_CAPABILITY_STREAMING |
                                         COG_MODEL_CAPABILITY_TOOLS};
    cog_model_table_t models = {&model, 1U};
    cog_endpoint_t endpoint = {.name = "openrouter",
                               .base_url = url,
                               .api_key = key,
                               .vtable = &cog_openrouter_provider_vtable,
                               .models = &models,
                               .implementation_config = &config,
                               .generation = 1U};
    cog_provider_t *p = NULL;
    rc = cog_provider_connect(&endpoint, &p);
    static const char tools[] =
        "[{\"type\":\"function\",\"function\":{\"name\":\"echo\","
        "\"description\":"
        "\"Returns supplied value "
        "unchanged\",\"parameters\":{\"type\":\"object\",\"properties\":{"
        "\"value\":{\"type\":\"string\"}},\"required\":[\"value\"],"
        "\"additionalProperties\":false}}}]";
    static const char prompt[] =
        "Call echo exactly once with value WIRE_OK. "
        "After the tool result, reply exactly WIRE_OK.";
    cog_message_t messages[4] = {{.role = COG_MESSAGE_ROLE_USER,
                                  .content = prompt,
                                  .content_len = sizeof(prompt) - 1U}};
    cog_provider_request_t request = {.model_id = model.id,
                                      .messages = messages,
                                      .message_count = 1U,
                                      .stream = true,
                                      .max_tokens = 512U,
                                      .tools_json = tools,
                                      .tools_json_len = sizeof(tools) - 1U,
                                      .configuration_generation = 1U};
    char id[128] = {0}, name[128] = {0}, arguments[8192] = {0},
         text[8192] = {0};
    unsigned char opaque[32768];
    size_t opaque_len = 0U;
    cog_usage_t usage = {0};
    if (rc == COG_OK)
        rc = cog_provider_send(p, &request);
    if (rc == COG_OK)
        rc = drive(p, id, name, arguments, text, opaque, &opaque_len, &usage);
    printf("synthetic-tool outcome=%d tokens_known=%d input=%llu output=%llu "
           "cost_known=%d cost=%.9f\n",
           (int)rc, (int)usage.tokens_known,
           (unsigned long long)usage.input_tokens,
           (unsigned long long)usage.output_tokens, (int)usage.cost_known,
           usage.cost);
    if (rc == COG_OK && (id[0] == '\0' || strcmp(name, "echo") != 0))
        rc = COG_ERROR_PROTOCOL;
    cog_message_tool_call_t call = {id, name, arguments, strlen(arguments)};
    cog_message_opaque_t continuation = {"openrouter.chat-completions",
                                         "json-v1", opaque, opaque_len};
    if (rc == COG_OK)
    {
        messages[1] =
            (cog_message_t){.role = COG_MESSAGE_ROLE_ASSISTANT,
                            .content = text,
                            .content_len = strlen(text),
                            .tool_calls = &call,
                            .tool_call_count = 1U,
                            .opaque = opaque_len != 0U ? &continuation : NULL,
                            .opaque_count = opaque_len != 0U ? 1U : 0U};
        messages[2] = (cog_message_t){.role = COG_MESSAGE_ROLE_TOOL,
                                      .content = "WIRE_OK",
                                      .content_len = 7U,
                                      .tool_call_id = id};
        request.message_count = 3U;
        request.tools_json = NULL;
        request.tools_json_len = 0U;
        rc = cog_provider_send(p, &request);
        if (rc == COG_OK)
        {
            id[0] = name[0] = arguments[0] = text[0] = '\0';
            opaque_len = 0U;
            usage = (cog_usage_t){0};
            rc = drive(p, id, name, arguments, text, opaque, &opaque_len,
                       &usage);
        }
        printf("synthetic-answer outcome=%d tokens_known=%d input=%llu "
               "output=%llu "
               "cost_known=%d cost=%.9f\n",
               (int)rc, (int)usage.tokens_known,
               (unsigned long long)usage.input_tokens,
               (unsigned long long)usage.output_tokens, (int)usage.cost_known,
               usage.cost);
        if (rc == COG_OK && (id[0] != '\0' || strstr(text, "WIRE_OK") == NULL))
            rc = COG_ERROR_PROTOCOL;
    }
    (void)cog_provider_disconnect(p);
    cog_net_free(net);
    volatile char *secret = key;
    for (size_t i = 0U; i < sizeof(key); ++i)
        secret[i] = 0;
    curl_global_cleanup();
    return rc == COG_OK ? 0 : 1;
}

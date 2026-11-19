/** @file test_toolkit_async.c @brief Strict schema, admission and retained effects. */
#define _POSIX_C_SOURCE 200809L
#include <cog/toolkit.h>
#include "fixtures/toolkit_core.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct counter { size_t constructed; size_t effects; } counter_t;
typedef struct handle { counter_t *counter; bool started; bool done; cog_result_t status; } handle_t;
static cog_result_t counter_new(cog_core_t *core, const char *request, size_t len,
    const cog_tool_async_config_t *config, void **out)
{
    assert(core != NULL && request != NULL && len != 0U);
    *out = NULL;
    handle_t *handle = calloc(1U, sizeof(*handle));
    if (handle == NULL) { return COG_ERROR_NOMEM; }
    handle->counter = config->user;
    handle->counter->constructed++;
    *out = handle;
    return COG_OK;
}
static cog_result_t counter_start(void *state, uint64_t now)
{
    (void)now;
    handle_t *handle = state;
    if (handle->started) { return COG_ERROR_INVALID_STATE; }
    handle->started = true;
    handle->counter->effects++;
    return COG_OK;
}
static cog_result_t counter_poll(void *state, uint64_t now, bool *done, uint64_t *deadline)
{
    (void)now;
    handle_t *handle = state;
    handle->done = true;
    *done = true; *deadline = UINT64_MAX;
    return COG_OK;
}
static cog_result_t counter_cancel(void *state, uint64_t now)
{
    (void)now;
    handle_t *handle = state;
    handle->status = COG_ERROR_ABORTED;
    handle->done = true;
    return COG_OK;
}
static cog_result_t counter_result(const void *state, cog_tool_output_t *out)
{
    const handle_t *handle = state;
    if (!handle->done) { return COG_ERROR_AGAIN; }
    *out = (cog_tool_output_t){ .status = handle->status, .output = "one effect",
        .output_len = 10U, .side_effects_possible = handle->started };
    return COG_OK;
}
static cog_result_t counter_destroy(void *state)
{
    handle_t *handle = state;
    if (handle != NULL && handle->started && !handle->done) { return COG_ERROR_INVALID_STATE; }
    free(handle);
    return COG_OK;
}
static const cog_tool_async_vtable_t counter_ops = {
    counter_new, counter_start, counter_poll, counter_cancel, counter_result, counter_destroy
};
static cog_result_t construct(cog_toolkit_execution_t **out, counter_t *counter,
    const char *schema, const char *arguments, const cog_tool_async_config_t *limits)
{
    const cog_tool_t tool = { .name = "counter", .input_schema = schema, .async = &counter_ops };
    const cog_tool_table_t table = { &tool, 1U };
    cog_tool_async_config_t config = { .max_output_bytes = 64U, .timeout_ms = 100U, .user = counter };
    if (limits != NULL) { config = *limits; config.user = counter; }
    /* The vtable is static and the handle retains no pointer to this descriptor. */
    return cog_toolkit_execution_new(out, toolkit_test_core, &table, "call-1", "counter",
        arguments, strlen(arguments), &config);
}
static void expect_schema(const char *schema, const char *arguments, cog_result_t expected)
{
    counter_t counter = {0};
    cog_toolkit_execution_t *execution = NULL;
    cog_result_t r = construct(&execution, &counter, schema, arguments, NULL);
    if (r != expected) { fprintf(stderr, "schema r=%d expected=%d: %s %s\n", (int)r, (int)expected, schema, arguments); abort(); }
    if (expected == COG_OK) { assert(execution != NULL && counter.constructed == 1U); assert(cog_toolkit_execution_free(execution) == COG_OK); }
    else { assert(execution == NULL && counter.constructed == 0U); }
    assert(counter.effects == 0U);
}
static void schema_cases(void)
{
    expect_schema("{}", "{}", COG_OK);
    expect_schema("{}", "[]", COG_ERROR_INVALID_ARG);
    expect_schema("{}", "{\"x\":1,\"x\":2}", COG_ERROR_INVALID_ARG);
    expect_schema("{}", "{\"x\":{\"a\":1,\"\\u0061\":2}}", COG_ERROR_INVALID_ARG);
    expect_schema("{}", "{\"x\":01}", COG_ERROR_INVALID_ARG);
    expect_schema("{}", "{\"x\":1e999}", COG_ERROR_UNSUPPORTED);
    expect_schema("{}", "{\"x\":\"\\ud800\"}", COG_ERROR_INVALID_ARG);
    expect_schema("{}", "{\"x\":\"\xc0\xaf\"}", COG_ERROR_INVALID_ARG);
    expect_schema("{}", "{\"x\":1} trailing", COG_ERROR_INVALID_ARG);
    expect_schema("{}", "{\"x\":1,}", COG_ERROR_INVALID_ARG);
    expect_schema("{\"type\":\"object\",\"type\":\"object\"}", "{}", COG_ERROR_INVALID_ARG);
    expect_schema("{\"pattern\":\"x\"}", "{}", COG_ERROR_UNSUPPORTED);
    expect_schema("{\"properties\":{\"unused\":{\"$ref\":\"http://invalid\"}}}", "{}", COG_ERROR_UNSUPPORTED);
    expect_schema("{\"required\":[\"missing\"]}", "{}", COG_ERROR_INVALID_ARG);
    expect_schema("{\"required\":[\"x\",\"x\"]}", "{\"x\":0}", COG_ERROR_INVALID_ARG);
    expect_schema("{\"properties\":{\"i\":{\"type\":\"integer\"}}}", "{\"i\":1.0e2}", COG_OK);
    expect_schema("{\"properties\":{\"i\":{\"type\":\"integer\"}}}", "{\"i\":9223372036854775807}", COG_OK);
    expect_schema("{\"properties\":{\"i\":{\"type\":\"integer\"}}}", "{\"i\":-9223372036854775808}", COG_OK);
    expect_schema("{\"properties\":{\"i\":{\"type\":\"integer\"}}}", "{\"i\":9223372036854775808}", COG_ERROR_UNSUPPORTED);
    expect_schema("{\"properties\":{\"i\":{\"type\":\"integer\"}}}", "{\"i\":9007199254740993.1}", COG_ERROR_INVALID_ARG);
    expect_schema("{\"properties\":{\"i\":{\"type\":\"integer\"}}}", "{\"i\":1.000000000000000000001}", COG_ERROR_INVALID_ARG);
    expect_schema("{\"properties\":{\"i\":{\"type\":\"number\",\"minimum\":2,\"maximum\":4}}}", "{\"i\":3.5}", COG_OK);
    expect_schema("{\"properties\":{\"i\":{\"minimum\":2}}}", "{\"i\":1}", COG_ERROR_INVALID_ARG);
    expect_schema("{\"minimum\":9223372036854775807}", "{}", COG_ERROR_UNSUPPORTED);
    expect_schema("{\"additionalProperties\":false,\"properties\":{\"x\":{}}}", "{\"x\":null}", COG_OK);
    expect_schema("{\"additionalProperties\":false}", "{\"x\":null}", COG_ERROR_INVALID_ARG);
    expect_schema("{\"properties\":{\"s\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":1}}}", "{\"s\":\"\\ud83d\\ude00\"}", COG_OK);
    expect_schema("{\"properties\":{\"s\":{\"maxLength\":1}}}", "{\"s\":\"e\\u0301\"}", COG_ERROR_INVALID_ARG);
    expect_schema("{\"properties\":{\"a\":{\"type\":\"array\",\"minItems\":1,\"maxItems\":2,\"items\":{\"type\":\"boolean\"}}}}", "{\"a\":[true,false]}", COG_OK);
    expect_schema("{\"properties\":{\"a\":{\"items\":{\"type\":\"boolean\"}}}}", "{\"a\":[0]}", COG_ERROR_INVALID_ARG);
    expect_schema("{\"properties\":{\"e\":{\"enum\":[{\"x\":[1,true,null]}]}}}", "{\"e\":{\"x\":[1.0,true,null]}}", COG_OK);
    expect_schema("{\"properties\":{\"e\":{\"enum\":[1,2]}}}", "{\"e\":3}", COG_ERROR_INVALID_ARG);
    expect_schema("{\"title\":\"ok\",\"description\":\"ok\",\"$schema\":\"annotation\"}", "{}", COG_OK);
}
static void bounded_cases(void)
{
    counter_t counter = {0};
    cog_toolkit_execution_t *execution = NULL;
    cog_tool_async_config_t config = { .max_output_bytes = 64U, .timeout_ms = 100U,
        .max_argument_bytes = 2U };
    assert(construct(&execution, &counter, "{}", "{}", &config) == COG_ERROR_LIMIT);
    config.max_argument_bytes = 3U;
    assert(construct(&execution, &counter, "{}", "{}", &config) == COG_OK);
    assert(cog_toolkit_execution_free(execution) == COG_OK);
    config.max_schema_bytes = 2U;
    assert(construct(&execution, &counter, "{}", "{}", &config) == COG_ERROR_LIMIT);
    config.max_schema_bytes = 3U;
    config.codec_workspace_bytes = 8U;
    assert(construct(&execution, &counter, "{}", "{}", &config) == COG_ERROR_LIMIT);
    config.codec_workspace_bytes = 0U;
    config.max_argument_bytes = 1024U;
    config.max_schema_bytes = 1024U;
    config.max_json_depth = 1U;
    assert(construct(&execution, &counter, "{}", "{\"a\":[]}", &config) == COG_ERROR_LIMIT);
    config.max_json_depth = 2U;
    assert(construct(&execution, &counter, "{}", "{\"a\":[]}", &config) == COG_OK);
    assert(cog_toolkit_execution_free(execution) == COG_OK);
    assert(counter.effects == 0U);
}
static void policy_and_retention(void)
{
    counter_t counter = {0};
    cog_toolkit_execution_t *execution = NULL;
    assert(construct(&execution, &counter, "{}", "{}", NULL) == COG_OK);
    bool done = false;
    uint64_t deadline = 0U;
    assert(cog_toolkit_execution_poll(execution, 1U, &done, &deadline) == COG_ERROR_INVALID_STATE);
    assert(cog_toolkit_execution_start(execution, COG_TOOLKIT_POLICY_UNDECIDED, 1U) == COG_ERROR_AGAIN);
    assert(counter.effects == 0U);
    assert(cog_toolkit_execution_start(execution, COG_TOOLKIT_POLICY_DENY, 2U) == COG_OK);
    char output[64] = "preserve";
    cog_toolkit_result_t result = {0};
    size_t required = 0U;
    assert(cog_toolkit_execution_result(execution, &result, output, sizeof(output), &required) == COG_OK);
    assert(result.status == COG_ERROR_DENIED && !result.side_effects_possible);
    assert(counter.effects == 0U);
    assert(cog_toolkit_execution_free(execution) == COG_OK);
    assert(construct(&execution, &counter, "{}", "{}", NULL) == COG_OK);
    assert(cog_toolkit_execution_start(execution, COG_TOOLKIT_POLICY_ALLOW, 10U) == COG_OK);
    assert(cog_toolkit_execution_start(execution, COG_TOOLKIT_POLICY_ALLOW, 10U) == COG_ERROR_INVALID_STATE);
    assert(cog_toolkit_execution_free(execution) == COG_ERROR_INVALID_STATE);
    assert(cog_toolkit_execution_result(execution, &result, output, sizeof(output), &required) == COG_ERROR_AGAIN);
    assert(cog_toolkit_execution_poll(execution, 9U, &done, &deadline) == COG_ERROR_INVALID_ARG);
    assert(cog_toolkit_execution_poll(execution, 11U, &done, &deadline) == COG_OK && done);
    strcpy(output, "preserve");
    result.status = COG_ERROR_IO;
    assert(cog_toolkit_execution_result(execution, &result, output, 2U, &required) == COG_ERROR_OVERFLOW);
    assert(strcmp(output, "preserve") == 0 && result.status == COG_ERROR_IO && required == 11U && result.output_len == 10U);
    for (size_t i = 0U; i < 10U; i++) {
        assert(cog_toolkit_execution_result(execution, &result, output, sizeof(output), &required) == COG_OK);
        assert(result.status == COG_OK && strcmp(output, "one effect") == 0 && strcmp(result.tool_call_id, "call-1") == 0);
    }
    assert(counter.effects == 1U);
    assert(cog_toolkit_execution_cancel(execution, 12U) == COG_OK);
    assert(cog_toolkit_execution_free(execution) == COG_OK);
    assert(construct(&execution, &counter, "{}", "{}", NULL) == COG_OK);
    assert(cog_toolkit_execution_cancel(execution, 0U) == COG_OK);
    assert(cog_toolkit_execution_cancel(execution, 0U) == COG_OK);
    assert(cog_toolkit_execution_start(execution, COG_TOOLKIT_POLICY_ALLOW, 0U) == COG_ERROR_INVALID_STATE);
    assert(cog_toolkit_execution_free(execution) == COG_OK);
    assert(counter.effects == 1U);
}
static void invalid_vtable(void)
{
    counter_t counter = {0};
    cog_tool_async_config_t config = { .max_output_bytes = 64U, .timeout_ms = 100U, .user = &counter };
    for (size_t i = 0U; i < 6U; i++) {
        cog_tool_async_vtable_t ops = counter_ops;
        switch (i) {
        case 0U: ops.new_ = NULL; break;
        case 1U: ops.start = NULL; break;
        case 2U: ops.poll = NULL; break;
        case 3U: ops.cancel = NULL; break;
        case 4U: ops.result = NULL; break;
        default: ops.destroy = NULL; break;
        }
        const cog_tool_t tool = { .name = "counter", .async = &ops };
        const cog_tool_table_t table = { &tool, 1U };
        cog_toolkit_execution_t *execution = NULL;
        assert(cog_toolkit_execution_new(&execution, toolkit_test_core, &table, "id", "counter", "{}", 2U, &config) == COG_ERROR_INVALID_ARG);
        assert(execution == NULL && counter.constructed == 0U);
    }
}
static void reusable_validator(void)
{
    const cog_tool_async_config_t config = { .max_argument_bytes = 12U };
    const cog_tool_t descriptor = { .input_schema = "{\"required\":[\"x\"]}" };
    const cog_tool_t unsupported = { .input_schema = "{\"pattern\":\"x\"}" };
    cog_toolkit_validator_t *validator = NULL;
    assert(cog_toolkit_validator_new(&validator, &config) == COG_OK);
    for (size_t i = 0U; i < 64U; i++) {
        assert(cog_toolkit_validate_schema(validator, &descriptor) == COG_OK);
        assert(cog_toolkit_validate_schema(validator, &unsupported) == COG_ERROR_UNSUPPORTED);
        assert(cog_toolkit_validate(validator, &descriptor, "{}", 2U) == COG_ERROR_INVALID_ARG);
        assert(cog_toolkit_validate(validator, &descriptor, "{\"x\":123456}", 12U) == COG_ERROR_LIMIT);
        assert(cog_toolkit_validate(validator, &descriptor, "{\"x\":1}", 7U) == COG_OK);
    }
    assert(cog_toolkit_validator_free(validator) == COG_OK);
}

int main(void)
{
    reusable_validator();
    toolkit_core_create();
    schema_cases(); bounded_cases(); policy_and_retention(); invalid_vtable();
    toolkit_core_destroy();
    puts("test_toolkit_async: ok");
    return 0;
}

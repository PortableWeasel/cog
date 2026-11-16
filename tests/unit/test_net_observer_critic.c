#define _POSIX_C_SOURCE 200809L
#include "net_internal.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static cog_net_ctx_t *fault_context;
static volatile bool fail_timeout;
static volatile unsigned injected;
static cog_net_observation_t records[32];
static size_t record_count;
CURLMcode __real_curl_multi_timeout(CURLM *multi, long *timeout);
CURLMcode __wrap_curl_multi_timeout(CURLM *multi, long *timeout);
CURLMcode __wrap_curl_multi_timeout(CURLM *multi, long *timeout)
{
    if (fail_timeout && fault_context->requests[0].bytes_in != 0U) {
        ++injected;
        return CURLM_INTERNAL_ERROR;
    }
    return __real_curl_multi_timeout(multi, timeout);
}
static cog_result_t observe(void *user, const cog_net_ctx_t *ctx,
                             const cog_net_observation_t *event)
{
    assert(user == NULL && ctx == fault_context && record_count < 32U);
    records[record_count++] = *event;
    return COG_OK;
}
static uint64_t now_ms(void)
{
    struct timespec time;
    assert(clock_gettime(CLOCK_MONOTONIC, &time) == 0);
    return (uint64_t)time.tv_sec * 1000U + (uint64_t)time.tv_nsec / 1000000U;
}
int main(void)
{
    const char *url = getenv("COG_NET_TEST_URL");
    static const char body[] = "critic-effect";
    assert(url != NULL && curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK);
    cog_net_config_t config = {.max_connections = 1U,
        .host_runtime_initialized = true, .dependency_memory = COG_NET_DEPENDENCY_MEMORY_DYNAMIC,
        .ca_bundle = getenv("COG_NET_TEST_CA"), .observer = observe,
        .connect_timeout_ms = 1000U, .stall_timeout_ms = 1000U};
    assert(cog_net_new(&fault_context, &config) == COG_OK);
    cog_request_id_t id = 0U;
    assert(cog_net_request(fault_context, "POST", url, NULL, body, sizeof(body) - 1U, &id) == COG_OK);
    fail_timeout = true;
    cog_result_t result = COG_OK;
    for (size_t i = 0U; i < 5000U && result == COG_OK; ++i) {
        uint64_t deadline;
        result = cog_net_service(fault_context, now_ms(), &deadline);
        struct timespec pause = {0, 1000000L};
        (void)nanosleep(&pause, NULL);
    }
    assert(result == COG_ERROR_IO && injected == 1U);
    assert(cog_net_active_count(fault_context) == 1U);
    assert(cog_net_observer_status(fault_context) == COG_OK);
    uint64_t bytes_in = 0U, bytes_out = 0U;
    unsigned connected = 0U;
    for (size_t i = 0U; i < record_count; ++i) {
        assert(records[i].sequence == i + 1U);
        assert(records[i].kind != COG_NET_OBSERVATION_CLOSED && records[i].kind != COG_NET_OBSERVATION_FAILED);
        if (records[i].kind == COG_NET_OBSERVATION_CONNECTED) ++connected;
        bytes_in += records[i].bytes_in_delta;
        bytes_out += records[i].bytes_out_delta;
    }
    assert(connected == 1U && bytes_in == 5U && bytes_out == sizeof(body) - 1U);
    cog_net_response_t held;
    assert(cog_net_peek_request(fault_context, id, &held) == COG_OK);
    while (held.chunk_len == 0U) {
        assert(!held.is_complete);
        assert(cog_net_ack(fault_context, id, held.sequence) == COG_OK);
        assert(cog_net_peek_request(fault_context, id, &held) == COG_OK);
    }
    assert(held.chunk_len == 5U && memcmp(held.chunk, "hello", 5U) == 0 && !held.is_complete);
    fail_timeout = false;
    assert(cog_net_cancel(fault_context, id) == COG_OK);
    assert(memcmp(held.chunk, "hello", 5U) == 0);
    const cog_net_observation_t *terminal = &records[record_count - 1U];
    assert(terminal->kind == COG_NET_OBSERVATION_FAILED && terminal->outcome == COG_ERROR_ABORTED);
    assert(terminal->error_kind == COG_NET_ERROR_CANCELLED && terminal->http_status == 200);
    assert(terminal->bytes_in_total == bytes_in && terminal->bytes_out_total == bytes_out);
    size_t count = record_count;
    assert(cog_net_cancel(fault_context, id) == COG_OK && record_count == count);
    assert(cog_net_ack(fault_context, id, held.sequence) == COG_OK);
    assert(cog_net_peek_request(fault_context, id, &held) == COG_OK && held.is_complete);
    assert(held.outcome == COG_ERROR_ABORTED && cog_net_observer_status(fault_context) == COG_OK);
    assert(cog_net_request_release(fault_context, id) == COG_OK && record_count == count);
    cog_net_free(fault_context);
    assert(record_count == count);
    curl_global_cleanup();
    return 0;
}

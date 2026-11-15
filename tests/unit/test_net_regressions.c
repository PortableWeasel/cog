#define _POSIX_C_SOURCE 200809L
#include <cog/net.h>
#include <curl/curl.h>
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint64_t now_ms(void)
{
    struct timespec t;
    assert(clock_gettime(CLOCK_MONOTONIC, &t) == 0);
    return (uint64_t)t.tv_sec * 1000U + (uint64_t)t.tv_nsec / 1000000U;
}

static void drive(cog_net_ctx_t *ctx, cog_request_id_t id, bool defer_completion,
                  cog_result_t expected, bool cancel_done, int expected_status)
{
    unsigned int i;
    size_t bytes = 0U;
    bool complete = false, cancelled_done = false;
    uint64_t shift = 0U, next;
    for (i = 0U; i < 5000U && !complete; ++i) {
        cog_net_response_t view;
        struct timespec delay = {0, 1000000L};
        assert(cog_net_service(ctx, now_ms() + shift, &next) == COG_OK);
        if (cancel_done && !cancelled_done) {
            bool active = true;
            assert(cog_net_request_status(ctx, id, &active) == COG_OK);
            if (!active) {
                assert(cog_net_cancel(ctx, id) == COG_OK);
                cancelled_done = true;
                shift = 1000U;
            }
        }
        while (cog_net_peek_request(ctx, id, &view) == COG_OK) {
            bytes += view.chunk_len;
            /* The body has arrived in full. Scheduling delay must not turn
             * the dependency's already queued completion into inactivity. */
            if (defer_completion && view.chunk_len != 0U) {
                shift = 1000U;
            }
            if (view.is_complete) {
                assert(view.outcome == expected);
                if (expected_status != 0) assert(view.http_status == expected_status);
                complete = true;
            }
            assert(cog_net_ack(ctx, id, view.sequence) == COG_OK);
        }
        (void)nanosleep(&delay, NULL);
    }
    assert(complete);
    if (cancel_done) assert(cancelled_done);
    if (defer_completion) assert(bytes == 5U);
    assert(cog_net_request_release(ctx, id) == COG_OK);
}

int main(int argc, char **argv)
{
    cog_net_config_t config = {0};
    cog_net_ctx_t *ctx = NULL;
    cog_request_id_t id;
    bool completion, cancel_done, error_cancel, response_only;
    int expected_status = 0;
    char challenge_url[1024];
    const char *url = getenv("COG_NET_TEST_URL");
    assert(argc == 2 && url != NULL);
    completion = strcmp(argv[1], "completion") == 0 || strcmp(argv[1], "completion-cancel") == 0;
    error_cancel = strcmp(argv[1], "error-cancel") == 0;
    cancel_done = strcmp(argv[1], "completion-cancel") == 0 || error_cancel;
    response_only = strcmp(argv[1], "auth") == 0 || strcmp(argv[1], "redirect") == 0 || strcmp(argv[1], "proxy") == 0;
    assert(completion || error_cancel || response_only || strcmp(argv[1], "replay") == 0);
    assert(curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK);
    config.host_runtime_initialized = true;
    config.dependency_memory = COG_NET_DEPENDENCY_MEMORY_DYNAMIC;
    config.ca_bundle = getenv("COG_NET_TEST_CA");
    config.max_connections = 1U;
    config.connect_timeout_ms = 1000U;
    config.stall_timeout_ms = 100U;
    config.service_quantum = (completion || error_cancel) ? 1U : 16U;
    if (error_cancel) config.max_header_bytes = 8U;
    assert(cog_net_new(&ctx, &config) == COG_OK);
    if (response_only) {
        assert(snprintf(challenge_url, sizeof(challenge_url), "%s%s", url, argv[1]) > 0);
        expected_status = strcmp(argv[1], "auth") == 0 ? 401 : (strcmp(argv[1], "proxy") == 0 ? 407 : 307);
        assert(cog_net_request(ctx, "POST", challenge_url, "Authorization: Bearer test", "effect", 6U, &id) == COG_OK);
        drive(ctx, id, false, COG_OK, false, expected_status);
    } else {
        assert(cog_net_request(ctx, "GET", url, NULL, NULL, 0U, &id) == COG_OK);
        drive(ctx, id, completion, error_cancel ? COG_ERROR_LIMIT : COG_OK, cancel_done, 0);
    }
    if (!completion && !error_cancel && !response_only) {
        /* Server consumes this request then closes before any response. */
        assert(cog_net_request(ctx, "POST", url, NULL, "effect", 6U, &id) == COG_OK);
        drive(ctx, id, false, COG_ERROR_IO, false, 0);
    }
    cog_net_free(ctx);
    curl_global_cleanup();
    return 0;
}

#define _POSIX_C_SOURCE 200809L
#include <cog/net.h>
#include <curl/curl.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef COG_NET_TEST_PROVEN
static uint64_t now_ms(void)
{
    struct timespec t;
    assert(clock_gettime(CLOCK_MONOTONIC, &t) == 0);
    return (uint64_t)t.tv_sec * 1000U + (uint64_t)t.tv_nsec / 1000000U;
}
#endif
int main(void)
{
    cog_net_ctx_t *ctx = NULL;
    cog_net_config_t config = {0};
#ifdef COG_NET_TEST_PROVEN
    cog_net_response_t v, again;
    cog_request_id_t a = 99U, b = 99U;
    uint64_t next;
    bool active;
    const char *url = getenv("COG_NET_TEST_URL");
#endif
    const char *ca = getenv("COG_NET_TEST_CA");
    cog_result_t rc;
    assert(curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK);
    config.host_runtime_initialized = true;
    config.dependency_memory = COG_NET_DEPENDENCY_MEMORY_DYNAMIC;
    config.max_connections = 2U;
    config.ca_bundle = ca;
    rc = cog_net_new(&ctx, &config);
    if (getenv("COG_NET_TEST_EXPECT_UNSUPPORTED") != NULL) {
        assert(rc == COG_ERROR_UNSUPPORTED && ctx == NULL);
        curl_global_cleanup();
        return 0;
    }
#ifndef COG_NET_TEST_PROVEN
    assert(rc == COG_ERROR_UNSUPPORTED && ctx == NULL);
    curl_global_cleanup();
    return 0;
#else
    assert(rc == COG_OK && ctx != NULL);
    assert(cog_net_request(ctx, "GET", "http://127.0.0.1/", NULL, NULL, 0U, &a) == COG_ERROR_INVALID_ARG);
    assert(a == 99U);
    assert(cog_net_request(ctx, "GET", "https://user@localhost/", NULL, NULL, 0U, &a) == COG_ERROR_INVALID_ARG);
    assert(cog_net_request(ctx, "GET", "https://localhost/", "X: a\r\nInjected: b", NULL, 0U, &a) == COG_ERROR_INVALID_ARG);
    assert(cog_net_request(ctx, "GET", "https://localhost/", NULL, NULL, 0U, &a) == COG_OK);
    assert(cog_net_request(ctx, "GET", "https://localhost/", NULL, NULL, 0U, &b) == COG_OK);
    assert(cog_net_active_count(ctx) == 2U);
    assert(cog_net_cancel(ctx, a) == COG_OK);
    assert(cog_net_cancel(ctx, a) == COG_OK);
    assert(cog_net_peek_request(ctx, a, &v) == COG_OK);
    assert(v.is_complete && v.outcome == COG_ERROR_ABORTED && v.sequence == 1U);
    assert(cog_net_peek(ctx, &again) == COG_OK && again.request_id == a);
    assert(cog_net_ack(ctx, a, 2U) == COG_ERROR_INVALID_STATE);
    assert(cog_net_ack(ctx, a, 1U) == COG_OK);
    assert(cog_net_request_status(ctx, a, &active) == COG_OK && !active);
    assert(cog_net_request_release(ctx, a) == COG_OK);
    assert(cog_net_request_release(ctx, b) == COG_OK);
    if (url != NULL) {
        unsigned int i;
        size_t bytes = 0U;
        bool complete = false, saw_headers = false;
        assert(cog_net_request(ctx, "GET", url, NULL, NULL, 0U, &a) == COG_OK);
        for (i = 0U; i < 5000U && !complete; ++i) {
            struct timespec delay = {0, 1000000L};
            assert(cog_net_service(ctx, now_ms(), &next) == COG_OK);
            while (cog_net_peek_request(ctx, a, &v) == COG_OK) {
                assert(v.http_status == 200);
                if (v.headers != NULL) saw_headers = true;
                bytes += v.chunk_len;
                if (v.is_complete) { assert(v.outcome == COG_OK); complete = true; }
                assert(cog_net_ack(ctx, a, v.sequence) == COG_OK);
            }
            (void)nanosleep(&delay, NULL);
        }
        assert(complete && saw_headers && bytes == 5U);
        assert(cog_net_request_release(ctx, a) == COG_OK);
        {
            cog_net_ctx_t *small = NULL;
            cog_net_config_t limited = config;
            bool limit_seen = false;
            limited.chunk_bytes = 1U;
            limited.chunks_per_request = 2U;
            assert(cog_net_new(&small, &limited) == COG_OK);
            assert(cog_net_request(small, "GET", url, NULL, NULL, 0U, &b) == COG_OK);
            for (i = 0U; i < 5000U && !limit_seen; ++i) {
                struct timespec delay = {0, 1000000L};
                assert(cog_net_service(small, now_ms(), &next) == COG_OK);
                while (cog_net_peek_request(small, b, &v) == COG_OK) {
                    if (v.is_complete) {
                        assert(v.outcome == COG_ERROR_LIMIT && v.error_kind == COG_NET_ERROR_CAPACITY);
                        limit_seen = true;
                    }
                    assert(cog_net_ack(small, b, v.sequence) == COG_OK);
                }
                (void)nanosleep(&delay, NULL);
            }
            assert(limit_seen);
            assert(cog_net_request_release(small, b) == COG_OK);
            cog_net_free(small);
        }
        {
            cog_net_ctx_t *shared = NULL;
            cog_net_config_t bounded = config;
            char large_url[1024];
            cog_net_response_t saved;
            bool ready = false, other_done = false;
            bounded.chunks_per_request = 1U;
            assert(snprintf(large_url, sizeof(large_url), "%slarge", url) > 0);
            assert(cog_net_new(&shared, &bounded) == COG_OK);
            assert(cog_net_request(shared, "GET", large_url, NULL, NULL, 0U, &a) == COG_OK);
            for (i = 0U; i < 5000U && !ready; ++i) {
                struct timespec delay = {0, 1000000L};
                assert(cog_net_service(shared, now_ms(), &next) == COG_OK);
                if (cog_net_peek(shared, &saved) == COG_OK) ready = true;
                (void)nanosleep(&delay, NULL);
            }
            assert(ready && saved.request_id == a && !saved.is_complete && saved.headers != NULL);
            assert(cog_net_request(shared, "GET", url, NULL, NULL, 0U, &b) == COG_OK);
            for (i = 0U; i < 5000U && !other_done; ++i) {
                struct timespec delay = {0, 1000000L};
                assert(cog_net_service(shared, now_ms(), &next) == COG_OK);
                assert(cog_net_peek(shared, &again) == COG_OK);
                assert(again.request_id == a && again.sequence == saved.sequence && again.headers == saved.headers);
                while (cog_net_peek_request(shared, b, &v) == COG_OK) {
                    if (v.is_complete) { assert(v.outcome == COG_OK); other_done = true; }
                    assert(cog_net_ack(shared, b, v.sequence) == COG_OK);
                }
                (void)nanosleep(&delay, NULL);
            }
            assert(other_done);
            assert(cog_net_cancel(shared, a) == COG_OK);
            assert(cog_net_peek_request(shared, a, &again) == COG_OK && again.headers == saved.headers);
            assert(cog_net_ack(shared, a, saved.sequence) == COG_OK);
            assert(cog_net_peek_request(shared, a, &v) == COG_OK && v.is_complete && v.outcome == COG_ERROR_ABORTED);
            assert(cog_net_ack(shared, a, v.sequence) == COG_OK);
            assert(cog_net_request_release(shared, a) == COG_OK);
            assert(cog_net_request_release(shared, b) == COG_OK);
            cog_net_free(shared);
        }
        {
            char rate_url[1024];
            bool done = false, saw_retry = false;
            assert(snprintf(rate_url, sizeof(rate_url), "%srate", url) > 0);
            assert(cog_net_request(ctx, "GET", rate_url, NULL, NULL, 0U, &a) == COG_OK);
            for (i = 0U; i < 5000U && !done; ++i) {
                struct timespec delay = {0, 1000000L};
                assert(cog_net_service(ctx, now_ms(), &next) == COG_OK);
                while (cog_net_peek_request(ctx, a, &v) == COG_OK) {
                    assert(v.http_status == 429);
                    if (v.retry_after_known) { assert(v.retry_after_ms == 7000U); saw_retry = true; }
                    if (v.is_complete) { assert(v.outcome == COG_OK); done = true; }
                    assert(cog_net_ack(ctx, a, v.sequence) == COG_OK);
                }
                (void)nanosleep(&delay, NULL);
            }
            assert(done && saw_retry);
            assert(cog_net_request_release(ctx, a) == COG_OK);
        }
        {
            cog_net_ctx_t *untrusted = NULL;
            cog_net_config_t invalid_ca = config;
            bool failed = false;
            invalid_ca.ca_bundle = NULL;
            assert(cog_net_new(&untrusted, &invalid_ca) == COG_OK);
            assert(cog_net_request(untrusted, "GET", url, NULL, NULL, 0U, &b) == COG_OK);
            for (i = 0U; i < 5000U && !failed; ++i) {
                struct timespec delay = {0, 1000000L};
                assert(cog_net_service(untrusted, now_ms(), &next) == COG_OK);
                if (cog_net_peek_request(untrusted, b, &v) == COG_OK) {
                    assert(v.is_complete && v.outcome == COG_ERROR_IO && v.error_kind == COG_NET_ERROR_TLS);
                    assert(cog_net_ack(untrusted, b, v.sequence) == COG_OK);
                    failed = true;
                }
                (void)nanosleep(&delay, NULL);
            }
            assert(failed);
            assert(cog_net_request_release(untrusted, b) == COG_OK);
            cog_net_free(untrusted);
        }
    }
    cog_net_free(ctx);
    curl_global_cleanup();
    puts("net contract checks passed");
    return 0;
#endif
}

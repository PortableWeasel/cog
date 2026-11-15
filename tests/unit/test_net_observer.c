#define _POSIX_C_SOURCE 200809L
#include "net_internal.h"
#include <assert.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define RECORDS 1024U
typedef struct observer_log {
    cog_net_observation_t records[RECORDS];
    const cog_net_ctx_t *contexts[RECORDS];
    size_t count;
    int fail_kind;
    cog_result_t failure;
    const cog_request_id_t *published;
} observer_log_t;
static volatile size_t allocations, fail_at;
static bool forbid_alloc;
static int upload_fault;
void *__real_malloc(size_t n);
void *__real_calloc(size_t n, size_t size);
void *__real_realloc(void *p, size_t n);
void *__wrap_malloc(size_t n);
void *__wrap_calloc(size_t n, size_t size);
void *__wrap_realloc(void *p, size_t n);
void *__wrap_malloc(size_t n)
{
    assert(!forbid_alloc); ++allocations;
    return allocations == fail_at ? NULL : __real_malloc(n);
}
void *__wrap_calloc(size_t n, size_t size)
{
    assert(!forbid_alloc); ++allocations;
    return allocations == fail_at ? NULL : __real_calloc(n, size);
}
void *__wrap_realloc(void *p, size_t n)
{
    assert(!forbid_alloc); ++allocations;
    return allocations == fail_at ? NULL : __real_realloc(p, n);
}
CURLcode __real_curl_easy_getinfo(CURL *easy, CURLINFO info, ...);
CURLcode __wrap_curl_easy_getinfo(CURL *easy, CURLINFO info, ...);
CURLcode __wrap_curl_easy_getinfo(CURL *easy, CURLINFO info, ...)
{
    va_list ap; va_start(ap, info);
    curl_off_t *out = va_arg(ap, curl_off_t *); va_end(ap);
    assert(info == CURLINFO_SIZE_UPLOAD_T);
    if (upload_fault == 1) return CURLE_UNKNOWN_OPTION;
    if (upload_fault == 2) { *out = -1; return CURLE_OK; }
    if (upload_fault == 3) { *out = 0; return CURLE_OK; }
    return __real_curl_easy_getinfo(easy, info, out);
}
static cog_result_t observer(void *user, const cog_net_ctx_t *ctx,
                             const cog_net_observation_t *value)
{
    observer_log_t *log = user;
    assert(ctx != NULL && ctx->observing && value->request_id != 0U && log->count < RECORDS);
    if (value->kind == COG_NET_OBSERVATION_OPEN && log->published != NULL)
        assert(*log->published == value->request_id); /* Publication precedes OPEN. */
    log->contexts[log->count] = ctx;
    log->records[log->count++] = *value;
    return log->fail_kind == (int)value->kind ? log->failure : COG_OK;
}
static cog_net_config_t settings(observer_log_t *log)
{
    cog_net_config_t config = {.max_connections = 2U,
        .host_runtime_initialized = true, .dependency_memory = COG_NET_DEPENDENCY_MEMORY_DYNAMIC,
        .ca_bundle = getenv("COG_NET_TEST_CA"), .connect_timeout_ms = 500U,
        .stall_timeout_ms = 100U, .observer = observer, .observer_user = log};
    return config;
}
static uint64_t now_ms(void)
{
    struct timespec t; assert(clock_gettime(CLOCK_MONOTONIC, &t) == 0);
    return (uint64_t)t.tv_sec * 1000U + (uint64_t)t.tv_nsec / 1000000U;
}
static void pause_briefly(void)
{
    struct timespec t = {0, 1000000L}; (void)nanosleep(&t, NULL);
}
static void service(cog_net_ctx_t *ctx, observer_log_t *log, uint64_t now)
{
    size_t before = log->count, old_allocations = allocations;
    uint64_t next;
    forbid_alloc = true;
    assert(cog_net_service(ctx, now, &next) == COG_OK);
    forbid_alloc = false;
    assert(allocations == old_allocations);
    for (size_t i = before; i < log->count; ++i) {
        size_t kinds[6] = {0};
        for (size_t j = before; j < log->count; ++j)
            if (log->records[j].request_id == log->records[i].request_id)
                ++kinds[log->records[j].kind];
        for (size_t k = 0U; k < 6U; ++k) assert(kinds[k] <= 1U);
        assert(kinds[COG_NET_OBSERVATION_OPEN] == 0U);
        assert(kinds[COG_NET_OBSERVATION_CLOSED] + kinds[COG_NET_OBSERVATION_FAILED] <= 1U);
    }
}
static cog_request_id_t submit(cog_net_ctx_t *ctx, observer_log_t *log,
                               const char *path, const char *body, size_t length)
{
    char url[2048];
    const char *base = getenv("COG_NET_TEST_URL"); assert(base != NULL);
    assert(strlen(base) + strlen(path) < sizeof(url));
    strcpy(url, base); strcat(url, path);
    cog_request_id_t id = 99U;
    log->published = &id;
    size_t old = allocations;
    forbid_alloc = true;
    assert(cog_net_request(ctx, body != NULL ? "POST" : "GET", url,
                          "X-Secret: synthetic-observer-secret", body, length, &id) == COG_OK);
    forbid_alloc = false;
    assert(allocations == old);
    log->published = NULL;
    return id;
}
static size_t consume(cog_net_ctx_t *ctx, cog_request_id_t id,
                       bool *done, cog_result_t expected, int status)
{
    cog_net_response_t view; size_t bytes = 0U;
    while (cog_net_peek_request(ctx, id, &view) == COG_OK) {
        bytes += view.chunk_len;
        if (view.is_complete) {
            assert(view.outcome == expected);
            if (status != 0) assert(view.http_status == status);
            *done = true;
        }
        assert(cog_net_ack(ctx, id, view.sequence) == COG_OK);
    }
    return bytes;
}
static size_t drive(cog_net_ctx_t *ctx, observer_log_t *log, cog_request_id_t id,
                     cog_result_t expected, int status, bool stall)
{
    bool done = false; size_t bytes = 0U; uint64_t shift = 0U;
    for (size_t i = 0U; i < 5000U && !done; ++i) {
        service(ctx, log, now_ms() + shift);
        bytes += consume(ctx, id, &done, expected, status);
        if (stall && bytes != 0U) shift = 1000U;
        pause_briefly();
    }
    assert(done); return bytes;
}
static void verify(observer_log_t *log, const cog_net_ctx_t *ctx,
                    cog_request_id_t id, uint64_t in, uint64_t out,
                    cog_result_t outcome, int status, bool connected, bool stalled)
{
    uint64_t sequence = 0U, sum_in = 0U, sum_out = 0U;
    size_t opens = 0U, connections = 0U, stalls = 0U, terminals = 0U;
    for (size_t i = 0U; i < log->count; ++i) {
        const cog_net_observation_t *v = &log->records[i];
        if (v->request_id != id || log->contexts[i] != ctx) continue;
        assert(terminals == 0U && v->sequence == ++sequence);
        if (v->kind == COG_NET_OBSERVATION_OPEN) {
            ++opens; assert(sequence == 1U && v->bytes_in_total == 0U && v->bytes_out_total == 0U);
        } else assert(opens == 1U);
        if (v->kind == COG_NET_OBSERVATION_CONNECTED) ++connections;
        if (v->kind == COG_NET_OBSERVATION_STALLED) ++stalls;
        if (v->kind == COG_NET_OBSERVATION_PROGRESS) {
            assert(v->bytes_in_delta != 0U || v->bytes_out_delta != 0U);
            sum_in += v->bytes_in_delta; sum_out += v->bytes_out_delta;
        } else assert(v->bytes_in_delta == 0U && v->bytes_out_delta == 0U);
        if (v->kind == COG_NET_OBSERVATION_CLOSED || v->kind == COG_NET_OBSERVATION_FAILED) {
            ++terminals; assert(v->outcome == outcome && v->http_status == status);
            if (outcome == COG_OK) assert(v->error_kind == COG_NET_ERROR_NONE);
            if (outcome == COG_ERROR_ABORTED) assert(v->error_kind == COG_NET_ERROR_CANCELLED);
            if (outcome == COG_ERROR_TIMEOUT) assert(v->error_kind == COG_NET_ERROR_TIMEOUT);
            assert(v->kind == (outcome == COG_OK ? COG_NET_OBSERVATION_CLOSED : COG_NET_OBSERVATION_FAILED));
            assert(v->bytes_in_total == in && v->bytes_out_total == out);
            assert(v->bytes_in_total == sum_in && v->bytes_out_total == sum_out);
        } else assert(v->outcome == COG_OK && v->error_kind == COG_NET_ERROR_NONE);
    }
    assert(opens == 1U && terminals == 1U && connections == (connected ? 1U : 0U));
    assert(stalls == (stalled ? 1U : 0U));
    /* The copied callback value contains no URL/header/body/credential strings. */
    static const char secret[] = "synthetic-observer-secret";
    const unsigned char *raw = (const unsigned char *)log->records;
    for (size_t i = 0U; i + sizeof(secret) <= log->count * sizeof(log->records[0]); ++i)
        assert(memcmp(raw + i, secret, sizeof(secret) - 1U) != 0);
}
static void allocation_control(void)
{
    size_t old = allocations;
    unsigned char *p = malloc(8U); assert(p != NULL && allocations == old + 1U);
    p[0] = 7U; old = allocations;
    unsigned char *q = realloc(p, 16U); assert(q != NULL && allocations == old + 1U && q[0] == 7U); free(q);
    old = allocations; p = calloc(8U, 1U); assert(p != NULL && allocations == old + 1U && p[0] == 0U); free(p);
}
static void admission_and_errors(void)
{
    observer_log_t log = {.fail_kind = -1}; cog_net_ctx_t *ctx = NULL;
    cog_net_config_t config = settings(&log);
    size_t old = allocations;
    assert(cog_net_new(&ctx, &config) == COG_OK && log.count == 0U);
    size_t constructor_calls = allocations - old;
    assert(cog_net_observer_status(NULL) == COG_ERROR_NULL_PTR);
    assert(cog_net_observer_status(ctx) == COG_OK);
    cog_request_id_t id = 99U;
    assert(cog_net_request(ctx, "GET", "http://invalid/", NULL, NULL, 0U, &id) == COG_ERROR_INVALID_ARG);
    assert(id == 99U && log.count == 0U);
    id = submit(ctx, &log, "", NULL, 0U);
    cog_request_id_t second = submit(ctx, &log, "", NULL, 0U), absent = 99U;
    assert(cog_net_request(ctx, "GET", "https://localhost/", NULL, NULL, 0U, &absent) == COG_ERROR_OVERFLOW);
    assert(absent == 99U && log.count == 2U);
    assert(cog_net_cancel(ctx, id) == COG_OK && cog_net_cancel(ctx, id) == COG_OK);
    verify(&log, ctx, id, 0U, 0U, COG_ERROR_ABORTED, 0, false, false);
    assert(cog_net_request_release(ctx, second) == COG_OK);
    verify(&log, ctx, second, 0U, 0U, COG_ERROR_ABORTED, 0, false, false);
    size_t count = log.count;
    assert(cog_net_request_release(ctx, id) == COG_OK && log.count == count);
    id = submit(ctx, &log, "", NULL, 0U); count = log.count;
    cog_net_free(ctx); assert(log.count == count); /* Free ends the domain silently. */
    for (size_t i = 1U; i <= constructor_calls; ++i) {
        log.count = 0U; ctx = NULL; fail_at = allocations + i;
        assert(cog_net_new(&ctx, &config) == COG_ERROR_NOMEM && ctx == NULL && log.count == 0U);
        fail_at = 0U;
    }
    log = (observer_log_t){.fail_kind = COG_NET_OBSERVATION_OPEN, .failure = COG_ERROR_AGAIN};
    assert(cog_net_new(&ctx, &config) == COG_OK);
    id = submit(ctx, &log, "", NULL, 0U);
    assert(log.count == 1U && cog_net_observer_status(ctx) == COG_ERROR_AGAIN);
    log.failure = COG_ERROR_IO;
    assert(cog_net_cancel(ctx, id) == COG_OK && cog_net_observer_status(ctx) == COG_ERROR_AGAIN);
    assert(log.count == 1U); cog_net_free(ctx);
    log = (observer_log_t){.fail_kind = COG_NET_OBSERVATION_OPEN, .failure = COG_ERROR_NOMEM};
    assert(cog_net_new(&ctx, &config) == COG_OK);
    id = submit(ctx, &log, "", NULL, 0U);
    assert(cog_net_observer_status(ctx) == COG_ERROR_NOMEM && cog_net_active_count(ctx) == 1U);
    assert(cog_net_cancel(ctx, id) == COG_OK && log.count == 1U);
    cog_net_free(ctx);
    config.observer = NULL; config.observer_user = (void *)(uintptr_t)1U;
    assert(cog_net_new(&ctx, &config) == COG_OK);
    assert(cog_net_observer_status(ctx) == COG_OK); cog_net_free(ctx);
}
static void real_transfers(void)
{
    observer_log_t log = {.fail_kind = -1}; cog_net_ctx_t *ctx = NULL;
    cog_net_config_t config = settings(&log); assert(cog_net_new(&ctx, &config) == COG_OK);
    static const char body[] = "synthetic-observer-secret";
    cog_request_id_t id = submit(ctx, &log, "post?query=synthetic-observer-secret", body, sizeof(body) - 1U);
    assert(log.records[0].bytes_out_total == 0U);
    assert(drive(ctx, &log, id, COG_OK, 200, false) == 5U);
    verify(&log, ctx, id, 5U, sizeof(body) - 1U, COG_OK, 200, true, false);
    size_t old = log.count; assert(cog_net_cancel(ctx, id) == COG_OK && log.count == old);
    assert(cog_net_request_release(ctx, id) == COG_OK && log.count == old);
    id = submit(ctx, &log, "rate", NULL, 0U);
    assert(drive(ctx, &log, id, COG_OK, 429, false) == 5U);
    verify(&log, ctx, id, 5U, 0U, COG_OK, 429, true, false);
    assert(cog_net_request_release(ctx, id) == COG_OK);
    id = submit(ctx, &log, "stall", NULL, 0U);
    assert(drive(ctx, &log, id, COG_ERROR_TIMEOUT, 200, true) == 5U);
    verify(&log, ctx, id, 5U, 0U, COG_ERROR_TIMEOUT, 200, true, true);
    assert(cog_net_request_release(ctx, id) == COG_OK);
    assert(cog_net_observer_status(ctx) == COG_OK); cog_net_free(ctx);
    /* Observer failure cannot change an actual POST outcome or replay it. */
    for (int kind = COG_NET_OBSERVATION_OPEN; kind <= COG_NET_OBSERVATION_CLOSED; ++kind) {
        if (kind == COG_NET_OBSERVATION_STALLED) continue;
        log = (observer_log_t){.fail_kind = kind, .failure = COG_ERROR_OVERFLOW};
        config = settings(&log); assert(cog_net_new(&ctx, &config) == COG_OK);
        id = submit(ctx, &log, "post", body, sizeof(body) - 1U);
        assert(drive(ctx, &log, id, COG_OK, 200, false) == 5U);
        assert(cog_net_observer_status(ctx) == COG_ERROR_OVERFLOW);
        old = log.count; assert(log.records[old - 1U].kind == (cog_net_observation_kind_t)kind);
        assert(cog_net_request_release(ctx, id) == COG_OK && log.count == old);
        cog_net_free(ctx);
    }
}
static void pause_and_pending(void)
{
    observer_log_t log = {.fail_kind = -1}; cog_net_ctx_t *ctx = NULL;
    cog_net_config_t config = settings(&log); config.chunks_per_request = 1U;
    config.service_quantum = 1U;
    assert(cog_net_new(&ctx, &config) == COG_OK);
    cog_request_id_t id = submit(ctx, &log, "large", NULL, 0U);
    bool held = false, done = false; uint64_t shift = 0U; size_t bytes = 0U;
    for (size_t i = 0U; i < 5000U && !done; ++i) {
        service(ctx, &log, now_ms() + shift);
        if (!held && ctx->requests[0].paused) {
            held = true; shift = 1000U;
            service(ctx, &log, now_ms() + shift); /* Backpressure is not STALLED. */
        }
        bytes += consume(ctx, id, &done, COG_OK, 200); pause_briefly();
    }
    assert(done && held && bytes == 65536U);
    verify(&log, ctx, id, 65536U, 0U, COG_OK, 200, true, false);
    assert(cog_net_request_release(ctx, id) == COG_OK); cog_net_free(ctx);
    log = (observer_log_t){.fail_kind = -1}; config = settings(&log); config.service_quantum = 1U;
    assert(cog_net_new(&ctx, &config) == COG_OK);
    id = submit(ctx, &log, "", NULL, 0U);
    bool captured = false; bytes = 0U;
    for (size_t i = 0U; i < 5000U && !captured; ++i) {
        service(ctx, &log, now_ms());
        if (ctx->requests[0].pending_terminal) captured = true;
        if (!captured) { bool ignored = false; bytes += consume(ctx, id, &ignored, COG_OK, 200); }
        pause_briefly();
    }
    assert(captured && ctx->requests[0].wire_done && ctx->requests[0].observation_done);
    size_t old = log.count;
    assert(cog_net_cancel(ctx, id) == COG_OK && log.count == old);
    done = false; bytes += consume(ctx, id, &done, COG_OK, 200); assert(done && bytes == 5U);
    verify(&log, ctx, id, 5U, 0U, COG_OK, 200, true, false);
    assert(cog_net_request_release(ctx, id) == COG_OK && log.count == old); cog_net_free(ctx);
}
static void accounting_boundaries(void)
{
    for (int fault = 1; fault <= 3; ++fault) {
        observer_log_t log = {.fail_kind = -1}; cog_net_ctx_t *ctx = NULL;
        cog_net_config_t config = settings(&log); assert(cog_net_new(&ctx, &config) == COG_OK);
        cog_request_id_t id = submit(ctx, &log, "", NULL, 0U);
        if (fault == 3) ctx->requests[0].bytes_out = 1U;
        upload_fault = fault;
        assert(cog_net_cancel(ctx, id) == COG_OK && cog_net_observer_status(ctx) == COG_ERROR_IO);
        upload_fault = 0;
        bool done = false; assert(consume(ctx, id, &done, COG_ERROR_ABORTED, 0) == 0U && done);
        assert(log.count == 1U); cog_net_free(ctx);
    }
    observer_log_t log = {.fail_kind = -1}; cog_net_ctx_t *ctx = NULL;
    cog_net_config_t config = settings(&log); assert(cog_net_new(&ctx, &config) == COG_OK);
    cog_request_id_t id = submit(ctx, &log, "", NULL, 0U);
    ctx->requests[0].observation_sequence = UINT64_MAX;
    assert(cog_net_cancel(ctx, id) == COG_OK && cog_net_observer_status(ctx) == COG_ERROR_LIMIT);
    assert(log.count == 1U); cog_net_free(ctx);
    log = (observer_log_t){.fail_kind = -1}; config = settings(&log); config.chunks_per_request = 1U;
    assert(cog_net_new(&ctx, &config) == COG_OK);
    id = submit(ctx, &log, "", NULL, 0U);
    bool paused = false;
    for (size_t i = 0U; i < 5000U && !paused; ++i) {
        service(ctx, &log, now_ms()); paused = ctx->requests[0].paused; pause_briefly();
    }
    assert(paused);
    ctx->requests[0].bytes_in = ctx->requests[0].reported_in = UINT64_MAX - 1U;
    bool done = false; size_t bytes = consume(ctx, id, &done, COG_OK, 200);
    bytes += drive(ctx, &log, id, COG_OK, 200, false);
    assert(bytes == 5U && cog_net_observer_status(ctx) == COG_ERROR_LIMIT);
    cog_net_free(ctx);
}
static size_t null_user_calls;
static cog_result_t null_user(void *user, const cog_net_ctx_t *ctx,
                              const cog_net_observation_t *value)
{
    assert(user == NULL && ctx != NULL && value->request_id != 0U);
    ++null_user_calls; return COG_OK;
}
static void identity_lifetime_and_shared_failure(void)
{
    observer_log_t log = {.fail_kind = -1}; cog_net_ctx_t *a = NULL, *b = NULL;
    cog_net_config_t config = settings(&log);
    assert(cog_net_new(&a, &config) == COG_OK && cog_net_new(&b, &config) == COG_OK);
    cog_request_id_t x = submit(a, &log, "", NULL, 0U), y = submit(b, &log, "", NULL, 0U);
    assert(x == 1U && y == 1U && a != b);
    assert(cog_net_cancel(a, x) == COG_OK && cog_net_request_release(b, y) == COG_OK);
    verify(&log, a, x, 0U, 0U, COG_ERROR_ABORTED, 0, false, false);
    verify(&log, b, y, 0U, 0U, COG_ERROR_ABORTED, 0, false, false);
    cog_net_free(a); cog_net_free(b);
    config.observer = null_user; config.observer_user = NULL;
    size_t old = null_user_calls;
    assert(cog_net_new(&a, &config) == COG_OK && null_user_calls == old);
    x = submit(a, &log, "", NULL, 0U); assert(null_user_calls == old + 1U);
    assert(cog_net_cancel(a, x) == COG_OK && null_user_calls == old + 2U);
    cog_net_free(a); assert(null_user_calls == old + 2U);

    log = (observer_log_t){.fail_kind = COG_NET_OBSERVATION_PROGRESS, .failure = COG_ERROR_DENIED};
    config = settings(&log); config.chunks_per_request = 1U; config.service_quantum = 1U;
    assert(cog_net_new(&a, &config) == COG_OK);
    x = submit(a, &log, "large", NULL, 0U);
    bool paused = false;
    for (size_t i = 0U; i < 5000U && !paused; ++i) {
        service(a, &log, now_ms());
        paused = a->requests[0].paused && a->requests[0].count != 0U;
        pause_briefly();
    }
    assert(paused);
    cog_net_response_t saved, again;
    assert(cog_net_peek(a, &saved) == COG_OK && saved.request_id == x);
    static const char body[] = "synthetic-observer-secret";
    y = submit(a, &log, "post", body, sizeof(body) - 1U);
    assert(drive(a, &log, y, COG_OK, 200, false) == 5U);
    assert(cog_net_observer_status(a) == COG_ERROR_DENIED);
    assert(cog_net_peek(a, &again) == COG_OK && again.request_id == x &&
           again.sequence == saved.sequence && again.headers == saved.headers);
    bool active = false; assert(cog_net_request_status(a, x, &active) == COG_OK && active);
    assert(drive(a, &log, x, COG_OK, 200, false) == 65536U);
    cog_net_free(a); /* The failed feed never cancels either shared request. */
}
static void failure_classes_and_callbacks(void)
{
    const char *urls[] = {getenv("COG_NET_TEST_REFUSED"), getenv("COG_NET_TEST_IP"),
                          "https://observer-nxdomain.invalid/"};
    const cog_net_error_kind_t classes[] = {COG_NET_ERROR_CONNECT, COG_NET_ERROR_TLS, COG_NET_ERROR_DNS};
    for (size_t i = 0U; i < 3U; ++i) {
        observer_log_t log = {.fail_kind = -1}; cog_net_ctx_t *ctx = NULL;
        cog_net_config_t config = settings(&log); cog_request_id_t id = 0U;
        assert(urls[i] != NULL && cog_net_new(&ctx, &config) == COG_OK);
        assert(cog_net_request(ctx, "GET", urls[i], NULL, NULL, 0U, &id) == COG_OK);
        if (i == 2U)
            assert(curl_easy_setopt(ctx->requests[0].easy, CURLOPT_DNS_SERVERS,
                                   getenv("COG_NET_TEST_DNS")) == CURLE_OK);
        assert(drive(ctx, &log, id, COG_ERROR_IO, 0, false) == 0U);
        verify(&log, ctx, id, 0U, 0U, COG_ERROR_IO, 0, false, false);
        assert(log.records[log.count - 1U].error_kind == classes[i]);
        cog_net_free(ctx);
    }
    observer_log_t log = {.fail_kind = -1}; cog_net_ctx_t *ctx = NULL;
    cog_net_config_t config = settings(&log); cog_request_id_t id;
    assert(cog_net_new(&ctx, &config) == COG_OK);
    assert(cog_net_request(ctx, "GET", "https://observer-timeout.invalid/", NULL, NULL, 0U, &id) == COG_OK);
    assert(curl_easy_setopt(ctx->requests[0].easy, CURLOPT_DNS_SERVERS, getenv("COG_NET_TEST_DNS")) == CURLE_OK);
    service(ctx, &log, 100U); service(ctx, &log, 1000U);
    bool done = false; assert(consume(ctx, id, &done, COG_ERROR_TIMEOUT, 0) == 0U && done);
    verify(&log, ctx, id, 0U, 0U, COG_ERROR_TIMEOUT, 0, false, false); cog_net_free(ctx);

    log = (observer_log_t){.fail_kind = COG_NET_OBSERVATION_FAILED, .failure = COG_ERROR_DENIED};
    config = settings(&log); assert(cog_net_new(&ctx, &config) == COG_OK);
    id = submit(ctx, &log, "", NULL, 0U);
    assert(cog_net_cancel(ctx, id) == COG_OK && cog_net_observer_status(ctx) == COG_ERROR_DENIED);
    done = false; assert(consume(ctx, id, &done, COG_ERROR_ABORTED, 0) == 0U && done);
    cog_net_free(ctx);
    log = (observer_log_t){.fail_kind = COG_NET_OBSERVATION_STALLED, .failure = COG_ERROR_IO};
    config = settings(&log); assert(cog_net_new(&ctx, &config) == COG_OK);
    id = submit(ctx, &log, "stall", NULL, 0U);
    assert(drive(ctx, &log, id, COG_ERROR_TIMEOUT, 200, true) == 5U);
    assert(cog_net_observer_status(ctx) == COG_ERROR_IO &&
           log.records[log.count - 1U].kind == COG_NET_OBSERVATION_STALLED);
    cog_net_free(ctx);

    log = (observer_log_t){.fail_kind = -1}; config = settings(&log);
    config.chunk_bytes = 1U; config.chunks_per_request = 2U;
    assert(cog_net_new(&ctx, &config) == COG_OK);
    id = submit(ctx, &log, "", NULL, 0U);
    assert(drive(ctx, &log, id, COG_ERROR_LIMIT, 200, false) == 0U);
    verify(&log, ctx, id, 0U, 0U, COG_ERROR_LIMIT, 200, true, false);
    assert(log.records[log.count - 1U].error_kind == COG_NET_ERROR_CAPACITY);
    cog_net_free(ctx);

    log = (observer_log_t){.fail_kind = -1}; config = settings(&log);
    assert(cog_net_new(&ctx, &config) == COG_OK);
    static const char body[] = "synthetic-observer-secret";
    id = submit(ctx, &log, "post", body, sizeof(body) - 1U);
    upload_fault = 1;
    assert(drive(ctx, &log, id, COG_OK, 200, false) == 5U);
    upload_fault = 0;
    assert(cog_net_observer_status(ctx) == COG_ERROR_IO);
    cog_net_free(ctx);
}
int main(void)
{
    allocation_control();
    assert(curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK);
    admission_and_errors(); real_transfers(); pause_and_pending(); accounting_boundaries();
    identity_lifetime_and_shared_failure(); failure_classes_and_callbacks();
    curl_global_cleanup(); return 0;
}

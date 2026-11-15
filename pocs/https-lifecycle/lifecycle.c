/** @file lifecycle.c
 * Standalone research host, NOT a Cog backend. No Cog header changes.
 * Contract sketch, written before experiment bodies:
 * main explicitly owns global init/cleanup; contexts never call either.
 * Each context owns one multi and at most two easy handles. No sharing.
 * step waits at most 10ms plus perform/callback/scheduler time (measured).
 * cancel runs outside callbacks, removes and destroys only its easy handle.
 * free_context removes/destroys remaining easy handles, then the multi.
 * Caller owns response capacity; overflow aborts loudly, without retry.
 * Threading: host drives all contexts on one thread. Memory observers are
 * atomic because curl allocator callbacks can run on resolver threads.
 */
#define _POSIX_C_SOURCE 200809L
#include <curl/curl.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/socket.h>
#include <errno.h>
#include <dirent.h>

typedef union block { max_align_t align; size_t size; } block_t;
/* Host-only instrumentation: curl's process-wide hooks have no userdata.
 * This mutable observer is explicitly NOT proposed as library state. */
static atomic_size_t alloc_calls, live_blocks, live_bytes, peak_bytes;
static atomic_size_t fail_at;
static atomic_bool failure_injected;

typedef struct transfer {
    CURL *easy;
    bool added, done;
    CURLcode result;
    char body[128];
    size_t used;
    double last_rx;
    long status;
    curl_socket_t socket_fd;
} transfer_t;
typedef struct context {
    CURLM *multi;
    transfer_t transfers[2];
    double max_step_ms;
    size_t poll_allocations;
    int wait_ms;
} context_t;

/* All helpers are private to this throwaway executable. */
static double milliseconds(void);
static int fd_count(void);
static void *observed_malloc(size_t size);
static void observed_free(void *ptr);
static void *observed_realloc(void *ptr, size_t size);
static void *observed_calloc(size_t count, size_t size);
static char *observed_strdup(const char *str);
static size_t receive(char *data, size_t size, size_t count, void *userdata);
static curl_socket_t open_socket(void *userdata, curlsocktype purpose, struct curl_sockaddr *address);
static bool init_context(context_t *ctx);
static bool start(context_t *ctx, size_t slot, const char *url, const char *ca);
static bool step(context_t *ctx);
static bool cancel(context_t *ctx, size_t slot);
static bool free_context(context_t *ctx);

static int fd_count(void)
{
    DIR *directory = opendir("/proc/self/fd");
    if (!directory) return -1;
    int count = 0;
    while (readdir(directory) != NULL) ++count;
    if (closedir(directory) != 0) return -1;
    return count;
}
static double milliseconds(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) abort();
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
}
static void *observed_malloc(size_t size)
{
    const size_t call = atomic_fetch_add(&alloc_calls, 1) + 1;
    if (call == atomic_load(&fail_at)) {
        atomic_store(&failure_injected, true);
        return NULL;
    }
    if (size > SIZE_MAX - sizeof(block_t)) return NULL;
    block_t *block = malloc(sizeof(*block) + size);
    if (!block) return NULL;
    block->size = size;
    atomic_fetch_add(&live_blocks, 1);
    const size_t live = atomic_fetch_add(&live_bytes, size) + size;
    size_t peak = atomic_load(&peak_bytes);
    while (live > peak && !atomic_compare_exchange_weak(&peak_bytes, &peak, live)) { }
    return block + 1;
}
static void observed_free(void *ptr)
{
    if (ptr) {
        block_t *block = (block_t *)ptr - 1;
        atomic_fetch_sub(&live_bytes, block->size);
        atomic_fetch_sub(&live_blocks, 1);
        free(block);
    }
}
static void *observed_realloc(void *ptr, size_t size)
{
    if (!ptr) return observed_malloc(size);
    if (size == 0) { observed_free(ptr); return NULL; }
    const block_t *old = (block_t *)ptr - 1;
    void *fresh = observed_malloc(size);
    if (!fresh) return NULL;
    memcpy(fresh, ptr, old->size < size ? old->size : size);
    observed_free(ptr);
    return fresh;
}
static void *observed_calloc(size_t count, size_t size)
{
    if (size != 0 && count > SIZE_MAX / size) return NULL;
    void *ptr = observed_malloc(count * size);
    if (ptr) memset(ptr, 0, count * size);
    return ptr;
}
static char *observed_strdup(const char *str)
{
    size_t length = strlen(str) + 1;
    char *ptr = observed_malloc(length);
    if (ptr) memcpy(ptr, str, length);
    return ptr;
}
static size_t receive(char *data, size_t size, size_t count, void *userdata)
{
    transfer_t *transfer = userdata;
    if (size != 0 && count > SIZE_MAX / size) return 0;
    const size_t length = size * count;
    if (length > sizeof(transfer->body) - transfer->used) return 0;
    memcpy(transfer->body + transfer->used, data, length);
    transfer->used += length;
    transfer->last_rx = milliseconds();
    return length;
}
static curl_socket_t open_socket(void *userdata, curlsocktype purpose, struct curl_sockaddr *address)
{
    transfer_t *transfer = userdata;
    (void)purpose;
    transfer->socket_fd = socket(address->family, address->socktype, address->protocol);
    return transfer->socket_fd;
}
static bool init_context(context_t *ctx)
{
    ctx->wait_ms = 10;
    ctx->multi = curl_multi_init();
    return ctx->multi != NULL;
}
static bool start(context_t *ctx, size_t slot, const char *url, const char *ca)
{
    transfer_t *transfer = &ctx->transfers[slot];
    transfer->easy = curl_easy_init();
    if (!transfer->easy) return false;
#define SET(option, value) do { if (curl_easy_setopt(transfer->easy, option, value) != CURLE_OK) return false; } while (0)
    SET(CURLOPT_URL, url);
    SET(CURLOPT_PROTOCOLS_STR, "https");
    SET(CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
    SET(CURLOPT_PROXY, ""); /* Isolate fixture from host proxy environment. */
    SET(CURLOPT_SSL_VERIFYPEER, 1L);
    SET(CURLOPT_SSL_VERIFYHOST, 2L);
    SET(CURLOPT_NOSIGNAL, 1L);
    SET(CURLOPT_CONNECTTIMEOUT_MS, 800L);
    SET(CURLOPT_TIMEOUT_MS, 2500L);
    SET(CURLOPT_WRITEFUNCTION, receive);
    SET(CURLOPT_WRITEDATA, transfer);
    SET(CURLOPT_OPENSOCKETFUNCTION, open_socket);
    SET(CURLOPT_OPENSOCKETDATA, transfer);
    if (ca) SET(CURLOPT_CAINFO, ca);
#undef SET
    if (curl_multi_add_handle(ctx->multi, transfer->easy) != CURLM_OK) return false;
    transfer->added = true;
    transfer->last_rx = milliseconds();
    return true;
}
static bool step(context_t *ctx)
{
    const double before = milliseconds();
    const size_t allocations = atomic_load(&alloc_calls);
    int running = 0;
    if (curl_multi_perform(ctx->multi, &running) != CURLM_OK) return false;
    if (running != 0) {
        if (curl_multi_poll(ctx->multi, NULL, 0, ctx->wait_ms, NULL) != CURLM_OK) return false;
        if (curl_multi_perform(ctx->multi, &running) != CURLM_OK) return false;
    }
    int remaining = 0;
    CURLMsg *message;
    while ((message = curl_multi_info_read(ctx->multi, &remaining)) != NULL) {
        if (message->msg != CURLMSG_DONE) return false;
        bool found = false;
        for (size_t i = 0; i < 2; ++i) {
            transfer_t *transfer = &ctx->transfers[i];
            if (transfer->easy == message->easy_handle) {
                transfer->done = true;
                transfer->result = message->data.result;
                if (curl_easy_getinfo(transfer->easy, CURLINFO_RESPONSE_CODE,
                                      &transfer->status) != CURLE_OK) return false;
                found = true;
                break;
            }
        }
        if (!found) return false;
    }
    const double elapsed = milliseconds() - before;
    if (elapsed > ctx->max_step_ms) ctx->max_step_ms = elapsed;
    ctx->poll_allocations += atomic_load(&alloc_calls) - allocations;
    return true;
}
static bool cancel(context_t *ctx, size_t slot)
{
    transfer_t *transfer = &ctx->transfers[slot];
    if (transfer->added) {
        if (curl_multi_remove_handle(ctx->multi, transfer->easy) != CURLM_OK) return false;
        transfer->added = false;
    }
    if (transfer->easy) curl_easy_cleanup(transfer->easy);
    transfer->easy = NULL;
    return true;
}
static bool free_context(context_t *ctx)
{
    bool success = true;
    for (size_t i = 0; i < 2; ++i) if (!cancel(ctx, i)) success = false;
    if (ctx->multi && curl_multi_cleanup(ctx->multi) != CURLM_OK) success = false;
    ctx->multi = NULL;
    return success;
}

int main(int argc, char **argv)
{
    if (argc != 7) {
        fprintf(stderr, "usage: %s MODE CA HTTPS_PORT TLS_STALL_PORT TCP_STALL_PORT OOM_N\n", argv[0]);
        return 2;
    }
    const char *mode = argv[1];
    const int initial_fds = fd_count();
    size_t queued_events_serviced = 0;
    context_t first = {0}, second = {0};
    bool success = true;
    bool global_ready = false;
    char url[256];
    const bool fairness = strcmp(mode, "fairness") == 0;
    const bool oom = strcmp(mode, "oom") == 0;
    const bool multi = strcmp(mode, "multi") == 0;
    const bool cancel_tcp = strcmp(mode, "cancel-connect") == 0;
    const bool cancel_tls = strcmp(mode, "cancel-tls") == 0;
    const bool timeout = strcmp(mode, "connect-timeout") == 0;
    const bool cancel_stall = strcmp(mode, "cancel-stall") == 0;
    const bool stall_timeout = strcmp(mode, "stall-timeout") == 0;
    const bool teardown = strcmp(mode, "teardown") == 0;
    const bool mismatch = strcmp(mode, "hostname") == 0;
    const bool untrusted = strcmp(mode, "untrusted") == 0;
    const bool overflow = strcmp(mode, "overflow") == 0;
#define CHECK(condition) do { if (!(condition)) { fprintf(stderr, "FAIL %s line %d: %s\n", mode, __LINE__, #condition); success = false; goto cleanup; } } while (0)
    /* Exactly one explicit host initialization before any other curl call. */
    CHECK(curl_global_init_mem(CURL_GLOBAL_ALL, observed_malloc, observed_free,
                              observed_realloc, observed_strdup, observed_calloc) == CURLE_OK);
    global_ready = true;
    const curl_version_info_data *version = curl_version_info(CURLVERSION_NOW);
    CHECK(version != NULL);
    CHECK((version->features & CURL_VERSION_ASYNCHDNS) != 0);
    printf("version=%s tls=%s async_dns=1 threadsafe=%d\n", version->version,
           version->ssl_version, (version->features & CURL_VERSION_THREADSAFE) != 0);
    if (oom) atomic_store(&fail_at, atomic_load(&alloc_calls) + (size_t)strtoul(argv[6], NULL, 10));
    if (!init_context(&first)) { CHECK(oom); goto cleanup; }
    if (fairness) first.wait_ms = 0;
    const char *path = (cancel_stall || stall_timeout || teardown || multi) ? "stall" : (overflow ? "large" : "ok");
    const char *port = (cancel_tls || timeout) ? argv[4] : (cancel_tcp ? argv[5] : argv[3]);
    CHECK(snprintf(url, sizeof(url), "https://%s:%s/%s", mismatch ? "127.0.0.1" : "localhost", port, path) > 0);
    if (!start(&first, 0, url, untrusted ? NULL : argv[2])) { CHECK(oom); goto cleanup; }
    if (multi) {
        CHECK(init_context(&second));
        CHECK(snprintf(url, sizeof(url), "https://localhost:%s/ok", argv[3]) > 0);
        CHECK(start(&first, 1, url, argv[2]));
        CHECK(start(&second, 0, url, argv[2]));
    }
    const double begin = milliseconds();
    bool cancelled = false;
    double cancel_ms = 0.0;
    while (milliseconds() - begin < 3500.0) {
        /* A perpetually nonempty synthetic queue must not suppress net service. */
        if (fairness) queued_events_serviced += 16;
        if (!step(&first)) { CHECK(oom); goto cleanup; }
        if (multi) CHECK(step(&second));
        transfer_t *transfer = &first.transfers[0];
        const double elapsed = milliseconds() - begin;
        if ((cancel_tcp || cancel_tls) && elapsed >= 100.0) {
            curl_off_t connected = -1, tls_ready = -1;
            CHECK(!transfer->done);
            CHECK(elapsed < 250.0);
            CHECK(curl_easy_getinfo(transfer->easy, CURLINFO_CONNECT_TIME_T, &connected) == CURLE_OK);
            CHECK(curl_easy_getinfo(transfer->easy, CURLINFO_APPCONNECT_TIME_T, &tls_ready) == CURLE_OK);
            CHECK(tls_ready == 0);
            struct sockaddr_storage peer;
            socklen_t peer_length = sizeof(peer);
            int peer_result = getpeername(transfer->socket_fd, (struct sockaddr *)&peer, &peer_length);
            CHECK(cancel_tcp ? (connected == 0 && peer_result == -1 && errno == ENOTCONN) : peer_result == 0);
            const double before = milliseconds();
            CHECK(cancel(&first, 0));
            cancel_ms = milliseconds() - before;
            cancelled = true;
            break;
        }
        if ((cancel_stall || stall_timeout || teardown || multi) && transfer->used > 0 &&
            (cancel_stall ? elapsed >= 100.0 : milliseconds() - transfer->last_rx >= 100.0)) {
            CHECK(!transfer->done);
            const double before = milliseconds();
            CHECK(teardown ? free_context(&first) : cancel(&first, 0));
            cancel_ms = milliseconds() - before;
            cancelled = true;
            if (multi) { /* Destroy A while B still has a handle; B stays usable. */
                CHECK(first.transfers[1].done && first.transfers[1].result == CURLE_OK);
                CHECK(free_context(&first));
                CHECK(start(&second, 1, url, argv[2]));
                while (!second.transfers[1].done && milliseconds() - begin < 3500.0) CHECK(step(&second));
                CHECK(second.transfers[0].done && second.transfers[0].result == CURLE_OK);
                CHECK(second.transfers[1].done && second.transfers[1].result == CURLE_OK);
            }
            break;
        }
        if (transfer->done) break;
    }
    if (oom) goto cleanup;
    if (cancel_tcp || cancel_tls || cancel_stall || stall_timeout || teardown || multi) CHECK(cancelled);
    else {
        CHECK(first.transfers[0].done);
        CURLcode expected = (mismatch || untrusted) ? CURLE_PEER_FAILED_VERIFICATION :
                            (timeout ? CURLE_OPERATION_TIMEDOUT : (overflow ? CURLE_WRITE_ERROR : CURLE_OK));
        CHECK(first.transfers[0].result == expected);
        if (timeout) CHECK(milliseconds() - begin >= 700.0 && milliseconds() - begin < 1200.0);
        if (expected == CURLE_OK) {
            static const char expected_body[] = "data: first\n\ndata: second\n\n";
            CHECK(first.transfers[0].status == 200);
            CHECK(first.transfers[0].used == sizeof(expected_body) - 1);
            CHECK(memcmp(first.transfers[0].body, expected_body, sizeof(expected_body) - 1) == 0);
        }
        if (mismatch || untrusted) CHECK(first.transfers[0].used == 0);
    }
    printf("mode=%s max_step_ms=%.3f other_step_ms=%.3f cancel_ms=%.3f poll_allocations=%zu\n",
           mode, first.max_step_ms, second.max_step_ms, cancel_ms, first.poll_allocations + second.poll_allocations);
    CHECK(first.max_step_ms < 150.0 && second.max_step_ms < 150.0);
    CHECK(cancel_ms < 150.0);
    if (fairness) CHECK(queued_events_serviced > 100);
    printf("queued_events_serviced=%zu\n", queued_events_serviced);
cleanup:
    if (!free_context(&first)) success = false;
    if (!free_context(&second)) success = false;
    if (global_ready) curl_global_cleanup(); /* All contexts gone first. */
    printf("alloc_calls=%zu live_blocks=%zu live_bytes=%zu peak_bytes=%zu injected=%d\n",
           atomic_load(&alloc_calls), atomic_load(&live_blocks), atomic_load(&live_bytes),
           atomic_load(&peak_bytes), atomic_load(&failure_injected));
    const int final_fds = fd_count();
    printf("fd_before=%d fd_after=%d\n", initial_fds, final_fds);
    if (initial_fds < 0 || final_fds != initial_fds) success = false;
    if (atomic_load(&live_blocks) != 0 || atomic_load(&live_bytes) != 0) success = false;
    return success ? 0 : 1;
}

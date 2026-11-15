/** @file probe.c
 * Standalone DNS teardown experiment, no Cog linkage or production API.
 * Host owns global init/cleanup. One context owns multi/easy until removed.
 * All service/cancel/free calls execute on the host thread. Test modes:
 * threaded-wait: synthetic libc resolver stall, default cleanup joins it.
 * threaded-quick: deliberately demonstrate QUICK_EXIT's residual thread.
 * cares-cancel / cares-timeout: local DNS blackhole, no resolver thread.
 * Failure paths clean up before returning nonzero. Bounds are measured test
 * ceilings, not hard real-time guarantees. No mutable application globals.
 */
#define _POSIX_C_SOURCE 200809L
#include <curl/curl.h>
#include <dirent.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static double milliseconds(void);
static int entries(const char *path);
static bool has_feature(const curl_version_info_data *version, const char *name);

static double milliseconds(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return -1.0;
    return (double)now.tv_sec * 1000.0 + (double)now.tv_nsec / 1000000.0;
}
static int entries(const char *path)
{
    DIR *directory = opendir(path);
    if (!directory) return -1;
    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL) {
        if (entry->d_name[0] != '.') ++count;
    }
    if (closedir(directory) != 0) return -1;
    return count;
}
static bool has_feature(const curl_version_info_data *version, const char *name)
{
    if (version->age < CURLVERSION_ELEVENTH || !version->feature_names) return false;
    for (const char *const *feature = version->feature_names; *feature; ++feature) {
        if (strcmp(*feature, name) == 0) return true;
    }
    return false;
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: %s MODE DNS_SERVER\n", argv[0]);
        return 2;
    }
    const char *mode = argv[1];
    const bool cares = strcmp(mode, "cares-cancel") == 0 || strcmp(mode, "cares-timeout") == 0;
    const bool timeout = strcmp(mode, "cares-timeout") == 0;
    const bool quick = strcmp(mode, "threaded-quick") == 0;
    const bool wait = strcmp(mode, "threaded-wait") == 0;
    bool ok = true, initialized = false, added = false, complete = false;
    CURL *easy = NULL;
    CURLM *multi = NULL;
    CURLcode result = CURLE_OK;
    double max_step = 0.0, remove_ms = 0.0, easy_ms = 0.0, multi_ms = 0.0;
    const int initial_threads = entries("/proc/self/task");
    const int initial_fds = entries("/proc/self/fd");
#define CHECK(condition) do { if (!(condition)) { fprintf(stderr, "FAIL %s line %d: %s\n", mode, __LINE__, #condition); ok = false; goto cleanup; } } while (0)
    CHECK(cares || quick || wait);
    CHECK(initial_threads == 1 && initial_fds >= 0);
    CHECK(curl_global_init(CURL_GLOBAL_ALL) == CURLE_OK);
    initialized = true;
    const curl_version_info_data *version = curl_version_info(CURLVERSION_NOW);
    CHECK(version != NULL && version->version_num >= 0x080e01);
    CHECK((version->features & CURL_VERSION_ASYNCHDNS) != 0);
    printf("mode=%s version=%s ares=%s asyn_rr=%d initial_threads=%d\n", mode,
           version->version, version->ares ? version->ares : "none",
           has_feature(version, "asyn-rr"), initial_threads);
    /* ASYNCHDNS and even a nonnull ares string alone do not exclude a
     * hybrid build whose primary A/AAAA resolver is still threaded. */
    CHECK(cares ? (version->ares != NULL && !has_feature(version, "asyn-rr")) : version->ares == NULL);
    easy = curl_easy_init();
    multi = curl_multi_init();
    CHECK(easy != NULL && multi != NULL);
#define SET(option, value) CHECK(curl_easy_setopt(easy, option, value) == CURLE_OK)
    SET(CURLOPT_URL, "https://stall.cog-delay.invalid/");
    SET(CURLOPT_PROXY, "");
    SET(CURLOPT_NOSIGNAL, 1L);
    SET(CURLOPT_CONNECTTIMEOUT_MS, timeout ? 800L : 5000L);
    if (quick) SET(CURLOPT_QUICK_EXIT, 1L);
    const CURLcode dns_option = curl_easy_setopt(easy, CURLOPT_DNS_SERVERS, cares ? argv[2] : (char *)NULL);
    printf("dns_servers_set=%d\n", (int)dns_option);
    CHECK(cares ? dns_option == CURLE_OK : dns_option == CURLE_UNKNOWN_OPTION);
    CHECK(curl_multi_add_handle(multi, easy) == CURLM_OK);
    added = true;
    const double begin = milliseconds();
    CHECK(begin >= 0.0);
    int running = 0;
    while (milliseconds() - begin < (timeout ? 1500.0 : 100.0)) {
        const double before = milliseconds();
        CHECK(curl_multi_perform(multi, &running) == CURLM_OK);
        if (running != 0) CHECK(curl_multi_poll(multi, NULL, 0, 10, NULL) == CURLM_OK);
        const double elapsed = milliseconds() - before;
        if (elapsed > max_step) max_step = elapsed;
        int remaining = 0;
        CURLMsg *message;
        while ((message = curl_multi_info_read(multi, &remaining)) != NULL) {
            CHECK(message->msg == CURLMSG_DONE && message->easy_handle == easy);
            complete = true;
            result = message->data.result;
        }
        if (complete) break;
    }
    const double elapsed = milliseconds() - begin;
    printf("before_cleanup running=%d complete=%d elapsed_ms=%.3f threads=%d max_step_ms=%.3f\n",
           running, complete, elapsed, entries("/proc/self/task"), max_step);
    CHECK(max_step < 150.0);
    if (timeout) {
        CHECK(complete && result == CURLE_OPERATION_TIMEDOUT);
        CHECK(elapsed >= 700.0 && elapsed < 1200.0);
    }
    else CHECK(!complete && elapsed < 250.0);
    CHECK(cares ? entries("/proc/self/task") == initial_threads : entries("/proc/self/task") > initial_threads);
cleanup:
    if (added) {
        const double before = milliseconds();
        if (curl_multi_remove_handle(multi, easy) != CURLM_OK) ok = false;
        remove_ms = milliseconds() - before;
    }
    if (easy) {
        const double before = milliseconds();
        curl_easy_cleanup(easy);
        easy_ms = milliseconds() - before;
    }
    if (multi) {
        const double before = milliseconds();
        if (curl_multi_cleanup(multi) != CURLM_OK) ok = false;
        multi_ms = milliseconds() - before;
    }
    const double before = milliseconds();
    if (initialized) curl_global_cleanup();
    const double global_ms = milliseconds() - before;
    const int cleanup_threads = entries("/proc/self/task");
    printf("remove_ms=%.3f easy_ms=%.3f multi_ms=%.3f global_ms=%.3f cleanup_threads=%d\n",
           remove_ms, easy_ms, multi_ms, global_ms, cleanup_threads);
    if (cares && (remove_ms >= 150.0 || easy_ms >= 150.0 || multi_ms >= 150.0 || global_ms >= 150.0 || cleanup_threads != initial_threads)) ok = false;
    if (wait && remove_ms < 1000.0) ok = false; /* Deliberately proves a violation. */
    if (quick) {
        /* This mode is evidence against QUICK_EXIT as an embedding solution.
         * Keep all code loaded until its detached resolver finally returns. */
        if (cleanup_threads <= initial_threads || remove_ms >= 150.0) ok = false;
        struct timespec remaining = {2, 200000000};
        while (nanosleep(&remaining, &remaining) != 0) { }
    }
    const int final_threads = entries("/proc/self/task");
    const int final_fds = entries("/proc/self/fd");
    printf("final_threads=%d fd_before=%d fd_after=%d result=%d\n",
           final_threads, initial_fds, final_fds, (int)result);
    if (final_threads != initial_threads || final_fds != initial_fds) ok = false;
    return ok ? 0 : 1;
}

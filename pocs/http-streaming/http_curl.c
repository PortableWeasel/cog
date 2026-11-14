/**
 * @file http_curl.c
 * @brief libcurl-based streaming client (multi interface).
 *
 * Why the multi interface instead of curl_easy_perform? The demo needs a
 * live view of the connection tracker while the transfer is streaming, and
 * the stall detection is driven by the tracker, not by libcurl. With the
 * multi interface the demo loop wakes up at least every 100 ms, can fire
 * the optional status callback, and can abort the transfer the moment the
 * tracker declares a stall. The write callback still receives body bytes
 * as they arrive, so chunk timing is preserved end to end.
 *
 * An alternative worth noting for production: CURLOPT_LOW_SPEED_LIMIT
 * combined with CURLOPT_LOW_SPEED_TIME makes libcurl abort transfers whose
 * average throughput drops below a floor — a built-in stall detector that
 * requires no application loop. It is coarser (average-speed based) and
 * cannot feed the per-chunk tracker, which is why the PoC uses the
 * tracker-driven path.
 */

#define _POSIX_C_SOURCE 200809L

#include "http_common.h"

#include <curl/curl.h>

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/** Retry backoff between attempts, in nanoseconds. */
#define RETRY_BACKOFF_NS 50000000L

/** Minimum cadence of the live status callback, in milliseconds. */
#define STATUS_INTERVAL_MS 300U

/** Per-transfer context shared with the libcurl callbacks. */
struct curl_ctx {
    const http_request_t *req;
    int status;          /**< Last status line seen (header callback). */
    uint64_t last_status_ms; /**< Throttle clock for the status callback. */
};

/**
 * @brief libcurl write callback: one slice of decoded body bytes.
 *
 * @param[in] ptr   Data.
 * @param[in] size  Element size.
 * @param[in] nmemb Element count.
 * @param[in] ud    struct curl_ctx.
 * @return Number of bytes consumed (must equal size * nmemb).
 */
static size_t curl_write_cb(char *ptr, size_t size, size_t nmemb, void *ud)
{
    struct curl_ctx *ctx = ud;
    size_t n = size * nmemb;
    conn_tracker_note_chunk(ctx->req->tracker, n);
    if (ctx->req->on_chunk != NULL) {
        ctx->req->on_chunk((const uint8_t *)ptr, n, ctx->req->user_data);
    }
    return n;
}

/**
 * @brief libcurl header callback: capture the response status line.
 *
 * @param[in] ptr   Header data.
 * @param[in] size  Element size.
 * @param[in] nmemb Element count.
 * @param[in] ud    struct curl_ctx.
 * @return Number of bytes consumed.
 */
static size_t curl_header_cb(char *ptr, size_t size, size_t nmemb, void *ud)
{
    struct curl_ctx *ctx = ud;
    size_t n = size * nmemb;
    if (n >= 5U && memcmp(ptr, "HTTP/", 5U) == 0) {
        const char *sp = NULL;
        size_t i;
        for (i = 5U; i < n; i++) {
            if (ptr[i] == ' ') {
                sp = ptr + i;
                break;
            }
        }
        if (sp != NULL) {
            char *end = NULL;
            unsigned long code = strtoul(sp, &end, 10);
            if (end != sp && code <= 999UL) {
                ctx->status = (int)code;
            }
        }
    }
    return n;
}

http_result_t curl_request(const http_request_t *req)
{
    http_result_t res;
    (void)memset(&res, 0, sizeof res);
    if (req == NULL || req->url == NULL || req->cfg == NULL ||
        req->tracker == NULL) {
        res.err = HTTP_ERR_PARSE;
        res.state = CONN_STATE_ERROR;
        return res;
    }

    CURLM *multi = curl_multi_init();
    if (multi == NULL) {
        res.err = HTTP_ERR_NET;
        res.state = CONN_STATE_ERROR;
        return res;
    }

    uint64_t start = conn_tracker_now_ms();
    unsigned attempt;
    for (attempt = 0U; attempt <= req->cfg->max_retries; attempt++) {
        if (attempt > 0U) {
            struct timespec backoff;
            backoff.tv_sec = 0;
            backoff.tv_nsec = RETRY_BACKOFF_NS;
            (void)nanosleep(&backoff, NULL);
        }
        if (conn_tracker_now_ms() - start > req->cfg->overall_timeout_ms) {
            res.err = HTTP_ERR_TIMEOUT;
            conn_tracker_set_state(req->tracker, CONN_STATE_ERROR);
            break;
        }
        conn_tracker_reset(req->tracker);
        conn_tracker_set_state(req->tracker, CONN_STATE_CONNECTING);
        res.retries = attempt;

        struct curl_ctx ctx;
        (void)memset(&ctx, 0, sizeof ctx);
        ctx.req = req;

        CURL *easy = curl_easy_init();
        if (easy == NULL) {
            res.err = HTTP_ERR_NET;
            conn_tracker_set_state(req->tracker, CONN_STATE_ERROR);
            break;
        }

        (void)curl_easy_setopt(easy, CURLOPT_URL, req->url);
        (void)curl_easy_setopt(easy, CURLOPT_HTTP_VERSION,
                               (long)CURL_HTTP_VERSION_1_1);
        (void)curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, curl_write_cb);
        (void)curl_easy_setopt(easy, CURLOPT_WRITEDATA, &ctx);
        (void)curl_easy_setopt(easy, CURLOPT_HEADERFUNCTION, curl_header_cb);
        (void)curl_easy_setopt(easy, CURLOPT_HEADERDATA, &ctx);
        (void)curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);
        (void)curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT_MS, 3000L);
        long overall;
        if (req->cfg->overall_timeout_ms > (uint64_t)LONG_MAX) {
            overall = LONG_MAX;
        } else {
            overall = (long)req->cfg->overall_timeout_ms;
        }
        (void)curl_easy_setopt(easy, CURLOPT_TIMEOUT_MS, overall);

        CURLMcode mc = curl_multi_add_handle(multi, easy);
        if (mc != CURLM_OK) {
            curl_easy_cleanup(easy);
            res.err = HTTP_ERR_NET;
            conn_tracker_set_state(req->tracker, CONN_STATE_ERROR);
            break;
        }

        int running = 0;
        (void)curl_multi_perform(multi, &running);
        bool stalled = false;
        bool over = false;
        while (running > 0) {
            uint64_t now = conn_tracker_now_ms();
            if (conn_tracker_check_stall(req->tracker, now,
                                         req->cfg->stall_ms)) {
                stalled = true;
                break;
            }
            if (now - start > req->cfg->overall_timeout_ms) {
                over = true;
                break;
            }
            if (req->on_status != NULL &&
                now - ctx.last_status_ms >= STATUS_INTERVAL_MS) {
                ctx.last_status_ms = now;
                req->on_status(req->tracker, req->user_data);
            }
            int nfds = 0;
            CURLMcode pr = curl_multi_poll(multi, NULL, 0U, 100, &nfds);
            if (pr != CURLM_OK) {
                break;
            }
            (void)nfds;
            (void)curl_multi_perform(multi, &running);
        }

        /* Fetch the transfer outcome before tearing the handle down. */
        CURLcode cres = CURLE_OK;
        {
            int msgs = 0;
            CURLMsg *msg;
            while ((msg = curl_multi_info_read(multi, &msgs)) != NULL) {
                if (msg->msg == CURLMSG_DONE) {
                    cres = msg->data.result;
                    break;
                }
            }
        }
        (void)curl_multi_remove_handle(multi, easy);
        curl_easy_cleanup(easy);

        if (stalled) {
            res.err = HTTP_ERR_STALL;
            conn_tracker_set_state(req->tracker, CONN_STATE_STALLED);
            break;
        }
        if (over) {
            res.err = HTTP_ERR_TIMEOUT;
            conn_tracker_set_state(req->tracker, CONN_STATE_ERROR);
            break;
        }
        if (cres != CURLE_OK) {
            if (cres == CURLE_OPERATION_TIMEDOUT) {
                res.err = HTTP_ERR_TIMEOUT;
            } else {
                res.err = HTTP_ERR_NET;
            }
            conn_tracker_set_state(req->tracker, CONN_STATE_ERROR);
            break;
        }

        res.status = ctx.status;
        if (ctx.status >= 500) {
            res.err = HTTP_ERR_RETRYABLE;
            if (attempt < req->cfg->max_retries) {
                continue; /* transient failure: retry */
            }
            conn_tracker_set_state(req->tracker, CONN_STATE_ERROR);
            break;
        }
        if (ctx.status >= 400) {
            res.err = HTTP_ERR_CLIENT;
            conn_tracker_set_state(req->tracker, CONN_STATE_ERROR);
            break;
        }

        conn_tracker_set_state(req->tracker, CONN_STATE_DONE);
        res.err = HTTP_ERR_NONE;
        break;
    }

    res.bytes = conn_tracker_bytes(req->tracker);
    res.chunks = conn_tracker_chunks(req->tracker);
    res.state = conn_tracker_state(req->tracker);
    (void)curl_multi_cleanup(multi);
    return res;
}

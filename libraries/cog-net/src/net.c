/**
 * @file net.c
 * @brief HTTP client with streaming, stall detection, and retry.
 *
 * Implements the public contract in <cog/net.h>.  The design follows the
 * hand-rolled HTTP/1.1 client proven in pocs/http-streaming:
 *
 *   - getaddrinfo + non-blocking connect (first reachable address).
 *   - A single request line with Host, user headers, Content-Length and
 *     Connection: close.
 *   - Response head parsing (status line + headers).
 *   - Body framing: Transfer-Encoding: chunked decoding, Content-Length,
 *     or plain read-until-close as a fallback.
 *   - Stall detection via poll() timeouts on every socket read.
 *   - Retry on transient failure (connect/network/stall/5xx) with a
 *     bounded backoff, up to config.max_retries.
 *
 * The whole module is single-threaded and poll-driven: cog_net_poll()
 * advances each in-flight request by one non-blocking step and returns
 * the next queued response chunk.  There is no global mutable state; all
 * state lives in the caller-allocated cog_net_ctx_t.  No provider-level
 * queuing is performed here — this library only speaks HTTP.
 */

#define _GNU_SOURCE

#include "cog/net.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* Tunables                                                            */
/* ------------------------------------------------------------------ */

/** Default maximum concurrent connections. */
#define DEFAULT_MAX_CONNECTIONS 16U

/** Default TCP connect timeout in milliseconds. */
#define DEFAULT_CONNECT_TIMEOUT_MS 5000U

/** Default stall threshold in milliseconds. */
#define DEFAULT_STALL_TIMEOUT_MS 30000U

/** Default maximum retries on transient failure. */
#define DEFAULT_MAX_RETRIES 2U

/** Default base backoff between retries in milliseconds. */
#define DEFAULT_RETRY_BACKOFF_MS 200U

/** Capacity of the buffered reader's internal buffer. */
#define READ_BUF_CAP 4096U

/** Cap on a single response head (status + headers). */
#define HEAD_CAP 4096U

/** Cap on a chunk-size / trailer line. */
#define LINE_CAP 256U

/** Reject absurd chunk sizes. */
#define MAX_CHUNK_PAYLOAD (1024U * 1024U)

/** Number of trailer lines tolerated after the terminal 0-chunk. */
#define MAX_TRAILER_LINES 16U

/* ------------------------------------------------------------------ */
/* Private types                                                       */
/* ------------------------------------------------------------------ */

/** Lifecycle state of a single request. */
typedef enum req_state {
    REQ_QUEUED = 0,   /**< Not yet started. */
    REQ_CONNECTING,   /**< Non-blocking connect in progress. */
    REQ_SENDING,      /**< Sending request bytes. */
    REQ_READING_HEAD, /**< Reading response head. */
    REQ_READING_BODY, /**< Reading response body. */
    REQ_BACKOFF,      /**< Waiting to retry. */
    REQ_DONE,         /**< Completed; final node enqueued. */
    REQ_FAILED,       /**< Terminally failed; error node enqueued. */
    REQ_CANCELLED     /**< Cancelled by the caller. */
} req_state_t;

/** One queued response node (owns copies of its payload). */
typedef struct cog_net_resp_node {
    struct cog_net_resp_node *next;
    cog_request_id_t id;
    int http_status;
    char *headers;      /**< Owned copy, may be NULL. */
    char *chunk;        /**< Owned copy, may be NULL. */
    size_t chunk_len;
    bool is_complete;
    cog_result_t error; /**< COG_OK for a normal node. */
} cog_net_resp_node_t;

/** One in-flight request slot. */
typedef struct cog_net_req {
    bool in_use;
    cog_request_id_t id;
    req_state_t state;

    /* Owned request data. */
    char *method;
    char *url;
    char *headers;
    char *body;
    size_t body_len;

    /* Parsed URL. */
    char host[256];
    uint16_t port;
    const char *path; /* points into url */

    /* Socket. */
    int fd;

    /* Send state. */
    char *send_buf;
    size_t send_len;
    size_t send_off;

    /* Buffered reader. */
    uint8_t rbuf[READ_BUF_CAP];
    size_t rbuf_start;
    size_t rbuf_end;

    /* Response head. */
    uint8_t head[HEAD_CAP];
    size_t head_len;
    int http_status;
    bool chunked;
    bool have_content_length;
    size_t content_length;

    /* Body framing. */
    size_t chunk_remaining;
    bool need_crlf;
    bool in_trailers;
    int trailer_lines;
    uint8_t exact_buf[8];
    size_t exact_len;
    uint8_t line_buf[LINE_CAP];
    size_t line_len;

    /* Timing / retry. */
    uint64_t phase_start_ms;
    uint64_t last_activity_ms;
    uint64_t backoff_until_ms;
    unsigned attempt;

    bool cancelled;
} cog_net_req_t;

/** The opaque network context. */
struct cog_net_ctx {
    cog_net_config_t config;
    size_t max_connections;
    cog_net_req_t *requests;
    cog_request_id_t next_id;
    cog_net_resp_node_t *out_head;
    cog_net_resp_node_t *out_tail;
    int wake_fd;
    bool oom; /* set if an internal allocation failed */

    /* Current response buffers (valid until the next cog_net_poll). */
    char *current_headers;
    size_t current_headers_cap;
    char *current_chunk;
    size_t current_chunk_cap;
};

/* ------------------------------------------------------------------ */
/* Time helpers                                                        */
/* ------------------------------------------------------------------ */

/** Monotonic time in milliseconds (CLOCK_MONOTONIC). */
static uint64_t now_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0U;
    }
    return (uint64_t)ts.tv_sec * 1000U + (uint64_t)ts.tv_nsec / 1000000U;
}

/* ------------------------------------------------------------------ */
/* Output queue                                                        */
/* ------------------------------------------------------------------ */

/**
 * @brief Append a response node to the context's output queue.
 *
 * Copies @p headers and @p chunk into the node so the node outlives the
 * request slot that produced it.
 */
static void req_enqueue(cog_net_ctx_t *ctx, cog_request_id_t id,
                        int http_status, const char *headers,
                        const char *chunk, size_t chunk_len,
                        bool is_complete, cog_result_t error)
{
    cog_net_resp_node_t *node = calloc(1U, sizeof *node);
    if (node == NULL) {
        ctx->oom = true;
        return;
    }
    node->id = id;
    node->http_status = http_status;
    node->is_complete = is_complete;
    node->error = error;
    if (headers != NULL) {
        node->headers = strdup(headers);
        if (node->headers == NULL) {
            ctx->oom = true;
            free(node);
            return;
        }
    }
    if (chunk != NULL && chunk_len > 0U) {
        node->chunk = malloc(chunk_len);
        if (node->chunk == NULL) {
            ctx->oom = true;
            free(node->headers);
            free(node);
            return;
        }
        (void)memcpy(node->chunk, chunk, chunk_len);
        node->chunk_len = chunk_len;
    }
    if (ctx->out_tail != NULL) {
        ctx->out_tail->next = node;
    } else {
        ctx->out_head = node;
    }
    ctx->out_tail = node;
}

/** @brief Free a response node. */
static void resp_node_free(cog_net_resp_node_t *node)
{
    if (node == NULL) {
        return;
    }
    free(node->headers);
    free(node->chunk);
    free(node);
}

/* ------------------------------------------------------------------ */
/* Request data ownership                                              */
/* ------------------------------------------------------------------ */

/** @brief Close the request socket, if any. */
static void req_close_fd(cog_net_req_t *req)
{
    if (req->fd >= 0) {
        (void)close(req->fd);
        req->fd = -1;
    }
}

/** @brief Free all owned request buffers and close the socket. */
static void req_free_data(cog_net_req_t *req)
{
    req_close_fd(req);
    free(req->method);
    free(req->url);
    free(req->headers);
    free(req->body);
    free(req->send_buf);
    req->method = NULL;
    req->url = NULL;
    req->headers = NULL;
    req->body = NULL;
    req->send_buf = NULL;
}

/** @brief Reset per-attempt state for a retry (keeps request data). */
static void req_reset_for_retry(cog_net_req_t *req)
{
    req_close_fd(req);
    req->rbuf_start = 0U;
    req->rbuf_end = 0U;
    req->head_len = 0U;
    req->http_status = 0;
    req->chunked = false;
    req->have_content_length = false;
    req->content_length = 0U;
    req->chunk_remaining = 0U;
    req->need_crlf = false;
    req->in_trailers = false;
    req->trailer_lines = 0;
    req->exact_len = 0U;
    req->line_len = 0U;
    req->send_off = 0U;
}

/* ------------------------------------------------------------------ */
/* Header parsing helpers                                              */
/* ------------------------------------------------------------------ */

/** @brief Case-insensitive equality of a header field name with a literal. */
static bool name_is(const uint8_t *name, size_t len, const char *lit)
{
    size_t i;
    for (i = 0U; i < len; i++) {
        if (lit[i] == '\0') {
            return false;
        }
        if ((char)(name[i] | 0x20U) != lit[i]) {
            return false;
        }
    }
    return lit[i] == '\0';
}

/** @brief Case-insensitive substring search over a header value. */
static bool value_has(const uint8_t *v, size_t vlen, const char *needle)
{
    size_t nlen = strlen(needle);
    if (vlen < nlen) {
        return false;
    }
    size_t i;
    for (i = 0U; i + nlen <= vlen; i++) {
        bool match = true;
        size_t j;
        for (j = 0U; j < nlen; j++) {
            if ((char)(v[i + j] | 0x20U) != needle[j]) {
                match = false;
                break;
            }
        }
        if (match) {
            return true;
        }
    }
    return false;
}

/**
 * @brief Parse the response head.
 *
 * Extracts the HTTP status code, chunked framing, and Content-Length.
 *
 * @return 0 on success, -1 on a malformed head.
 */
static int req_parse_head(cog_net_req_t *req)
{
    size_t line_end = 0U;
    while (line_end < req->head_len && req->head[line_end] != '\n') {
        line_end++;
    }
    if (line_end >= req->head_len || line_end < 12U ||
        memcmp(req->head, "HTTP/", 5U) != 0) {
        return -1;
    }
    char line[64];
    size_t ln = line_end;
    if (ln >= sizeof line) {
        return -1;
    }
    if (ln > 0U && req->head[ln - 1U] == '\r') {
        ln--;
    }
    (void)memcpy(line, req->head, ln);
    line[ln] = '\0';
    char *end = NULL;
    unsigned long code = strtoul(line + 9U, &end, 10); /* past "HTTP/1.1 " */
    if (end == line + 9U || code < 100UL || code > 999UL) {
        return -1;
    }
    req->http_status = (int)code;

    req->chunked = false;
    req->have_content_length = false;
    req->content_length = 0U;
    size_t pos = line_end + 1U;
    while (pos < req->head_len) {
        size_t eol = pos;
        while (eol < req->head_len && req->head[eol] != '\n') {
            eol++;
        }
        size_t hlen = eol - pos;
        if (hlen > 0U && req->head[pos + hlen - 1U] == '\r') {
            hlen--;
        }
        if (hlen == 0U) {
            break; /* blank line terminates the head */
        }
        size_t colon = 0U;
        while (colon < hlen && req->head[pos + colon] != ':') {
            colon++;
        }
        if (colon < hlen) {
            const uint8_t *value = req->head + pos + colon + 1U;
            size_t vlen = hlen - colon - 1U;
            if (name_is(req->head + pos, colon, "transfer-encoding") &&
                value_has(value, vlen, "chunked")) {
                req->chunked = true;
            }
            if (name_is(req->head + pos, colon, "content-length")) {
                char vbuf[32];
                size_t vn = vlen < sizeof vbuf - 1U ? vlen : sizeof vbuf - 1U;
                (void)memcpy(vbuf, value, vn);
                vbuf[vn] = '\0';
                char *vend = NULL;
                unsigned long cl = strtoul(vbuf, &vend, 10);
                if (vend != vbuf) {
                    req->have_content_length = true;
                    req->content_length = (size_t)cl;
                }
            }
        }
        pos = eol + 1U;
    }
    return 0;
}

/**
 * @brief Build the response header string (newline-separated key:value).
 *
 * Returns a newly allocated NUL-terminated string, or NULL on failure.
 */
static char *build_headers_string(const cog_net_req_t *req)
{
    size_t pos = 0U;
    while (pos < req->head_len && req->head[pos] != '\n') {
        pos++;
    }
    if (pos >= req->head_len) {
        return NULL;
    }
    pos++; /* skip past the status line's '\n' */

    size_t cap = req->head_len - pos + 1U;
    char *out = malloc(cap);
    if (out == NULL) {
        return NULL;
    }
    size_t o = 0U;
    while (pos < req->head_len) {
        size_t eol = pos;
        while (eol < req->head_len && req->head[eol] != '\n') {
            eol++;
        }
        size_t hlen = eol - pos;
        if (hlen > 0U && req->head[pos + hlen - 1U] == '\r') {
            hlen--;
        }
        if (hlen == 0U) {
            break; /* blank line ends the header section */
        }
        if (o + hlen + 1U >= cap) {
            break;
        }
        (void)memcpy(out + o, req->head + pos, hlen);
        o += hlen;
        out[o++] = '\n';
        pos = eol + 1U;
    }
    if (o > 0U && out[o - 1U] == '\n') {
        o--;
    }
    out[o] = '\0';
    return out;
}

/* ------------------------------------------------------------------ */
/* URL parsing                                                         */
/* ------------------------------------------------------------------ */

/**
 * @brief Split an "http://host[:port]/path" URL into its parts.
 *
 * @return 0 on success, -1 on parse failure.
 */
static int split_url(const char *url, char *host, size_t host_cap,
                     uint16_t *port, const char **path)
{
    static const char k_prefix[] = "http://";
    static const size_t k_prefix_len = sizeof k_prefix - 1U;

    if (url == NULL || host == NULL || host_cap == 0U || port == NULL ||
        path == NULL) {
        return -1;
    }
    if (strncmp(url, k_prefix, k_prefix_len) != 0) {
        return -1;
    }
    const char *p = url + k_prefix_len;
    size_t host_len = strcspn(p, ":/");
    if (host_len == 0U || host_len >= host_cap) {
        return -1;
    }
    (void)memcpy(host, p, host_len);
    host[host_len] = '\0';

    const char *q = p + host_len;
    unsigned long parsed_port = 80UL;
    if (*q == ':') {
        char *end = NULL;
        parsed_port = strtoul(q + 1U, &end, 10);
        if (end == q + 1U || parsed_port == 0UL || parsed_port > 65535UL) {
            return -1;
        }
        q = end;
    }
    *port = (uint16_t)parsed_port;
    *path = (*q == '/') ? q : "/";
    return 0;
}

/* ------------------------------------------------------------------ */
/* Socket helpers                                                      */
/* ------------------------------------------------------------------ */

/**
 * @brief Open a non-blocking TCP connection to the first reachable address.
 *
 * @return 0 on success (connect initiated), -1 on failure.
 */
static int req_open_connect(cog_net_req_t *req)
{
    if (req->path == NULL) {
        if (split_url(req->url, req->host, sizeof req->host, &req->port,
                      &req->path) != 0) {
            return -1;
        }
    }
    char port_str[8];
    int pn = snprintf(port_str, sizeof port_str, "%u", (unsigned)req->port);
    if (pn < 0 || (size_t)pn >= sizeof port_str) {
        return -1;
    }
    struct addrinfo hints;
    (void)memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *res = NULL;
    int rc = getaddrinfo(req->host, port_str, &hints, &res);
    if (rc != 0) {
        return -1;
    }
    int fd = -1;
    struct addrinfo *ai;
    for (ai = res; ai != NULL; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) {
            continue;
        }
        int flags = fcntl(fd, F_GETFL, 0);
        if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
            (void)close(fd);
            fd = -1;
            continue;
        }
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) {
            break; /* connected immediately */
        }
        if (errno == EINPROGRESS) {
            break; /* connect in progress */
        }
        (void)close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) {
        return -1;
    }
    req->fd = fd;
    return 0;
}

/**
 * @brief Check whether a non-blocking connect has completed.
 *
 * @return 1 if connected, 0 if still pending, -1 on error.
 */
static int req_connect_done(cog_net_req_t *req)
{
    struct pollfd pfd;
    pfd.fd = req->fd;
    pfd.events = (short)POLLOUT;
    pfd.revents = 0;
    int rc;
    do {
        rc = poll(&pfd, 1, 0);
    } while (rc < 0 && errno == EINTR);
    if (rc == 0) {
        return 0;
    }
    if (rc < 0) {
        return -1;
    }
    int err = 0;
    socklen_t elen = (socklen_t)sizeof err;
    if (getsockopt(req->fd, SOL_SOCKET, SO_ERROR, &err, &elen) != 0) {
        return -1;
    }
    if (err != 0) {
        errno = err;
        return -1;
    }
    return 1;
}

/**
 * @brief Send as much of the request as the socket will accept.
 *
 * @return 1 if all bytes sent, 0 if would-block (more to send), -1 on error.
 */
static int req_send_step(cog_net_req_t *req)
{
    while (req->send_off < req->send_len) {
        ssize_t w = send(req->fd, req->send_buf + req->send_off,
                         req->send_len - req->send_off, 0);
        if (w < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN) {
                return 0;
            }
            return -1;
        }
        req->send_off += (size_t)w;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* Buffered reader                                                     */
/* ------------------------------------------------------------------ */

/**
 * @brief Refill the read buffer from the socket (non-blocking).
 *
 * @return 1 if data was read, 0 if would-block, -1 on EOF, -2 on error.
 */
static int req_rbuf_fill(cog_net_req_t *req)
{
    if (req->rbuf_start != 0U) {
        size_t n = req->rbuf_end - req->rbuf_start;
        (void)memmove(req->rbuf, req->rbuf + req->rbuf_start, n);
        req->rbuf_start = 0U;
        req->rbuf_end = n;
    }
    struct pollfd pfd;
    pfd.fd = req->fd;
    pfd.events = (short)POLLIN;
    pfd.revents = 0;
    int rc;
    do {
        rc = poll(&pfd, 1, 0);
    } while (rc < 0 && errno == EINTR);
    if (rc == 0) {
        return 0; /* would-block */
    }
    if (rc < 0) {
        return -2;
    }
    ssize_t n = recv(req->fd, req->rbuf + req->rbuf_end,
                     sizeof req->rbuf - req->rbuf_end, 0);
    if (n < 0) {
        if (errno == EINTR) {
            return req_rbuf_fill(req);
        }
        return -2;
    }
    if (n == 0) {
        return -1; /* EOF */
    }
    req->rbuf_end += (size_t)n;
    return 1;
}

/**
 * @brief Read a single buffered byte.
 *
 * @return 1 on success, 0 if would-block, -1 on EOF, -2 on error.
 */
static int req_rbuf_getc(cog_net_req_t *req, uint8_t *out)
{
    if (req->rbuf_start < req->rbuf_end) {
        *out = req->rbuf[req->rbuf_start++];
        return 1;
    }
    int rc = req_rbuf_fill(req);
    if (rc <= 0) {
        return rc;
    }
    *out = req->rbuf[req->rbuf_start++];
    return 1;
}

/**
 * @brief Read up to @p cap bytes into @p dst (non-blocking).
 *
 * @return bytes read (>0), 0 if would-block, -1 on EOF, -2 on error.
 */
static int req_read_available(cog_net_req_t *req, uint8_t *dst, size_t cap)
{
    if (req->rbuf_start < req->rbuf_end) {
        size_t avail = req->rbuf_end - req->rbuf_start;
        size_t take = avail < cap ? avail : cap;
        (void)memcpy(dst, req->rbuf + req->rbuf_start, take);
        req->rbuf_start += take;
        return (int)take;
    }
    int rc = req_rbuf_fill(req);
    if (rc <= 0) {
        return rc;
    }
    size_t avail = req->rbuf_end - req->rbuf_start;
    size_t take = avail < cap ? avail : cap;
    (void)memcpy(dst, req->rbuf + req->rbuf_start, take);
    req->rbuf_start += take;
    return (int)take;
}

/**
 * @brief Read a line (up to and including '\n') into req->line_buf.
 *
 * Stateful: partial lines survive would-block.  On completion the line is
 * NUL-terminated in req->line_buf and req->line_len is reset to 0.
 *
 * @return 1 on success, 0 if would-block, -1 on EOF, -2 on error.
 */
static int req_read_line(cog_net_req_t *req)
{
    for (;;) {
        uint8_t c;
        int rc = req_rbuf_getc(req, &c);
        if (rc == 0) {
            return 0;
        }
        if (rc < 0) {
            return rc;
        }
        if (c == '\n') {
            if (req->line_len > 0U &&
                req->line_buf[req->line_len - 1U] == '\r') {
                req->line_len--;
            }
            req->line_buf[req->line_len] = '\0';
            req->line_len = 0U;
            return 1;
        }
        if (req->line_len + 1U >= LINE_CAP) {
            return -2; /* line too long */
        }
        req->line_buf[req->line_len++] = c;
    }
}

/**
 * @brief Read exactly @p want bytes into @p dst (stateful).
 *
 * @return 1 on success, 0 if would-block, -1 on EOF, -2 on error.
 */
static int req_read_exact(cog_net_req_t *req, uint8_t *dst, size_t want)
{
    while (req->exact_len < want) {
        int rc = req_rbuf_getc(req, &req->exact_buf[req->exact_len]);
        if (rc == 0) {
            return 0;
        }
        if (rc < 0) {
            return rc;
        }
        req->exact_len++;
    }
    (void)memcpy(dst, req->exact_buf, want);
    req->exact_len = 0U;
    return 1;
}

/* ------------------------------------------------------------------ */
/* Response head reading                                               */
/* ------------------------------------------------------------------ */

/**
 * @brief Read the response head up to CRLFCRLF.
 *
 * @return 1 when complete, 0 if would-block, -1 on EOF, -2 on error.
 */
static int req_head_step(cog_net_req_t *req)
{
    for (;;) {
        if (req->head_len >= 4U) {
            size_t i = req->head_len - 4U;
            size_t j = 0U;
            while (j < 4U && req->head[i + j] == "\r\n\r\n"[j]) {
                j++;
            }
            if (j == 4U) {
                return 1;
            }
        }
        uint8_t c;
        int rc = req_rbuf_getc(req, &c);
        if (rc == 0) {
            return 0;
        }
        if (rc < 0) {
            return rc;
        }
        if (req->head_len + 1U >= HEAD_CAP) {
            return -2; /* head too long */
        }
        req->head[req->head_len++] = c;
    }
}

/* ------------------------------------------------------------------ */
/* Response body reading                                               */
/* ------------------------------------------------------------------ */

/** @brief Read chunked trailers until the terminal blank line. */
static int req_trailers_step(cog_net_req_t *req)
{
    if (req->trailer_lines >= (int)MAX_TRAILER_LINES) {
        return 1; /* done */
    }
    int rc = req_read_line(req);
    if (rc == 0) {
        return 0;
    }
    if (rc < 0) {
        return rc;
    }
    if (req->line_buf[0] == '\0') {
        return 1; /* blank line -> done */
    }
    req->trailer_lines++;
    return 0;
}

/** @brief Read one step of a chunked-encoded body. */
static int req_chunked_step(cog_net_ctx_t *ctx, cog_net_req_t *req)
{
    if (req->in_trailers) {
        return req_trailers_step(req);
    }
    if (req->need_crlf) {
        uint8_t crlf[2];
        int rc = req_read_exact(req, crlf, 2U);
        if (rc == 0) {
            return 0;
        }
        if (rc < 0) {
            return rc;
        }
        if (crlf[0] != '\r' || crlf[1] != '\n') {
            return -2; /* protocol violation */
        }
        req->need_crlf = false;
        req->chunk_remaining = 0U;
        return 0;
    }
    if (req->chunk_remaining == 0U) {
        int rc = req_read_line(req);
        if (rc == 0) {
            return 0;
        }
        if (rc < 0) {
            return rc;
        }
        char *end = NULL;
        unsigned long csize = strtoul((const char *)req->line_buf, &end, 16);
        if (end == (const char *)req->line_buf) {
            return -2; /* parse error */
        }
        if (csize == 0UL) {
            req->in_trailers = true;
            return req_trailers_step(req);
        }
        if (csize > MAX_CHUNK_PAYLOAD) {
            return -2;
        }
        req->chunk_remaining = (size_t)csize;
        return 0;
    }
    uint8_t slice[READ_BUF_CAP];
    size_t want = req->chunk_remaining < sizeof slice
                      ? req->chunk_remaining
                      : sizeof slice;
    int rc = req_read_available(req, slice, want);
    if (rc > 0) {
        req_enqueue(ctx, req->id, req->http_status, NULL, (const char *)slice,
                    (size_t)rc, false, COG_OK);
        req->chunk_remaining -= (size_t)rc;
        req->last_activity_ms = now_ms();
        if (req->chunk_remaining == 0U) {
            req->need_crlf = true;
            req->exact_len = 0U;
        }
        return 0;
    }
    if (rc == 0) {
        return 0;
    }
    return rc;
}

/** @brief Read a plain body until the peer closes the connection. */
static int req_until_close_step(cog_net_ctx_t *ctx, cog_net_req_t *req)
{
    uint8_t slice[READ_BUF_CAP];
    int rc = req_read_available(req, slice, sizeof slice);
    if (rc > 0) {
        req_enqueue(ctx, req->id, req->http_status, NULL, (const char *)slice,
                    (size_t)rc, false, COG_OK);
        req->last_activity_ms = now_ms();
        return 0;
    }
    if (rc == 0) {
        return 0;
    }
    if (rc == -1) {
        return 1; /* EOF is the normal end here */
    }
    return rc;
}

/** @brief Read one step of the response body. */
static int req_body_step(cog_net_ctx_t *ctx, cog_net_req_t *req)
{
    if (req->chunked) {
        return req_chunked_step(ctx, req);
    }
    if (req->have_content_length) {
        if (req->content_length == 0U) {
            return 1; /* complete */
        }
        uint8_t slice[READ_BUF_CAP];
        size_t want = req->content_length < sizeof slice
                          ? req->content_length
                          : sizeof slice;
        int rc = req_read_available(req, slice, want);
        if (rc > 0) {
            req_enqueue(ctx, req->id, req->http_status, NULL,
                        (const char *)slice, (size_t)rc, false, COG_OK);
            req->content_length -= (size_t)rc;
            req->last_activity_ms = now_ms();
            if (req->content_length == 0U) {
                return 1;
            }
            return 0;
        }
        if (rc == 0) {
            return 0;
        }
        return rc;
    }
    return req_until_close_step(ctx, req);
}

/* ------------------------------------------------------------------ */
/* Request lifecycle                                                   */
/* ------------------------------------------------------------------ */

/** @brief Wake the external poll loop. */
static void wake(cog_net_ctx_t *ctx)
{
    if (ctx->wake_fd < 0) {
        return;
    }
    uint64_t one = 1U;
    ssize_t rc = write(ctx->wake_fd, &one, sizeof one);
    (void)rc;
}

/** @brief Emit the response headers node for a parsed head. */
static void req_emit_headers(cog_net_ctx_t *ctx, cog_net_req_t *req)
{
    char *h = build_headers_string(req);
    req_enqueue(ctx, req->id, req->http_status, h, NULL, 0U, false, COG_OK);
    free(h);
}

/** @brief Enqueue a terminal error node and release the request slot. */
static void req_fail(cog_net_ctx_t *ctx, cog_net_req_t *req,
                     cog_result_t error)
{
    req_enqueue(ctx, req->id, req->http_status, NULL, NULL, 0U, true, error);
    req_free_data(req);
    req->in_use = false;
}

/** @brief Enqueue a terminal success node and release the request slot. */
static void req_complete(cog_net_ctx_t *ctx, cog_net_req_t *req)
{
    req_enqueue(ctx, req->id, req->http_status, NULL, NULL, 0U, true, COG_OK);
    req_free_data(req);
    req->in_use = false;
}

/** @brief Handle a transient failure: retry if budget remains, else fail. */
static void req_handle_transient(cog_net_ctx_t *ctx, cog_net_req_t *req,
                                 cog_result_t error);

/** @brief Move a request into the backoff state before a retry. */
static void req_go_backoff(cog_net_ctx_t *ctx, cog_net_req_t *req)
{
    req->attempt++;
    req_close_fd(req);
    req->state = REQ_BACKOFF;
    req->backoff_until_ms = now_ms() + ctx->config.retry_backoff_ms;
    wake(ctx);
}

/** @brief Retry a request: reset per-attempt state and reconnect. */
static void req_retry(cog_net_ctx_t *ctx, cog_net_req_t *req)
{
    req_reset_for_retry(req);
    req->state = REQ_CONNECTING;
    req->phase_start_ms = now_ms();
    req->last_activity_ms = now_ms();
    if (req_open_connect(req) != 0) {
        req_handle_transient(ctx, req, COG_ERROR_IO);
    }
}

/**
 * @brief Handle a transient failure: retry if budget remains, else fail.
 */
static void req_handle_transient(cog_net_ctx_t *ctx, cog_net_req_t *req,
                                 cog_result_t error)
{
    if (req->attempt < ctx->config.max_retries) {
        req_go_backoff(ctx, req);
    } else {
        req_fail(ctx, req, error);
    }
}

/** @brief Advance one request by a single non-blocking step. */
static void req_step(cog_net_ctx_t *ctx, cog_net_req_t *req)
{
    uint64_t now = now_ms();
    switch (req->state) {
    case REQ_QUEUED:
        req->state = REQ_CONNECTING;
        req->phase_start_ms = now;
        req->last_activity_ms = now;
        if (req_open_connect(req) != 0) {
            req_handle_transient(ctx, req, COG_ERROR_IO);
        }
        break;

    case REQ_CONNECTING:
        if (now - req->phase_start_ms > ctx->config.connect_timeout_ms) {
            req_handle_transient(ctx, req, COG_ERROR_TIMEOUT);
            break;
        }
        {
            int rc = req_connect_done(req);
            if (rc == 1) {
                req->state = REQ_SENDING;
                req->last_activity_ms = now;
            } else if (rc < 0) {
                req_handle_transient(ctx, req, COG_ERROR_IO);
            }
        }
        break;

    case REQ_SENDING: {
        int rc = req_send_step(req);
        if (rc == 1) {
            req->state = REQ_READING_HEAD;
            req->last_activity_ms = now;
        } else if (rc < 0) {
            req_handle_transient(ctx, req, COG_ERROR_IO);
        } else if (now - req->last_activity_ms >
                   ctx->config.stall_timeout_ms) {
            req_handle_transient(ctx, req, COG_ERROR_TIMEOUT);
        }
        break;
    }

    case REQ_READING_HEAD:
        if (now - req->last_activity_ms > ctx->config.stall_timeout_ms) {
            req_handle_transient(ctx, req, COG_ERROR_TIMEOUT);
            break;
        }
        {
            int rc = req_head_step(req);
            if (rc == 1) {
                if (req_parse_head(req) != 0) {
                    req_fail(ctx, req, COG_ERROR_IO);
                    break;
                }
                req->last_activity_ms = now;
                if (req->http_status >= 500) {
                    if (req->attempt < ctx->config.max_retries) {
                        req_go_backoff(ctx, req);
                    } else {
                        req_emit_headers(ctx, req);
                        req_complete(ctx, req);
                    }
                    break;
                }
                req_emit_headers(ctx, req);
                req->state = REQ_READING_BODY;
            } else if (rc < 0) {
                req_handle_transient(ctx, req, COG_ERROR_IO);
            }
        }
        break;

    case REQ_READING_BODY:
        if (now - req->last_activity_ms > ctx->config.stall_timeout_ms) {
            req_handle_transient(ctx, req, COG_ERROR_TIMEOUT);
            break;
        }
        {
            int rc = req_body_step(ctx, req);
            if (rc == 1) {
                req_complete(ctx, req);
            } else if (rc < 0) {
                req_handle_transient(ctx, req, COG_ERROR_IO);
            }
        }
        break;

    case REQ_BACKOFF:
        if (now >= req->backoff_until_ms) {
            req_retry(ctx, req);
        }
        break;

    default:
        break;
    }
}

/** @brief Advance all active requests by one non-blocking step. */
static void net_work(cog_net_ctx_t *ctx)
{
    size_t i;
    for (i = 0U; i < ctx->max_connections; i++) {
        cog_net_req_t *req = &ctx->requests[i];
        if (!req->in_use) {
            continue;
        }
        req_step(ctx, req);
    }
}

/** @brief Find an active request by id. */
static const cog_net_req_t *find_request(const cog_net_ctx_t *ctx,
                                         cog_request_id_t id)
{
    size_t i;
    for (i = 0U; i < ctx->max_connections; i++) {
        const cog_net_req_t *req = &ctx->requests[i];
        if (req->in_use && req->id == id) {
            return req;
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

cog_result_t cog_net_new(cog_net_ctx_t **out, const cog_net_config_t *config)
{
    if (out == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    *out = NULL;
    cog_net_ctx_t *ctx = calloc(1U, sizeof *ctx);
    if (ctx == NULL) {
        return COG_ERROR_NOMEM;
    }
    ctx->config.max_connections = DEFAULT_MAX_CONNECTIONS;
    ctx->config.connect_timeout_ms = DEFAULT_CONNECT_TIMEOUT_MS;
    ctx->config.stall_timeout_ms = DEFAULT_STALL_TIMEOUT_MS;
    ctx->config.max_retries = DEFAULT_MAX_RETRIES;
    ctx->config.retry_backoff_ms = DEFAULT_RETRY_BACKOFF_MS;
    if (config != NULL) {
        ctx->config.max_connections =
            config->max_connections != 0U ? config->max_connections
                                          : DEFAULT_MAX_CONNECTIONS;
        ctx->config.connect_timeout_ms =
            config->connect_timeout_ms != 0U ? config->connect_timeout_ms
                                             : DEFAULT_CONNECT_TIMEOUT_MS;
        ctx->config.stall_timeout_ms =
            config->stall_timeout_ms != 0U ? config->stall_timeout_ms
                                           : DEFAULT_STALL_TIMEOUT_MS;
        ctx->config.max_retries = config->max_retries;
        ctx->config.retry_backoff_ms = config->retry_backoff_ms;
    }
    ctx->max_connections = ctx->config.max_connections;
    ctx->requests = calloc(ctx->max_connections, sizeof *ctx->requests);
    if (ctx->requests == NULL) {
        free(ctx);
        return COG_ERROR_NOMEM;
    }
    ctx->next_id = 1U;
    ctx->wake_fd = eventfd(0U, EFD_NONBLOCK | EFD_CLOEXEC);
    if (ctx->wake_fd < 0) {
        free(ctx->requests);
        free(ctx);
        return COG_ERROR_IO;
    }
    *out = ctx;
    return COG_OK;
}

void cog_net_free(cog_net_ctx_t *ctx)
{
    if (ctx == NULL) {
        return;
    }
    size_t i;
    for (i = 0U; i < ctx->max_connections; i++) {
        cog_net_req_t *req = &ctx->requests[i];
        if (req->in_use) {
            req_free_data(req);
            req->in_use = false;
        }
    }
    while (ctx->out_head != NULL) {
        cog_net_resp_node_t *n = ctx->out_head;
        ctx->out_head = n->next;
        resp_node_free(n);
    }
    ctx->out_tail = NULL;
    if (ctx->wake_fd >= 0) {
        (void)close(ctx->wake_fd);
        ctx->wake_fd = -1;
    }
    free(ctx->current_headers);
    free(ctx->current_chunk);
    free(ctx->requests);
    free(ctx);
}

cog_result_t cog_net_request(cog_net_ctx_t *ctx, const char *method,
                             const char *url, const char *headers,
                             const char *body, size_t body_len,
                             cog_request_id_t *out_id)
{
    if (ctx == NULL || method == NULL || url == NULL || out_id == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    cog_net_req_t *req = NULL;
    size_t i;
    for (i = 0U; i < ctx->max_connections; i++) {
        if (!ctx->requests[i].in_use) {
            req = &ctx->requests[i];
            break;
        }
    }
    if (req == NULL) {
        return COG_ERROR_OVERFLOW;
    }

    (void)memset(req, 0, sizeof *req);
    req->method = strdup(method);
    req->url = strdup(url);
    if (req->method == NULL || req->url == NULL) {
        req_free_data(req);
        return COG_ERROR_NOMEM;
    }
    if (headers != NULL) {
        req->headers = strdup(headers);
        if (req->headers == NULL) {
            req_free_data(req);
            return COG_ERROR_NOMEM;
        }
    }
    if (body != NULL && body_len > 0U) {
        req->body = malloc(body_len);
        if (req->body == NULL) {
            req_free_data(req);
            return COG_ERROR_NOMEM;
        }
        (void)memcpy(req->body, body, body_len);
    }
    req->body_len = body_len;
    req->fd = -1;
    req->id = ctx->next_id++;
    req->state = REQ_QUEUED;

    /* Parse the URL so host/port/path are available for the request line. */
    if (split_url(req->url, req->host, sizeof req->host, &req->port,
                  &req->path) != 0) {
        req_free_data(req);
        return COG_ERROR_INVALID_ARG;
    }

    /* Build the request bytes. */
    size_t path_len = strlen(req->path);
    size_t method_len = strlen(req->method);
    size_t host_len = strlen(req->host);
    size_t headers_len = req->headers != NULL ? strlen(req->headers) : 0U;
    size_t cap = method_len + path_len + host_len + headers_len + body_len +
                 128U;
    char *buf = malloc(cap);
    if (buf == NULL) {
        req_free_data(req);
        return COG_ERROR_NOMEM;
    }
    size_t o = 0U;
    (void)memcpy(buf + o, req->method, method_len);
    o += method_len;
    buf[o++] = ' ';
    (void)memcpy(buf + o, req->path, path_len);
    o += path_len;
    (void)memcpy(buf + o, " HTTP/1.1\r\n", 11U);
    o += 11U;
    (void)memcpy(buf + o, "Host: ", 6U);
    o += 6U;
    (void)memcpy(buf + o, req->host, host_len);
    o += host_len;
    char port_str[8];
    int pn = snprintf(port_str, sizeof port_str, ":%u", (unsigned)req->port);
    if (pn < 0 || (size_t)pn >= sizeof port_str) {
        free(buf);
        req_free_data(req);
        return COG_ERROR_INVALID_ARG;
    }
    (void)memcpy(buf + o, port_str, (size_t)pn);
    o += (size_t)pn;
    (void)memcpy(buf + o, "\r\n", 2U);
    o += 2U;
    if (req->headers != NULL && headers_len > 0U) {
        (void)memcpy(buf + o, req->headers, headers_len);
        o += headers_len;
        (void)memcpy(buf + o, "\r\n", 2U);
        o += 2U;
    }
    char cl[32];
    int cln = snprintf(cl, sizeof cl, "Content-Length: %zu\r\n", body_len);
    if (cln < 0 || (size_t)cln >= sizeof cl) {
        free(buf);
        req_free_data(req);
        return COG_ERROR_INVALID_ARG;
    }
    (void)memcpy(buf + o, cl, (size_t)cln);
    o += (size_t)cln;
    (void)memcpy(buf + o, "Connection: close\r\n\r\n", 21U);
    o += 21U;
    if (req->body != NULL && body_len > 0U) {
        (void)memcpy(buf + o, req->body, body_len);
        o += body_len;
    }
    req->send_buf = buf;
    req->send_len = o;
    req->send_off = 0U;
    req->in_use = true;

    *out_id = req->id;
    wake(ctx);
    return COG_OK;
}

cog_result_t cog_net_cancel(cog_net_ctx_t *ctx, cog_request_id_t id)
{
    if (ctx == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    const cog_net_req_t *creq = find_request(ctx, id);
    if (creq == NULL) {
        return COG_ERROR_NOT_FOUND;
    }
    cog_net_req_t *req = (cog_net_req_t *)creq;
    if (req->cancelled) {
        return COG_OK;
    }
    req->cancelled = true;
    req_enqueue(ctx, id, req->http_status, NULL, NULL, 0U, true,
                COG_ERROR_ABORTED);
    req_free_data(req);
    req->in_use = false;
    wake(ctx);
    return COG_OK;
}

cog_result_t cog_net_poll(cog_net_ctx_t *ctx, cog_net_response_t *out_response)
{
    if (ctx == NULL || out_response == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    if (ctx->oom) {
        return COG_ERROR_NOMEM;
    }
    if (ctx->out_head == NULL) {
        net_work(ctx);
    }
    if (ctx->oom) {
        return COG_ERROR_NOMEM;
    }
    cog_net_resp_node_t *node = ctx->out_head;
    if (node == NULL) {
        return COG_ERROR_AGAIN;
    }
    ctx->out_head = node->next;
    if (ctx->out_head == NULL) {
        ctx->out_tail = NULL;
    }

    (void)memset(out_response, 0, sizeof *out_response);
    out_response->http_status = node->http_status;
    out_response->request_id = node->id;
    out_response->is_complete = node->is_complete;

    if (node->headers != NULL) {
        size_t n = strlen(node->headers);
        if (ctx->current_headers_cap < n + 1U) {
            char *nb = realloc(ctx->current_headers, n + 1U);
            if (nb == NULL) {
                resp_node_free(node);
                return COG_ERROR_NOMEM;
            }
            ctx->current_headers = nb;
            ctx->current_headers_cap = n + 1U;
        }
        (void)memcpy(ctx->current_headers, node->headers, n + 1U);
        out_response->headers = ctx->current_headers;
    }
    if (node->chunk != NULL && node->chunk_len > 0U) {
        if (ctx->current_chunk_cap < node->chunk_len) {
            char *nb = realloc(ctx->current_chunk, node->chunk_len);
            if (nb == NULL) {
                resp_node_free(node);
                return COG_ERROR_NOMEM;
            }
            ctx->current_chunk = nb;
            ctx->current_chunk_cap = node->chunk_len;
        }
        (void)memcpy(ctx->current_chunk, node->chunk, node->chunk_len);
        out_response->chunk = ctx->current_chunk;
        out_response->chunk_len = node->chunk_len;
    }

    cog_result_t err = node->error;
    resp_node_free(node);
    return err;
}

int cog_net_fd(const cog_net_ctx_t *ctx)
{
    if (ctx == NULL) {
        return -1;
    }
    return ctx->wake_fd;
}

size_t cog_net_active_count(const cog_net_ctx_t *ctx)
{
    if (ctx == NULL) {
        return 0U;
    }
    size_t n = 0U;
    size_t i;
    for (i = 0U; i < ctx->max_connections; i++) {
        if (ctx->requests[i].in_use) {
            n++;
        }
    }
    return n;
}

cog_result_t cog_net_active_ids(const cog_net_ctx_t *ctx,
                                cog_request_id_t *out_ids, size_t max_ids,
                                size_t *out_count)
{
    if (ctx == NULL || out_ids == NULL || out_count == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    size_t n = 0U;
    size_t i;
    for (i = 0U; i < ctx->max_connections; i++) {
        if (ctx->requests[i].in_use) {
            if (n >= max_ids) {
                return COG_ERROR_OVERFLOW;
            }
            out_ids[n++] = ctx->requests[i].id;
        }
    }
    *out_count = n;
    return COG_OK;
}

cog_result_t cog_net_request_status(const cog_net_ctx_t *ctx,
                                    cog_request_id_t id, bool *out_active)
{
    if (ctx == NULL || out_active == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    const cog_net_req_t *req = find_request(ctx, id);
    if (req == NULL) {
        return COG_ERROR_NOT_FOUND;
    }
    *out_active = req->in_use;
    return COG_OK;
}

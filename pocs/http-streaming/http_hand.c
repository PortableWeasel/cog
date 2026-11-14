/**
 * @file http_hand.c
 * @brief Minimal hand-rolled HTTP/1.1 streaming client.
 *
 * This transport has no dependencies beyond POSIX sockets. It implements
 * exactly what the PoC needs:
 *
 *   1. getaddrinfo + connect (first reachable address).
 *   2. A single GET request with stream-friendly headers.
 *   3. Response head parsing (status line + headers).
 *   4. Body framing: Transfer-Encoding: chunked decoding, or plain
 *      read-until-close as a fallback.
 *   5. Stall detection via poll() timeouts on every socket read.
 *   6. Retry on 5xx responses, with a small fixed backoff.
 *
 * Reads go through a small buffered reader (struct hread) so that line
 * parsing never loses bytes that over-read past a line terminator.
 */

#define _POSIX_C_SOURCE 200809L

#include "http_common.h"

#include <errno.h>
#include <limits.h>
#include <netdb.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/** Capacity of the buffered reader's internal buffer. */
#define HREAD_CAP 4096U

/** Cap on a single response head (status + headers). */
#define HEAD_CAP 4096U

/** Cap on a chunk-size line. */
#define CHUNK_LINE_CAP 256U

/** Reject absurd chunk sizes (the mock server never exceeds ~200 bytes). */
#define MAX_CHUNK_PAYLOAD (1024U * 1024U)

/** Number of trailer lines tolerated after the terminal 0-chunk. */
#define MAX_TRAILER_LINES 16

/** Retry backoff between attempts, in nanoseconds. */
#define RETRY_BACKOFF_NS 50000000L

/**
 * Buffered reader over a socket fd with a per-read stall timeout.
 *
 * Internal buffer codes used by the helper functions below:
 *   0  success
 *  -1  EOF (peer closed the connection)
 *  -2  stall timeout (no data within the threshold)
 *  -3  hard error / protocol violation
 */
struct hread {
    int fd;
    uint64_t stall_ms;      /**< poll() timeout per socket read. */
    uint64_t last_status_ms; /**< Throttle clock for the status callback. */
    uint8_t buf[HREAD_CAP];
    size_t start;
    size_t end;
};

/**
 * @brief Refill the read buffer from the socket.
 *
 * @param[in,out] r Reader.
 * @return 0 on success, -1 on EOF, -2 on stall timeout, -3 on error.
 */
static int hread_fill(struct hread *r)
{
    if (r->start != 0U) {
        size_t n = r->end - r->start;
        (void)memmove(r->buf, r->buf + r->start, n);
        r->start = 0U;
        r->end = n;
    }
    struct pollfd pfd;
    pfd.fd = r->fd;
    pfd.events = (short)POLLIN;
    pfd.revents = 0;
    int timeout;
    if (r->stall_ms > (uint64_t)INT_MAX) {
        timeout = INT_MAX;
    } else {
        timeout = (int)r->stall_ms;
    }
    int rc;
    do {
        rc = poll(&pfd, 1, timeout);
    } while (rc < 0 && errno == EINTR);
    if (rc == 0) {
        return -2; /* stall */
    }
    if (rc < 0) {
        return -3;
    }
    ssize_t n = recv(r->fd, r->buf + r->end, sizeof r->buf - r->end, 0);
    if (n < 0) {
        if (errno == EINTR) {
            return hread_fill(r);
        }
        return -3;
    }
    if (n == 0) {
        return -1; /* EOF */
    }
    r->end += (size_t)n;
    return 0;
}

/**
 * @brief Read a single byte, buffered.
 *
 * @param[in,out] r   Reader.
 * @param[out]    out Byte read.
 * @return 0 on success, -1 on EOF, -2 on stall, -3 on error.
 */
static int hread_getc(struct hread *r, uint8_t *out)
{
    for (;;) {
        if (r->start < r->end) {
            *out = r->buf[r->start++];
            return 0;
        }
        int rc = hread_fill(r);
        if (rc != 0) {
            return rc;
        }
    }
}

/**
 * @brief Read a line (up to and including '\n').
 *
 * Bytes up to — not including — the '\n' are nul-terminated into @p out;
 * a trailing '\r' is stripped. Ideal for status lines, chunk sizes, and
 * trailer headers.
 *
 * @param[in,out] r       Reader.
 * @param[out]    out     Line buffer.
 * @param[in]     cap     Line buffer capacity.
 * @param[out]    len_out Line length (excluding NUL).
 * @return 0 on success, -1 on EOF, -2 on stall, -3 on overflow/error.
 */
static int hread_line(struct hread *r, uint8_t *out, size_t cap,
                      size_t *len_out)
{
    size_t n = 0U;
    for (;;) {
        uint8_t c;
        int rc = hread_getc(r, &c);
        if (rc < 0) {
            return rc;
        }
        if (c == '\n') {
            break;
        }
        if (n + 1U >= cap) {
            return -3; /* line too long */
        }
        out[n++] = c;
    }
    if (n > 0U && out[n - 1U] == '\r') {
        n--;
    }
    out[n] = '\0';
    *len_out = n;
    return 0;
}

/**
 * @brief Read exactly @p want bytes into @p out.
 *
 * @param[in,out] r    Reader.
 * @param[out]    out  Destination buffer.
 * @param[in]     want Byte count.
 * @return 0 on success, -1 on EOF, -2 on stall, -3 on error.
 */
static int hread_exact(struct hread *r, uint8_t *out, size_t want)
{
    size_t got = 0U;
    while (got < want) {
        if (r->start < r->end) {
            size_t avail = r->end - r->start;
            size_t take = want - got;
            if (take > avail) {
                take = avail;
            }
            (void)memcpy(out + got, r->buf + r->start, take);
            r->start += take;
            got += take;
            continue;
        }
        int rc = hread_fill(r);
        if (rc != 0) {
            return rc;
        }
    }
    return 0;
}

/**
 * @brief Map buffered-reader return codes to http_err_t values.
 *
 * @param[in] rc Reader return code (0/-1/-2/-3).
 * @return Corresponding http_err_t.
 */
static http_err_t hread_err(int rc)
{
    switch (rc) {
    case -2:
        return HTTP_ERR_STALL;
    case -1: /* EOF at an unexpected point */
        return HTTP_ERR_NET;
    default:
        return HTTP_ERR_NET;
    }
}

/**
 * @brief Read the response head (status line + headers) up to CRLFCRLF.
 *
 * @param[in,out] r        Reader.
 * @param[out]    head     Buffer receiving the raw head.
 * @param[in]     cap      Head buffer capacity.
 * @param[out]    head_len Bytes written to @p head.
 * @return 0 on success, or a negative reader code.
 */
static int recv_head(struct hread *r, uint8_t *head, size_t cap,
                     size_t *head_len)
{
    size_t n = 0U;
    for (;;) {
        if (n >= 4U) {
            size_t i = n - 4U;
            size_t j = 0U;
            while (j < 4U && head[i + j] == "\r\n\r\n"[j]) {
                j++;
            }
            if (j == 4U) {
                *head_len = n;
                return 0;
            }
        }
        uint8_t c;
        int rc = hread_getc(r, &c);
        if (rc != 0) {
            return rc;
        }
        if (n + 1U >= cap) {
            return -3;
        }
        head[n++] = c;
    }
}

/**
 * @brief Case-insensitive equality of a header field name with a literal.
 *
 * @param[in] name Field name bytes (not nul-terminated).
 * @param[in] len  Field name length.
 * @param[in] lit  Lowercase literal to compare against.
 * @return true on a match.
 */
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

/**
 * @brief Case-insensitive substring search over a header value.
 *
 * @param[in] v      Value bytes.
 * @param[in] vlen   Value length.
 * @param[in] needle Lowercase needle.
 * @return true if @p needle occurs in @p v.
 */
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
 * @brief Parse a response head.
 *
 * Extracts the HTTP status code and whether the body is chunked.
 *
 * @param[in]  head     Raw head bytes.
 * @param[in]  head_len Head length.
 * @param[out] status   Status code.
 * @param[out] chunked  true if Transfer-Encoding: chunked.
 * @return 0 on success, -1 on a malformed head.
 */
static int parse_head(const uint8_t *head, size_t head_len, int *status,
                      bool *chunked)
{
    size_t line_end = 0U;
    while (line_end < head_len && head[line_end] != '\n') {
        line_end++;
    }
    if (line_end >= head_len || line_end < 12U ||
        memcmp(head, "HTTP/", 5U) != 0) {
        return -1;
    }
    char line[64];
    size_t ln = line_end;
    if (ln >= sizeof line) {
        return -1;
    }
    if (ln > 0U && head[ln - 1U] == '\r') {
        ln--;
    }
    (void)memcpy(line, head, ln);
    line[ln] = '\0';
    char *end = NULL;
    unsigned long code = strtoul(line + 9U, &end, 10); /* past "HTTP/1.1 " */
    if (end == line + 9U || code < 100UL || code > 999UL) {
        return -1;
    }
    *status = (int)code;

    *chunked = false;
    size_t pos = line_end + 1U;
    while (pos < head_len) {
        size_t eol = pos;
        while (eol < head_len && head[eol] != '\n') {
            eol++;
        }
        size_t hlen = eol - pos;
        if (hlen > 0U && head[pos + hlen - 1U] == '\r') {
            hlen--;
        }
        if (hlen == 0U) {
            break; /* blank line terminates the head */
        }
        size_t colon = 0U;
        while (colon < hlen && head[pos + colon] != ':') {
            colon++;
        }
        if (colon < hlen) {
            const uint8_t *value = head + pos + colon + 1U;
            size_t vlen = hlen - colon - 1U;
            if (name_is(head + pos, colon, "transfer-encoding") &&
                value_has(value, vlen, "chunked")) {
                *chunked = true;
            }
        }
        pos = eol + 1U;
    }
    return 0;
}

/**
 * @brief Emit a status snapshot through the optional status callback,
 *        throttled to one call per 300 ms.
 *
 * @param[in,out] r   Reader carrying the throttle clock.
 * @param[in]     req Request context.
 */
static void maybe_status(struct hread *r, const http_request_t *req)
{
    if (req->on_status == NULL) {
        return;
    }
    uint64_t now = conn_tracker_now_ms();
    if (now - r->last_status_ms < 300U) {
        return;
    }
    r->last_status_ms = now;
    req->on_status(req->tracker, req->user_data);
}

/**
 * @brief Read a chunked-encoded body, invoking the chunk callback.
 *
 * @param[in,out] r   Reader.
 * @param[in]     req Request context (tracker + callbacks).
 * @return HTTP_ERR_NONE on success or the failing http_err_t.
 */
static http_err_t read_chunked_body(struct hread *r, const http_request_t *req)
{
    for (;;) {
        uint8_t size_line[CHUNK_LINE_CAP];
        size_t slen = 0U;
        int rc = hread_line(r, size_line, sizeof size_line, &slen);
        if (rc != 0) {
            return hread_err(rc);
        }
        char *end = NULL;
        unsigned long csize = strtoul((const char *)size_line, &end, 16);
        if (end == (const char *)size_line) {
            return HTTP_ERR_PARSE;
        }
        if (csize == 0UL) {
            break; /* terminal chunk reached */
        }
        if (csize > MAX_CHUNK_PAYLOAD) {
            return HTTP_ERR_PARSE;
        }
        size_t remaining = (size_t)csize;
        uint8_t slice[HREAD_CAP];
        while (remaining > 0U) {
            size_t take = remaining;
            if (take > HREAD_CAP) {
                take = HREAD_CAP;
            }
            rc = hread_exact(r, slice, take);
            if (rc != 0) {
                return hread_err(rc);
            }
            conn_tracker_note_chunk(req->tracker, take);
            if (req->on_chunk != NULL) {
                req->on_chunk(slice, take, req->user_data);
            }
            remaining -= take;
            maybe_status(r, req);
        }
        uint8_t crlf[2];
        rc = hread_exact(r, crlf, 2U);
        if (rc != 0) {
            return hread_err(rc);
        }
        if (crlf[0] != '\r' || crlf[1] != '\n') {
            return HTTP_ERR_PARSE;
        }
    }
    /* After the 0-chunk: optional trailer headers, ending with a blank line. */
    int i;
    for (i = 0; i < MAX_TRAILER_LINES; i++) {
        uint8_t tl[256];
        size_t tlen = 0U;
        int rc = hread_line(r, tl, sizeof tl, &tlen);
        if (rc != 0) {
            return hread_err(rc);
        }
        if (tlen == 0U) {
            break; /* trailers done */
        }
    }
    return HTTP_ERR_NONE;
}

/**
 * @brief Read a plain body until the peer closes the connection.
 *
 * @param[in,out] r   Reader.
 * @param[in]     req Request context (tracker + callbacks).
 * @return HTTP_ERR_NONE on success or the failing http_err_t.
 */
static http_err_t read_until_close(struct hread *r, const http_request_t *req)
{
    uint8_t slice[HREAD_CAP];
    for (;;) {
        if (r->start >= r->end) {
            int rc = hread_fill(r);
            if (rc == -1) {
                return HTTP_ERR_NONE; /* EOF is the normal end here */
            }
            if (rc != 0) {
                return hread_err(rc);
            }
        }
        size_t avail = r->end - r->start;
        (void)memcpy(slice, r->buf + r->start, avail);
        r->start = r->end;
        conn_tracker_note_chunk(req->tracker, avail);
        if (req->on_chunk != NULL) {
            req->on_chunk(slice, avail, req->user_data);
        }
        maybe_status(r, req);
    }
}

/**
 * @brief Open a TCP connection to the first reachable address.
 *
 * @param[in] host Hostname or literal address.
 * @param[in] port TCP port.
 * @return Socket fd, or -1 on failure.
 */
static int open_connection(const char *host, uint16_t port)
{
    char port_str[8];
    int n = snprintf(port_str, sizeof port_str, "%u", (unsigned)port);
    if (n < 0 || (size_t)n >= sizeof port_str) {
        return -1;
    }
    struct addrinfo hints;
    (void)memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *res = NULL;
    int rc = getaddrinfo(host, port_str, &hints, &res);
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
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) {
            break;
        }
        (void)close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

/**
 * @brief Send the GET request.
 *
 * @param[in] fd   Socket.
 * @param[in] host Host header value.
 * @param[in] port Port for the Host header.
 * @param[in] path Request target.
 * @return 0 on success, -1 on failure.
 */
static int send_request(int fd, const char *host, uint16_t port,
                        const char *path)
{
    char req[1024];
    int n = snprintf(req, sizeof req,
                     "GET %s HTTP/1.1\r\n"
                     "Host: %s:%u\r\n"
                     "Accept: text/event-stream\r\n"
                     "User-Agent: cog-http-poc/0.1\r\n"
                     "Connection: close\r\n"
                     "\r\n",
                     path, host, (unsigned)port);
    if (n < 0 || (size_t)n >= sizeof req) {
        return -1;
    }
    size_t off = 0U;
    while (off < (size_t)n) {
        ssize_t w = send(fd, req + off, (size_t)n - off, 0);
        if (w < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        off += (size_t)w;
    }
    return 0;
}

http_result_t hand_request(const http_request_t *req)
{
    http_result_t res;
    (void)memset(&res, 0, sizeof res);
    if (req == NULL || req->url == NULL || req->cfg == NULL ||
        req->tracker == NULL) {
        res.err = HTTP_ERR_PARSE;
        res.state = CONN_STATE_ERROR;
        return res;
    }

    char host[256];
    uint16_t port = 80U;
    const char *path = NULL;
    if (http_split_url(req->url, host, sizeof host, &port, &path) != 0) {
        res.err = HTTP_ERR_PARSE;
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

        int fd = open_connection(host, port);
        if (fd < 0) {
            res.err = HTTP_ERR_NET;
            conn_tracker_set_state(req->tracker, CONN_STATE_ERROR);
            break;
        }
        if (send_request(fd, host, port, path) != 0) {
            (void)close(fd);
            res.err = HTTP_ERR_NET;
            conn_tracker_set_state(req->tracker, CONN_STATE_ERROR);
            break;
        }

        struct hread reader;
        (void)memset(&reader, 0, sizeof reader);
        reader.fd = fd;
        reader.stall_ms = req->cfg->stall_ms;

        uint8_t head[HEAD_CAP];
        size_t head_len = 0U;
        int rc = recv_head(&reader, head, sizeof head, &head_len);
        if (rc == -2) {
            (void)close(fd);
            res.err = HTTP_ERR_STALL;
            conn_tracker_set_state(req->tracker, CONN_STATE_STALLED);
            break;
        }
        if (rc != 0) {
            (void)close(fd);
            res.err = HTTP_ERR_NET;
            conn_tracker_set_state(req->tracker, CONN_STATE_ERROR);
            break;
        }
        int status = 0;
        bool chunked = false;
        if (parse_head(head, head_len, &status, &chunked) != 0) {
            (void)close(fd);
            res.err = HTTP_ERR_PARSE;
            conn_tracker_set_state(req->tracker, CONN_STATE_ERROR);
            break;
        }
        res.status = status;

        if (status >= 500) {
            (void)close(fd);
            res.err = HTTP_ERR_RETRYABLE;
            if (attempt < req->cfg->max_retries) {
                continue; /* transient failure: retry */
            }
            conn_tracker_set_state(req->tracker, CONN_STATE_ERROR);
            break;
        }
        if (status >= 400) {
            (void)close(fd);
            res.err = HTTP_ERR_CLIENT;
            conn_tracker_set_state(req->tracker, CONN_STATE_ERROR);
            break;
        }

        conn_tracker_set_state(req->tracker, CONN_STATE_STREAMING);
        http_err_t berr;
        if (chunked) {
            berr = read_chunked_body(&reader, req);
        } else {
            berr = read_until_close(&reader, req);
        }
        (void)close(fd);
        if (berr != HTTP_ERR_NONE) {
            res.err = berr;
            if (berr == HTTP_ERR_STALL) {
                conn_tracker_set_state(req->tracker, CONN_STATE_STALLED);
            } else {
                conn_tracker_set_state(req->tracker, CONN_STATE_ERROR);
            }
            break;
        }
        conn_tracker_set_state(req->tracker, CONN_STATE_DONE);
        res.err = HTTP_ERR_NONE;
        break;
    }

    res.bytes = conn_tracker_bytes(req->tracker);
    res.chunks = conn_tracker_chunks(req->tracker);
    res.state = conn_tracker_state(req->tracker);
    return res;
}

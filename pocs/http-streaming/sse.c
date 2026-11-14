/**
 * @file sse.c
 * @brief Incremental SSE parser implementation (see sse.h).
 *
 * Two fixed buffers are used: one for the current line (fields arrive one
 * line at a time) and one for the payload of the in-progress event. Both
 * are allocated explicitly at create time and freed by destroy — no hidden
 * allocations.
 */

#include "sse.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** Default per-event payload cap when the caller passes 0. */
#define SSE_DEFAULT_MAX_EVENT 4096U

struct sse_parser {
    uint8_t *line;   /**< Current line buffer (nul-terminated). */
    size_t line_len; /**< Bytes in line. */
    uint8_t *event;  /**< Accumulated payload of the in-progress event. */
    size_t event_len; /**< Bytes in event. */
    size_t max_event; /**< Maximum payload size in bytes. */
    bool truncated;   /**< Current event hit the size cap. */
};

/**
 * @brief Append payload bytes to the in-progress event.
 *
 * @param[in,out] p    Parser.
 * @param[in]     data Bytes to append.
 * @param[in]     len  Byte count.
 */
static void sse_append(struct sse_parser *p, const uint8_t *data, size_t len)
{
    size_t i;
    for (i = 0U; i < len; i++) {
        if (p->event_len < p->max_event) {
            p->event[p->event_len++] = data[i];
        } else {
            p->truncated = true;
            break;
        }
    }
}

/**
 * @brief Emit the accumulated event, if any, and reset the accumulator.
 *
 * @param[in,out] p  Parser.
 * @param[in]     cb Event callback.
 * @param[in]     ud User data.
 */
static void sse_emit(struct sse_parser *p, sse_event_fn cb, void *ud)
{
    if (p->event_len > 0U && cb != NULL) {
        p->event[p->event_len] = '\0';
        cb((const char *)p->event, p->event_len, ud);
    }
    p->event_len = 0U;
    p->truncated = false;
}

/**
 * @brief True if a line is a `data` field.
 *
 * Per the SSE spec a field name is terminated by ':' (or end of line); the
 * value may be prefixed by a single leading space.
 *
 * @param[in] line Line bytes (not nul-terminated).
 * @param[in] n    Line length.
 */
static bool sse_is_data_field(const uint8_t *line, size_t n)
{
    if (n < 4U) {
        return false;
    }
    if (line[0] != 'd' || line[1] != 'a' || line[2] != 't' || line[3] != 'a') {
        return false;
    }
    if (n == 4U) {
        return true; /* bare "data" => empty value */
    }
    return line[4] == ':' || line[4] == ' ';
}

/**
 * @brief Process one complete line (without its trailing '\n').
 *
 * A blank line terminates the in-progress event; a `data:` line appends to
 * it; comments and other fields are ignored.
 *
 * @param[in,out] p  Parser.
 * @param[in]     cb Event callback.
 * @param[in]     ud User data.
 */
static void sse_process_line(struct sse_parser *p, sse_event_fn cb, void *ud)
{
    size_t n = p->line_len;
    if (n > 0U && p->line[n - 1U] == '\r') {
        n--; /* tolerate CRLF line endings */
    }
    if (n == 0U) {
        sse_emit(p, cb, ud);
        return;
    }
    if (p->line[0] == ':') {
        return; /* SSE comment line */
    }
    if (sse_is_data_field(p->line, n)) {
        size_t off = 4U; /* past "data" */
        if (off < n && p->line[off] == ':') {
            off++;
        }
        if (off < n && p->line[off] == ' ') {
            off++; /* strip one optional leading space */
        }
        sse_append(p, p->line + off, n - off);
    }
    /* Other fields (event:, id:, retry:) are irrelevant for this PoC. */
}

sse_parser_t *sse_parser_create(size_t max_event)
{
    if (max_event == 0U) {
        max_event = SSE_DEFAULT_MAX_EVENT;
    }
    if (max_event > (size_t)-2U) {
        return NULL; /* capacity computation below would overflow */
    }
    sse_parser_t *p = calloc(1U, sizeof *p);
    if (p == NULL) {
        return NULL;
    }
    size_t cap = max_event + 2U; /* +2 headroom for the terminators */
    p->line = malloc(cap);
    p->event = malloc(cap);
    if (p->line == NULL || p->event == NULL) {
        free(p->line);
        free(p->event);
        free(p);
        return NULL;
    }
    p->max_event = max_event;
    return p;
}

void sse_parser_destroy(sse_parser_t *p)
{
    if (p == NULL) {
        return;
    }
    free(p->line);
    free(p->event);
    free(p);
}

void sse_parser_feed(sse_parser_t *p, const uint8_t *data, size_t len,
                     sse_event_fn cb, void *ud)
{
    size_t i;
    if (p == NULL || data == NULL) {
        return;
    }
    for (i = 0U; i < len; i++) {
        uint8_t c = data[i];
        if (c == '\n') {
            sse_process_line(p, cb, ud);
            p->line_len = 0U;
        } else if (p->line_len < p->max_event) {
            p->line[p->line_len++] = c;
        } else {
            p->truncated = true; /* line longer than the cap; drop excess */
        }
    }
}

void sse_parser_finish(sse_parser_t *p, sse_event_fn cb, void *ud)
{
    if (p == NULL) {
        return;
    }
    if (p->line_len > 0U) {
        sse_process_line(p, cb, ud);
        p->line_len = 0U;
    }
    sse_emit(p, cb, ud);
}

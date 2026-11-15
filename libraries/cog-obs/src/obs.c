/**
 * @file obs.c
 * @brief Structured logging, trace spans, and connection diagnostics.
 *
 * Implements the full public contract of @c cog/obs.h on top of the
 * value types in @c cog/obs_types.h.  Design constraints honoured here:
 *
 *  - **Caller-allocated context.**  @c cog_obs_new allocates one heap
 *    block holding the context header, the diagnostic table, and the
 *    span stack; @c cog_obs_free releases it.  There is no global
 *    instance and no implicit initialisation.
 *  - **No allocation on the hot path.**  Logging formats into a
 *    context-owned scratch buffer; span and diagnostic calls touch only
 *    preallocated arrays.
 *  - **Static sinks.**  Delivery targets come from a static const table
 *    supplied at construction; there is no runtime registration.
 *  - **Fail closed.**  Every public function returns @c cog_result_t.  A
 *    full diagnostic table, an over-long record, or a rejecting sink is
 *    reported to the caller, never silently swallowed.
 *
 * Ticket: 7f5b166 (Stage 12.3b — Implement obs.c)
 */
#define _POSIX_C_SOURCE 200809L

#include "cog/obs.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Private constants and types                                         */
/* ------------------------------------------------------------------ */

/** Default diagnostic table capacity when @c diag_capacity is zero. */
#define COG_OBS_DEFAULT_DIAG_CAPACITY 16U

/** Default span stack capacity when @c span_capacity is zero. */
#define COG_OBS_DEFAULT_SPAN_CAPACITY 32U

/** Size of the context-owned record scratch buffer, in bytes. */
#define COG_OBS_SCRATCH_CAPACITY 1024U

/** Number of connection states; kept in sync with the enum. */
#define COG_OBS_CONN_STATE_COUNT 6

/** Number of connection event kinds; kept in sync with the enum. */
#define COG_OBS_CONN_EVENT_KIND_COUNT 7

/**
 * @brief One entry of the context-owned span stack.
 */
typedef struct cog_obs_span_entry {
    cog_obs_span_id_t id;     /**< Span id returned to the caller. */
    cog_obs_span_id_t parent; /**< Enclosing span id, or NONE. */
    const char *name;         /**< Borrowed span name. */
    uint64_t start_ns;        /**< Clock reading at begin. */
} cog_obs_span_entry_t;

/**
 * @brief Bounded text writer used for record and diagnostic formatting.
 *
 * Tracks the required length even after the buffer is exhausted, so an
 * overflow can report the exact capacity a retry needs.  The buffer is
 * always kept NUL-terminated while @c len < @c cap.
 */
typedef struct cog_obs_writer {
    char *buf;    /**< Destination buffer. */
    size_t cap;   /**< Capacity including the NUL terminator. */
    size_t len;   /**< Bytes written, excluding the terminator. */
    int overflow; /**< Non-zero once the text no longer fits. */
} cog_obs_writer_t;

/** Lowercase connection-state names, indexed by @c cog_obs_conn_state_t. */
static const char *const cog_obs_conn_state_names[COG_OBS_CONN_STATE_COUNT] = {
    "idle", "connecting", "active", "stalled", "closed", "failed",
};

/**
 * @brief The opaque context (definition of @c cog_obs_t).
 *
 * All mutable state lives here; there is no global instance.  The
 * diagnostic table (@p diag) and the span stack (@p spans) are
 * allocated in the same heap block as the header, immediately after the
 * scratch buffer.
 */
struct cog_obs {
    cog_obs_severity_t min_severity;      /**< Context filter threshold. */
    const cog_obs_field_table_t *fields;  /**< Static field table. */
    const cog_obs_sink_table_t *sinks;    /**< Static sink table. */
    cog_obs_trace_sink_fn trace_sink;     /**< Span receiver; may be NULL. */
    void *trace_user;                     /**< Handle for @p trace_sink. */
    cog_obs_conn_event_sink_fn conn_event_sink; /**< Event receiver. */
    void *conn_event_user;                /**< Handle for @p conn_event_sink. */
    const char *service;                  /**< Service name; may be NULL. */
    cog_result_t (*clock_ns)(uint64_t *out_now_ns); /**< Time source. */

    size_t diag_capacity;                 /**< Number of diag slots. */
    size_t diag_count;                    /**< Occupied diag slots. */
    cog_obs_conn_snapshot_t *diag;        /**< Diag table; into the block. */

    size_t span_capacity;                 /**< Maximum open spans. */
    size_t span_depth;                    /**< Currently open spans. */
    uint64_t next_span_id;                /**< Next span id to hand out. */
    cog_obs_span_entry_t *spans;          /**< Span stack; into the block. */

    char scratch[COG_OBS_SCRATCH_CAPACITY]; /**< Record scratch buffer. */
};

/* ------------------------------------------------------------------ */
/* Bounded writer                                                      */
/* ------------------------------------------------------------------ */

static void cog_obs_writer_init(cog_obs_writer_t *w, char *buf, size_t cap)
{
    w->buf = buf;
    w->cap = cap;
    w->len = 0U;
    w->overflow = 0;
    if (cap > 0U) {
        buf[0] = '\0';
    }
}

static void cog_obs_writer_putc(cog_obs_writer_t *w, char c)
{
    if (w->overflow) {
        w->len += 1U;
        return;
    }
    if (w->len + 1U >= w->cap) {
        w->overflow = 1;
        w->len += 1U;
        return;
    }
    w->buf[w->len] = c;
    w->len += 1U;
    w->buf[w->len] = '\0';
}

static void cog_obs_writer_puts(cog_obs_writer_t *w, const char *s)
{
    while (*s != '\0') {
        cog_obs_writer_putc(w, *s);
        ++s;
    }
}

/** Append an unsigned 64-bit value in decimal. */
static void cog_obs_writer_put_u64(cog_obs_writer_t *w, uint64_t v)
{
    char tmp[24];
    int n;

    n = snprintf(tmp, sizeof(tmp), "%llu", (unsigned long long)v);
    if (n > 0) {
        cog_obs_writer_puts(w, tmp);
    }
}

/** Append a signed 64-bit value in decimal. */
static void cog_obs_writer_put_i64(cog_obs_writer_t *w, int64_t v)
{
    char tmp[24];
    int n;

    n = snprintf(tmp, sizeof(tmp), "%lld", (long long)v);
    if (n > 0) {
        cog_obs_writer_puts(w, tmp);
    }
}

/** Append an unsigned 32-bit value in decimal. */
static void cog_obs_writer_put_u32(cog_obs_writer_t *w, uint32_t v)
{
    char tmp[16];
    int n;

    n = snprintf(tmp, sizeof(tmp), "%u", (unsigned)v);
    if (n > 0) {
        cog_obs_writer_puts(w, tmp);
    }
}

/** Append a signed int in decimal. */
static void cog_obs_writer_put_int(cog_obs_writer_t *w, int v)
{
    char tmp[16];
    int n;

    n = snprintf(tmp, sizeof(tmp), "%d", v);
    if (n > 0) {
        cog_obs_writer_puts(w, tmp);
    }
}

/** Append a double with six fractional digits. */
static void cog_obs_writer_put_double6(cog_obs_writer_t *w, double v)
{
    char tmp[384];
    int n;

    n = snprintf(tmp, sizeof(tmp), "%.6f", v);
    if (n > 0) {
        cog_obs_writer_puts(w, tmp);
    }
}

/**
 * @brief Append a string quoted and escaped.
 *
 * Escapes `"` as `\"`, `\` as `\\`, and the control characters newline,
 * carriage return, and tab as `\n`, `\r`, `\t`.
 */
static void cog_obs_writer_put_escaped(cog_obs_writer_t *w, const char *s)
{
    cog_obs_writer_putc(w, '"');
    while (*s != '\0') {
        switch (*s) {
        case '"':
            cog_obs_writer_puts(w, "\\\"");
            break;
        case '\\':
            cog_obs_writer_puts(w, "\\\\");
            break;
        case '\n':
            cog_obs_writer_puts(w, "\\n");
            break;
        case '\r':
            cog_obs_writer_puts(w, "\\r");
            break;
        case '\t':
            cog_obs_writer_puts(w, "\\t");
            break;
        default:
            cog_obs_writer_putc(w, *s);
            break;
        }
        ++s;
    }
    cog_obs_writer_putc(w, '"');
}

/** Append one field value according to its kind. */
static void cog_obs_writer_put_field_value(cog_obs_writer_t *w,
                                           const cog_obs_field_t *field)
{
    switch (field->kind) {
    case COG_OBS_FIELD_STRING:
        cog_obs_writer_put_escaped(w, field->value.string);
        break;
    case COG_OBS_FIELD_INT:
        cog_obs_writer_put_i64(w, field->value.integer);
        break;
    case COG_OBS_FIELD_UINT:
        cog_obs_writer_put_u64(w, field->value.unsigned_integer);
        break;
    case COG_OBS_FIELD_BOOL:
        cog_obs_writer_puts(w, field->value.boolean ? "true" : "false");
        break;
    case COG_OBS_FIELD_DOUBLE:
        cog_obs_writer_put_double6(w, field->value.real);
        break;
    default:
        /* Unreachable: kinds are validated before formatting. */
        cog_obs_writer_puts(w, "?");
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Shared validation and formatting helpers                            */
/* ------------------------------------------------------------------ */

/**
 * @brief Validate a field array against the context's field table.
 *
 * Checks the NULL-with-nonzero-count rule, index bounds, kind
 * agreement with the descriptor, and non-NULL string values.
 */
static cog_result_t cog_obs_validate_fields(const cog_obs_t *obs,
                                            const cog_obs_field_t *fields,
                                            size_t field_count)
{
    size_t i;

    if (field_count == 0U) {
        return COG_OK;
    }
    if (fields == NULL) {
        return COG_ERROR_INVALID_ARG;
    }
    if (obs->fields == NULL || obs->fields->fields == NULL) {
        return COG_ERROR_INVALID_ARG;
    }
    for (i = 0U; i < field_count; ++i) {
        const cog_obs_field_desc_t *desc;
        const cog_obs_field_t *f = &fields[i];

        if ((size_t)f->index >= obs->fields->count) {
            return COG_ERROR_INVALID_ARG;
        }
        desc = &obs->fields->fields[f->index];
        if (f->kind != desc->kind) {
            return COG_ERROR_INVALID_ARG;
        }
        if (f->kind == COG_OBS_FIELD_STRING && f->value.string == NULL) {
            return COG_ERROR_INVALID_ARG;
        }
    }
    return COG_OK;
}

/**
 * @brief Format one record as `timestamp severity [service] message
 *        key=value ...` into @p buf.
 *
 * On overflow the writer keeps counting, so @p *out_len receives the
 * required capacity (excluding the terminator) and @c COG_ERROR_OVERFLOW
 * is returned.
 */
static cog_result_t cog_obs_format_record(const cog_obs_t *obs,
                                          cog_obs_severity_t severity,
                                          const char *message,
                                          const cog_obs_field_t *fields,
                                          size_t field_count,
                                          char *buf,
                                          size_t cap,
                                          size_t *out_len)
{
    cog_obs_writer_t w;
    const char *sev_name = NULL;
    uint64_t now_ns = 0U;
    size_t i;

    if (obs->clock_ns(&now_ns) != COG_OK) {
        return COG_ERROR_IO;
    }
    if (cog_obs_severity_name(severity, &sev_name) != COG_OK) {
        return COG_ERROR_INVALID_ARG;
    }

    cog_obs_writer_init(&w, buf, cap);
    cog_obs_writer_put_u64(&w, now_ns);
    cog_obs_writer_putc(&w, ' ');
    cog_obs_writer_puts(&w, sev_name);
    if (obs->service != NULL) {
        cog_obs_writer_putc(&w, ' ');
        cog_obs_writer_puts(&w, obs->service);
    }
    cog_obs_writer_putc(&w, ' ');
    cog_obs_writer_puts(&w, message);
    for (i = 0U; i < field_count; ++i) {
        const cog_obs_field_t *f = &fields[i];
        const cog_obs_field_desc_t *desc = &obs->fields->fields[f->index];

        cog_obs_writer_putc(&w, ' ');
        cog_obs_writer_puts(&w, desc->name);
        cog_obs_writer_putc(&w, '=');
        cog_obs_writer_put_field_value(&w, f);
    }
    if (out_len != NULL) {
        *out_len = w.len;
    }
    return w.overflow ? COG_ERROR_OVERFLOW : COG_OK;
}

/**
 * @brief Offer a formatted record to every sink whose threshold admits
 *        it.
 *
 * All eligible sinks are offered the record even if one rejects it; the
 * first rejection turns the return value into @c COG_ERROR_IO.
 */
static cog_result_t cog_obs_deliver(const cog_obs_t *obs,
                                    cog_obs_severity_t severity,
                                    const char *record,
                                    size_t record_len)
{
    cog_result_t rc = COG_OK;
    size_t i;

    if (obs->sinks == NULL) {
        return COG_OK;
    }
    for (i = 0U; i < obs->sinks->count; ++i) {
        const cog_obs_sink_t *sink = &obs->sinks->sinks[i];
        cog_result_t r;

        /* cog_obs_new rejects sinks without a deliver callback, so this
         * loop only ever sees non-NULL callbacks. */
        if ((int)severity < (int)sink->min_severity) {
            continue; /* Record more verbose than this sink accepts. */
        }
        r = sink->deliver(sink->user, record, record_len);
        if (r != COG_OK) {
            rc = COG_ERROR_IO;
        }
    }
    return rc;
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

cog_result_t cog_obs_new(cog_obs_t **out, const cog_obs_config_t *config)
{
    struct cog_obs *obs;
    size_t diag_capacity;
    size_t span_capacity;
    size_t header_bytes;
    size_t diag_bytes;
    size_t span_bytes;
    size_t total;

    if (out == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    *out = NULL;
    if (config == NULL) {
        return COG_ERROR_NULL_PTR;
    }

    if ((int)config->min_severity < 0 ||
        (int)config->min_severity >= COG_OBS_SEVERITY_COUNT) {
        return COG_ERROR_INVALID_ARG;
    }
    if (config->fields != NULL && config->fields->count > 0U &&
        config->fields->fields == NULL) {
        return COG_ERROR_INVALID_ARG;
    }
    if (config->sinks != NULL && config->sinks->count > 0U &&
        config->sinks->sinks == NULL) {
        return COG_ERROR_INVALID_ARG;
    }
    /* Fail closed on a misconfigured sink: a configured sink must have a
     * deliver callback.  The header documents this rejection; without it
     * a NULL callback would silently drop every record. */
    if (config->sinks != NULL) {
        size_t sink_i;

        for (sink_i = 0U; sink_i < config->sinks->count; ++sink_i) {
            if (config->sinks->sinks[sink_i].deliver == NULL) {
                return COG_ERROR_INVALID_ARG;
            }
        }
    }

    diag_capacity = (config->diag_capacity == 0U)
                        ? COG_OBS_DEFAULT_DIAG_CAPACITY
                        : config->diag_capacity;
    span_capacity = (config->span_capacity == 0U)
                        ? COG_OBS_DEFAULT_SPAN_CAPACITY
                        : config->span_capacity;

    /* Single-block layout: header + diag table + span stack.  The size
     * arithmetic must not wrap; a request that cannot be sized is
     * reported as an allocation failure. */
    header_bytes = sizeof(struct cog_obs);
    if (diag_capacity >
        (SIZE_MAX - header_bytes) / sizeof(cog_obs_conn_snapshot_t)) {
        return COG_ERROR_NOMEM;
    }
    diag_bytes = diag_capacity * sizeof(cog_obs_conn_snapshot_t);
    if (span_capacity >
        (SIZE_MAX - header_bytes - diag_bytes) / sizeof(cog_obs_span_entry_t)) {
        return COG_ERROR_NOMEM;
    }
    span_bytes = span_capacity * sizeof(cog_obs_span_entry_t);
    total = header_bytes + diag_bytes + span_bytes;

    obs = (struct cog_obs *)malloc(total);
    if (obs == NULL) {
        return COG_ERROR_NOMEM;
    }
    memset(obs, 0, total);

    obs->min_severity = config->min_severity;
    obs->fields = config->fields;
    obs->sinks = config->sinks;
    obs->trace_sink = config->trace_sink;
    obs->trace_user = config->trace_user;
    obs->conn_event_sink = config->conn_event_sink;
    obs->conn_event_user = config->conn_event_user;
    obs->service = config->service;
    obs->clock_ns = (config->clock_ns != NULL) ? config->clock_ns
                                               : cog_obs_clock_now_ns;
    obs->diag_capacity = diag_capacity;
    obs->diag_count = 0U;
    obs->diag = (cog_obs_conn_snapshot_t *)(void *)((char *)obs + header_bytes);
    obs->span_capacity = span_capacity;
    obs->span_depth = 0U;
    obs->next_span_id = 1U;
    obs->spans = (cog_obs_span_entry_t *)(void *)((char *)obs->diag + diag_bytes);

    *out = obs;
    return COG_OK;
}

void cog_obs_free(cog_obs_t *obs)
{
    free(obs);
}

/* ------------------------------------------------------------------ */
/* Structured logging                                                  */
/* ------------------------------------------------------------------ */

cog_result_t cog_obs_log(cog_obs_t *obs,
                         cog_obs_severity_t severity,
                         const char *message,
                         const cog_obs_field_t *fields,
                         size_t field_count)
{
    size_t len = 0U;
    cog_result_t rc;

    if (obs == NULL || message == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    if ((int)severity < 0 || (int)severity >= COG_OBS_SEVERITY_COUNT) {
        return COG_ERROR_INVALID_ARG;
    }
    rc = cog_obs_validate_fields(obs, fields, field_count);
    if (rc != COG_OK) {
        return rc;
    }
    if ((int)severity < (int)obs->min_severity) {
        return COG_OK; /* More verbose than the context threshold. */
    }
    rc = cog_obs_format_record(obs, severity, message, fields, field_count,
                               obs->scratch, sizeof(obs->scratch), &len);
    if (rc != COG_OK) {
        return rc;
    }
    return cog_obs_deliver(obs, severity, obs->scratch, len);
}

cog_result_t cog_obs_log_to(cog_obs_t *obs,
                            cog_obs_severity_t severity,
                            const char *message,
                            const cog_obs_field_t *fields,
                            size_t field_count,
                            char *out_buf,
                            size_t buf_capacity,
                            size_t *out_len)
{
    size_t len = 0U;
    cog_result_t rc;

    if (obs == NULL || message == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    if (out_buf == NULL && buf_capacity != 0U) {
        return COG_ERROR_NULL_PTR;
    }
    if ((int)severity < 0 || (int)severity >= COG_OBS_SEVERITY_COUNT) {
        return COG_ERROR_INVALID_ARG;
    }
    rc = cog_obs_validate_fields(obs, fields, field_count);
    if (rc != COG_OK) {
        return rc;
    }
    if ((int)severity < (int)obs->min_severity) {
        if (out_buf != NULL && buf_capacity > 0U) {
            out_buf[0] = '\0';
        }
        if (out_len != NULL) {
            *out_len = 0U;
        }
        return COG_OK;
    }
    rc = cog_obs_format_record(obs, severity, message, fields, field_count,
                               obs->scratch, sizeof(obs->scratch), &len);
    if (rc != COG_OK) {
        if (out_len != NULL) {
            *out_len = len;
        }
        return rc;
    }
    if (out_buf != NULL) {
        if (len + 1U > buf_capacity) {
            if (out_len != NULL) {
                *out_len = len;
            }
            return COG_ERROR_OVERFLOW;
        }
        memcpy(out_buf, obs->scratch, len + 1U);
    }
    if (out_len != NULL) {
        *out_len = len;
    }
    return cog_obs_deliver(obs, severity, obs->scratch, len);
}

cog_result_t cog_obs_enabled(const cog_obs_t *obs,
                             cog_obs_severity_t severity,
                             int *out_enabled)
{
    if (obs == NULL || out_enabled == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    if ((int)severity < 0 || (int)severity >= COG_OBS_SEVERITY_COUNT) {
        return COG_ERROR_INVALID_ARG;
    }
    *out_enabled = ((int)severity >= (int)obs->min_severity) ? 1 : 0;
    return COG_OK;
}

/* ------------------------------------------------------------------ */
/* Trace spans                                                         */
/* ------------------------------------------------------------------ */

cog_result_t cog_obs_trace_begin(cog_obs_t *obs,
                                 const char *name,
                                 cog_obs_span_id_t *out_id)
{
    cog_obs_span_entry_t *entry;
    cog_obs_span_id_t id;
    uint64_t now_ns = 0U;

    if (obs == NULL || name == NULL || out_id == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    if (obs->span_depth >= obs->span_capacity) {
        return COG_ERROR_OVERFLOW;
    }
    id = obs->next_span_id;
    if (id == COG_OBS_SPAN_ID_NONE) {
        return COG_ERROR_OVERFLOW; /* Id space exhausted. */
    }
    obs->next_span_id = id + 1U;
    if (obs->clock_ns(&now_ns) != COG_OK) {
        return COG_ERROR_IO;
    }
    entry = &obs->spans[obs->span_depth];
    entry->id = id;
    entry->parent = (obs->span_depth > 0U)
                        ? obs->spans[obs->span_depth - 1U].id
                        : COG_OBS_SPAN_ID_NONE;
    entry->name = name;
    entry->start_ns = now_ns;
    obs->span_depth += 1U;
    *out_id = id;
    return COG_OK;
}

cog_result_t cog_obs_trace_end(cog_obs_t *obs,
                               cog_obs_span_id_t id,
                               cog_result_t status)
{
    cog_obs_span_record_t record;
    cog_obs_span_entry_t entry;
    uint64_t now_ns = 0U;
    size_t i;
    cog_result_t rc;

    if (obs == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    if (obs->span_depth == 0U) {
        return COG_ERROR_NOT_FOUND;
    }
    if (obs->spans[obs->span_depth - 1U].id != id) {
        for (i = 0U; i < obs->span_depth; ++i) {
            if (obs->spans[i].id == id) {
                return COG_ERROR_INVALID_ARG; /* Open but not innermost. */
            }
        }
        return COG_ERROR_NOT_FOUND;
    }
    if (obs->clock_ns(&now_ns) != COG_OK) {
        return COG_ERROR_IO;
    }
    entry = obs->spans[obs->span_depth - 1U];
    obs->span_depth -= 1U;

    record.id = entry.id;
    record.parent = entry.parent;
    record.name = entry.name;
    record.start_ns = entry.start_ns;
    record.duration_ns = now_ns - entry.start_ns;
    record.status = status;
    record.ok = (status == COG_OK) ? 1 : 0;

    if (obs->trace_sink != NULL) {
        rc = obs->trace_sink(obs->trace_user, &record);
        if (rc != COG_OK) {
            /* Restore the span so the stack is unchanged on failure and
             * the caller may retry the end. */
            obs->spans[obs->span_depth] = entry;
            obs->span_depth += 1U;
            return COG_ERROR_IO;
        }
    }
    return COG_OK;
}

cog_result_t cog_obs_trace_current(const cog_obs_t *obs,
                                   cog_obs_span_id_t *out_id)
{
    if (obs == NULL || out_id == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    *out_id = (obs->span_depth > 0U)
                  ? obs->spans[obs->span_depth - 1U].id
                  : COG_OBS_SPAN_ID_NONE;
    return COG_OK;
}

cog_result_t cog_obs_trace_depth(const cog_obs_t *obs, size_t *out_depth)
{
    if (obs == NULL || out_depth == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    *out_depth = obs->span_depth;
    return COG_OK;
}

/* ------------------------------------------------------------------ */
/* Connection diagnostics                                              */
/* ------------------------------------------------------------------ */

cog_result_t cog_obs_diag_publish(cog_obs_t *obs,
                                  const cog_obs_conn_snapshot_t *snapshot)
{
    size_t i;
    size_t free_slot;

    if (obs == NULL || snapshot == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    free_slot = obs->diag_capacity;
    if (snapshot->conn_id == 0U) {
        return COG_ERROR_INVALID_ARG;
    }
    if ((int)snapshot->state < 0 ||
        (int)snapshot->state >= COG_OBS_CONN_STATE_COUNT) {
        return COG_ERROR_INVALID_ARG;
    }
    for (i = 0U; i < obs->diag_capacity; ++i) {
        if (obs->diag[i].conn_id == snapshot->conn_id) {
            obs->diag[i] = *snapshot; /* Update in place. */
            return COG_OK;
        }
        if (obs->diag[i].conn_id == 0U && free_slot == obs->diag_capacity) {
            free_slot = i; /* First free slot, in slot order. */
        }
    }
    if (free_slot == obs->diag_capacity) {
        return COG_ERROR_OVERFLOW;
    }
    obs->diag[free_slot] = *snapshot;
    obs->diag_count += 1U;
    return COG_OK;
}

cog_result_t cog_obs_diag_clear(cog_obs_t *obs, uint64_t conn_id)
{
    size_t i;

    if (obs == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    if (conn_id == 0U) {
        return COG_ERROR_INVALID_ARG;
    }
    for (i = 0U; i < obs->diag_capacity; ++i) {
        if (obs->diag[i].conn_id == conn_id) {
            memset(&obs->diag[i], 0, sizeof(obs->diag[i]));
            obs->diag_count -= 1U;
            return COG_OK;
        }
    }
    return COG_ERROR_NOT_FOUND;
}

cog_result_t cog_obs_diag_at(const cog_obs_t *obs,
                             size_t index,
                             cog_obs_conn_snapshot_t *out_snapshot)
{
    size_t i;
    size_t seen = 0U;

    if (obs == NULL || out_snapshot == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    if (index >= obs->diag_count) {
        return COG_ERROR_INVALID_ARG;
    }
    for (i = 0U; i < obs->diag_capacity; ++i) {
        if (obs->diag[i].conn_id == 0U) {
            continue; /* Free slot; not part of the enumeration. */
        }
        if (seen == index) {
            *out_snapshot = obs->diag[i];
            return COG_OK;
        }
        seen += 1U;
    }
    return COG_ERROR_NOT_FOUND; /* Unreachable: index < diag_count. */
}

cog_result_t cog_obs_diag_count(const cog_obs_t *obs, size_t *out_count)
{
    if (obs == NULL || out_count == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    *out_count = obs->diag_count;
    return COG_OK;
}

cog_result_t cog_obs_diag_count_state(const cog_obs_t *obs,
                                      cog_obs_conn_state_t state,
                                      size_t *out_count)
{
    size_t i;
    size_t n = 0U;

    if (obs == NULL || out_count == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    if ((int)state < 0 || (int)state >= COG_OBS_CONN_STATE_COUNT) {
        return COG_ERROR_INVALID_ARG;
    }
    for (i = 0U; i < obs->diag_capacity; ++i) {
        if (obs->diag[i].conn_id != 0U && obs->diag[i].state == state) {
            n += 1U;
        }
    }
    *out_count = n;
    return COG_OK;
}

cog_result_t cog_obs_diag_format(const cog_obs_t *obs,
                                 const cog_obs_conn_snapshot_t *snapshot,
                                 char *out_buf,
                                 size_t buf_capacity,
                                 size_t *out_len)
{
    cog_obs_writer_t w;
    uint64_t now_ns = 0U;
    const char *state_name = NULL;

    if (obs == NULL || snapshot == NULL || out_buf == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    if ((int)snapshot->state < 0 ||
        (int)snapshot->state >= COG_OBS_CONN_STATE_COUNT) {
        return COG_ERROR_INVALID_ARG;
    }
    if (obs->clock_ns(&now_ns) != COG_OK) {
        return COG_ERROR_IO;
    }
    state_name = cog_obs_conn_state_names[(int)snapshot->state];

    cog_obs_writer_init(&w, out_buf, buf_capacity);
    cog_obs_writer_puts(&w, "conn=");
    cog_obs_writer_put_u64(&w, snapshot->conn_id);
    cog_obs_writer_puts(&w, " state=");
    cog_obs_writer_puts(&w, state_name);
    if (snapshot->endpoint != NULL) {
        cog_obs_writer_puts(&w, " endpoint=");
        cog_obs_writer_puts(&w, snapshot->endpoint);
    }
    if (snapshot->provider != NULL) {
        cog_obs_writer_puts(&w, " provider=");
        cog_obs_writer_puts(&w, snapshot->provider);
    }
    cog_obs_writer_puts(&w, " in=");
    cog_obs_writer_put_u64(&w, snapshot->bytes_in);
    cog_obs_writer_puts(&w, " out=");
    cog_obs_writer_put_u64(&w, snapshot->bytes_out);
    cog_obs_writer_puts(&w, " retries=");
    cog_obs_writer_put_u32(&w, snapshot->retries);
    cog_obs_writer_puts(&w, " age_ms=");
    cog_obs_writer_put_u64(&w, (now_ns - snapshot->started_ns) / 1000000U);
    cog_obs_writer_puts(&w, " idle_ms=");
    cog_obs_writer_put_u64(&w,
                           (now_ns - snapshot->last_progress_ns) / 1000000U);
    cog_obs_writer_puts(&w, " status=");
    cog_obs_writer_put_int(&w, (int)snapshot->last_status);

    if (out_len != NULL) {
        *out_len = w.len;
    }
    return w.overflow ? COG_ERROR_OVERFLOW : COG_OK;
}

/* ------------------------------------------------------------------ */
/* Connection event stream (GUI table feed)                            */
/* ------------------------------------------------------------------ */

cog_result_t cog_obs_conn_event_emit(cog_obs_t *obs,
                                     const cog_obs_conn_event_t *event)
{
    cog_result_t rc;

    if (obs == NULL || event == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    if ((int)event->kind < 0 ||
        (int)event->kind >= COG_OBS_CONN_EVENT_KIND_COUNT) {
        return COG_ERROR_INVALID_ARG;
    }
    if (obs->conn_event_sink == NULL) {
        return COG_OK;
    }
    rc = obs->conn_event_sink(obs->conn_event_user, event);
    return (rc == COG_OK) ? COG_OK : COG_ERROR_IO;
}

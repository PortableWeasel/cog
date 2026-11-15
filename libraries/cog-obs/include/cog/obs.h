/**
 * @file obs.h
 * @brief Structured logging, trace spans, and connection diagnostics.
 *
 * cog-obs is the observability layer of a Cog process.  It provides
 * three related capabilities that all feed from one caller-allocated
 * context, @c cog_obs_t:
 *
 * 1. **Structured logging.**  Records carry a severity, a static
 *    message id, and a small set of typed fields drawn from a static
 *    const field table.  Records are formatted once into a
 *    caller-provided buffer and handed to every configured sink.
 *    No allocation happens during a log call.
 * 2. **Trace spans.**  @c cog_obs_trace_begin and @c cog_obs_trace_end
 *    bracket a unit of work while maintaining an explicit span stack
 *    inside the context.  Completed spans are emitted to the trace
 *    sink as @c cog_obs_span_record_t values.  There is no ambient
 *    thread-local state: the context is the only state.
 * 3. **Connection diagnostics.**  A fixed-capacity table of
 *    caller-owned connection snapshots makes in-flight transport
 *    state queryable by the CLI, a UI process, or a health report,
 *    without the transport having to format anything itself.
 *
 * @section obs_design Design constraints
 *
 * - **Caller-allocated context.**  @c cog_obs_t is opaque.  It is
 *   obtained from @c cog_obs_new with a caller-supplied configuration
 *   and destroyed with @c cog_obs_free.  There is no global instance,
 *   no constructor attribute, and no implicit initialisation.
 * - **No allocation on the hot path.**  Logging, span, and diagnostic
 *   calls never allocate.  Buffers are either caller-provided or sized
 *   once at construction.
 * - **Static sink table.**  Sinks are addressed by index into a static
 *   const @c cog_obs_sink_table_t supplied at construction.  Sinks are
 *   never registered or unregistered at runtime.
 * - **Fail closed.**  Every public function returns @c cog_result_t.
 *   A full diagnostic table or an over-long format returns
 *   @c COG_ERROR_OVERFLOW rather than truncating silently.
 * - **Observability must not break the host.**  Logging failures are
 *   reported to the caller but never abort progress in the agent loop
 *   unless the caller chooses to treat them as fatal.
 *
 * @section obs_threading Threading model
 *
 * A @c cog_obs_t instance is **not thread-safe**.  It owns a mutable
 * span stack, an output scratch buffer, and a diagnostic table, all of
 * which are updated without locking.  The intended model matches the
 * rest of Cog: one context per thread or per core process, driven by
 * the single thread that runs @c cog_core_run.
 *
 * Sinks, field tables, and message ids are read-only after
 * construction and may be shared between contexts and threads.
 *
 * If multi-threaded logging is required, the caller either creates one
 * context per thread or serialises access with an external mutex;
 * cog-obs does not provide locking itself.  Sinks must not call back
 * into the same @c cog_obs_t instance.
 */
#ifndef COG_OBS_H
#define COG_OBS_H

#include <cog/obs_types.h>
#include <cog/result.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Opaque observability context.
 *
 * Created with @c cog_obs_new and destroyed with @c cog_obs_free.  All
 * logging, tracing, and diagnostic calls take a pointer to this type.
 */
typedef struct cog_obs cog_obs_t;

/* ------------------------------------------------------------------ */
/* Sinks                                                               */
/* ------------------------------------------------------------------ */

/**
 * @brief Reference to one output destination.
 *
 * A sink receives already-formatted record text; it performs no
 * formatting of its own.  The caller opens the file, socket, or buffer
 * and supplies its handle through @p user: cog-obs never opens, closes,
 * or flushes a destination on its own.
 */
typedef struct cog_obs_sink {
    /**
     * @brief Human-readable sink name, used in diagnostics.
     *
     * Must be NUL-terminated.  Borrowed; must outlive the context.
     */
    const char *name;

    /**
     * @brief Delivery callback.
     *
     * Invoked once per emitted record with the fully formatted record
     * text.  The callback must not retain the buffer past its return
     * and must not re-enter the same @c cog_obs_t.
     *
     * @param[in] user       Opaque pointer from this descriptor.
     * @param[in] record     Formatted record text, NUL-terminated.
     * @param[in] record_len Length of @p record in bytes, excluding the
     *                       NUL terminator.
     * @return @c COG_OK if the record was accepted.  A non-zero value
     *         is recorded as a sink failure and propagated from the
     *         logging call.
     */
    cog_result_t (*deliver)(void *user, const char *record, size_t record_len);

    /**
     * @brief Opaque handle passed to @p deliver; may be NULL.
     *
     * Typically a @c FILE * or a socket descriptor owned by the caller.
     */
    void *user;

    /**
     * @brief Minimum severity this sink accepts.
     *
     * Records more verbose than this are not delivered to this sink.
     */
    cog_obs_severity_t min_severity;
} cog_obs_sink_t;

/**
 * @brief Static const table of sinks.
 *
 * The underlying array must remain valid for the lifetime of every
 * context that references it.
 */
typedef struct cog_obs_sink_table {
    const cog_obs_sink_t *sinks; /**< Pointer to first descriptor; may be
                                      NULL when @p count is zero. */
    size_t count;                /**< Number of descriptors.  May be zero,
                                      in which case records are formatted
                                      but delivered nowhere. */
} cog_obs_sink_table_t;

/* ------------------------------------------------------------------ */
/* Configuration                                                       */
/* ------------------------------------------------------------------ */

/**
 * @brief Callback receiving a completed trace span.
 *
 * Invoked once per span closed by @c cog_obs_trace_end.  The record is
 * a value passed by pointer; string pointers inside it remain valid
 * only for the duration of the call.
 *
 * @param[in] user   Opaque pointer from @c cog_obs_config_t::trace_user.
 * @param[in] span   Completed span record.
 * @return @c COG_OK if the span was accepted.  A non-zero value is
 *         propagated from @c cog_obs_trace_end.
 */
typedef cog_result_t (*cog_obs_trace_sink_fn)(void *user,
                                             const cog_obs_span_record_t *span);

/* Forward declaration for connection event sink callback. */
struct cog_obs_conn_event;
typedef cog_result_t (*cog_obs_conn_event_sink_fn)(void *user,
                                                   const struct cog_obs_conn_event *event);

/**
 * @brief Configuration passed to @c cog_obs_new.
 *
 * Callers should zero-initialise this struct to obtain defaults; adding
 * fields later will not break callers that zero-init.
 */
typedef struct cog_obs_config {
    /**
     * @brief Overall minimum severity to record.
     *
     * Records more verbose than this are discarded before formatting.
     * Zero (@c COG_OBS_TRACE) records everything.
     */
    cog_obs_severity_t min_severity;

    /**
     * @brief Static const field table, or NULL for no fields.
     *
     * Field indices in @c cog_obs_field_t refer into this table.  The
     * table must remain valid for the lifetime of the context.
     */
    const cog_obs_field_table_t *fields;

    /**
     * @brief Static const sink table, or NULL for no sinks.
     *
     * The table must remain valid for the lifetime of the context.
     */
    const cog_obs_sink_table_t *sinks;

    /**
     * @brief Optional receiver for completed trace spans.
     *
     * If NULL, spans are opened and closed normally but their records
     * are discarded.  Tracing therefore costs timestamps and stack
     * pushes only; enabling it is a configuration decision, not a
     * compile-time one.
     */
    cog_obs_trace_sink_fn trace_sink;

    /**
     * @brief Opaque handle passed to @c trace_sink; may be NULL.
     */
    void *trace_user;

    /**
     * @brief Capacity of the context-owned diagnostic table.
     *
     * Zero selects a sensible default.  Each slot holds one connection
     * snapshot; see @c cog_obs_diag_publish.
     */
    size_t diag_capacity;

    /**
     * @brief Maximum number of simultaneously open trace spans.
     *
     * Zero selects a sensible default.  Bounds the context-owned span
     * stack; exceeding it makes @c cog_obs_trace_begin fail with
     * @c COG_ERROR_OVERFLOW rather than grow.
     */
    size_t span_capacity;

    /**
     * @brief Service name emitted with every record.
     *
     * May be NULL, in which case records omit the service attribute.
     * Borrowed; must outlive the context.
     */
    const char *service;

    /**
     * @brief Optional clock override used for record and span timestamps.
     *
     * Must have the semantics of @c cog_obs_clock_now_ns.  If NULL, the
     * context uses @c cog_obs_clock_now_ns.  Supplying a deterministic
     * clock is how tests make timestamps reproducible.
     */
    cog_result_t (*clock_ns)(uint64_t *out_now_ns);

    /**
     * @brief Optional receiver for connection events.
     *
     * If non-NULL, every call to @c cog_obs_conn_event_emit delivers
     * the event to this callback.  This is the feed for a GUI table
     * showing live connection state.
     */
    cog_obs_conn_event_sink_fn conn_event_sink;

    /**
     * @brief Opaque handle passed to @c conn_event_sink; may be NULL.
     */
    void *conn_event_user;
} cog_obs_config_t;

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

/**
 * @brief Create a new observability context.
 *
 * @param[out] out    Uninitialised pointer; set to the new context on
 *                    success.
 * @param[in]  config Configuration; must be non-NULL.
 * @return COG_OK on success.
 * @return COG_ERROR_NULL_PTR if @p out or @p config is NULL.
 * @return COG_ERROR_INVALID_ARG if @p config->min_severity is outside
 *         the defined range, or if @p config->fields has a NULL
 *         @c fields pointer with a non-zero @c count, or if
 *         @p config->sinks has a NULL @c sinks pointer with a non-zero
 *         @c count.
 * @return COG_ERROR_NOMEM if allocation of the context or its owned
 *         tables fails.
 *
 * @pre @p config->fields and @p config->sinks, when non-NULL, reference
 *      static const tables whose lifetime exceeds that of the context.
 *
 * @post On success, @p *out is a valid context with an empty span stack
 *       and an empty diagnostic table, and the caller owns it.
 * @post On failure, @p *out is NULL and no resources are held.
 *
 * Threading: not thread-safe.  The returned context must not be shared
 * between threads without external synchronisation.
 */
cog_result_t cog_obs_new(cog_obs_t **out, const cog_obs_config_t *config);

/**
 * @brief Destroy an observability context.
 *
 * @param[in,out] obs Context to destroy; may be NULL (no-op).
 *
 * @post All storage owned by @p obs is released, including the span
 *       stack and diagnostic table.  Sinks are not closed, flushed, or
 *       destroyed: the caller retains ownership of every destination.
 * @post @p obs must not be used after this call.
 *
 * @note Open spans are discarded, not emitted.  A caller that wants
 *       complete traces must end its spans before destroying the
 *       context.
 *
 * Threading: not thread-safe.
 */
void cog_obs_free(cog_obs_t *obs);

/* ------------------------------------------------------------------ */
/* Structured logging                                                  */
/* ------------------------------------------------------------------ */

/**
 * @brief Emit one structured log record.
 *
 * The record is filtered by @p obs's minimum severity, formatted once
 * into an internal scratch buffer, and delivered to every sink whose
 * own @c min_severity admits it.  Formatting is deliberately simple and
 * allocation-free: `timestamp severity service message key=value ...`.
 * String values are quoted and escaped; booleans render as `true` and
 * `false`; double values are rendered with six fractional digits.
 *
 * @param[in,out] obs      Context to log through.
 * @param[in]     severity Severity of the record.
 * @param[in]     message  Static, NUL-terminated message id or short
 *                         text.  Must not be a per-call formatted
 *                         string: cog-obs does not copy it into any
 *                         persistent store, and callers should pass a
 *                         literal so the message set stays enumerable.
 * @param[in]     fields   Array of field values; may be NULL when
 *                         @p field_count is zero.
 * @param[in]     field_count Number of entries in @p fields.
 * @return COG_OK on success, including when the record was filtered out
 *         or no sinks are configured.
 * @return COG_ERROR_NULL_PTR if @p obs or @p message is NULL.
 * @return COG_ERROR_INVALID_ARG if @p severity is outside the defined
 *         range, if @p fields is NULL with a non-zero @p field_count, if
 *         a field index is out of range for the configured field table,
 *         if a field's kind disagrees with its descriptor, or if a
 *         string-valued field is NULL.
 * @return COG_ERROR_OVERFLOW if the formatted record exceeds the
 *         context's internal scratch buffer.  The record is not
 *         truncated and not delivered.
 * @return COG_ERROR_IO if a sink's delivery callback returned an error.
 *         Remaining sinks are still offered the record before the error
 *         is returned.
 *
 * @pre @p obs was created by @c cog_obs_new.
 * @pre Every string value referenced by @p fields is valid for the
 *      duration of the call.
 *
 * @post On @c COG_OK with the record admitted, every eligible sink has
 *       received the record exactly once.
 * @post On failure, no partial record is delivered to any sink.
 *
 * Threading: not thread-safe.  Must not be called concurrently on the
 * same @p obs.  Sinks must not call back into @p obs.
 */
cog_result_t cog_obs_log(cog_obs_t *obs,
                         cog_obs_severity_t severity,
                         const char *message,
                         const cog_obs_field_t *fields,
                         size_t field_count);

/**
 * @brief Emit a record and additionally return the formatted text.
 *
 * Behaves exactly like @c cog_obs_log but copies the formatted record
 * into a caller-provided buffer before delivering it, so a caller (for
 * example the CLI) can show the same text it logged.
 *
 * @param[in,out] obs           Context to log through.
 * @param[in]     severity      Severity of the record.
 * @param[in]     message       Static message id or short text.
 * @param[in]     fields        Array of field values; may be NULL when
 *                              @p field_count is zero.
 * @param[in]     field_count   Number of entries in @p fields.
 * @param[out]    out_buf       Buffer that receives the formatted
 *                              record, NUL-terminated.  May be NULL if
 *                              only logging is wanted.
 * @param[in]     buf_capacity  Size of @p out_buf in bytes, including
 *                              space for the NUL terminator.
 * @param[out]    out_len       Receives the formatted length in bytes,
 *                              excluding the terminator.  May be NULL.
 * @return COG_OK on success, including when the record was filtered out
 *         (in which case @p out_buf receives an empty string and
 *         @p *out_len is zero).
 * @return COG_ERROR_NULL_PTR if @p obs or @p message is NULL, or if
 *         @p out_buf is NULL while @p buf_capacity is non-zero.
 * @return COG_ERROR_INVALID_ARG under the same conditions as
 *         @c cog_obs_log.
 * @return COG_ERROR_OVERFLOW if the record does not fit in
 *         @p buf_capacity or in the internal scratch buffer.  On this
 *         error @p *out_len receives the required capacity, excluding
 *         the terminator, so the caller can retry with a larger buffer.
 * @return COG_ERROR_IO under the same conditions as @c cog_obs_log.
 *
 * @post On success, @p out_buf holds the same text delivered to the
 *       sinks.
 * @post On @c COG_ERROR_OVERFLOW, nothing is delivered to any sink.
 *
 * Threading: not thread-safe.
 */
cog_result_t cog_obs_log_to(cog_obs_t *obs,
                            cog_obs_severity_t severity,
                            const char *message,
                            const cog_obs_field_t *fields,
                            size_t field_count,
                            char *out_buf,
                            size_t buf_capacity,
                            size_t *out_len);

/**
 * @brief Report whether a severity would currently be recorded.
 *
 * Intended to guard expensive field construction at call sites:
 * @code
 * if (cog_obs_enabled(obs, COG_OBS_DEBUG, &enabled) == COG_OK && enabled) { ... }
 * @endcode
 *
 * @param[in]  obs      Context to query.
 * @param[in]  severity Severity to test.
 * @param[out] out_enabled Receives non-zero if a record at @p severity
 *                         would pass the context filter.  Unchanged on
 *                         failure.
 * @return COG_OK on success.
 * @return COG_ERROR_NULL_PTR if @p obs or @p out_enabled is NULL.
 * @return COG_ERROR_INVALID_ARG if @p severity is outside the defined
 *         range.
 *
 * @note This reports the context filter only.  A sink with a stricter
 *       @c min_severity may still discard the record.
 *
 * Threading: not thread-safe.
 */
cog_result_t cog_obs_enabled(const cog_obs_t *obs,
                             cog_obs_severity_t severity,
                             int *out_enabled);

/* ------------------------------------------------------------------ */
/* Trace spans                                                         */
/* ------------------------------------------------------------------ */

/**
 * @brief Open a trace span.
 *
 * Pushes a new span onto the context's span stack and returns its id.
 * The new span's parent is whatever span is currently on top of the
 * stack, or @c COG_OBS_SPAN_ID_NONE when the stack is empty.
 *
 * @param[in,out] obs      Context to trace through.
 * @param[in]     name     Static, NUL-terminated span name.  Borrowed;
 *                         must remain valid until the matching
 *                         @c cog_obs_trace_end returns.
 * @param[out]    out_id   Receives the new span id.  Unchanged on
 *                         failure.
 * @return COG_OK on success.
 * @return COG_ERROR_NULL_PTR if @p obs, @p name, or @p out_id is NULL.
 * @return COG_ERROR_OVERFLOW if the span stack is already at
 *         @c cog_obs_config_t::span_capacity.
 *
 * @pre @p obs was created by @c cog_obs_new.
 *
 * @post On success, exactly one span has been pushed and @p *out_id is
 *       non-zero.
 * @post On failure, the span stack is unchanged.
 *
 * Threading: not thread-safe.  Spans are strictly nested per context;
 * spans may not be ended out of order.
 */
cog_result_t cog_obs_trace_begin(cog_obs_t *obs,
                                 const char *name,
                                 cog_obs_span_id_t *out_id);

/**
 * @brief Close the innermost open trace span and emit it.
 *
 * Pops the top of the span stack, computes its duration from the
 * context clock, fills a @c cog_obs_span_record_t, and passes it to the
 * context's trace sink.
 *
 * @param[in,out] obs    Context to trace through.
 * @param[in]     id     Span id returned by @c cog_obs_trace_begin.
 * @param[in]     status Outcome to record for the span; @c COG_OK marks
 *                       it successful.
 * @return COG_OK on success.
 * @return COG_ERROR_NULL_PTR if @p obs is NULL.
 * @return COG_ERROR_NOT_FOUND if no span with id @p id is open.
 * @return COG_ERROR_INVALID_ARG if @p id identifies a span that is not
 *         the innermost open span.  Spans must be closed in the reverse
 *         order they were opened.
 * @return COG_ERROR_IO if the trace sink rejected the record.
 *
 * @pre @p id was returned by @c cog_obs_trace_begin on this context and
 *      has not already been ended.
 *
 * @post On success, the span is no longer open and has been offered to
 *       the trace sink exactly once.
 * @post On failure, the span stack is unchanged and nothing is emitted.
 *
 * Threading: not thread-safe.
 */
cog_result_t cog_obs_trace_end(cog_obs_t *obs,
                               cog_obs_span_id_t id,
                               cog_result_t status);

/**
 * @brief Read the innermost open span without closing it.
 *
 * @param[in]  obs    Context to query.
 * @param[out] out_id Receives the innermost open span id, or
 *                    @c COG_OBS_SPAN_ID_NONE when no span is open.
 *                    Unchanged on failure.
 * @return COG_OK on success.
 * @return COG_ERROR_NULL_PTR if @p obs or @p out_id is NULL.
 *
 * Threading: not thread-safe.
 */
cog_result_t cog_obs_trace_current(const cog_obs_t *obs,
                                   cog_obs_span_id_t *out_id);

/**
 * @brief Read the number of currently open spans.
 *
 * @param[in]  obs   Context to query.
 * @param[out] out_depth Receives the span stack depth; zero when no span
 *                       is open.  Unchanged on failure.
 * @return COG_OK on success.
 * @return COG_ERROR_NULL_PTR if @p obs or @p out_depth is NULL.
 *
 * Threading: not thread-safe.
 */
cog_result_t cog_obs_trace_depth(const cog_obs_t *obs, size_t *out_depth);

/* ------------------------------------------------------------------ */
/* Connection diagnostics                                              */
/* ------------------------------------------------------------------ */

/**
 * @brief State of a tracked network connection.
 *
 * The numeric values are stable and may be reported over IPC.
 */
typedef enum cog_obs_conn_state {
    COG_OBS_CONN_IDLE       = 0, /**< Registered but not connected. */
    COG_OBS_CONN_CONNECTING = 1, /**< Connection attempt in progress. */
    COG_OBS_CONN_ACTIVE     = 2, /**< Connected and transferring. */
    COG_OBS_CONN_STALLED    = 3, /**< Connected but no progress within the
                                      stall threshold. */
    COG_OBS_CONN_CLOSED     = 4, /**< Closed normally. */
    COG_OBS_CONN_FAILED     = 5, /**< Closed with an error. */
} cog_obs_conn_state_t;

/**
 * @brief One caller-owned connection snapshot.
 *
 * The transport fills this in and publishes it with
 * @c cog_obs_diag_publish.  cog-obs copies the struct into its own
 * table, so the caller may reuse its struct immediately.
 *
 * All string pointers are borrowed and are **not** copied: the caller
 * guarantees each remains valid until the next publish or clear
 * touching the same slot.  Passing static literals for endpoint and
 * provider names is the intended usage.
 */
typedef struct cog_obs_conn_snapshot {
    /**
     * @brief Caller-assigned connection id, unique within the table.
     *
     * Must be non-zero so that zero can mean "empty slot".
     */
    uint64_t conn_id;

    /** @brief Current state of the connection. */
    cog_obs_conn_state_t state;

    /** @brief Endpoint name this connection targets; may be NULL. */
    const char *endpoint;

    /** @brief Provider name serving the connection; may be NULL. */
    const char *provider;

    /** @brief Timestamp the connection attempt started. */
    uint64_t started_ns;

    /** @brief Timestamp of the most recent byte of progress. */
    uint64_t last_progress_ns;

    /** @brief Bytes received so far. */
    uint64_t bytes_in;

    /** @brief Bytes sent so far. */
    uint64_t bytes_out;

    /** @brief Number of retry attempts made so far. */
    uint32_t retries;

    /** @brief Outcome of the most recent operation; @c COG_OK if none. */
    cog_result_t last_status;
} cog_obs_conn_snapshot_t;

/**
 * @brief Insert or update a connection snapshot.
 *
 * The snapshot is matched on @p snapshot->conn_id.  If a slot with that
 * id already exists it is overwritten; otherwise the snapshot takes the
 * first free slot.
 *
 * @param[in,out] obs      Context to publish into.
 * @param[in]     snapshot Snapshot to copy.
 * @return COG_OK on success.
 * @return COG_ERROR_NULL_PTR if @p obs or @p snapshot is NULL.
 * @return COG_ERROR_INVALID_ARG if @p snapshot->conn_id is zero or if
 *         @p snapshot->state is outside the defined range.
 * @return COG_ERROR_OVERFLOW if the table is full and no slot matches
 *         @p snapshot->conn_id.  The table is not grown; the caller must
 *         raise @c cog_obs_config_t::diag_capacity or clear entries.
 *
 * @post On success, exactly one slot of the table holds a copy of
 *       @p *snapshot.
 * @post On failure, the table is unchanged.
 *
 * Threading: not thread-safe.
 */
cog_result_t cog_obs_diag_publish(cog_obs_t *obs,
                                  const cog_obs_conn_snapshot_t *snapshot);

/**
 * @brief Remove a connection snapshot from the table.
 *
 * @param[in,out] obs     Context to update.
 * @param[in]     conn_id Id of the connection to remove.
 * @return COG_OK on success.
 * @return COG_ERROR_NULL_PTR if @p obs is NULL.
 * @return COG_ERROR_INVALID_ARG if @p conn_id is zero.
 * @return COG_ERROR_NOT_FOUND if no slot holds @p conn_id.
 *
 * @post On success, the slot is free for reuse and subsequent
 *       enumeration no longer reports the connection.
 * @post On failure, the table is unchanged.
 *
 * Threading: not thread-safe.
 */
cog_result_t cog_obs_diag_clear(cog_obs_t *obs, uint64_t conn_id);

/**
 * @brief Enumerate the table in slot order, skipping free slots.
 *
 * Together with @c cog_obs_diag_count this lets a UI enumerate without
 * allocating:
 * @code
 * size_t n = 0;
 * cog_obs_diag_count(obs, &n);
 * for (size_t i = 0; i < n; ++i) {
 *     cog_obs_conn_snapshot_t snap;
 *     cog_obs_diag_at(obs, i, &snap);
 *     ...
 * }
 * @endcode
 *
 * @param[in]  obs   Context to query.
 * @param[in]  index Zero-based index into the sequence of occupied
 *                   slots, not the underlying slot number.
 * @param[out] out_snapshot Receives a copy of the snapshot.  Unchanged
 *                   on failure.
 * @return COG_OK on success.
 * @return COG_ERROR_NULL_PTR if @p obs or @p out_snapshot is NULL.
 * @return COG_ERROR_INVALID_ARG if @p index is greater than or equal to
 *         the occupied count reported by @c cog_obs_diag_count.
 *
 * @post On success, @p *out_snapshot is a byte copy whose string
 *       pointers remain owned by the context and valid until the slot
 *       is republished or cleared.
 *
 * Threading: not thread-safe.  The result is a snapshot and may be
 * stale the moment it is returned.
 */
cog_result_t cog_obs_diag_at(const cog_obs_t *obs,
                             size_t index,
                             cog_obs_conn_snapshot_t *out_snapshot);

/**
 * @brief Return the number of occupied diagnostic slots.
 *
 * @param[in]  obs   Context to query.
 * @param[out] out_count Receives the number of occupied slots.  Unchanged
 *                       on failure.
 * @return COG_OK on success.
 * @return COG_ERROR_NULL_PTR if @p obs or @p out_count is NULL.
 *
 * Threading: not thread-safe.
 */
cog_result_t cog_obs_diag_count(const cog_obs_t *obs, size_t *out_count);

/**
 * @brief Count connections currently in a given state.
 *
 * Intended for health reporting: for example, the number of
 * @c COG_OBS_CONN_STALLED connections is a direct signal that the
 * transport needs attention.
 *
 * @param[in]  obs      Context to query.
 * @param[in]  state    State to count.
 * @param[out] out_count Receives the number of matching connections.
 *                       Unchanged on failure.
 * @return COG_OK on success.
 * @return COG_ERROR_NULL_PTR if @p obs or @p out_count is NULL.
 * @return COG_ERROR_INVALID_ARG if @p state is outside the defined range.
 *
 * Threading: not thread-safe.
 */
cog_result_t cog_obs_diag_count_state(const cog_obs_t *obs,
                                      cog_obs_conn_state_t state,
                                      size_t *out_count);

/**
 * @brief Format a connection snapshot as one text line.
 *
 * The layout is `conn=<id> state=<name> endpoint=<e> provider=<p>
 * in=<n> out=<n> retries=<n> age_ms=<n> idle_ms=<n> status=<code>`,
 * omitting attributes whose source pointer is NULL.  This is the same
 * shape the CLI prints, so diagnostics stay greppable.
 *
 * @param[in]  obs          Context that owns the clock used for the age
 *                          and idle computations.
 * @param[in]  snapshot     Snapshot to format.
 * @param[out] out_buf      Buffer that receives the NUL-terminated line.
 * @param[in]  buf_capacity Size of @p out_buf in bytes, including space
 *                          for the NUL terminator.
 * @param[out] out_len      Receives the written length in bytes,
 *                          excluding the terminator.  May be NULL.
 * @return COG_OK on success.
 * @return COG_ERROR_NULL_PTR if @p obs, @p snapshot, or @p out_buf is
 *         NULL.
 * @return COG_ERROR_INVALID_ARG if @p snapshot->state is outside the
 *         defined range.
 * @return COG_ERROR_OVERFLOW if the line does not fit in
 *         @p buf_capacity.  On this error @p *out_len receives the
 *         required capacity, excluding the terminator.
 *
 * @post On success, @p out_buf holds a single line without a trailing
 *       newline.
 * @post On failure, @p out_buf contents are unspecified.
 *
 * Threading: not thread-safe, because the elapsed-time fields are
 * computed against @p obs's clock.
 */
cog_result_t cog_obs_diag_format(const cog_obs_t *obs,
                                 const cog_obs_conn_snapshot_t *snapshot,
                                 char *out_buf,
                                 size_t buf_capacity,
                                 size_t *out_len);

/* ------------------------------------------------------------------ */
/* Connection event stream (GUI table feed)                            */
/* ------------------------------------------------------------------ */

/**
 * @brief Kind of connection event.
 *
 * These events form a stream suitable for a GUI table where each row
 * is one logical upstream connection.  Events are emitted by the
 * transport layer and consumed by a @c cog_obs_conn_event_sink_fn.
 */
typedef enum cog_obs_conn_event_kind {
    COG_OBS_CONN_EVENT_OPEN       = 0, /**< Connection attempt started. */
    COG_OBS_CONN_EVENT_CONNECTED  = 1, /**< Connection established. */
    COG_OBS_CONN_EVENT_PROGRESS   = 2, /**< Bytes transferred (periodic). */
    COG_OBS_CONN_EVENT_STALLED    = 3, /**< No progress within threshold. */
    COG_OBS_CONN_EVENT_RETRY      = 4, /**< Retry attempt started. */
    COG_OBS_CONN_EVENT_CLOSED     = 5, /**< Connection closed normally. */
    COG_OBS_CONN_EVENT_FAILED     = 6, /**< Connection failed. */
} cog_obs_conn_event_kind_t;

/**
 * @brief One connection event for the GUI stream.
 *
 * Unlike @c cog_obs_conn_snapshot_t (which is a point-in-time state
 * query), this is an **event** that describes something that happened.
 * A GUI accumulates these into a table where each row is keyed by
 * @c conn_id.
 *
 * All string pointers are borrowed and valid only for the duration of
 * the sink call.  The sink must copy them if it needs to retain them.
 */
struct cog_obs_conn_event {
    /** @brief Event kind. */
    cog_obs_conn_event_kind_t kind;

    /** @brief Connection id (matches @c cog_obs_conn_snapshot_t). */
    uint64_t conn_id;

    /** @brief Timestamp of this event, in nanoseconds. */
    uint64_t timestamp_ns;

    /** @brief Session id this connection belongs to; may be NULL. */
    const char *session_id;

    /** @brief Endpoint URL or name; may be NULL. */
    const char *endpoint;

    /** @brief Provider name; may be NULL. */
    const char *provider;

    /** @brief Model name; may be NULL. */
    const char *model;

    /** @brief Bytes received in this event (delta). */
    uint64_t bytes_in_delta;

    /** @brief Bytes sent in this event (delta). */
    uint64_t bytes_out_delta;

    /** @brief Total bytes received so far. */
    uint64_t bytes_in_total;

    /** @brief Total bytes sent so far. */
    uint64_t bytes_out_total;

    /** @brief Current retry attempt number (0 = first attempt). */
    uint32_t retry_attempt;

    /** @brief Outcome code for CLOSED/FAILED events. */
    cog_result_t status;

    /** @brief HTTP status code for FAILED events; 0 if not applicable. */
    uint16_t http_status;
};

/** @brief Convenience alias for @c struct cog_obs_conn_event. */
typedef struct cog_obs_conn_event cog_obs_conn_event_t;

/**
 * @brief Emit a connection event to the configured sink.
 *
 * The transport layer calls this for every connection lifecycle event.
 * If no sink is configured, the call is a no-op.
 *
 * @param[in,out] obs    Context to emit through.
 * @param[in]     event  Event to emit; must not be NULL.
 * @return COG_OK on success (including when no sink is configured).
 * @return COG_ERROR_NULL_PTR if @p obs or @p event is NULL.
 * @return COG_ERROR_INVALID_ARG if @p event->kind is outside the
 *         defined range.
 * @return COG_ERROR_IO if the sink returned an error.
 *
 * Threading: not thread-safe.  Must be called from the same thread
 * that runs the transport.
 */
cog_result_t cog_obs_conn_event_emit(cog_obs_t *obs,
                                     const cog_obs_conn_event_t *event);

#ifdef __cplusplus
}
#endif

#endif /* COG_OBS_H */

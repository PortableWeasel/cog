/**
 * @file obs_types.h
 * @brief Shared value types for observability: severity, fields, spans.
 *
 * This header defines the static const vocabulary that the rest of
 * cog-obs uses: log severities, field kinds, field descriptors, the
 * structured-key table, and the trace span record.  It holds no state
 * and performs no work; every type here is a value type or a read-only
 * descriptor.
 *
 * Field *names* never appear as literals at the call site.  The caller
 * declares a static const @c cog_obs_field_desc_t per attribute and
 * wraps them in a static const @c cog_obs_field_table_t.  The logging
 * calls then reference fields by table index.  This keeps the emitted
 * key set enumerable at build time and avoids per-call string
 * formatting.
 *
 * Every public function returns @c cog_result_t; value queries write
 * their result through an out parameter.
 *
 * Threading model: every type in this header is either a value type or
 * a read-only static const descriptor table.  Read-only tables may be
 * shared freely by any number of threads.  The two functions declared
 * here are re-entrant and allocate nothing.
 */
#ifndef COG_OBS_TYPES_H
#define COG_OBS_TYPES_H

#include <cog/result.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Log severity, ordered from most to least verbose.
 *
 * The numeric order is significant: a logger emits a record when the
 * record's severity is less than or equal to the configured threshold.
 * Existing values are stable; new levels are appended at the end.
 */
typedef enum cog_obs_severity {
    COG_OBS_TRACE = 0, /**< Fine-grained step-by-step diagnostics. */
    COG_OBS_DEBUG = 1, /**< Developer-facing detail. */
    COG_OBS_INFO  = 2, /**< Normal operational events. */
    COG_OBS_WARN  = 3, /**< Recoverable anomaly. */
    COG_OBS_ERROR = 4, /**< Operation failed. */
    COG_OBS_FATAL = 5, /**< Process cannot continue. */
} cog_obs_severity_t;

/**
 * @brief Number of severities in @c cog_obs_severity_t.
 *
 * Used to size static const name tables.  Keep in sync when appending
 * severities.
 */
#define COG_OBS_SEVERITY_COUNT 6

/**
 * @brief Return the lowercase name of a severity.
 *
 * Names are served from a private @c static @c const table indexed by
 * severity, so the call performs no formatting and no allocation.
 *
 * @param[in]  severity Severity to name.
 * @param[out] out_name Receives a pointer to read-only static storage
 *                      holding the NUL-terminated lowercase name
 *                      ("trace", "debug", "info", "warn", "error",
 *                      "fatal").  Unchanged on failure.
 * @return COG_OK on success.
 * @return COG_ERROR_NULL_PTR if @p out_name is NULL.
 * @return COG_ERROR_INVALID_ARG if @p severity is outside the defined
 *         range.
 *
 * @pre @p severity is one of the values in @c cog_obs_severity_t.
 *
 * @post On success, @p *out_name points to static storage that remains
 *       valid for the lifetime of the process; the caller must not free
 *       or modify it.
 *
 * Threading: re-entrant and thread-safe; the returned string is
 * immutable.
 */
cog_result_t cog_obs_severity_name(cog_obs_severity_t severity,
                                   const char **out_name);

/**
 * @brief Read the process monotonic clock in nanoseconds.
 *
 * This is the default time source used by the logger and the trace
 * machinery when no clock override is supplied in the logger
 * configuration.  The implementation wraps the platform monotonic clock
 * (`clock_gettime(CLOCK_MONOTONIC)`); the origin is unspecified, but
 * successive calls never go backwards.
 *
 * @param[out] out_now_ns Receives the current timestamp in nanoseconds.
 *                        Unchanged on failure.
 * @return COG_OK on success.
 * @return COG_ERROR_NULL_PTR if @p out_now_ns is NULL.
 *
 * @post On success, @p *out_now_ns is a monotonically non-decreasing
 *       nanosecond timestamp.
 *
 * Threading: callable from any thread; performs no locking.
 */
cog_result_t cog_obs_clock_now_ns(uint64_t *out_now_ns);

/**
 * @brief Kind of value carried by a structured log field.
 *
 * Determines how the value union in @c cog_obs_field_t is
 * interpreted when the record is formatted.
 */
typedef enum cog_obs_field_kind {
    COG_OBS_FIELD_STRING = 0, /**< NUL-terminated string; borrowed. */
    COG_OBS_FIELD_INT    = 1, /**< Signed 64-bit integer. */
    COG_OBS_FIELD_UINT   = 2, /**< Unsigned 64-bit integer. */
    COG_OBS_FIELD_BOOL   = 3, /**< Boolean, rendered as "true"/"false". */
    COG_OBS_FIELD_DOUBLE = 4, /**< Double-precision float. */
} cog_obs_field_kind_t;

/**
 * @brief Static descriptor for one structured log attribute.
 *
 * A field descriptor is declared once, statically, and referenced from
 * a @c cog_obs_field_table_t.  It carries no value: values are supplied
 * per call through a @c cog_obs_field_t.
 */
typedef struct cog_obs_field_desc {
    /**
     * @brief Key name as it appears in the emitted record.
     *
     * Must be non-NULL and NUL-terminated.  The string is borrowed and
     * must outlive every record that references the descriptor.  Keys
     * must be unique within a field table.
     */
    const char *name;

    /**
     * @brief Declared kind of the field's value.
     *
     * A record whose value kind does not match this declaration is
     * rejected with @c COG_ERROR_INVALID_ARG.
     */
    cog_obs_field_kind_t kind;
} cog_obs_field_desc_t;

/**
 * @brief Static const table of field descriptors.
 *
 * The underlying array must remain valid for the lifetime of any
 * logger that references it.  Key names are resolved to indices once,
 * at logger construction, so the table must not be modified afterwards.
 */
typedef struct cog_obs_field_table {
    const cog_obs_field_desc_t *fields; /**< Pointer to first descriptor;
                                             may be NULL when @p count is
                                             zero. */
    size_t count;                       /**< Number of descriptors. */
} cog_obs_field_table_t;

/**
 * @brief A single attribute value supplied for one log call.
 *
 * The @c string variant borrows its pointer: the caller guarantees the
 * string remains valid for the duration of the log call, and the logger
 * copies it into the output buffer (it does not retain it).  The other
 * variants are passed by value.
 */
typedef struct cog_obs_field {
    /**
     * @brief Index into the logger's field table identifying the key.
     *
     * Must be less than the field table's @c count.  An out-of-range
     * index is rejected with @c COG_ERROR_INVALID_ARG.
     */
    uint16_t index;

    /**
     * @brief Kind of the value in @p value.
     *
     * Must match the kind declared by the referenced field descriptor.
     */
    cog_obs_field_kind_t kind;

    /** @brief Value payload; interpret according to @p kind. */
    union {
        const char *string; /**< Valid when @p kind is
                                 @c COG_OBS_FIELD_STRING. */
        int64_t integer;    /**< Valid when @p kind is
                                 @c COG_OBS_FIELD_INT. */
        uint64_t unsigned_integer; /**< Valid when @p kind is
                                        @c COG_OBS_FIELD_UINT. */
        int boolean;        /**< Valid when @p kind is
                                 @c COG_OBS_FIELD_BOOL; zero is false,
                                 non-zero is true. */
        double real;        /**< Valid when @p kind is
                                 @c COG_OBS_FIELD_DOUBLE. */
    } value;
} cog_obs_field_t;

/**
 * @brief Identifier of a trace span within a trace tree.
 *
 * Span ids are opaque to callers: they are produced by
 * @c cog_obs_trace_begin and consumed by @c cog_obs_trace_end.  Zero is
 * reserved to mean "no span".
 */
typedef uint64_t cog_obs_span_id_t;

/**
 * @brief The reserved span id meaning "no span".
 */
#define COG_OBS_SPAN_ID_NONE ((cog_obs_span_id_t)0)

/**
 * @brief A completed trace span record.
 *
 * Produced by @c cog_obs_trace_end and passed to the caller's sink.
 * The struct is a value type; any string pointers inside it are
 * borrowed from the caller's static name and are valid for the
 * duration of the sink call only.
 */
typedef struct cog_obs_span_record {
    cog_obs_span_id_t id;      /**< Id of the completed span. */
    cog_obs_span_id_t parent;  /**< Id of the enclosing span, or
                                    @c COG_OBS_SPAN_ID_NONE if the span
                                    was a root. */
    const char *name;          /**< Span name supplied to
                                    @c cog_obs_trace_begin; borrowed. */
    uint64_t start_ns;         /**< Start timestamp, in nanoseconds from
                                    an unspecified monotonic origin. */
    uint64_t duration_ns;      /**< Elapsed time in nanoseconds. */
    cog_result_t status;       /**< Outcome reported to
                                    @c cog_obs_trace_end.  @c COG_OK for
                                    a successful span. */
    int ok;                    /**< Non-zero if @p status is @c COG_OK. */
} cog_obs_span_record_t;

#ifdef __cplusplus
}
#endif

#endif /* COG_OBS_TYPES_H */

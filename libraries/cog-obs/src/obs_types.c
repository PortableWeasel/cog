/**
 * @file obs_types.c
 * @brief Value-type helpers for cog-obs: severity names and the clock.
 *
 * Implements the two functions declared in @c cog/obs_types.h.  Both are
 * re-entrant, allocate nothing, and touch no global mutable state: the
 * severity names come from a private static const table and the clock
 * wraps the platform monotonic clock.
 *
 * Ticket: 7f5b166 (Stage 12.3b — Implement obs.c)
 */
#define _POSIX_C_SOURCE 200809L

#include "cog/obs_types.h"

#include <time.h>

/**
 * @brief Lowercase severity names, indexed by @c cog_obs_severity_t.
 *
 * Static const: shared freely by any number of threads, never modified.
 */
static const char *const cog_obs_severity_names[COG_OBS_SEVERITY_COUNT] = {
    "trace", "debug", "info", "warn", "error", "fatal",
};

cog_result_t cog_obs_severity_name(cog_obs_severity_t severity,
                                   const char **out_name)
{
    if (out_name == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    if ((int)severity < 0 || (int)severity >= COG_OBS_SEVERITY_COUNT) {
        return COG_ERROR_INVALID_ARG;
    }
    *out_name = cog_obs_severity_names[(int)severity];
    return COG_OK;
}

cog_result_t cog_obs_clock_now_ns(uint64_t *out_now_ns)
{
    struct timespec ts;

    if (out_now_ns == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return COG_ERROR_IO;
    }
    *out_now_ns = (uint64_t)ts.tv_sec * UINT64_C(1000000000) +
                  (uint64_t)ts.tv_nsec;
    return COG_OK;
}

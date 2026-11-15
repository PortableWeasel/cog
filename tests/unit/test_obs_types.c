/**
 * @file test_obs_types.c
 * @brief Contract test for cog/obs_types.h: severity names, the clock,
 *        and the static value vocabulary.
 *
 * This test pins the public contract of libraries/cog-obs/include/cog/
 * obs_types.h as implemented by libraries/cog-obs/src/obs_types.c:
 *
 *   - the six severities keep their declared order (0 = TRACE);
 *   - COG_OBS_SEVERITY_COUNT matches the enum;
 *   - severity names are served from static storage, lowercase, in
 *     declared order, with NULL/invalid-argument handling;
 *   - the monotonic clock rejects a NULL out pointer and returns
 *     non-decreasing nanosecond timestamps;
 *   - field kinds keep their declared order;
 *   - COG_OBS_SPAN_ID_NONE is exactly zero;
 *   - struct field types are exact (compile-time checks).
 *
 * Nothing is tested here that is not either stated in the header or
 * guaranteed by C11 semantics.
 *
 * Ticket: 7f5b166 (Stage 12.3b — Implement obs.c)
 */

#include "cog/obs_types.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifndef COG_OBS_TYPES_H
#error "COG_OBS_TYPES_H include guard missing"
#endif

/* ------------------------------------------------------------------ */
/* Compile-time checks: enum values, struct shapes, field types.       */
/* ------------------------------------------------------------------ */

_Static_assert(COG_OBS_TRACE == 0, "COG_OBS_TRACE must be exactly 0");
_Static_assert(COG_OBS_DEBUG == 1, "COG_OBS_DEBUG must be exactly 1");
_Static_assert(COG_OBS_INFO == 2, "COG_OBS_INFO must be exactly 2");
_Static_assert(COG_OBS_WARN == 3, "COG_OBS_WARN must be exactly 3");
_Static_assert(COG_OBS_ERROR == 4, "COG_OBS_ERROR must be exactly 4");
_Static_assert(COG_OBS_FATAL == 5, "COG_OBS_FATAL must be exactly 5");
_Static_assert(COG_OBS_SEVERITY_COUNT == 6,
               "COG_OBS_SEVERITY_COUNT must be 6");

_Static_assert(COG_OBS_FIELD_STRING == 0,
               "COG_OBS_FIELD_STRING must be exactly 0");
_Static_assert(COG_OBS_FIELD_INT == 1, "COG_OBS_FIELD_INT must be exactly 1");
_Static_assert(COG_OBS_FIELD_UINT == 2,
               "COG_OBS_FIELD_UINT must be exactly 2");
_Static_assert(COG_OBS_FIELD_BOOL == 3,
               "COG_OBS_FIELD_BOOL must be exactly 3");
_Static_assert(COG_OBS_FIELD_DOUBLE == 4,
               "COG_OBS_FIELD_DOUBLE must be exactly 4");

_Static_assert(COG_OBS_SPAN_ID_NONE == 0,
               "COG_OBS_SPAN_ID_NONE must be exactly 0");

/* Field types: exact, verified via type-generic selection (unevaluated). */
#define FIELD_TYPE_ASSERT(strukt, field, type)                                 \
    _Static_assert(_Generic(((strukt){0}).field, type : 1, default : 0),        \
                   #strukt "." #field " must have type " #type)

FIELD_TYPE_ASSERT(cog_obs_field_desc_t, name, const char *);
FIELD_TYPE_ASSERT(cog_obs_field_desc_t, kind, cog_obs_field_kind_t);
FIELD_TYPE_ASSERT(cog_obs_field_table_t, fields, const cog_obs_field_desc_t *);
FIELD_TYPE_ASSERT(cog_obs_field_table_t, count, size_t);
FIELD_TYPE_ASSERT(cog_obs_field_t, index, uint16_t);
FIELD_TYPE_ASSERT(cog_obs_field_t, kind, cog_obs_field_kind_t);
FIELD_TYPE_ASSERT(cog_obs_span_record_t, id, cog_obs_span_id_t);
FIELD_TYPE_ASSERT(cog_obs_span_record_t, parent, cog_obs_span_id_t);
FIELD_TYPE_ASSERT(cog_obs_span_record_t, name, const char *);
FIELD_TYPE_ASSERT(cog_obs_span_record_t, start_ns, uint64_t);
FIELD_TYPE_ASSERT(cog_obs_span_record_t, duration_ns, uint64_t);
FIELD_TYPE_ASSERT(cog_obs_span_record_t, status, cog_result_t);
FIELD_TYPE_ASSERT(cog_obs_span_record_t, ok, int);

#undef FIELD_TYPE_ASSERT

/* ------------------------------------------------------------------ */
/* Runtime checks.                                                     */
/* ------------------------------------------------------------------ */

/** Expected lowercase names, in declared severity order. */
static const char *const expected_names[COG_OBS_SEVERITY_COUNT] = {
    "trace", "debug", "info", "warn", "error", "fatal",
};

static int test_severity_name_all(void)
{
    int i;

    for (i = 0; i < COG_OBS_SEVERITY_COUNT; ++i) {
        const char *name = NULL;
        cog_result_t rc;

        rc = cog_obs_severity_name((cog_obs_severity_t)i, &name);
        if (rc != COG_OK) {
            (void)fprintf(stderr, "severity_name(%d) rc=%d\n", i, (int)rc);
            return 1;
        }
        if (name == NULL || strcmp(name, expected_names[i]) != 0) {
            (void)fprintf(stderr, "severity_name(%d) = \"%s\", expected \"%s\"\n",
                          i, name != NULL ? name : "(null)", expected_names[i]);
            return 1;
        }
    }
    return 0;
}

static int test_severity_name_null_out(void)
{
    if (cog_obs_severity_name(COG_OBS_INFO, NULL) != COG_ERROR_NULL_PTR) {
        return 1;
    }
    return 0;
}

static int test_severity_name_invalid(void)
{
    const char *name = "sentinel";

    /* Below the enum range. */
    if (cog_obs_severity_name((cog_obs_severity_t)-1, &name) !=
        COG_ERROR_INVALID_ARG) {
        return 1;
    }
    if (name == NULL || strcmp(name, "sentinel") != 0) {
        return 1; /* Out parameter must be unchanged on failure. */
    }
    /* Above the enum range. */
    if (cog_obs_severity_name((cog_obs_severity_t)COG_OBS_SEVERITY_COUNT,
                              &name) != COG_ERROR_INVALID_ARG) {
        return 1;
    }
    if (name == NULL || strcmp(name, "sentinel") != 0) {
        return 1;
    }
    return 0;
}

static int test_clock_null_out(void)
{
    if (cog_obs_clock_now_ns(NULL) != COG_ERROR_NULL_PTR) {
        return 1;
    }
    return 0;
}

static int test_clock_monotonic(void)
{
    uint64_t a = 0U;
    uint64_t b = 0U;
    uint64_t c = 0U;

    if (cog_obs_clock_now_ns(&a) != COG_OK) {
        return 1;
    }
    if (cog_obs_clock_now_ns(&b) != COG_OK) {
        return 1;
    }
    if (cog_obs_clock_now_ns(&c) != COG_OK) {
        return 1;
    }
    if (b < a || c < b) {
        (void)fprintf(stderr, "clock went backwards: %llu %llu %llu\n",
                      (unsigned long long)a, (unsigned long long)b,
                      (unsigned long long)c);
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int main(void)
{
    unsigned failures = 0U;

#define CHECK(label, expr)                                                   \
    do {                                                                     \
        if ((expr) != 0) {                                                   \
            (void)fprintf(stderr, "FAIL: %s\n", label);                      \
            ++failures;                                                      \
        }                                                                    \
    } while (0)

    CHECK("severity names all", test_severity_name_all());
    CHECK("severity name NULL out", test_severity_name_null_out());
    CHECK("severity name invalid", test_severity_name_invalid());

    CHECK("clock NULL out", test_clock_null_out());
    CHECK("clock monotonic", test_clock_monotonic());

#undef CHECK

    if (failures == 0U) {
        (void)printf("test_obs_types: ok\n");
    }
    return failures == 0U ? 0 : 1;
}

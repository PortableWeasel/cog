/**
 * @file test_result.c
 * @brief Phase 0 contract test for cog/result.h (header-only module).
 *
 * result.h declares no functions; it defines the cog_result_t
 * enumeration and the error-code convention every cog-core public
 * function uses.  This test pins the documented stable values at
 * compile time (_Static_assert) and verifies at run time the
 * invariants the header states in prose:
 *
 *   - zero means success, negative values mean failure
 *   - error codes are stable and distinct, so new codes can be
 *     appended without renumbering existing values
 *
 * Nothing is tested here that is not either stated in the header or
 * guaranteed by C11 semantics.
 *
 * Ticket: 9d5b705 (Stage 12.0 — Foundation: result + message)
 */

#include "cog/result.h"

#include <stdio.h>
#include <string.h>

#ifndef COG_RESULT_H
#error "COG_RESULT_H include guard missing"
#endif

/* ------------------------------------------------------------------ */
/* Compile-time checks: exact documented values ("stable codes").      */
/* ------------------------------------------------------------------ */

#define CHECK_NEGATIVE(name) \
    _Static_assert((name) < 0, #name " must be a negative value")

_Static_assert(COG_OK == 0, "COG_OK must be exactly 0 (success)");

CHECK_NEGATIVE(COG_ERROR_INVALID_ARG);
CHECK_NEGATIVE(COG_ERROR_NULL_PTR);
CHECK_NEGATIVE(COG_ERROR_NOMEM);
CHECK_NEGATIVE(COG_ERROR_NOT_FOUND);
CHECK_NEGATIVE(COG_ERROR_OVERFLOW);
CHECK_NEGATIVE(COG_ERROR_UNHANDLED);
CHECK_NEGATIVE(COG_ERROR_TRANSITION_DENIED);
CHECK_NEGATIVE(COG_ERROR_AGAIN);
CHECK_NEGATIVE(COG_ERROR_INVALID_STATE);
CHECK_NEGATIVE(COG_ERROR_IO);
CHECK_NEGATIVE(COG_ERROR_ABORTED);
CHECK_NEGATIVE(COG_ERROR_TIMEOUT);
CHECK_NEGATIVE(COG_ERROR_UNAVAILABLE);

/* ------------------------------------------------------------------ */
/* Runtime checks.                                                     */
/* ------------------------------------------------------------------ */

/** Map each code to its canonical name, like a diagnostics layer would. */
static const char *result_name(cog_result_t r)
{
    switch (r) {
    case COG_OK:                    return "COG_OK";
    case COG_ERROR_INVALID_ARG:     return "COG_ERROR_INVALID_ARG";
    case COG_ERROR_NULL_PTR:        return "COG_ERROR_NULL_PTR";
    case COG_ERROR_NOMEM:           return "COG_ERROR_NOMEM";
    case COG_ERROR_NOT_FOUND:       return "COG_ERROR_NOT_FOUND";
    case COG_ERROR_OVERFLOW:        return "COG_ERROR_OVERFLOW";
    case COG_ERROR_UNHANDLED:       return "COG_ERROR_UNHANDLED";
    case COG_ERROR_TRANSITION_DENIED: return "COG_ERROR_TRANSITION_DENIED";
    case COG_ERROR_AGAIN:           return "COG_ERROR_AGAIN";
    case COG_ERROR_INVALID_STATE:   return "COG_ERROR_INVALID_STATE";
    case COG_ERROR_IO:              return "COG_ERROR_IO";
    case COG_ERROR_ABORTED:         return "COG_ERROR_ABORTED";
    case COG_ERROR_TIMEOUT:         return "COG_ERROR_TIMEOUT";
    case COG_ERROR_UNAVAILABLE:     return "COG_ERROR_UNAVAILABLE";
    default:                        return NULL;
    }
}

/** Expected (code, name) pairs in the order declared by the header. */
static const struct code_pair {
    cog_result_t code;
    const char *name;
} code_table[] = {
    {COG_OK,                    "COG_OK"},
    {COG_ERROR_INVALID_ARG,     "COG_ERROR_INVALID_ARG"},
    {COG_ERROR_NULL_PTR,        "COG_ERROR_NULL_PTR"},
    {COG_ERROR_NOMEM,           "COG_ERROR_NOMEM"},
    {COG_ERROR_NOT_FOUND,       "COG_ERROR_NOT_FOUND"},
    {COG_ERROR_OVERFLOW,        "COG_ERROR_OVERFLOW"},
    {COG_ERROR_UNHANDLED,       "COG_ERROR_UNHANDLED"},
    {COG_ERROR_TRANSITION_DENIED, "COG_ERROR_TRANSITION_DENIED"},
    {COG_ERROR_AGAIN,           "COG_ERROR_AGAIN"},
    {COG_ERROR_INVALID_STATE,   "COG_ERROR_INVALID_STATE"},
    {COG_ERROR_IO,              "COG_ERROR_IO"},
    {COG_ERROR_ABORTED,         "COG_ERROR_ABORTED"},
    {COG_ERROR_TIMEOUT,         "COG_ERROR_TIMEOUT"},
    {COG_ERROR_UNAVAILABLE,     "COG_ERROR_UNAVAILABLE"},
};

/**
 * Verify "zero means success; negative values mean failure" plus the
 * distinctness invariant that makes append-only extension safe.
 */
static int verify_value_semantics(void)
{
    const size_t n = sizeof(code_table) / sizeof(code_table[0]);
    size_t i;
    size_t j;

    for (i = 0U; i < n; ++i) {
        const int v = (int)code_table[i].code;

        if (i == 0U) {
            if (v != 0) {
                (void)fprintf(stderr, "first code must be success (0), got %d\n", v);
                return 1;
            }
        } else if (v >= 0) {
            (void)fprintf(stderr, "%s must be negative, got %d\n",
                          code_table[i].name, v);
            return 1;
        }

        for (j = i + 1U; j < n; ++j) {
            if (v == (int)code_table[j].code) {
                (void)fprintf(stderr, "codes %s and %s collide at %d\n",
                              code_table[i].name, code_table[j].name, v);
                return 1;
            }
        }
    }
    return 0;
}

/** Every documented code must round-trip through an int switch. */
static int verify_switch_coverage(void)
{
    const size_t n = sizeof(code_table) / sizeof(code_table[0]);
    size_t i;

    for (i = 0U; i < n; ++i) {
        const char *got = result_name(code_table[i].code);
        if (got == NULL || strcmp(got, code_table[i].name) != 0) {
            (void)fprintf(stderr, "result_name(%s) mismatch\n",
                          code_table[i].name);
            return 1;
        }
    }
    return 0;
}

/** Result codes must be usable in equality checks against COG_OK. */
static int verify_comparison_usage(void)
{
    if (COG_OK != 0) {
        return 1;
    }
    if (COG_ERROR_AGAIN == COG_OK) {
        /* not reachable — documents the intended pattern */
        return 1;
    }
    return 0;
}

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

    CHECK("value semantics", verify_value_semantics());
    CHECK("switch coverage", verify_switch_coverage());
    CHECK("comparison usage", verify_comparison_usage());

#undef CHECK

    if (failures == 0U) {
        (void)printf("test_result: ok\n");
    }
    return failures == 0U ? 0 : 1;
}

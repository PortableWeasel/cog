/**
 * @file toolkit_result.c
 * @brief Implementation of the result types and validation helpers
 *        declared in cog/toolkit_result.h.
 *
 * A @c cog_toolkit_result_t is a value type describing the outcome of a
 * single tool invocation.  A @c cog_toolkit_result_collection_t is an
 * opaque collection that owns copies of the individual results (their
 * output strings and error messages) and provides indexed access.
 *
 * The collection is allocated by @c cog_toolkit_execute_all in
 * @c toolkit_exec.c using the private layout in @c toolkit_internal.h;
 * this module provides the query and destruction functions.
 *
 * Schema validation implements a small, documented JSON Schema subset:
 * the @c type keyword (object/array/string/number/integer/boolean/null)
 * and, for objects, the @c required keyword (an array of property names).
 * Unknown keywords are ignored.  A NULL or empty schema means "no
 * validation required".
 *
 * The module holds no global mutable state.
 *
 * Ticket: 8d52c1a (Stage 12.4b — Implement toolkit)
 */

#include "cog/toolkit_result.h"
#include "toolkit_internal.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/* Maximum number of required-property names a schema may list.  Schemas
 * that exceed this are rejected as malformed/unsupported. */
#define COG_TOOLKIT_SCHEMA_MAX_REQUIRED 64U

/* ------------------------------------------------------------------ */
/* Result collection                                                   */
/* ------------------------------------------------------------------ */

size_t cog_toolkit_result_collection_count(
    const cog_toolkit_result_collection_t *collection)
{
    return collection != NULL ? collection->count : 0U;
}

cog_result_t cog_toolkit_result_collection_get(
    const cog_toolkit_result_collection_t *collection,
    size_t index,
    const cog_toolkit_result_t **out_result)
{
    if (collection == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    if (index >= collection->count) {
        return COG_ERROR_INVALID_ARG;
    }
    if (out_result != NULL) {
        *out_result = &collection->results[index];
    }
    return COG_OK;
}

void cog_toolkit_result_collection_free(
    cog_toolkit_result_collection_t *collection)
{
    if (collection == NULL) {
        return;
    }
    free(collection->results);
    free(collection->storage);
    free(collection);
}

bool cog_toolkit_result_is_ok(const cog_toolkit_result_t *result)
{
    return result != NULL && result->status == COG_OK;
}

/* ------------------------------------------------------------------ */
/* Minimal JSON subset scanner                                         */
/* ------------------------------------------------------------------ */

/* Value type codes returned by the parser. */
enum {
    JS_INVALID = 0,
    JS_OBJECT = 1,
    JS_ARRAY = 2,
    JS_STRING = 3,
    JS_NUMBER = 4,
    JS_BOOL = 5,
    JS_NULL = 6
};

typedef struct js {
    const char *p;
    const char *end;
} js_t;

static void js_skip_ws(js_t *j)
{
    while (j->p < j->end && isspace((unsigned char)*j->p) != 0) {
        j->p++;
    }
}

static int js_peek(js_t *j)
{
    js_skip_ws(j);
    return j->p < j->end ? (unsigned char)*j->p : 0;
}

static bool js_match(js_t *j, const char *lit)
{
    size_t n = strlen(lit);

    js_skip_ws(j);
    if ((size_t)(j->end - j->p) >= n && memcmp(j->p, lit, n) == 0) {
        j->p += n;
        return true;
    }
    return false;
}

static bool js_string(js_t *j, const char **out, size_t *out_len)
{
    const char *start;
    const char *q;

    js_skip_ws(j);
    if (j->p >= j->end || *j->p != '"') {
        return false;
    }
    j->p++;
    start = j->p;
    q = memchr(j->p, '"', (size_t)(j->end - j->p));
    if (q == NULL) {
        return false;
    }
    j->p = q + 1;
    *out = start;
    *out_len = (size_t)(q - start);
    return true;
}

static bool js_number(js_t *j)
{
    js_skip_ws(j);
    if (j->p >= j->end) {
        return false;
    }
    if (*j->p == '-') {
        j->p++;
    }
    if (j->p >= j->end || *j->p < '0' || *j->p > '9') {
        return false;
    }
    while (j->p < j->end && *j->p >= '0' && *j->p <= '9') {
        j->p++;
    }
    if (j->p < j->end && *j->p == '.') {
        j->p++;
        if (j->p >= j->end || *j->p < '0' || *j->p > '9') {
            return false;
        }
        while (j->p < j->end && *j->p >= '0' && *j->p <= '9') {
            j->p++;
        }
    }
    if (j->p < j->end && (*j->p == 'e' || *j->p == 'E')) {
        j->p++;
        if (j->p < j->end && (*j->p == '+' || *j->p == '-')) {
            j->p++;
        }
        if (j->p >= j->end || *j->p < '0' || *j->p > '9') {
            return false;
        }
        while (j->p < j->end && *j->p >= '0' && *j->p <= '9') {
            j->p++;
        }
    }
    return true;
}

static int js_value(js_t *j);

static int js_object(js_t *j)
{
    const char *key;
    size_t key_len;

    if (!js_match(j, "{")) {
        return JS_INVALID;
    }
    if (js_peek(j) == '}') {
        j->p++;
        return JS_OBJECT;
    }
    for (;;) {
        if (!js_string(j, &key, &key_len)) {
            return JS_INVALID;
        }
        if (!js_match(j, ":")) {
            return JS_INVALID;
        }
        if (js_value(j) == JS_INVALID) {
            return JS_INVALID;
        }
        if (js_peek(j) == ',') {
            j->p++;
            continue;
        }
        if (js_peek(j) == '}') {
            j->p++;
            return JS_OBJECT;
        }
        return JS_INVALID;
    }
}

static int js_array(js_t *j)
{
    if (!js_match(j, "[")) {
        return JS_INVALID;
    }
    if (js_peek(j) == ']') {
        j->p++;
        return JS_ARRAY;
    }
    for (;;) {
        if (js_value(j) == JS_INVALID) {
            return JS_INVALID;
        }
        if (js_peek(j) == ',') {
            j->p++;
            continue;
        }
        if (js_peek(j) == ']') {
            j->p++;
            return JS_ARRAY;
        }
        return JS_INVALID;
    }
}

static int js_value(js_t *j)
{
    int c = js_peek(j);

    if (c == '{') {
        return js_object(j);
    }
    if (c == '[') {
        return js_array(j);
    }
    if (c == '"') {
        const char *s;
        size_t n;
        if (!js_string(j, &s, &n)) {
            return JS_INVALID;
        }
        return JS_STRING;
    }
    if (c == 't' && js_match(j, "true")) {
        return JS_BOOL;
    }
    if (c == 'f' && js_match(j, "false")) {
        return JS_BOOL;
    }
    if (c == 'n' && js_match(j, "null")) {
        return JS_NULL;
    }
    if (c == '-' || (c >= '0' && c <= '9')) {
        return js_number(j) ? JS_NUMBER : JS_INVALID;
    }
    return JS_INVALID;
}

/* ------------------------------------------------------------------ */
/* Schema parsing                                                      */
/* ------------------------------------------------------------------ */

typedef struct schema {
    int type;              /* -1 if unspecified; else a JS_* code. */
    const char *required[COG_TOOLKIT_SCHEMA_MAX_REQUIRED];
    size_t required_len[COG_TOOLKIT_SCHEMA_MAX_REQUIRED];
    size_t required_count;
} schema_t;

static int schema_type_code(const char *s, size_t n)
{
    if (n == 6U && memcmp(s, "object", 6U) == 0) {
        return JS_OBJECT;
    }
    if (n == 5U && memcmp(s, "array", 5U) == 0) {
        return JS_ARRAY;
    }
    if (n == 6U && memcmp(s, "string", 6U) == 0) {
        return JS_STRING;
    }
    if (n == 6U && memcmp(s, "number", 6U) == 0) {
        return JS_NUMBER;
    }
    /* "integer" is validated as a number (the subset does not distinguish
     * integral from fractional JSON numbers). */
    if (n == 7U && memcmp(s, "integer", 7U) == 0) {
        return JS_NUMBER;
    }
    if (n == 7U && memcmp(s, "boolean", 7U) == 0) {
        return JS_BOOL;
    }
    if (n == 4U && memcmp(s, "null", 4U) == 0) {
        return JS_NULL;
    }
    return JS_INVALID;
}

static bool schema_required(js_t *j, schema_t *out)
{
    const char *name;
    size_t name_len;

    if (!js_match(j, "[")) {
        return false;
    }
    if (js_peek(j) == ']') {
        j->p++;
        return true;
    }
    for (;;) {
        if (out->required_count >= COG_TOOLKIT_SCHEMA_MAX_REQUIRED) {
            return false;
        }
        if (!js_string(j, &name, &name_len)) {
            return false;
        }
        out->required[out->required_count] = name;
        out->required_len[out->required_count] = name_len;
        out->required_count++;
        if (js_peek(j) == ',') {
            j->p++;
            continue;
        }
        if (js_peek(j) == ']') {
            j->p++;
            return true;
        }
        return false;
    }
}

static int schema_parse(const char *schema, size_t schema_len, schema_t *out)
{
    js_t j;
    const char *key;
    size_t key_len;

    j.p = schema;
    j.end = schema + schema_len;
    out->type = -1;
    out->required_count = 0U;

    if (!js_match(&j, "{")) {
        return JS_INVALID;
    }
    if (js_peek(&j) == '}') {
        j.p++;
        js_skip_ws(&j);
        return j.p == j.end ? JS_OBJECT : JS_INVALID;
    }
    for (;;) {
        if (!js_string(&j, &key, &key_len)) {
            return JS_INVALID;
        }
        if (!js_match(&j, ":")) {
            return JS_INVALID;
        }
        if (key_len == 4U && memcmp(key, "type", 4U) == 0) {
            const char *tv;
            size_t tv_len;
            if (!js_string(&j, &tv, &tv_len)) {
                return JS_INVALID;
            }
            out->type = schema_type_code(tv, tv_len);
            if (out->type == JS_INVALID) {
                return JS_INVALID; /* unknown type value */
            }
        } else if (key_len == 8U && memcmp(key, "required", 8U) == 0) {
            if (!schema_required(&j, out)) {
                return JS_INVALID;
            }
        } else {
            /* Unknown keyword: skip its value. */
            if (js_value(&j) == JS_INVALID) {
                return JS_INVALID;
            }
        }
        if (js_peek(&j) == ',') {
            j.p++;
            continue;
        }
        if (js_peek(&j) == '}') {
            j.p++;
            js_skip_ws(&j);
            return j.p == j.end ? JS_OBJECT : JS_INVALID;
        }
        return JS_INVALID;
    }
}

/** True if the JSON object @p json contains a top-level key @p key of
 *  length @p key_len.  The key is a borrowed pointer into the schema
 *  string and is not NUL-terminated, so the length is passed explicitly. */
static bool object_has_key(const char *json, size_t len, const char *key,
                           size_t key_len)
{
    js_t j;
    const char *k;
    size_t klen;

    j.p = json;
    j.end = json + len;
    if (!js_match(&j, "{")) {
        return false;
    }
    if (js_peek(&j) == '}') {
        return false;
    }
    for (;;) {
        if (!js_string(&j, &k, &klen)) {
            return false;
        }
        if (klen == key_len && memcmp(k, key, key_len) == 0) {
            return true;
        }
        if (!js_match(&j, ":")) {
            return false;
        }
        if (js_value(&j) == JS_INVALID) {
            return false;
        }
        if (js_peek(&j) == ',') {
            j.p++;
            continue;
        }
        if (js_peek(&j) == '}') {
            return false;
        }
        return false;
    }
}

cog_result_t cog_toolkit_result_validate_schema(
    const cog_toolkit_result_t *result,
    const char *schema)
{
    schema_t sch;
    js_t out;
    int out_type;
    size_t schema_len;

    if (result == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    if (result->status != COG_OK) {
        return COG_ERROR_INVALID_STATE;
    }
    if (result->output == NULL) {
        return COG_ERROR_INVALID_ARG;
    }
    if (schema == NULL || schema[0] == '\0') {
        return COG_OK; /* no validation required */
    }

    schema_len = strlen(schema);
    if (schema_parse(schema, schema_len, &sch) == JS_INVALID) {
        return COG_ERROR_INVALID_ARG; /* malformed schema */
    }

    /* Parse the output as a JSON value and require the whole string to be
     * consumed. */
    out.p = result->output;
    out.end = result->output + result->output_len;
    out_type = js_value(&out);
    if (out_type == JS_INVALID) {
        return COG_ERROR_INVALID_ARG; /* output is not valid JSON */
    }
    js_skip_ws(&out);
    if (out.p != out.end) {
        return COG_ERROR_INVALID_ARG; /* trailing garbage after the value */
    }

    if (sch.type != -1 && sch.type != out_type) {
        return COG_ERROR_INVALID_ARG;
    }
    if (out_type == JS_OBJECT && sch.required_count > 0U) {
        size_t i;
        for (i = 0U; i < sch.required_count; i++) {
            if (!object_has_key(result->output, result->output_len,
                                sch.required[i], sch.required_len[i])) {
                return COG_ERROR_INVALID_ARG;
            }
        }
    }
    return COG_OK;
}

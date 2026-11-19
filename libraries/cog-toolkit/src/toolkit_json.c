/** @file toolkit_json.c @brief Strict constructor-bounded JSON/schema subset. */
#include "toolkit_json.h"
#include <cog/toolkit_exec.h>
#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static bool equal_string(yyjson_val *a, yyjson_val *b)
{
    return yyjson_get_len(a) == yyjson_get_len(b) &&
        memcmp(yyjson_get_str(a), yyjson_get_str(b), yyjson_get_len(a)) == 0;
}

/* Check nesting before entering the parser, whose storage is also pool-bound. */
static cog_result_t depth_check(const char *text, size_t len, size_t limit)
{
    size_t depth = 0U;
    bool string = false;
    bool escape = false;
    for (size_t i = 0U; i < len; i++) {
        char ch = text[i];
        if (string) {
            if (escape) { escape = false; }
            else if (ch == '\\') { escape = true; }
            else if (ch == '"') { string = false; }
        } else if (ch == '"') { string = true; }
        else if (ch == '{' || ch == '[') {
            if (++depth > limit) { return COG_ERROR_LIMIT; }
        } else if (ch == '}' || ch == ']') {
            if (depth != 0U) { depth--; }
        }
    }
    return COG_OK;
}

/* Exact integer arithmetic over a decimal JSON number, without double rounding. */
static cog_result_t integer_parse(yyjson_val *value, int64_t *out)
{
    const char *raw = yyjson_get_raw(value);
    const char *p = raw;
    size_t digits = 0U;
    size_t fractional = 0U;
    size_t trailing = 0U;
    bool decimal = false;
    bool negative;
    int exponent = 0;
    int64_t shift;
    uint64_t magnitude = 0U;
    uint64_t bound;
    if (raw == NULL) { return COG_ERROR_INVALID_ARG; }
    negative = *p == '-';
    if (negative) { p++; }
    const char *start = p;
    while (*p != '\0' && *p != 'e' && *p != 'E') {
        if (*p == '.') { decimal = true; p++; continue; }
        digits++;
        if (decimal) { fractional++; }
        trailing = *p == '0' ? trailing + 1U : 0U;
        p++;
    }
    if (*p != '\0') {
        bool exp_negative;
        p++;
        exp_negative = *p == '-';
        if (*p == '-' || *p == '+') { p++; }
        while (*p != '\0') {
            if (exponent < 100000) { exponent = exponent * 10 + (*p - '0'); }
            p++;
        }
        if (exp_negative) { exponent = -exponent; }
    }
    /* All-zero numbers remain exact even with extreme exponents. */
    if (trailing == digits) { *out = 0; return COG_OK; }
    if (fractional > (size_t)INT32_MAX) { return COG_ERROR_INVALID_ARG; }
    shift = (int64_t)exponent - (int64_t)fractional;
    if (shift < 0 && (size_t)(-shift) > trailing) { return COG_ERROR_INVALID_ARG; }
    size_t consume = shift < 0 ? digits - (size_t)(-shift) : digits;
    bound = negative ? (uint64_t)INT64_MAX + 1U : (uint64_t)INT64_MAX;
    size_t seen = 0U;
    for (p = start; seen < consume; p++) {
        if (*p == '.') { continue; }
        uint64_t digit = (uint64_t)(unsigned char)(*p - '0');
        if (magnitude > (bound - digit) / 10U) { return COG_ERROR_UNSUPPORTED; }
        magnitude = magnitude * 10U + digit;
        seen++;
    }
    if (shift > 19) { return COG_ERROR_UNSUPPORTED; }
    while (shift > 0) {
        if (magnitude > bound / 10U) { return COG_ERROR_UNSUPPORTED; }
        magnitude *= 10U;
        shift--;
    }
    if (negative && magnitude == (uint64_t)INT64_MAX + 1U) { *out = INT64_MIN; }
    else { *out = negative ? -(int64_t)magnitude : (int64_t)magnitude; }
    return COG_OK;
}

static bool integer_value(yyjson_val *value, int64_t *out)
{
    return integer_parse(value, out) == COG_OK;
}

/* The pinned fast floating reader allocates nothing. Explicit refusal callbacks
   also prevent a future disabled-fast-reader build from falling back to libc. */
static void *numeric_malloc(void *context, size_t bytes)
{ (void)context; (void)bytes; return NULL; }
static void *numeric_realloc(void *context, void *memory, size_t old_bytes, size_t bytes)
{ (void)context; (void)memory; (void)old_bytes; (void)bytes; return NULL; }
static void numeric_free(void *context, void *memory)
{ (void)context; (void)memory; }
static const yyjson_alc numeric_allocator = { numeric_malloc, numeric_realloc, numeric_free, NULL };

static bool number_value(yyjson_val *value, double *out)
{
    const char *raw = yyjson_get_raw(value);
    yyjson_val parsed;
    if (raw == NULL) { return false; }
    const char *end = yyjson_read_number(raw, &parsed, 0U, &numeric_allocator, NULL);
    if (end == NULL || *end != '\0') { return false; }
    *out = yyjson_get_num(&parsed);
    return isfinite(*out);
}

static cog_result_t strict_value(yyjson_val *value)
{
    if (yyjson_is_raw(value)) {
        double number;
        return number_value(value, &number) ? COG_OK : COG_ERROR_UNSUPPORTED;
    }
    if (yyjson_is_obj(value)) {
        size_t index, max;
        yyjson_val *key, *member;
        yyjson_obj_foreach(value, index, max, key, member) {
            size_t j, n;
            yyjson_val *other, *other_value;
            yyjson_obj_foreach(value, j, n, other, other_value) {
                (void)other_value;
                if (j >= index) { break; }
                if (equal_string(key, other)) { return COG_ERROR_INVALID_ARG; }
            }
            cog_result_t r = strict_value(member);
            if (r != COG_OK) { return r; }
        }
    } else if (yyjson_is_arr(value)) {
        size_t i, n;
        yyjson_val *member;
        yyjson_arr_foreach(value, i, n, member) {
            cog_result_t r = strict_value(member);
            if (r != COG_OK) { return r; }
        }
    }
    return COG_OK;
}

static bool is_name(yyjson_val *value, const char *name)
{
    return yyjson_is_str(value) && yyjson_get_len(value) == strlen(name) &&
        memcmp(yyjson_get_str(value), name, strlen(name)) == 0;
}

static bool size_value(yyjson_val *value, size_t *out)
{
    int64_t integer;
    if (!integer_value(value, &integer) || integer < 0) { return false; }
    if ((uint64_t)integer > (uint64_t)SIZE_MAX) { return false; }
    *out = (size_t)integer;
    return true;
}

static bool known_type(yyjson_val *value)
{
    static const char *const names[] = {
        "object", "array", "string", "number", "integer", "boolean", "null"
    };
    for (size_t i = 0U; i < sizeof(names) / sizeof(names[0]); i++) {
        if (is_name(value, names[i])) { return true; }
    }
    return false;
}

static cog_result_t schema_check(yyjson_val *schema)
{
    size_t index, max;
    yyjson_val *key, *value;
    if (!yyjson_is_obj(schema)) { return COG_ERROR_UNSUPPORTED; }
    yyjson_obj_foreach(schema, index, max, key, value) {
        if (is_name(key, "type")) {
            if (!known_type(value)) { return COG_ERROR_INVALID_ARG; }
        } else if (is_name(key, "properties")) {
            size_t i, n;
            yyjson_val *k, *v;
            if (!yyjson_is_obj(value)) { return COG_ERROR_INVALID_ARG; }
            yyjson_obj_foreach(value, i, n, k, v) {
                (void)k;
                cog_result_t r = schema_check(v);
                if (r != COG_OK) { return r; }
            }
        } else if (is_name(key, "items")) {
            cog_result_t r = schema_check(value);
            if (r != COG_OK) { return r; }
        } else if (is_name(key, "required")) {
            size_t i, n;
            yyjson_val *v;
            if (!yyjson_is_arr(value)) { return COG_ERROR_INVALID_ARG; }
            yyjson_arr_foreach(value, i, n, v) {
                if (!yyjson_is_str(v)) { return COG_ERROR_INVALID_ARG; }
                for (size_t j = 0U; j < i; j++) {
                    if (equal_string(v, yyjson_arr_get(value, j))) {
                        return COG_ERROR_INVALID_ARG;
                    }
                }
            }
        } else if (is_name(key, "additionalProperties")) {
            if (!yyjson_is_bool(value)) { return COG_ERROR_UNSUPPORTED; }
            if (yyjson_get_bool(value)) { return COG_ERROR_UNSUPPORTED; }
        } else if (is_name(key, "enum")) {
            if (!yyjson_is_arr(value) || yyjson_arr_size(value) == 0U) {
                return COG_ERROR_INVALID_ARG;
            }
        } else if (is_name(key, "minimum") || is_name(key, "maximum")) {
            double number;
            if (!number_value(value, &number)) { return COG_ERROR_UNSUPPORTED; }
            if (fabs(number) > 9007199254740991.0) { return COG_ERROR_UNSUPPORTED; }
        } else if (is_name(key, "minLength") || is_name(key, "maxLength") ||
                   is_name(key, "minItems") || is_name(key, "maxItems")) {
            size_t count;
            if (!size_value(value, &count)) { return COG_ERROR_INVALID_ARG; }
        } else if (is_name(key, "title") || is_name(key, "description") ||
                   is_name(key, "$schema")) {
            if (!yyjson_is_str(value)) { return COG_ERROR_INVALID_ARG; }
        } else { return COG_ERROR_UNSUPPORTED; }
    }
    return COG_OK;
}

static bool json_equal(yyjson_val *a, yyjson_val *b)
{
    if (yyjson_is_raw(a) && yyjson_is_raw(b)) {
        int64_t ia, ib;
        double da, db;
        if (integer_value(a, &ia) && integer_value(b, &ib)) { return ia == ib; }
        return number_value(a, &da) && number_value(b, &db) && da == db;
    }
    if (yyjson_get_type(a) != yyjson_get_type(b)) { return false; }
    if (yyjson_is_str(a)) { return equal_string(a, b); }
    if (yyjson_is_bool(a)) { return yyjson_get_bool(a) == yyjson_get_bool(b); }
    if (yyjson_is_null(a)) { return true; }
    if (yyjson_is_arr(a)) {
        size_t n = yyjson_arr_size(a);
        if (n != yyjson_arr_size(b)) { return false; }
        for (size_t i = 0U; i < n; i++) {
            if (!json_equal(yyjson_arr_get(a, i), yyjson_arr_get(b, i))) { return false; }
        }
        return true;
    }
    if (yyjson_is_obj(a)) {
        size_t i, n;
        yyjson_val *key, *value;
        if (yyjson_obj_size(a) != yyjson_obj_size(b)) { return false; }
        yyjson_obj_foreach(a, i, n, key, value) {
            yyjson_val *other = yyjson_obj_getn(b, yyjson_get_str(key), yyjson_get_len(key));
            if (other == NULL || !json_equal(value, other)) { return false; }
        }
        return true;
    }
    return false;
}

static bool matches_type(yyjson_val *value, yyjson_val *type)
{
    int64_t integer;
    return type == NULL ||
        (is_name(type, "object") && yyjson_is_obj(value)) ||
        (is_name(type, "array") && yyjson_is_arr(value)) ||
        (is_name(type, "string") && yyjson_is_str(value)) ||
        (is_name(type, "number") && yyjson_is_raw(value)) ||
        (is_name(type, "integer") && integer_value(value, &integer)) ||
        (is_name(type, "boolean") && yyjson_is_bool(value)) ||
        (is_name(type, "null") && yyjson_is_null(value));
}

static cog_result_t schema_apply(yyjson_val *value, yyjson_val *schema)
{
    yyjson_val *type = yyjson_obj_get(schema, "type");
    if (is_name(type, "integer") && yyjson_is_raw(value)) {
        int64_t integer;
        cog_result_t numeric = integer_parse(value, &integer);
        if (numeric == COG_ERROR_UNSUPPORTED) { return numeric; }
    }
    if (!matches_type(value, yyjson_obj_get(schema, "type"))) {
        return COG_ERROR_INVALID_ARG;
    }
    yyjson_val *enumeration = yyjson_obj_get(schema, "enum");
    if (enumeration != NULL) {
        bool found = false;
        for (size_t i = 0U; i < yyjson_arr_size(enumeration); i++) {
            if (json_equal(value, yyjson_arr_get(enumeration, i))) { found = true; break; }
        }
        if (!found) { return COG_ERROR_INVALID_ARG; }
    }
    if (yyjson_is_obj(value)) {
        yyjson_val *properties = yyjson_obj_get(schema, "properties");
        yyjson_val *required = yyjson_obj_get(schema, "required");
        for (size_t i = 0U; i < yyjson_arr_size(required); i++) {
            yyjson_val *key = yyjson_arr_get(required, i);
            if (yyjson_obj_getn(value, yyjson_get_str(key), yyjson_get_len(key)) == NULL) {
                return COG_ERROR_INVALID_ARG;
            }
        }
        size_t i, n;
        yyjson_val *key, *member;
        yyjson_obj_foreach(value, i, n, key, member) {
            yyjson_val *property = yyjson_obj_getn(properties, yyjson_get_str(key), yyjson_get_len(key));
            if (property == NULL && yyjson_obj_get(schema, "additionalProperties") != NULL) {
                return COG_ERROR_INVALID_ARG;
            }
            if (property != NULL) {
                cog_result_t r = schema_apply(member, property);
                if (r != COG_OK) { return r; }
            }
        }
    }
    if (yyjson_is_arr(value) || yyjson_is_str(value)) {
        size_t count = yyjson_arr_size(value);
        bool string = yyjson_is_str(value);
        if (string) {
            const unsigned char *p = (const unsigned char *)yyjson_get_str(value);
            count = 0U;
            for (size_t i = 0U; i < yyjson_get_len(value); i++) {
                if ((p[i] & 0xC0U) != 0x80U) { count++; }
            }
        }
        yyjson_val *minimum = yyjson_obj_get(schema, string ? "minLength" : "minItems");
        yyjson_val *maximum = yyjson_obj_get(schema, string ? "maxLength" : "maxItems");
        size_t limit;
        if (minimum != NULL && size_value(minimum, &limit) && count < limit) { return COG_ERROR_INVALID_ARG; }
        if (maximum != NULL && size_value(maximum, &limit) && count > limit) { return COG_ERROR_INVALID_ARG; }
        yyjson_val *items = yyjson_obj_get(schema, "items");
        if (!string && items != NULL) {
            for (size_t i = 0U; i < count; i++) {
                cog_result_t r = schema_apply(yyjson_arr_get(value, i), items);
                if (r != COG_OK) { return r; }
            }
        }
    }
    if (yyjson_is_raw(value)) {
        double number, boundary;
        (void)number_value(value, &number);
        yyjson_val *minimum = yyjson_obj_get(schema, "minimum");
        yyjson_val *maximum = yyjson_obj_get(schema, "maximum");
        if (minimum != NULL && number_value(minimum, &boundary) && number < boundary) { return COG_ERROR_INVALID_ARG; }
        if (maximum != NULL && number_value(maximum, &boundary) && number > boundary) { return COG_ERROR_INVALID_ARG; }
    }
    return COG_OK;
}

static cog_result_t parse(cog_toolkit_json_t *json, const char *text,
                          size_t len, yyjson_doc **out)
{
    yyjson_read_err error;
    cog_result_t r = depth_check(text, len, json->max_depth);
    if (r != COG_OK) { return r; }
    /* No permissive flags: UTF-8, escapes, numbers and trailing bytes are strict. */
    *out = yyjson_read_opts((char *)(void *)text, len, YYJSON_READ_NUMBER_AS_RAW,
                            &json->allocator, &error);
    if (*out == NULL) {
        return error.code == YYJSON_READ_ERROR_MEMORY_ALLOCATION ? COG_ERROR_LIMIT : COG_ERROR_INVALID_ARG;
    }
    return strict_value(yyjson_doc_get_root(*out));
}

struct cog_toolkit_validator {
    cog_toolkit_json_t json;
    size_t argument_limit;
    size_t schema_limit;
};

static size_t byte_limit(size_t configured)
{
    return configured != 0U ? configured : 65536U;
}

static size_t schema_length(const char *schema, size_t limit)
{
    size_t length = 0U;
    if (schema != NULL) {
        while (length < limit && schema[length] != '\0') { length++; }
    }
    return length;
}

static cog_result_t reserve_pool(cog_toolkit_json_t *json, const cog_tool_async_config_t *config)
{
    json->pool_bytes = config->codec_workspace_bytes != 0U ? config->codec_workspace_bytes : 1048576U;
    json->max_depth = config->max_json_depth != 0U ? config->max_json_depth : 64U;
    if (json->max_depth > 256U) { return COG_ERROR_LIMIT; }
    json->pool = malloc(json->pool_bytes);
    if (json->pool == NULL) { return COG_ERROR_NOMEM; }
    if (!yyjson_alc_pool_init(&json->allocator, json->pool, json->pool_bytes)) {
        cog_toolkit_json_free(json);
        return COG_ERROR_LIMIT;
    }
    return COG_OK;
}

static cog_result_t check_documents(cog_toolkit_json_t *json, const char *arguments,
    size_t arguments_len, const char *schema, size_t schema_len)
{
    cog_result_t r = COG_OK;
    if (arguments != NULL) {
        r = parse(json, arguments, arguments_len, &json->arguments);
        if (r == COG_OK && !yyjson_is_obj(yyjson_doc_get_root(json->arguments))) {
            return COG_ERROR_INVALID_ARG;
        }
    }
    if (r == COG_OK && schema_len != 0U) {
        r = parse(json, schema, schema_len, &json->schema);
        if (r == COG_OK) { r = schema_check(yyjson_doc_get_root(json->schema)); }
        if (r == COG_OK && arguments != NULL) {
            r = schema_apply(yyjson_doc_get_root(json->arguments), yyjson_doc_get_root(json->schema));
        }
    }
    return r;
}

static void release_documents(cog_toolkit_json_t *json)
{
    yyjson_doc_free(json->arguments);
    yyjson_doc_free(json->schema);
    json->arguments = NULL;
    json->schema = NULL;
}

cog_result_t cog_toolkit_json_new(cog_toolkit_json_t *json,
    const char *arguments, size_t arguments_len, const char *schema,
    const cog_tool_async_config_t *config)
{
    size_t schema_max = byte_limit(config->max_schema_bytes);
    size_t schema_len = schema_length(schema, schema_max);
    memset(json, 0, sizeof(*json));
    if (arguments_len >= byte_limit(config->max_argument_bytes) ||
        schema_len >= schema_max) { return COG_ERROR_LIMIT; }
    cog_result_t r = reserve_pool(json, config);
    if (r == COG_OK) { r = check_documents(json, arguments, arguments_len, schema, schema_len); }
    if (r != COG_OK) { cog_toolkit_json_free(json); }
    return r;
}

void cog_toolkit_json_free(cog_toolkit_json_t *json)
{
    if (json == NULL) { return; }
    release_documents(json);
    free(json->pool);
    memset(json, 0, sizeof(*json));
}

cog_result_t cog_toolkit_validator_new(cog_toolkit_validator_t **out,
    const cog_tool_async_config_t *config)
{
    if (out == NULL) { return COG_ERROR_NULL_PTR; }
    *out = NULL;
    if (config == NULL) { return COG_ERROR_NULL_PTR; }
    cog_toolkit_validator_t *validator = calloc(1U, sizeof(*validator));
    if (validator == NULL) { return COG_ERROR_NOMEM; }
    cog_result_t r = reserve_pool(&validator->json, config);
    if (r != COG_OK) {
        free(validator);
        return r;
    }
    validator->argument_limit = byte_limit(config->max_argument_bytes);
    validator->schema_limit = byte_limit(config->max_schema_bytes);
    *out = validator;
    return COG_OK;
}

static cog_result_t validate(cog_toolkit_validator_t *validator, const cog_tool_t *descriptor,
    const char *arguments, size_t bytes)
{
    if (validator == NULL || descriptor == NULL) { return COG_ERROR_NULL_PTR; }
    const char *schema = descriptor->input_schema;
    size_t schema_len = schema_length(schema, validator->schema_limit);
    if (schema_len >= validator->schema_limit || bytes >= validator->argument_limit) {
        return COG_ERROR_LIMIT;
    }
    cog_result_t r = check_documents(&validator->json, arguments, bytes, schema, schema_len);
    release_documents(&validator->json);
    return r;
}

cog_result_t cog_toolkit_validate_schema(cog_toolkit_validator_t *validator,
    const cog_tool_t *descriptor)
{
    return validate(validator, descriptor, NULL, 0U);
}

cog_result_t cog_toolkit_validate(cog_toolkit_validator_t *validator,
    const cog_tool_t *descriptor, const char *arguments, size_t bytes)
{
    if (arguments == NULL) { return COG_ERROR_NULL_PTR; }
    return validate(validator, descriptor, arguments, bytes);
}

cog_result_t cog_toolkit_validator_free(cog_toolkit_validator_t *validator)
{
    if (validator != NULL) {
        cog_toolkit_json_free(&validator->json);
        free(validator);
    }
    return COG_OK;
}

/* Legacy validator has no constructor/config parameter. Automatic local storage
   bounds its parser without hidden allocation or a process-global codec pool. */
cog_result_t cog_toolkit_json_validate_output(const char *text, size_t length,
                                             const char *schema)
{
    _Alignas(max_align_t) unsigned char pool[1048576U];
    cog_toolkit_json_t json = {0};
    json.pool = pool;
    json.pool_bytes = sizeof(pool);
    json.max_depth = 64U;
    if (length >= 65536U || strlen(schema) >= 65536U) { return COG_ERROR_LIMIT; }
    if (!yyjson_alc_pool_init(&json.allocator, pool, sizeof(pool))) { return COG_ERROR_LIMIT; }
    cog_result_t r = parse(&json, text, length, &json.arguments);
    if (r == COG_OK) { r = parse(&json, schema, strlen(schema), &json.schema); }
    if (r == COG_OK) { r = schema_check(yyjson_doc_get_root(json.schema)); }
    if (r == COG_OK) { r = schema_apply(yyjson_doc_get_root(json.arguments), yyjson_doc_get_root(json.schema)); }
    yyjson_doc_free(json.arguments);
    yyjson_doc_free(json.schema);
    return r;
}

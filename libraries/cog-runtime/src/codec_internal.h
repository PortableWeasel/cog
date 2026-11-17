/** @file codec_internal.h
 * @brief Pure private wire helpers shared by the four O5 codecs.
 * No allocation, I/O, authority or mutable global state. Writers are two-pass:
 * first count under checked SIZE_MAX arithmetic, then write the proven size.
 * Readers consume only complete bounded fields. UTF-8 excludes embedded NUL,
 * overlong forms, surrogates and values beyond U+10FFFF. Region checks use
 * integer addresses without forming out-of-object pointers; all named caller
 * inputs/outputs must be disjoint before the writing pass.
 */
#ifndef COG_CLI_CODEC_INTERNAL_H
#define COG_CLI_CODEC_INTERNAL_H
#include <cog/result.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef struct cli_wire_writer {
    unsigned char *bytes;
    size_t used;
    bool valid;
} cli_wire_writer_t;
typedef struct cli_wire_reader {
    const unsigned char *bytes;
    size_t length, used;
    bool valid;
} cli_wire_reader_t;
typedef struct cli_wire_region { const void *bytes; size_t length; } cli_wire_region_t;

static inline bool cli_wire_add(size_t *value, size_t increment)
{
    if (increment > SIZE_MAX - *value) return false;
    *value += increment;
    return true;
}
static inline bool cli_wire_disjoint(const void *a, size_t an, const void *b, size_t bn)
{
    if (an == 0U || bn == 0U) return true;
    uintptr_t x = (uintptr_t)a, y = (uintptr_t)b;
    return x <= y ? an <= y - x : bn <= x - y;
}
static inline bool cli_wire_regions(const cli_wire_region_t *regions, size_t count)
{
    for (size_t i = 0U; i < count; ++i)
        for (size_t j = 0U; j < i; ++j)
            if (!cli_wire_disjoint(regions[i].bytes, regions[i].length,
                regions[j].bytes, regions[j].length)) return false;
    return true;
}
static inline bool cli_wire_utf8(const void *text, size_t length)
{
    if (text == NULL) return length == 0U;
    const unsigned char *p = text;
    for (size_t i = 0U; i < length;) {
        unsigned char c = p[i++];
        if (c == 0U) return false;
        if (c < 0x80U) continue;
        size_t n;
        uint32_t value, minimum;
        if (c >= 0xc2U && c <= 0xdfU) { n = 1U; value = c & 0x1fU; minimum = 0x80U; }
        else if (c >= 0xe0U && c <= 0xefU) { n = 2U; value = c & 0x0fU; minimum = 0x800U; }
        else if (c >= 0xf0U && c <= 0xf4U) { n = 3U; value = c & 0x07U; minimum = 0x10000U; }
        else return false;
        if (n > length - i) return false;
        for (size_t j = 0U; j < n; ++j) {
            unsigned char tail = p[i++];
            if ((tail & 0xc0U) != 0x80U) return false;
            value = (value << 6U) | (uint32_t)(tail & 0x3fU);
        }
        if (value < minimum || value > 0x10ffffU ||
            (value >= 0xd800U && value <= 0xdfffU)) return false;
    }
    return true;
}
static inline bool cli_wire_cstr(const char *text, size_t maximum, size_t *length)
{
    if (text == NULL) return false;
    size_t n = 0U;
    while (text[n] != '\0') { if (n == maximum) return false; ++n; }
    if (!cli_wire_utf8(text, n)) return false;
    *length = n;
    return true;
}
static inline bool cli_wire_session(const void *text, size_t length)
{
    if (length == 0U || length > 127U || !cli_wire_utf8(text, length)) return false;
    const unsigned char *p = text;
    for (size_t i = 0U; i < length; ++i)
        if (p[i] < 32U || p[i] == 127U || p[i] == '/' || p[i] == '\\') return false;
    return true;
}
static inline bool cli_wire_result(cog_result_t result) { return result <= COG_OK && result >= COG_ERROR_INDETERMINATE; }
static inline cog_result_t cli_wire_signed(uint64_t value)
{
    /* Avoid implementation-defined unsigned-to-signed conversion for negative codes. */
    return value <= INT32_MAX ? (cog_result_t)value :
        (cog_result_t)(-1 - (int32_t)(UINT32_MAX - value));
}
static inline void cli_wire_bytes(cli_wire_writer_t *w, const void *bytes, size_t length)
{
    if (!w->valid || length > SIZE_MAX - w->used) { w->valid = false; return; }
    if (w->bytes != NULL && length != 0U) memcpy(w->bytes + w->used, bytes, length);
    w->used += length;
}
static inline void cli_wire_put(cli_wire_writer_t *w, uint64_t value, size_t width)
{
    unsigned char bytes[8];
    for (size_t i = 0U; i < width; ++i) bytes[i] = (unsigned char)(value >> ((width - i - 1U) * 8U));
    cli_wire_bytes(w, bytes, width);
}
static inline void cli_wire_string(cli_wire_writer_t *w, const char *text, size_t length)
{
    cli_wire_put(w, length, 4U); cli_wire_bytes(w, text, length);
}
static inline const unsigned char *cli_wire_take(cli_wire_reader_t *r, size_t length)
{
    if (!r->valid || length > r->length - r->used) { r->valid = false; return NULL; }
    const unsigned char *p = r->bytes + r->used; r->used += length; return p;
}
static inline uint64_t cli_wire_get(cli_wire_reader_t *r, size_t width)
{
    const unsigned char *p = cli_wire_take(r, width);
    uint64_t value = 0U;
    if (p != NULL) for (size_t i = 0U; i < width; ++i) value = (value << 8U) | p[i];
    return value;
}
static inline const char *cli_wire_read_string(cli_wire_reader_t *r, size_t *length)
{
    *length = (size_t)cli_wire_get(r, 4U);
    const unsigned char *p = cli_wire_take(r, *length);
    if (!r->valid || !cli_wire_utf8(p, *length)) { r->valid = false; return NULL; }
    return (const char *)p;
}
static inline char *cli_wire_copy(char **destination, const void *source, size_t length)
{
    char *p = *destination;
    if (length != 0U) memcpy(p, source, length);
    p[length] = '\0'; *destination += length + 1U; return p;
}
#endif

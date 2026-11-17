#include "internal.h"
bool ci_add(size_t a, size_t b, size_t *out) {
    if (b > SIZE_MAX - a)
        return false;
    *out = a + b;
    return true;
}
void *ci_array(size_t n, size_t width) {
    if (width != 0U && n > SIZE_MAX / width)
        return NULL;
    return calloc(n == 0U ? 1U : n, width);
}
char *ci_copy(ci_arena *a, const char *s, size_t n) {
    if (n == SIZE_MAX || n + 1U > a->capacity - a->used)
        return NULL;
    char *p = a->data + a->used;
    if (n != 0U)
        memcpy(p, s, n);
    p[n] = '\0';
    a->used += n + 1U;
    return p;
}
char *ci_string(ci_arena *a, const char *s) { return s == NULL ? NULL : ci_copy(a, s, strlen(s)); }
bool ci_equal(const char *a, const char *b) {
    return a == NULL ? b == NULL : b != NULL && strcmp(a, b) == 0;
}
bool ci_utf8_feed(ci_utf8 *s, const unsigned char *p, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        unsigned c = p[i];
        if (s->remaining == 0U) {
            if (c == 0U)
                return false;
            if (c < 128U)
                continue;
            if (c >= 194U && c <= 223U) {
                s->value = c & 31U;
                s->minimum = 128U;
                s->remaining = 1U;
            } else if (c >= 224U && c <= 239U) {
                s->value = c & 15U;
                s->minimum = 2048U;
                s->remaining = 2U;
            } else if (c >= 240U && c <= 244U) {
                s->value = c & 7U;
                s->minimum = 65536U;
                s->remaining = 3U;
            } else
                return false;
        } else {
            if ((c & 192U) != 128U)
                return false;
            s->value = (s->value << 6U) | (c & 63U);
            --s->remaining;
            if (s->remaining == 0U && (s->value < s->minimum || s->value > 1114111U ||
                                       (s->value >= 55296U && s->value <= 57343U)))
                return false;
        }
    }
    return true;
}
bool ci_text(const char *s, size_t n, size_t *characters) {
    if (s == NULL && n != 0U)
        return false;
    ci_utf8 u = {0};
    if (!ci_utf8_feed(&u, (const unsigned char *)s, n) || u.remaining != 0U)
        return false;
    if (characters != NULL) {
        size_t k = 0;
        for (size_t i = 0; i < n; ++i)
            if (((unsigned char)s[i] & 192U) != 128U)
                ++k;
        *characters = k;
    }
    return true;
}
bool ci_path(const char *s, bool empty, size_t *depth) {
    if (s == NULL)
        return false;
    size_t n = strlen(s), d = 0;
    if (n == 0U) {
        if (depth != NULL)
            *depth = 0;
        return empty;
    }
    if (!ci_text(s, n, NULL) || s[0] == '/' || s[n - 1U] == '/')
        return false;
    size_t begin = 0;
    for (size_t i = 0; i <= n; ++i)
        if (i == n || s[i] == '/') {
            size_t k = i - begin;
            if (k == 0U || (k == 1U && s[begin] == '.') ||
                (k == 2U && s[begin] == '.' && s[begin + 1U] == '.'))
                return false;
            ++d;
            begin = i + 1U;
        }
    if (depth != NULL)
        *depth = d;
    return true;
}
bool ci_root(const char *s) {
    return s != NULL && s[0] != '\0' && strchr(s, ':') == NULL && strchr(s, '/') == NULL &&
           ci_text(s, strlen(s), NULL);
}
bool ci_name(const char *s) {
    if (s == NULL)
        return false;
    size_t n = strlen(s);
    if (n == 0U || n > 64U || s[0] == '-' || s[n - 1U] == '-')
        return false;
    for (size_t i = 0; i < n; ++i) {
        char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-') ||
            (c == '-' && i != 0U && s[i - 1U] == '-'))
            return false;
    }
    return true;
}
void ci_diag(cog_instruction_diagnostic_t *e, cog_result_t rc, const cog_instruction_source_t *s,
             size_t offset, bool known) {
    if (e != NULL) {
        *e = (cog_instruction_diagnostic_t){.result = rc};
        if (rc != COG_OK && s != NULL) {
            e->root_id = ci_root(s->root_id) ? s->root_id : NULL;
            e->path = ci_path(s->path, true, NULL) ? s->path : NULL;
            e->byte_offset = offset;
            e->offset_known = known;
        }
    }
}
bool ci_limits(const cog_instruction_limits_t *l) {
    return l != NULL && l->max_sources && l->max_entries && l->max_string_bytes &&
           l->max_source_bytes && l->max_metadata_bytes && l->max_total_bytes && l->max_fragments &&
           l->max_tools && l->max_active_skills && l->max_resources;
}
static uint32_t rr(uint32_t x, unsigned n) { return (x >> n) | (x << (32U - n)); }
static void block(ci_sha *s, const unsigned char *p) {
    static const uint32_t k[64] = {
        0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U,
        0xab1c5ed5U, 0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU,
        0x9bdc06a7U, 0xc19bf174U, 0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU,
        0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU, 0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
        0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU,
        0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U, 0xa2bfe8a1U, 0xa81a664bU,
        0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U, 0x19a4c116U,
        0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
        0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U,
        0xc67178f2U};
    uint32_t w[64];
    for (size_t i = 0; i < 16U; ++i)
        w[i] = ((uint32_t)p[i * 4U] << 24U) | ((uint32_t)p[i * 4U + 1U] << 16U) |
               ((uint32_t)p[i * 4U + 2U] << 8U) | p[i * 4U + 3U];
    for (size_t i = 16; i < 64U; ++i) {
        uint32_t x = w[i - 15U], y = w[i - 2U];
        w[i] = w[i - 16U] + (rr(x, 7) ^ rr(x, 18) ^ (x >> 3U)) + w[i - 7U] +
               (rr(y, 17) ^ rr(y, 19) ^ (y >> 10U));
    }
    uint32_t a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4], f = s->h[5],
             g = s->h[6], h = s->h[7];
    for (size_t i = 0; i < 64U; ++i) {
        uint32_t t = h + (rr(e, 6) ^ rr(e, 11) ^ rr(e, 25)) + ((e & f) ^ ((~e) & g)) + k[i] + w[i];
        uint32_t u = (rr(a, 2) ^ rr(a, 13) ^ rr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g;
        g = f;
        f = e;
        e = d + t;
        d = c;
        c = b;
        b = a;
        a = t + u;
    }
    s->h[0] += a;
    s->h[1] += b;
    s->h[2] += c;
    s->h[3] += d;
    s->h[4] += e;
    s->h[5] += f;
    s->h[6] += g;
    s->h[7] += h;
}
void ci_sha_init(ci_sha *s) {
    *s = (ci_sha){.h = {0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU, 0x510e527fU,
                        0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U}};
}
void ci_sha_update(ci_sha *s, const void *bytes, size_t n) {
    const unsigned char *p = bytes;
    s->bytes += (uint64_t)n;
    while (n != 0U) {
        size_t k = 64U - s->used;
        if (k > n)
            k = n;
        memcpy(s->block + s->used, p, k);
        s->used += k;
        p += k;
        n -= k;
        if (s->used == 64U) {
            block(s, s->block);
            s->used = 0;
        }
    }
}
void ci_sha_finish(ci_sha *s, unsigned char out[32]) {
    uint64_t bits = s->bytes * 8U;
    s->block[s->used++] = 128U;
    if (s->used > 56U) {
        memset(s->block + s->used, 0, 64U - s->used);
        block(s, s->block);
        s->used = 0;
    }
    memset(s->block + s->used, 0, 56U - s->used);
    for (size_t i = 0; i < 8U; ++i)
        s->block[63U - i] = (unsigned char)(bits >> (i * 8U));
    block(s, s->block);
    for (size_t i = 0; i < 8U; ++i)
        for (size_t j = 0; j < 4U; ++j)
            out[i * 4U + j] = (unsigned char)(s->h[i] >> (24U - (unsigned)j * 8U));
}
void ci_hash(const void *p, size_t n, unsigned char out[32]) {
    ci_sha s;
    ci_sha_init(&s);
    ci_sha_update(&s, p, n);
    ci_sha_finish(&s, out);
}

void ci_wipe(void *bytes, size_t capacity) {
    if (bytes != NULL) {
        volatile unsigned char *p = bytes;
        while (capacity-- != 0U) *p++ = 0U;
    }
}

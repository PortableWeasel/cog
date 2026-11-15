#include <cog/ipc.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Independent reference window; fixed seed makes every failing trace repeatable. */
typedef struct model_item { uint64_t sequence; uint16_t type; size_t len; unsigned char bytes[18]; } model_item_t;
static uint32_t random_state = UINT32_C(0x579ad432);
static uint32_t random_next(void)
{
    random_state ^= random_state << 13;
    random_state ^= random_state >> 17;
    random_state ^= random_state << 5;
    return random_state;
}
/* Observe linker-wrapper effects across builtin/libc leaf assumptions. */
static volatile size_t allocation_calls, fail_at, live;
void *__real_malloc(size_t size);
void *__real_calloc(size_t count, size_t size);
void __real_free(void *ptr);
void *__wrap_malloc(size_t size);
void *__wrap_calloc(size_t count, size_t size);
void __wrap_free(void *ptr);
void *__wrap_malloc(size_t size)
{
    void *p;
    ++allocation_calls;
    if (allocation_calls == fail_at) return NULL;
    p = __real_malloc(size);
    if (p != NULL) ++live;
    return p;
}
void *__wrap_calloc(size_t count, size_t size)
{
    void *p;
    ++allocation_calls;
    if (allocation_calls == fail_at) return NULL;
    p = __real_calloc(count, size);
    if (p != NULL) ++live;
    return p;
}
void __wrap_free(void *ptr)
{
    if (ptr != NULL) { assert(live != 0); --live; }
    __real_free(ptr);
}
static void check_cursor(const cog_ipc_replay_t *r, const model_item_t *items,
                         size_t count, uint64_t tail, uint64_t generation, uint64_t after)
{
    cog_ipc_replay_record_t out, sentinel;
    unsigned char storage[128], before[128], wire[256], decoded[128];
    cog_ipc_replay_record_t roundtrip;
    size_t required = 12345, wn, i, need;
    uint64_t first = count != 0 ? items[0].sequence : 0;
    cog_result_t rc;
    memset(&out, 0x5a, sizeof(out)); memcpy(&sentinel, &out, sizeof(out));
    memset(storage, 0xa5, sizeof(storage)); memcpy(before, storage, sizeof(before));
    rc = cog_ipc_replay_copy_after(r, generation, after, &out, storage, sizeof(storage), &required);
    if (generation == 13 && after > tail) {
        assert(rc == COG_ERROR_INVALID_ARG && required == 12345);
        assert(memcmp(&out, &sentinel, sizeof(out)) == 0 && memcmp(storage, before, sizeof(storage)) == 0);
        return;
    }
    if (generation == 13 && after == tail) {
        assert(rc == COG_ERROR_AGAIN && required == 0);
        assert(memcmp(&out, &sentinel, sizeof(out)) == 0 && memcmp(storage, before, sizeof(storage)) == 0);
        return;
    }
    assert(rc == COG_OK && out.generation == 13 && strcmp(out.session_id, "reference") == 0);
    assert(out.first_sequence == first && out.last_sequence == (count != 0 ? tail : 0));
    need = 10;
    if (generation != 13 || (first != 0 && after < first - 1)) {
        assert(out.kind == COG_IPC_REPLAY_GAP && out.sequence == (first != 0 ? first - 1 : 0));
        assert(out.type == 0 && out.payload_len == 0 && out.payload == NULL);
    } else {
        for (i = 0; i < count && items[i].sequence != after + 1; ++i) {}
        assert(i < count && out.kind == COG_IPC_REPLAY_DATA && out.sequence == items[i].sequence);
        assert(out.type == items[i].type && out.payload_len == items[i].len);
        if (out.payload_len != 0) assert(memcmp(out.payload, items[i].bytes, out.payload_len) == 0);
        need += items[i].len;
    }
    assert(required == need);
    assert(cog_ipc_replay_encode(&out, wire, sizeof(wire), &wn) == COG_OK);
    assert(cog_ipc_replay_decode(wire, wn, &roundtrip, decoded, sizeof(decoded), &required) == COG_OK);
    assert(required == need && roundtrip.sequence == out.sequence && roundtrip.kind == out.kind);
    /* A one-byte-short retry must preserve every caller output and the source. */
    memcpy(&sentinel, &out, sizeof(out)); memcpy(before, storage, sizeof(before)); required = 12345;
    assert(cog_ipc_replay_copy_after(r, generation, after, &out, storage, need - 1, &required) == COG_ERROR_OVERFLOW);
    assert(required == need && memcmp(&out, &sentinel, sizeof(out)) == 0 && memcmp(storage, before, sizeof(storage)) == 0);
}
static void model_tests(void)
{
    size_t max_entries, max_bytes, step, i;
    for (max_entries = 1; max_entries <= 8; ++max_entries) {
        for (max_bytes = 1; max_bytes <= 16; ++max_bytes) {
            cog_ipc_replay_t *r = NULL;
            model_item_t items[9];
            size_t count = 0, used = 0, allocations;
            uint64_t tail = 0;
            assert(cog_ipc_replay_new(&r, "reference", 13, max_entries, max_bytes) == COG_OK);
            allocations = allocation_calls;
            check_cursor(r, items, count, tail, 13, 0);
            check_cursor(r, items, count, tail, 0, UINT64_MAX);
            for (step = 0; step < 200; ++step) {
                unsigned char bytes[18];
                size_t len = (size_t)(random_next() % (uint32_t)(max_bytes + 3));
                uint16_t type = (uint16_t)(1u + random_next() % 65535u);
                uint64_t sequence = UINT64_MAX;
                for (i = 0; i < sizeof(bytes); ++i) bytes[i] = (unsigned char)random_next();
                if (len > max_bytes) {
                    assert(cog_ipc_replay_append(r, type, bytes, len, &sequence) == COG_ERROR_LIMIT && sequence == UINT64_MAX);
                } else {
                    assert(cog_ipc_replay_append(r, type, len != 0 ? bytes : NULL, len, &sequence) == COG_OK);
                    assert(sequence == ++tail);
                    while (count >= max_entries || used + len > max_bytes) {
                        used -= items[0].len;
                        --count;
                        for (i = 0; i < count; ++i) items[i] = items[i + 1];
                    }
                    items[count].sequence = tail; items[count].type = type; items[count].len = len;
                    memcpy(items[count].bytes, bytes, len); ++count; used += len;
                    /* The owner must have copied the source bytes. */
                    memset(bytes, 0, sizeof(bytes));
                }
                for (i = 0; i <= count; ++i) {
                    uint64_t after = count != 0 ? items[0].sequence - 1 + (uint64_t)i : 0;
                    check_cursor(r, items, count, tail, 13, after);
                }
                check_cursor(r, items, count, tail, 13, 0);
                check_cursor(r, items, count, tail, 13, tail + 1);
                check_cursor(r, items, count, tail, 14, UINT64_MAX);
            }
            assert(allocation_calls == allocations);
            cog_ipc_replay_free(r); assert(live == 0);
        }
    }
}
static void decode_case(const unsigned char *wire, size_t len)
{
    unsigned char storage[512], before[512], encoded[512];
    cog_ipc_replay_record_t out, sentinel;
    size_t required = 12345, wn;
    cog_result_t rc;
    memset(&out, 0xa5, sizeof(out)); memcpy(&sentinel, &out, sizeof(out));
    memset(storage, 0x5a, sizeof(storage)); memcpy(before, storage, sizeof(storage));
    rc = cog_ipc_replay_decode(wire, len, &out, storage, sizeof(storage), &required);
    if (rc == COG_OK) {
        assert(cog_ipc_replay_encode(&out, encoded, sizeof(encoded), &wn) == COG_OK);
        assert(wn == len && memcmp(wire, encoded, len) == 0);
        assert(required == strlen(out.session_id) + 1 + out.payload_len);
    } else {
        assert(rc == COG_ERROR_PROTOCOL || rc == COG_ERROR_UNSUPPORTED);
        assert(required == 12345 && memcmp(&out, &sentinel, sizeof(out)) == 0 && memcmp(storage, before, sizeof(storage)) == 0);
    }
}
static void mutation_tests(void)
{
    static const unsigned char payload[] = {0, 0x80, 0xff};
    static const unsigned char golden[] = {
        'C','G','R','1',1, 1,2,3,4,5,6,7,8, 0,0,0,0,0,0,0,2,
        0,0,0,0,0,0,0,1, 0,0,0,0,0,0,0,3, 0xab,0xcd,
        0,0,0,3, 0,0,0,3, 's',0xc3,0xa9, 0,0x80,0xff
    };
    cog_ipc_replay_record_t rec = {COG_IPC_REPLAY_DATA, "s\xc3\xa9", UINT64_C(0x0102030405060708), 2, 1, 3, 0xabcd, payload, sizeof(payload)};
    unsigned char wire[512], altered[512];
    size_t wn, i, bit, kind, iterations;
    assert(cog_ipc_replay_encode(&rec, wire, sizeof(wire), &wn) == COG_OK);
    assert(wn == sizeof(golden) && memcmp(wire, golden, wn) == 0);
    for (kind = 1; kind <= 4; ++kind) {
        if (kind == 2) { rec.kind = COG_IPC_REPLAY_GAP; rec.sequence = 0; rec.type = 0; rec.payload_len = 0; rec.payload = NULL; }
        if (kind == 3) rec.first_sequence = rec.last_sequence = 0;
        if (kind == 4) { rec.kind = COG_IPC_REPLAY_CURSOR; rec.sequence = UINT64_MAX; }
        assert(cog_ipc_replay_encode(&rec, wire, sizeof(wire), &wn) == COG_OK);
        decode_case(wire, wn);
        for (i = 0; i < wn; ++i) {
            decode_case(wire, i);
            for (bit = 0; bit < 8; ++bit) {
                memcpy(altered, wire, wn); altered[i] ^= (unsigned char)(1u << bit); decode_case(altered, wn);
            }
        }
        memcpy(altered, wire, wn); altered[wn] = 0; decode_case(altered, wn + 1);
        for (iterations = 0; iterations < 3000; ++iterations) {
            size_t length = (size_t)(random_next() % 100u);
            memcpy(altered, wire, wn);
            for (i = wn; i < length; ++i) altered[i] = (unsigned char)random_next();
            for (i = 0; i < 3; ++i) altered[random_next() % 100u] = (unsigned char)random_next();
            decode_case(altered, length);
        }
    }
}
static void constructor_tests(void)
{
    cog_ipc_replay_t *r;
    char id[129];
    size_t i, before;
    memset(id, 'x', sizeof(id)); id[128] = '\0';
    for (i = 1; i <= 3; ++i) {
        allocation_calls = 0; fail_at = i; r = (cog_ipc_replay_t *)(uintptr_t)1;
        assert(cog_ipc_replay_new(&r, "s", 1, 1, 1) == COG_ERROR_NOMEM && r == NULL && live == 0);
    }
    fail_at = 0; before = allocation_calls;
    assert(cog_ipc_replay_new(&r, id, 1, 1, 1) == COG_ERROR_INVALID_ARG && r == NULL);
    assert(cog_ipc_replay_new(&r, "s", 1, 0, 1) == COG_ERROR_INVALID_ARG && r == NULL);
    assert(cog_ipc_replay_new(&r, "s", 1, 1, 0) == COG_ERROR_INVALID_ARG && r == NULL);
    assert(cog_ipc_replay_new(&r, "s", 1, SIZE_MAX, 1) == COG_ERROR_INVALID_ARG && r == NULL);
    assert(cog_ipc_replay_new(&r, NULL, 1, 1, 1) == COG_ERROR_NULL_PTR && r == NULL);
    assert(cog_ipc_replay_new(NULL, "s", 1, 1, 1) == COG_ERROR_NULL_PTR);
    assert(allocation_calls == before);
    id[127] = '\0'; assert(cog_ipc_replay_new(&r, id, UINT64_MAX, 1, 1) == COG_OK);
    cog_ipc_replay_free(r); assert(live == 0);
}

/* Build candidate frames independently of encode, then classify from the contract. */
static void write_scalar(unsigned char *p, uint64_t value, size_t width)
{
    size_t i;
    for (i = 0; i < width; ++i) p[i] = (unsigned char)(value >> (8u * (unsigned)(width - i - 1u)));
}
static void scalar_contract_tests(void)
{
    static const uint64_t values[] = {0, 1, 2, UINT64_MAX};
    static const uint16_t types[] = {0, 1, UINT16_MAX};
    unsigned char wire[64], encoded[64], storage[64], before[64];
    size_t kind, g, a, b, c, t, n;
    for (kind = 1; kind <= 3; ++kind) for (g = 0; g < 4; ++g)
    for (a = 0; a < 4; ++a) for (b = 0; b < 4; ++b) for (c = 0; c < 4; ++c)
    for (t = 0; t < 3; ++t) for (n = 0; n < 2; ++n) {
        cog_ipc_replay_record_t rec, out, saved;
        size_t required = 999;
        int valid;
        cog_result_t rc;
        rec.kind = (cog_ipc_replay_kind_t)kind; rec.session_id = "s";
        rec.generation = values[g]; rec.sequence = values[a];
        rec.first_sequence = values[b]; rec.last_sequence = values[c];
        rec.type = types[t]; rec.payload_len = n; rec.payload = "p";
        valid = rec.generation != 0;
        if (kind == 1) valid = valid && rec.first_sequence >= 1 && rec.first_sequence <= rec.sequence && rec.sequence <= rec.last_sequence && rec.type != 0;
        else if (kind == 2) valid = valid && n == 0 && rec.type == 0 &&
            ((rec.sequence == 0 && rec.first_sequence == 0 && rec.last_sequence == 0) ||
             (rec.first_sequence >= 1 && rec.first_sequence <= rec.last_sequence && rec.sequence == rec.first_sequence - 1));
        else valid = valid && n == 0 && rec.type == 0 && rec.first_sequence == 0 && rec.last_sequence == 0;
        memset(encoded, 0x59, sizeof(encoded)); memcpy(before, encoded, sizeof(before));
        rc = cog_ipc_replay_encode(&rec, encoded, sizeof(encoded), &required);
        assert(rc == (valid ? COG_OK : COG_ERROR_INVALID_ARG));
        if (!valid) assert(required == 999 && memcmp(encoded, before, sizeof(encoded)) == 0);
        memcpy(wire, "CGR1", 4); wire[4] = (unsigned char)kind;
        write_scalar(wire + 5, rec.generation, 8); write_scalar(wire + 13, rec.sequence, 8);
        write_scalar(wire + 21, rec.first_sequence, 8); write_scalar(wire + 29, rec.last_sequence, 8);
        write_scalar(wire + 37, rec.type, 2); write_scalar(wire + 39, 1, 4); write_scalar(wire + 43, (uint64_t)n, 4);
        wire[47] = 's'; wire[48] = 'p';
        if (valid) assert(required == 48 + n && memcmp(wire, encoded, required) == 0);
        memset(&out, 0x95, sizeof(out)); memcpy(&saved, &out, sizeof(out));
        memset(storage, 0x59, sizeof(storage)); memcpy(before, storage, sizeof(before)); required = 999;
        rc = cog_ipc_replay_decode(wire, 48 + n, &out, storage, sizeof(storage), &required);
        assert(rc == (valid ? COG_OK : COG_ERROR_PROTOCOL));
        if (!valid) assert(required == 999 && memcmp(&out, &saved, sizeof(out)) == 0 && memcmp(storage, before, sizeof(storage)) == 0);
        else assert(required == 2 + n && out.sequence == rec.sequence && out.generation == rec.generation);
    }
}
static void utf8_contract_tests(void)
{
    static const char *const invalid[] = {
        "\x80", "\xc0\x80", "\xc1\xbf", "\xc2", "\xc2\x20", "\xe0\x80\x80", "\xed\xa0\x80",
        "\xef\xbf", "\xf0\x80\x80\x80", "\xf4\x90\x80\x80", "\xf5\x80\x80\x80", "\xff"
    };
    static const char *const valid[] = {"\x01", "\x7f", "\xc2\x80", "\xdf\xbf", "\xe0\xa0\x80", "\xed\x9f\xbf", "\xf0\x90\x80\x80", "\xf4\x8f\xbf\xbf"};
    size_t i, n, required;
    unsigned char wire[64], storage[64];
    cog_ipc_replay_record_t rec = {COG_IPC_REPLAY_CURSOR, "s", 1, 0, 0, 0, 0, NULL, 0}, out;
    cog_ipc_replay_t *r = NULL;
    for (i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        rec.session_id = invalid[i];
        assert(cog_ipc_replay_new(&r, invalid[i], 1, 1, 1) == COG_ERROR_INVALID_ARG && r == NULL);
        assert(cog_ipc_replay_encode(&rec, wire, sizeof(wire), &required) == COG_ERROR_INVALID_ARG);
        rec.session_id = "s"; assert(cog_ipc_replay_encode(&rec, wire, sizeof(wire), &required) == COG_OK);
        n = strlen(invalid[i]); write_scalar(wire + 39, (uint64_t)n, 4); memcpy(wire + 47, invalid[i], n);
        assert(cog_ipc_replay_decode(wire, 47 + n, &out, storage, sizeof(storage), &required) == COG_ERROR_PROTOCOL);
    }
    for (i = 0; i < sizeof(valid) / sizeof(valid[0]); ++i) {
        rec.session_id = valid[i];
        assert(cog_ipc_replay_new(&r, valid[i], 1, 1, 1) == COG_OK); cog_ipc_replay_free(r);
        assert(cog_ipc_replay_encode(&rec, wire, sizeof(wire), &required) == COG_OK);
        assert(cog_ipc_replay_decode(wire, required, &out, storage, sizeof(storage), &n) == COG_OK);
        assert(strcmp(out.session_id, valid[i]) == 0);
    }
    rec.session_id = "s"; assert(cog_ipc_replay_encode(&rec, wire, sizeof(wire), &required) == COG_OK);
    wire[47] = 0; assert(cog_ipc_replay_decode(wire, required, &out, storage, sizeof(storage), &n) == COG_ERROR_PROTOCOL);
}


static void stable_copy_tests(void)
{
    cog_ipc_replay_t *r = NULL;
    cog_ipc_replay_record_t copied, gap;
    unsigned char storage[16], other[16], saved[16];
    uint64_t sequence;
    size_t required;
    assert(cog_ipc_replay_new(&r, "copy", 1, 1, 3) == COG_OK);
    assert(cog_ipc_replay_append(r, 7, "abc", 3, &sequence) == COG_OK);
    assert(cog_ipc_replay_copy_after(r, 1, 0, &copied, storage, sizeof(storage), &required) == COG_OK);
    assert(required == 8); memcpy(saved, storage, required);
    assert(cog_ipc_replay_append(r, 8, "xyz", 3, &sequence) == COG_OK);
    assert(cog_ipc_replay_copy_after(r, 1, 0, &gap, other, sizeof(other), &required) == COG_OK && gap.kind == COG_IPC_REPLAY_GAP);
    cog_ipc_replay_free(r);
    assert(memcmp(storage, saved, 8) == 0 && strcmp(copied.session_id, "copy") == 0);
    assert(copied.sequence == 1 && copied.type == 7 && copied.payload_len == 3 && memcmp(copied.payload, "abc", 3) == 0);
}

/* Positive control: optimized direct allocator calls must reach wrappers,
 * update the real oracle, and honor its fault injection before use. */
static void allocation_oracle_control(void)
{
    size_t before = allocation_calls;
    unsigned char *bytes = malloc(8U);
    assert(bytes != NULL && allocation_calls == before + 1U);
    bytes[0] = 0x5aU;
    assert(live == 1U);
    free(bytes);
    assert(live == 0U);
    before = allocation_calls;
    bytes = calloc(8U, 1U);
    assert(bytes != NULL && allocation_calls == before + 1U);
    assert(bytes[0] == 0U && bytes[7] == 0U);
    free(bytes);
    assert(live == 0U);
    before = allocation_calls;
    bytes = malloc(8U);
    free(bytes);
    assert(allocation_calls == before + 1U); /* Detect optimized-away allocator pairs. */
    before = allocation_calls;
    fail_at = allocation_calls + 1U;
    bytes = malloc(8U);
    assert(bytes == NULL && allocation_calls == before + 1U);
    fail_at = 0U;
    allocation_calls = 0U;
}

int main(void)
{
    allocation_oracle_control();
    constructor_tests(); model_tests(); mutation_tests(); scalar_contract_tests(); utf8_contract_tests(); stable_copy_tests(); assert(live == 0);
    puts("independent replay model, mutation and allocation checks passed");
    return 0;
}

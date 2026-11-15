#include <cog/ipc.h>
#include "ipc_replay_internal.h"
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static size_t allocation_calls;
static size_t fail_at;
void *__real_malloc(size_t size);
void *__real_calloc(size_t count, size_t size);
void *__real_realloc(void *ptr, size_t size);
void *__wrap_malloc(size_t size);
void *__wrap_calloc(size_t count, size_t size);
void *__wrap_realloc(void *ptr, size_t size);
void *__wrap_malloc(size_t size) { ++allocation_calls; return allocation_calls == fail_at ? NULL : __real_malloc(size); }
void *__wrap_calloc(size_t count, size_t size) { ++allocation_calls; return allocation_calls == fail_at ? NULL : __real_calloc(count, size); }
void *__wrap_realloc(void *ptr, size_t size) { ++allocation_calls; return allocation_calls == fail_at ? NULL : __real_realloc(ptr, size); }

static void ring_tests(void)
{
    cog_ipc_replay_t *r = NULL;
    cog_ipc_replay_record_t rec, saved;
    unsigned char buf[128], copy[128];
    const unsigned char payload[] = {0, 42, 255};
    uint64_t seq = 77;
    size_t need = 999;
    memset(&rec, 0xa5, sizeof(rec)); saved = rec;
    assert(cog_ipc_replay_new(&r, "s", 1, 2, 4) == COG_OK);
    assert(cog_ipc_replay_copy_after(r, 2, 0, &rec, NULL, 0, &need) == COG_ERROR_OVERFLOW && need == 2);
    assert(memcmp(&rec, &saved, sizeof(rec)) == 0);
    assert(cog_ipc_replay_copy_after(r, 2, 0, &rec, buf, sizeof(buf), &need) == COG_OK);
    assert(rec.kind == COG_IPC_REPLAY_GAP && rec.sequence == 0 && rec.first_sequence == 0 && rec.last_sequence == 0);
    assert(cog_ipc_replay_copy_after(r, 1, 0, &rec, buf, sizeof(buf), &need) == COG_ERROR_AGAIN && need == 0);
    assert(cog_ipc_replay_copy_after(r, 1, 1, &rec, buf, sizeof(buf), &need) == COG_ERROR_INVALID_ARG);
    assert(cog_ipc_replay_append(r, 0, payload, 3, &seq) == COG_ERROR_INVALID_ARG && seq == 77);
    assert(cog_ipc_replay_append(r, 1, payload, 5, &seq) == COG_ERROR_LIMIT && seq == 77);
    assert(cog_ipc_replay_append(r, 1, payload, 3, &seq) == COG_OK && seq == 1);
    assert(cog_ipc_replay_append(r, 2, NULL, 0, &seq) == COG_OK && seq == 2);
    memset(buf, 0x33, sizeof(buf)); memcpy(copy, buf, sizeof(buf)); saved = rec;
    assert(cog_ipc_replay_copy_after(r, 1, 0, &rec, buf, 4, &need) == COG_ERROR_OVERFLOW && need == 5);
    assert(memcmp(buf, copy, sizeof(buf)) == 0 && memcmp(&saved, &rec, sizeof(rec)) == 0);
    assert(cog_ipc_replay_copy_after(r, 1, 0, &rec, buf, 5, &need) == COG_OK);
    assert(rec.kind == COG_IPC_REPLAY_DATA && rec.sequence == 1 && rec.first_sequence == 1 && rec.last_sequence == 2);
    assert(rec.type == 1 && rec.payload_len == 3 && memcmp(rec.payload, payload, 3) == 0);
    assert(cog_ipc_replay_append(r, 3, payload, 3, &seq) == COG_OK && seq == 3);
    assert(cog_ipc_replay_copy_after(r, 1, 0, &rec, buf, sizeof(buf), &need) == COG_OK);
    assert(rec.kind == COG_IPC_REPLAY_GAP && rec.sequence == 1 && rec.first_sequence == 2 && rec.last_sequence == 3);
    assert(cog_ipc_replay_copy_after(r, 1, 1, &rec, buf, sizeof(buf), &need) == COG_OK);
    assert(rec.sequence == 2 && rec.payload_len == 0 && rec.type == 2);
    assert(cog_ipc_replay_copy_after(r, 1, 2, &rec, buf, sizeof(buf), &need) == COG_OK && rec.sequence == 3);
    assert(cog_ipc_replay_append(r, 4, payload, 3, &seq) == COG_OK && seq == 4);
    assert(cog_ipc_replay_copy_after(r, 1, 2, &rec, buf, sizeof(buf), &need) == COG_OK && rec.kind == COG_IPC_REPLAY_GAP && rec.first_sequence == 4);
    r->tail = UINT64_MAX - 1u;
    assert(cog_ipc_replay_append(r, 5, NULL, 0, &seq) == COG_OK && seq == UINT64_MAX);
    assert(cog_ipc_replay_copy_after(r, 1, UINT64_MAX - 1u, &rec, buf, sizeof(buf), &need) == COG_OK && rec.sequence == UINT64_MAX);
    seq = 99;
    assert(cog_ipc_replay_append(r, 5, NULL, 0, &seq) == COG_ERROR_LIMIT && seq == 99);
    assert(cog_ipc_replay_copy_after(r, 1, UINT64_MAX, &rec, buf, sizeof(buf), &need) == COG_ERROR_AGAIN);
    cog_ipc_replay_free(r);
    cog_ipc_replay_free(NULL);
    assert(cog_ipc_replay_new(&r, "", 1, 1, 1) == COG_ERROR_INVALID_ARG && r == NULL);
    assert(cog_ipc_replay_new(&r, "\xc0\x80", 1, 1, 1) == COG_ERROR_INVALID_ARG);
    assert(cog_ipc_replay_new(&r, "s", 0, 1, 1) == COG_ERROR_INVALID_ARG);
    assert(cog_ipc_replay_new(&r, "s", 1, SIZE_MAX, 1) == COG_ERROR_INVALID_ARG);
}
static void codec_tests(void)
{
    unsigned char wire[128], dest[128], before[128], altered[128];
    const unsigned char data[] = {0, 1, 255};
    cog_ipc_replay_record_t r = {COG_IPC_REPLAY_DATA, "\xc3\xa9", 9, 2, 1, 3, 7, data, 3}, out, saved;
    size_t need = 0, wn, i;
    memset(wire, 0x55, sizeof(wire)); memcpy(before, wire, sizeof(wire));
    assert(cog_ipc_replay_encode(&r, wire, 51, &need) == COG_ERROR_OVERFLOW && need == 52);
    assert(memcmp(before, wire, sizeof(wire)) == 0);
    assert(cog_ipc_replay_encode(&r, wire, sizeof(wire), &wn) == COG_OK && wn == 52);
    assert(memcmp(wire, "CGR1\1", 5) == 0 && wire[12] == 9 && wire[20] == 2 && wire[38] == 7);
    memset(&out, 0xa5, sizeof(out)); saved = out; memset(dest, 0x66, sizeof(dest)); memcpy(before, dest, sizeof(dest));
    assert(cog_ipc_replay_decode(wire, wn, &out, dest, 5, &need) == COG_ERROR_OVERFLOW && need == 6);
    assert(memcmp(&out, &saved, sizeof(out)) == 0 && memcmp(dest, before, sizeof(dest)) == 0);
    assert(cog_ipc_replay_decode(wire, wn, &out, dest, sizeof(dest), &need) == COG_OK);
    assert(strcmp(out.session_id, r.session_id) == 0 && out.payload_len == 3 && memcmp(out.payload, data, 3) == 0);
    memset(wire, 0, sizeof(wire)); assert(memcmp(out.payload, data, 3) == 0);
    assert(cog_ipc_replay_encode(&r, wire, sizeof(wire), &wn) == COG_OK);
    for (i = 0; i < wn; ++i) assert(cog_ipc_replay_decode(wire, i, &out, dest, sizeof(dest), &need) != COG_OK);
    memcpy(altered, wire, wn); altered[3] = '2'; assert(cog_ipc_replay_decode(altered, wn, &out, dest, sizeof(dest), &need) == COG_ERROR_UNSUPPORTED);
    memcpy(altered, wire, wn); altered[4] = 99; assert(cog_ipc_replay_decode(altered, wn, &out, dest, sizeof(dest), &need) == COG_ERROR_UNSUPPORTED);
    memcpy(altered, wire, wn); altered[47] = 0xff; assert(cog_ipc_replay_decode(altered, wn, &out, dest, sizeof(dest), &need) == COG_ERROR_PROTOCOL);
    memcpy(altered, wire, wn); altered[20] = 4; assert(cog_ipc_replay_decode(altered, wn, &out, dest, sizeof(dest), &need) == COG_ERROR_PROTOCOL);
    memcpy(altered, wire, wn); altered[wn] = 0; assert(cog_ipc_replay_decode(altered, wn+1, &out, dest, sizeof(dest), &need) == COG_ERROR_PROTOCOL);
    r.kind = COG_IPC_REPLAY_GAP; r.sequence = 0; r.first_sequence = 1; r.last_sequence = 3; r.type = 0; r.payload = NULL; r.payload_len = 0;
    assert(cog_ipc_replay_encode(&r, wire, sizeof(wire), &wn) == COG_OK);
    assert(cog_ipc_replay_decode(wire, wn, &out, dest, sizeof(dest), &need) == COG_OK && out.kind == COG_IPC_REPLAY_GAP);
    r.first_sequence = 0; r.last_sequence = 0; assert(cog_ipc_replay_encode(&r, wire, sizeof(wire), &wn) == COG_OK);
    r.kind = COG_IPC_REPLAY_CURSOR; r.sequence = UINT64_MAX; assert(cog_ipc_replay_encode(&r, wire, sizeof(wire), &wn) == COG_OK);
    assert(cog_ipc_replay_decode(wire, wn, &out, dest, sizeof(dest), &need) == COG_OK && out.sequence == UINT64_MAX);
#if SIZE_MAX > UINT32_MAX
    r.kind = COG_IPC_REPLAY_DATA; r.sequence = r.first_sequence = r.last_sequence = 1; r.type = 1;
    r.payload = data; r.payload_len = (size_t)UINT32_MAX + 1u; need = 77; memset(wire, 0x55, sizeof(wire)); memcpy(before, wire, sizeof(wire));
    assert(cog_ipc_replay_encode(&r, wire, sizeof(wire), &need) == COG_ERROR_LIMIT && need == 77 && memcmp(before, wire, sizeof(wire)) == 0);
#endif
}
static void allocation_tests(void)
{
    cog_ipc_replay_t *r = NULL; cog_ipc_replay_record_t rec;
    unsigned char b[128], wire[128]; size_t n, wn, prior, i; uint64_t seq;
    for (i = 1; i <= 3; ++i) {
        allocation_calls = 0; fail_at = i; r = (cog_ipc_replay_t *)(uintptr_t)1;
        assert(cog_ipc_replay_new(&r, "id", 1, 2, 10) == COG_ERROR_NOMEM && r == NULL);
    }
    fail_at = 0; assert(cog_ipc_replay_new(&r, "id", 1, 2, 10) == COG_OK);
    prior = allocation_calls;
    assert(cog_ipc_replay_append(r, 1, "abc", 3, &seq) == COG_OK);
    assert(cog_ipc_replay_copy_after(r, 1, 0, &rec, b, sizeof(b), &n) == COG_OK);
    assert(cog_ipc_replay_encode(&rec, wire, sizeof(wire), &wn) == COG_OK);
    assert(cog_ipc_replay_decode(wire, wn, &rec, b, sizeof(b), &n) == COG_OK);
    assert(allocation_calls == prior);
    cog_ipc_replay_free(r);
}
int main(void) { ring_tests(); codec_tests(); allocation_tests(); puts("replay tests passed"); return 0; }

#include <cog/orchestrator.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(expr) do { if (!(expr)) { \
    (void)fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); exit(1); \
} } while (0)

void *__wrap_malloc(size_t bytes);
void *__wrap_calloc(size_t count, size_t bytes);
void *__wrap_realloc(void *pointer, size_t bytes);
void __wrap_free(void *pointer);
void *__wrap_malloc(size_t bytes) { (void)bytes; abort(); }
void *__wrap_calloc(size_t count, size_t bytes) { (void)count; (void)bytes; abort(); }
void *__wrap_realloc(void *pointer, size_t bytes) { (void)pointer; (void)bytes; abort(); }
void __wrap_free(void *pointer) { (void)pointer; abort(); }

static cog_worker_control_t worker(cog_worker_control_kind_t kind)
{
    cog_worker_control_t m = {0};
    m.kind = kind;
    m.worker.incarnation = UINT64_C(0x0102030405060708);
    m.worker.number = UINT64_C(0x1112131415161718);
    if (kind == COG_WORKER_HEARTBEAT) m.active_subagents = UINT32_MAX;
    if (kind == COG_WORKER_SHUTDOWN || kind == COG_WORKER_SHUTDOWN_ACK)
        m.deadline_ms = UINT64_C(0x2122232425262728);
    if (kind == COG_WORKER_SHUTDOWN_ACK) m.outcome = COG_ERROR_INDETERMINATE;
    return m;
}

static cog_handoff_message_t handoff(cog_handoff_kind_t kind)
{
    cog_handoff_message_t m = {0};
    m.kind = kind; m.session_id = "s";
    m.source_peer = 0; m.target_peer = 2;
    m.expected.epoch = 3; m.expected.instance_id = 4;
    m.next.epoch = 4; m.next.instance_id = 5;
    if (kind != COG_HANDOFF_QUIESCE) m.checkpoint.generation = 6;
    return m;
}

static void worker_decode_failure(const unsigned char *bytes, size_t length,
                                  cog_result_t expected)
{
    cog_worker_control_t out;
    unsigned char before[sizeof(out)];
    memset(&out, 0xa5, sizeof(out)); memcpy(before, &out, sizeof(out));
    CHECK(cog_orchestrator_worker_decode(bytes, length, &out) == expected);
    CHECK(memcmp(before, &out, sizeof(out)) == 0);
}

static void handoff_decode_failure(const unsigned char *bytes, size_t length,
                                   cog_result_t expected)
{
    cog_handoff_message_t out;
    unsigned char before[sizeof(out)];
    char id[128], prior[128];
    size_t required = 777;
    memset(&out, 0xa5, sizeof(out)); memcpy(before, &out, sizeof(out));
    memset(id, 0x5a, sizeof(id)); memcpy(prior, id, sizeof(id));
    CHECK(cog_orchestrator_handoff_decode(bytes, length, &out, id, sizeof(id), &required) == expected);
    CHECK(memcmp(before, &out, sizeof(out)) == 0);
    CHECK(memcmp(prior, id, sizeof(id)) == 0);
    CHECK(required == 777);
}

static void worker_encode_failure(cog_worker_control_t m)
{
    unsigned char out[37], prior[37];
    size_t required = 777;
    memset(out, 0x5a, sizeof(out)); memcpy(prior, out, sizeof(out));
    CHECK(cog_orchestrator_worker_encode(&m, out, sizeof(out), &required) == COG_ERROR_INVALID_ARG);
    CHECK(memcmp(out, prior, sizeof(out)) == 0 && required == 777);
    CHECK(cog_orchestrator_worker_encode(&m, NULL, 0, &required) == COG_ERROR_INVALID_ARG);
    CHECK(required == 777);
}

static void handoff_encode_failure(cog_handoff_message_t m)
{
    unsigned char out[189], prior[189];
    size_t required = 777;
    memset(out, 0x5a, sizeof(out)); memcpy(prior, out, sizeof(out));
    CHECK(cog_orchestrator_handoff_encode(&m, out, sizeof(out), &required) == COG_ERROR_INVALID_ARG);
    CHECK(memcmp(out, prior, sizeof(out)) == 0 && required == 777);
    CHECK(cog_orchestrator_handoff_encode(&m, NULL, 0, &required) == COG_ERROR_INVALID_ARG);
    CHECK(required == 777);
}

static void test_worker(void)
{
    static const unsigned char golden[37] = {
        'C','G','W','1',4,1,2,3,4,5,6,7,8,17,18,19,20,21,22,23,24,
        0,0,0,0,33,34,35,36,37,38,39,40,255,255,255,238
    };
    unsigned char bytes[38], again[37];
    cog_worker_control_t m = worker(COG_WORKER_SHUTDOWN_ACK), out;
    size_t required = 777;
    CHECK(cog_orchestrator_worker_encode(&m, bytes, sizeof(bytes), &required) == COG_OK);
    CHECK(required == 37 && memcmp(bytes, golden, 37) == 0);
    for (size_t n = 0; n < 37; ++n) {
        worker_decode_failure(bytes, n, COG_ERROR_PROTOCOL);
        memset(again, 0x5a, sizeof(again)); required = 777;
        CHECK(cog_orchestrator_worker_encode(&m, again, n, &required) == COG_ERROR_OVERFLOW);
        CHECK(required == 37);
        for (size_t j = 0; j < sizeof(again); ++j) CHECK(again[j] == 0x5a);
    }
    worker_decode_failure(bytes, 38, COG_ERROR_PROTOCOL);
    bytes[3] = '2'; worker_decode_failure(bytes, 4, COG_ERROR_UNSUPPORTED);
    worker_decode_failure(bytes, 37, COG_ERROR_UNSUPPORTED); bytes[3] = '1';
    bytes[0] = 'X'; worker_decode_failure(bytes, 37, COG_ERROR_PROTOCOL); bytes[0] = 'C';
    for (int kind = 1; kind <= 4; ++kind) {
        m = worker((cog_worker_control_kind_t)kind);
        CHECK(cog_orchestrator_worker_encode(&m, bytes, 37, &required) == COG_OK);
        CHECK(cog_orchestrator_worker_decode(bytes, 37, &out) == COG_OK);
        CHECK(out.kind == m.kind && out.worker.incarnation == m.worker.incarnation &&
              out.worker.number == m.worker.number && out.active_subagents == m.active_subagents &&
              out.deadline_ms == m.deadline_ms && out.outcome == m.outcome);
        CHECK(cog_orchestrator_worker_encode(&out, again, 37, &required) == COG_OK);
        CHECK(memcmp(bytes, again, 37) == 0);
    }
    for (int result = -18; result <= 0; ++result) {
        m = worker(COG_WORKER_SHUTDOWN_ACK); m.outcome = (cog_result_t)result;
        CHECK(cog_orchestrator_worker_encode(&m, bytes, 37, &required) == COG_OK);
        CHECK(cog_orchestrator_worker_decode(bytes, 37, &out) == COG_OK);
        CHECK(out.outcome == m.outcome);
    }
    m = worker(COG_WORKER_HELLO); m.worker.incarnation = 0; worker_encode_failure(m);
    m = worker(COG_WORKER_HELLO); m.worker.number = 0; worker_encode_failure(m);
    m = worker(COG_WORKER_HELLO); m.active_subagents = 1; worker_encode_failure(m);
    m = worker(COG_WORKER_HELLO); m.deadline_ms = 1; worker_encode_failure(m);
    m = worker(COG_WORKER_HEARTBEAT); m.deadline_ms = 1; worker_encode_failure(m);
    m = worker(COG_WORKER_SHUTDOWN); m.deadline_ms = 0; worker_encode_failure(m);
    m = worker(COG_WORKER_SHUTDOWN); m.active_subagents = 1; worker_encode_failure(m);
    m = worker(COG_WORKER_SHUTDOWN_ACK); m.outcome = (cog_result_t)-19; worker_encode_failure(m);
    m.outcome = (cog_result_t)1; worker_encode_failure(m);
    m = worker((cog_worker_control_kind_t)0); worker_encode_failure(m);
    m = worker((cog_worker_control_kind_t)5); worker_encode_failure(m);
    for (int kind = 1; kind <= 3; ++kind) {
        m = worker((cog_worker_control_kind_t)kind); m.outcome = COG_ERROR_IO;
        worker_encode_failure(m);
    }
    /* Direct malformed wire checks do not rely on the encoder validator. */
    memcpy(bytes, golden, 37); memset(bytes + 5, 0, 8);
    worker_decode_failure(bytes, 37, COG_ERROR_PROTOCOL);
    memcpy(bytes, golden, 37); memset(bytes + 13, 0, 8);
    worker_decode_failure(bytes, 37, COG_ERROR_PROTOCOL);
    memcpy(bytes, golden, 37); bytes[24] = 1;
    worker_decode_failure(bytes, 37, COG_ERROR_PROTOCOL);
    memcpy(bytes, golden, 37); memset(bytes + 25, 0, 8);
    worker_decode_failure(bytes, 37, COG_ERROR_PROTOCOL);
    memcpy(bytes, golden, 37); bytes[36] = 237;
    worker_decode_failure(bytes, 37, COG_ERROR_PROTOCOL);
    memcpy(bytes, golden, 37); memset(bytes + 33, 0, 4); bytes[36] = 1;
    worker_decode_failure(bytes, 37, COG_ERROR_PROTOCOL);
    for (unsigned kind = 1; kind <= 3; ++kind) {
        memcpy(bytes, golden, 37); bytes[4] = (unsigned char)kind;
        worker_decode_failure(bytes, 37, COG_ERROR_PROTOCOL);
    }
    /* Every byte/value perturbation must reject transactionally or be canonical. */
    for (size_t i = 0; i < 37; ++i) for (unsigned value = 0; value <= 255; ++value) {
        cog_result_t rc;
        memcpy(bytes, golden, 37); bytes[i] = (unsigned char)value;
        rc = cog_orchestrator_worker_decode(bytes, 37, &out);
        if (rc == COG_OK) {
            CHECK(cog_orchestrator_worker_encode(&out, again, 37, &required) == COG_OK);
            CHECK(memcmp(bytes, again, 37) == 0);
        } else worker_decode_failure(bytes, 37, rc);
    }
}

static void test_handoff(void)
{
    static const unsigned char golden[63] = {
        [0]='C',[1]='G',[2]='H',[3]='1',[4]=6,[5]=1,[6]='s',
        [22]=2,[30]=3,[38]=4,[46]=4,[54]=5,[62]=6
    };
    unsigned char bytes[190], again[189];
    char id[128], long_id[129];
    cog_handoff_message_t m = handoff(COG_HANDOFF_CHECKPOINTED), out;
    size_t required = 777;
    CHECK(cog_orchestrator_handoff_encode(&m, bytes, sizeof(bytes), &required) == COG_OK);
    CHECK(required == 63 && memcmp(bytes, golden, 63) == 0);
    for (size_t n = 0; n < 63; ++n) {
        handoff_decode_failure(bytes, n, COG_ERROR_PROTOCOL);
        memset(again, 0x5a, sizeof(again)); required = 777;
        CHECK(cog_orchestrator_handoff_encode(&m, again, n, &required) == COG_ERROR_OVERFLOW);
        CHECK(required == 63);
        for (size_t j = 0; j < sizeof(again); ++j) CHECK(again[j] == 0x5a);
    }
    handoff_decode_failure(bytes, 64, COG_ERROR_PROTOCOL);
    bytes[3] = '2'; handoff_decode_failure(bytes, 4, COG_ERROR_UNSUPPORTED);
    handoff_decode_failure(bytes, 63, COG_ERROR_UNSUPPORTED); bytes[3] = '1';
    bytes[4] = 0; handoff_decode_failure(bytes, 63, COG_ERROR_UNSUPPORTED);
    bytes[4] = 9; handoff_decode_failure(bytes, 63, COG_ERROR_UNSUPPORTED);
    bytes[4] = 6; bytes[5] = 0; handoff_decode_failure(bytes, 62, COG_ERROR_PROTOCOL);
    memcpy(bytes, golden, 63);
    for (size_t n = 0; n < 2; ++n) {
        unsigned char before[sizeof(out)];
        memset(&out, 0xa5, sizeof(out)); memcpy(before, &out, sizeof(out));
        memset(id, 0x5a, sizeof(id)); required = 777;
        CHECK(cog_orchestrator_handoff_decode(bytes, 63, &out, id, n, &required) == COG_ERROR_OVERFLOW);
        CHECK(required == 2 && memcmp(&out, before, sizeof(out)) == 0);
        for (size_t j = 0; j < sizeof(id); ++j) CHECK(id[j] == 0x5a);
    }
    for (int kind = 1; kind <= 8; ++kind) {
        m = handoff((cog_handoff_kind_t)kind);
        CHECK(cog_orchestrator_handoff_encode(&m, bytes, sizeof(bytes), &required) == COG_OK);
        CHECK(cog_orchestrator_handoff_decode(bytes, required, &out, id, sizeof(id), &required) == COG_OK);
        CHECK(required == 2 && out.session_id == id && strcmp(id, "s") == 0);
        CHECK(out.kind == m.kind && out.source_peer == m.source_peer && out.target_peer == m.target_peer &&
              out.expected.epoch == m.expected.epoch && out.expected.instance_id == m.expected.instance_id &&
              out.next.epoch == m.next.epoch && out.next.instance_id == m.next.instance_id &&
              out.checkpoint.generation == m.checkpoint.generation);
        CHECK(cog_orchestrator_handoff_encode(&out, again, sizeof(again), &required) == COG_OK);
        CHECK(required == 63 && memcmp(bytes, again, 63) == 0);
        if (kind == COG_HANDOFF_ABORT || kind == COG_HANDOFF_FAILED) {
            m.checkpoint.generation = 0;
            CHECK(cog_orchestrator_handoff_encode(&m, bytes, sizeof(bytes), &required) == COG_OK);
        } else {
            m.checkpoint.generation = m.checkpoint.generation == 0 ? 1 : 0;
            handoff_encode_failure(m);
        }
    }
    memset(long_id, 'x', 128); long_id[128] = '\0';
    m = handoff(COG_HANDOFF_READY); m.session_id = long_id; handoff_encode_failure(m);
    long_id[127] = '\0';
    CHECK(cog_orchestrator_handoff_encode(&m, bytes, sizeof(bytes), &required) == COG_OK && required == 189);
    CHECK(cog_orchestrator_handoff_decode(bytes, 189, &out, id, 128, &required) == COG_OK && required == 128);
    CHECK(strcmp(id, long_id) == 0);
    m.session_id = ""; handoff_encode_failure(m);
    m = handoff(COG_HANDOFF_READY); m.source_peer = m.target_peer; handoff_encode_failure(m);
    m.source_peer = 0; m.target_peer = 0; handoff_encode_failure(m);
    m = handoff(COG_HANDOFF_READY); m.expected.epoch = 0; handoff_encode_failure(m);
    m = handoff(COG_HANDOFF_READY); m.expected.instance_id = 0; handoff_encode_failure(m);
    m = handoff(COG_HANDOFF_READY); m.next.instance_id = 0; handoff_encode_failure(m);
    m = handoff(COG_HANDOFF_READY); m.next.epoch = 3; handoff_encode_failure(m);
    m = handoff(COG_HANDOFF_READY); m.expected.epoch = UINT64_MAX; m.next.epoch = 0; handoff_encode_failure(m);
    m = handoff((cog_handoff_kind_t)0); handoff_encode_failure(m);
    m = handoff((cog_handoff_kind_t)9); handoff_encode_failure(m);
    m = handoff(COG_HANDOFF_READY); m.source_peer = UINT64_MAX; m.target_peer = 0;
    m.expected.epoch = UINT64_MAX - 1; m.next.epoch = UINT64_MAX;
    m.expected.instance_id = UINT64_MAX; m.next.instance_id = UINT64_MAX;
    m.checkpoint.generation = UINT64_MAX;
    CHECK(cog_orchestrator_handoff_encode(&m, bytes, sizeof(bytes), &required) == COG_OK);
    CHECK(cog_orchestrator_handoff_decode(bytes, required, &out, id, sizeof(id), &required) == COG_OK);
    CHECK(out.next.epoch == UINT64_MAX && out.source_peer == UINT64_MAX && out.target_peer == 0);
    for (size_t field = 0; field < 4; ++field) {
        static const size_t offsets[] = {23, 31, 39, 47};
        memcpy(bytes, golden, 63); memset(bytes + offsets[field], 0, 8);
        handoff_decode_failure(bytes, 63, COG_ERROR_PROTOCOL);
    }
    memcpy(bytes, golden, 63); bytes[22] = 0; /* both peers local */
    handoff_decode_failure(bytes, 63, COG_ERROR_PROTOCOL);
    memcpy(bytes, golden, 63); bytes[14] = 2; /* equal nonzero peers */
    handoff_decode_failure(bytes, 63, COG_ERROR_PROTOCOL);
    memcpy(bytes, golden, 63); memset(bytes + 23, 255, 8);
    memset(bytes + 39, 0, 8); /* epoch exhaustion cannot wrap */
    handoff_decode_failure(bytes, 63, COG_ERROR_PROTOCOL);
    memcpy(bytes, golden, 63); bytes[46] = 5; /* skip epoch */
    handoff_decode_failure(bytes, 63, COG_ERROR_PROTOCOL);
    memcpy(bytes, golden, 63); bytes[4] = COG_HANDOFF_QUIESCE;
    handoff_decode_failure(bytes, 63, COG_ERROR_PROTOCOL);
    for (unsigned kind = 1; kind <= 8; ++kind) {
        if (kind == COG_HANDOFF_QUIESCE || kind == COG_HANDOFF_ABORT || kind == COG_HANDOFF_FAILED) continue;
        memcpy(bytes, golden, 63); bytes[4] = (unsigned char)kind; bytes[62] = 0;
        handoff_decode_failure(bytes, 63, COG_ERROR_PROTOCOL);
    }
    for (size_t i = 0; i < 63; ++i) for (unsigned value = 0; value <= 255; ++value) {
        cog_result_t rc;
        memcpy(bytes, golden, 63); bytes[i] = (unsigned char)value;
        rc = cog_orchestrator_handoff_decode(bytes, 63, &out, id, sizeof(id), &required);
        if (rc == COG_OK) {
            CHECK(cog_orchestrator_handoff_encode(&out, again, sizeof(again), &required) == COG_OK);
            CHECK(required == 63 && memcmp(bytes, again, 63) == 0);
        } else handoff_decode_failure(bytes, 63, rc);
    }
}

static void test_unicode(void)
{
    static const char *const valid[] = {
        "\x7f", "\xc2\x80", "\xdf\xbf", "\xe0\xa0\x80", "\xed\x9f\xbf",
        "\xee\x80\x80", "\xef\xbf\xbf", "\xf0\x90\x80\x80", "\xf4\x8f\xbf\xbf"
    };
    static const char *const invalid[] = {
        "\x80", "\xc0\x80", "\xc1\xbf", "\xc2", "\xc2x", "\xe0\x9f\xbf",
        "\xed\xa0\x80", "\xed\xbf\xbf", "\xe1\x80", "\xf0\x8f\xbf\xbf",
        "\xf4\x90\x80\x80", "\xf5\x80\x80\x80", "\xff", "\xf1\x80\x80"
    };
    unsigned char bytes[189]; char id[128]; size_t required;
    cog_handoff_message_t m = handoff(COG_HANDOFF_PREPARED), out;
    for (size_t i = 0; i < sizeof(valid) / sizeof(valid[0]); ++i) {
        m.session_id = valid[i];
        CHECK(cog_orchestrator_handoff_encode(&m, bytes, sizeof(bytes), &required) == COG_OK);
        CHECK(cog_orchestrator_handoff_decode(bytes, required, &out, id, sizeof(id), &required) == COG_OK);
        CHECK(strcmp(id, valid[i]) == 0);
    }
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        char placeholder[5] = "xxxx";
        size_t length = strlen(invalid[i]);
        m.session_id = invalid[i]; handoff_encode_failure(m);
        placeholder[length] = '\0'; m.session_id = placeholder;
        CHECK(cog_orchestrator_handoff_encode(&m, bytes, sizeof(bytes), &required) == COG_OK);
        memcpy(bytes + 6, invalid[i], length);
        handoff_decode_failure(bytes, required, COG_ERROR_PROTOCOL);
        /* Shape/UTF-8 must fail before insufficient caller storage. */
        required = 777;
        CHECK(cog_orchestrator_handoff_decode(bytes, 62 + length, &out, NULL, 0, &required) == COG_ERROR_PROTOCOL);
        CHECK(required == 777);
    }
    m.session_id = "a";
    CHECK(cog_orchestrator_handoff_encode(&m, bytes, sizeof(bytes), &required) == COG_OK);
    bytes[6] = 0; handoff_decode_failure(bytes, required, COG_ERROR_PROTOCOL);
}

static void test_pointers(void)
{
    cog_worker_control_t w = worker(COG_WORKER_HELLO), wout;
    cog_handoff_message_t h = handoff(COG_HANDOFF_QUIESCE), hout;
    unsigned char bytes[189]; char id[128]; size_t required = 777;
    CHECK(cog_orchestrator_worker_encode(NULL, bytes, sizeof(bytes), &required) == COG_ERROR_NULL_PTR);
    CHECK(cog_orchestrator_worker_encode(&w, NULL, 1, &required) == COG_ERROR_NULL_PTR);
    CHECK(cog_orchestrator_worker_encode(&w, bytes, sizeof(bytes), NULL) == COG_ERROR_NULL_PTR);
    CHECK(required == 777);
    CHECK(cog_orchestrator_worker_encode(&w, NULL, 0, &required) == COG_ERROR_OVERFLOW && required == 37);
    CHECK(cog_orchestrator_worker_decode(NULL, 0, &wout) == COG_ERROR_NULL_PTR);
    CHECK(cog_orchestrator_worker_decode(bytes, 0, NULL) == COG_ERROR_NULL_PTR);
    required = 777;
    CHECK(cog_orchestrator_handoff_encode(NULL, bytes, sizeof(bytes), &required) == COG_ERROR_NULL_PTR);
    CHECK(cog_orchestrator_handoff_encode(&h, NULL, 1, &required) == COG_ERROR_NULL_PTR);
    CHECK(cog_orchestrator_handoff_encode(&h, bytes, sizeof(bytes), NULL) == COG_ERROR_NULL_PTR);
    h.session_id = NULL;
    CHECK(cog_orchestrator_handoff_encode(&h, bytes, sizeof(bytes), &required) == COG_ERROR_NULL_PTR);
    CHECK(required == 777); h.session_id = "s";
    CHECK(cog_orchestrator_handoff_encode(&h, NULL, 0, &required) == COG_ERROR_OVERFLOW && required == 63);
    CHECK(cog_orchestrator_handoff_encode(&h, bytes, sizeof(bytes), &required) == COG_OK);
    CHECK(cog_orchestrator_handoff_decode(NULL, 0, &hout, id, sizeof(id), &required) == COG_ERROR_NULL_PTR);
    CHECK(cog_orchestrator_handoff_decode(bytes, 63, NULL, id, sizeof(id), &required) == COG_ERROR_NULL_PTR);
    CHECK(cog_orchestrator_handoff_decode(bytes, 63, &hout, NULL, 1, &required) == COG_ERROR_NULL_PTR);
    CHECK(cog_orchestrator_handoff_decode(bytes, 63, &hout, id, sizeof(id), NULL) == COG_ERROR_NULL_PTR);
    CHECK(required == 63);
    CHECK(cog_orchestrator_handoff_decode(bytes, 63, &hout, NULL, 0, &required) == COG_ERROR_OVERFLOW && required == 2);
}

int main(void)
{
    test_worker(); test_handoff(); test_unicode(); test_pointers();
    (void)puts("orchestrator wire: golden, scalar, truncation, capacity, UTF-8, mutation, no-allocation PASS");
    return 0;
}

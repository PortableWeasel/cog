/** @file orchestrator_wire.c
 * @brief Pure transactional CGW1 and CGH1 codecs; no runtime authority.
 */
#include "orchestrator_wire_internal.h"

#include <string.h>

static void put_u64(unsigned char *p, uint64_t value)
{
    for (size_t i = 8; i != 0; --i) {
        p[i - 1] = (unsigned char)(value & UINT64_C(255));
        value >>= 8;
    }
}

static uint64_t get_u64(const unsigned char *p)
{
    uint64_t value = 0;
    for (size_t i = 0; i < 8; ++i) value = (value << 8) | p[i];
    return value;
}

static void put_u32(unsigned char *p, uint32_t value)
{
    for (size_t i = 4; i != 0; --i) {
        p[i - 1] = (unsigned char)(value & UINT32_C(255));
        value >>= 8;
    }
}

static uint32_t get_u32(const unsigned char *p)
{
    uint32_t value = 0;
    for (size_t i = 0; i < 4; ++i) value = (value << 8) | p[i];
    return value;
}

/* Complete Unicode scalar values only, excluding NUL in session identifiers. */
static bool valid_id(const unsigned char *p, size_t length)
{
    size_t i = 0;
    if (length == 0 || length >= ORCH_SESSION_BYTES) return false;
    while (i < length) {
        uint32_t value = p[i++];
        uint32_t minimum;
        size_t tail;
        if (value < 0x80U) {
            if (value == 0) return false;
            continue;
        }
        if (value >= 0xc2U && value <= 0xdfU) {
            tail = 1; minimum = 0x80U; value &= 0x1fU;
        } else if (value >= 0xe0U && value <= 0xefU) {
            tail = 2; minimum = 0x800U; value &= 0x0fU;
        } else if (value >= 0xf0U && value <= 0xf4U) {
            tail = 3; minimum = 0x10000U; value &= 0x07U;
        } else {
            return false;
        }
        if (tail > length - i) return false;
        for (size_t j = 0; j < tail; ++j) {
            const unsigned char byte = p[i++];
            if ((byte & 0xc0U) != 0x80U) return false;
            value = (value << 6) | (uint32_t)(byte & 0x3fU);
        }
        if (value < minimum || value > 0x10ffffU ||
            (value >= 0xd800U && value <= 0xdfffU)) return false;
    }
    return true;
}

static cog_result_t prefix(const unsigned char *p, size_t length,
                           const char *magic)
{
    if (length < 4 || memcmp(p, magic, 3) != 0) return COG_ERROR_PROTOCOL;
    if (p[3] != (unsigned char)'1') return COG_ERROR_UNSUPPORTED;
    return COG_OK;
}

static bool valid_worker(const cog_worker_control_t *m)
{
    if (m->worker.incarnation == 0 || m->worker.number == 0) return false;
    switch (m->kind) {
    case COG_WORKER_HELLO:
        return m->active_subagents == 0 && m->deadline_ms == 0 && m->outcome == COG_OK;
    case COG_WORKER_HEARTBEAT:
        return m->deadline_ms == 0 && m->outcome == COG_OK;
    case COG_WORKER_SHUTDOWN:
        return m->active_subagents == 0 && m->deadline_ms != 0 && m->outcome == COG_OK;
    case COG_WORKER_SHUTDOWN_ACK:
        return m->active_subagents == 0 && m->deadline_ms != 0 &&
               m->outcome <= COG_OK && m->outcome >= COG_ERROR_INDETERMINATE;
    default:
        return false;
    }
}

/** @copydoc cog_orchestrator_worker_encode */
cog_result_t cog_orchestrator_worker_encode(const cog_worker_control_t *message,
    void *output, size_t capacity, size_t *required)
{
    unsigned char *p = output;
    if (message == NULL || required == NULL || (output == NULL && capacity != 0))
        return COG_ERROR_NULL_PTR;
    if (!valid_worker(message)) return COG_ERROR_INVALID_ARG;
    if (capacity < ORCH_CGW1_BYTES) {
        *required = ORCH_CGW1_BYTES;
        return COG_ERROR_OVERFLOW;
    }
    memcpy(p, "CGW1", 4);
    p[4] = (unsigned char)message->kind;
    put_u64(p + 5, message->worker.incarnation);
    put_u64(p + 13, message->worker.number);
    put_u32(p + 21, message->active_subagents);
    put_u64(p + 25, message->deadline_ms);
    put_u32(p + 33, (uint32_t)message->outcome);
    *required = ORCH_CGW1_BYTES;
    return COG_OK;
}

/** @copydoc cog_orchestrator_worker_decode */
cog_result_t cog_orchestrator_worker_decode(const void *input, size_t input_len,
    cog_worker_control_t *message)
{
    const unsigned char *p = input;
    cog_worker_control_t decoded = {0};
    cog_result_t rc;
    uint32_t outcome;
    if (input == NULL || message == NULL) return COG_ERROR_NULL_PTR;
    rc = prefix(p, input_len, "CGW");
    if (rc != COG_OK) return rc;
    if (input_len != ORCH_CGW1_BYTES) return COG_ERROR_PROTOCOL;
    if (p[4] < COG_WORKER_HELLO || p[4] > COG_WORKER_SHUTDOWN_ACK)
        return COG_ERROR_PROTOCOL;
    decoded.kind = (cog_worker_control_kind_t)p[4];
    decoded.worker.incarnation = get_u64(p + 5);
    decoded.worker.number = get_u64(p + 13);
    decoded.active_subagents = get_u32(p + 21);
    decoded.deadline_ms = get_u64(p + 25);
    outcome = get_u32(p + 33);
    if (outcome == 0) decoded.outcome = COG_OK;
    else if (outcome >= UINT32_MAX - UINT32_C(17))
        decoded.outcome = (cog_result_t)(-(int)(UINT32_MAX - outcome + UINT32_C(1)));
    else return COG_ERROR_PROTOCOL;
    if (!valid_worker(&decoded)) return COG_ERROR_PROTOCOL;
    *message = decoded;
    return COG_OK;
}

static bool valid_handoff(const cog_handoff_message_t *m)
{
    if (m->source_peer == m->target_peer || m->expected.epoch == 0 ||
        m->expected.instance_id == 0 || m->next.instance_id == 0 ||
        m->expected.epoch == UINT64_MAX || m->next.epoch != m->expected.epoch + 1)
        return false;
    switch (m->kind) {
    case COG_HANDOFF_QUIESCE:
        return m->checkpoint.generation == 0;
    case COG_HANDOFF_PREPARED:
    case COG_HANDOFF_COMMIT:
    case COG_HANDOFF_CHECKPOINTED:
    case COG_HANDOFF_READY:
    case COG_HANDOFF_ACTIVATE:
        return m->checkpoint.generation != 0;
    case COG_HANDOFF_ABORT:
    case COG_HANDOFF_FAILED:
        return true;
    default:
        return false;
    }
}

/** @copydoc cog_orchestrator_handoff_encode */
cog_result_t cog_orchestrator_handoff_encode(const cog_handoff_message_t *message,
    void *output, size_t capacity, size_t *required)
{
    unsigned char *p = output;
    size_t length = 0;
    size_t bytes;
    if (message == NULL || required == NULL || (output == NULL && capacity != 0))
        return COG_ERROR_NULL_PTR;
    if (message->session_id == NULL) return COG_ERROR_NULL_PTR;
    while (length < ORCH_SESSION_BYTES && message->session_id[length] != '\0') ++length;
    if (!valid_handoff(message) ||
        !valid_id((const unsigned char *)message->session_id, length))
        return COG_ERROR_INVALID_ARG;
    bytes = ORCH_CGH1_FIXED_BYTES + length;
    if (capacity < bytes) {
        *required = bytes;
        return COG_ERROR_OVERFLOW;
    }
    memcpy(p, "CGH1", 4);
    p[4] = (unsigned char)message->kind;
    p[5] = (unsigned char)length;
    memcpy(p + 6, message->session_id, length);
    p += 6 + length;
    put_u64(p, message->source_peer);
    put_u64(p + 8, message->target_peer);
    put_u64(p + 16, message->expected.epoch);
    put_u64(p + 24, message->expected.instance_id);
    put_u64(p + 32, message->next.epoch);
    put_u64(p + 40, message->next.instance_id);
    put_u64(p + 48, message->checkpoint.generation);
    *required = bytes;
    return COG_OK;
}

/** @copydoc cog_orchestrator_handoff_decode */
cog_result_t cog_orchestrator_handoff_decode(const void *input, size_t input_len,
    cog_handoff_message_t *message, char *session_id, size_t capacity,
    size_t *required)
{
    const unsigned char *p = input;
    cog_handoff_message_t decoded = {0};
    cog_result_t rc;
    size_t length;
    if (input == NULL || message == NULL || required == NULL ||
        (session_id == NULL && capacity != 0)) return COG_ERROR_NULL_PTR;
    rc = prefix(p, input_len, "CGH");
    if (rc != COG_OK) return rc;
    if (input_len < ORCH_CGH1_FIXED_BYTES) return COG_ERROR_PROTOCOL;
    if (p[4] < COG_HANDOFF_QUIESCE || p[4] > COG_HANDOFF_ACTIVATE)
        return COG_ERROR_UNSUPPORTED;
    length = p[5];
    if (length >= ORCH_SESSION_BYTES || input_len != ORCH_CGH1_FIXED_BYTES + length ||
        !valid_id(p + 6, length)) return COG_ERROR_PROTOCOL;
    decoded.kind = (cog_handoff_kind_t)p[4];
    p += 6 + length;
    decoded.source_peer = get_u64(p);
    decoded.target_peer = get_u64(p + 8);
    decoded.expected.epoch = get_u64(p + 16);
    decoded.expected.instance_id = get_u64(p + 24);
    decoded.next.epoch = get_u64(p + 32);
    decoded.next.instance_id = get_u64(p + 40);
    decoded.checkpoint.generation = get_u64(p + 48);
    if (!valid_handoff(&decoded)) return COG_ERROR_PROTOCOL;
    if (capacity < length + 1) {
        *required = length + 1;
        return COG_ERROR_OVERFLOW;
    }
    memcpy(session_id, (const unsigned char *)input + 6, length);
    session_id[length] = '\0';
    decoded.session_id = session_id;
    *message = decoded;
    *required = length + 1;
    return COG_OK;
}

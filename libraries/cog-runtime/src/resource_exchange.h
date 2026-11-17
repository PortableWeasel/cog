/** @file resource_exchange.h @brief Private bounded CGIR receipt storage.
 * Dedicated secret arenas, never UI/replay lanes. The transport owner verifies
 * native peer/attempt and durable proofs before using these pure byte receipts.
 * Retain all five exact frames through CONSUMED copied-send/receive. Correlation
 * and exact duplicate checks precede old-context/current-association checks.
 * No operation below reads sources, saves a checkpoint, admits effects or sends.
 */
#ifndef COG_RUNTIME_RESOURCE_EXCHANGE_H
#define COG_RUNTIME_RESOURCE_EXCHANGE_H
#include "control.h"
#include "instruction_transfer.h"
#include <stdbool.h>
#define CLI_RESOURCE_FRAME_COUNT 5U
#define CLI_RESOURCE_TIMEOUT_MS 30000U

typedef struct cli_resource_frame {
    unsigned char *bytes;
    size_t capacity, length;
    uint64_t wire, reply_to;
} cli_resource_frame_t;
typedef struct cli_resource_exchange {
    /* Index order is REQUEST, RESULT, COMMIT, READY, CONSUMED. */
    cli_resource_frame_t frames[CLI_RESOURCE_FRAME_COUNT];
    unsigned char *request_storage, *result_storage, *scratch;
    size_t request_capacity, result_capacity, scratch_capacity;
    cog_cli_instruction_resource_t request, result, commit, ready;
    uint64_t deadline_ms, copied_wire, next_send_ms, retired_request;
    uint64_t retired_wire[CLI_RESOURCE_FRAME_COUNT];
    size_t send_index;
    bool active, frozen, send_pending;
} cli_resource_exchange_t;
/** Constructors reserve complete framing/decoding arrays. Checked bounds include
 * CGL and CGIR overhead. instruction_bytes is trusted maximum complete CGIM.
 * Constructor permits no truncation; frame_bytes may be smaller and each actual
 * encode still fails LIMIT/OVERFLOW before any publication. */
cog_result_t cli_resource_exchange_new(cli_resource_exchange_t **out,
    size_t frame_bytes, size_t instruction_bytes);
void cli_resource_exchange_free(cli_resource_exchange_t **owner);
/** Wipe all buffers and transient proofs after required CONSUMED/loss cleanup.
 * Keeps constructor arenas and retired REQUEST watermark. Frozen transactions
 * may only reset after the owner has completed explicit recovery/teardown. */
void cli_resource_exchange_reset(cli_resource_exchange_t *exchange);
/** Physical slot reuse after full attempt teardown: reset receipt watermarks too. */
void cli_resource_exchange_rebind(cli_resource_exchange_t *exchange);
/** Build or retain exact frame. Caller reserves wire IDs before REQUEST and
 * owns setting finite min(parent/turn/30s) deadline once, never on duplicate.
 * receive returns AGAIN for exact retained duplicate, PROTOCOL for same-phase
 * byte conflict or wrong chain/identity. It leaves prior receipts unchanged.
 * New records are accepted only in protocol order; decoder verifies all inner
 * schemas. build uses the same checks and never changes an existing record. */
cog_result_t cli_resource_exchange_build(cli_resource_exchange_t *exchange,
    const cog_cli_control_message_t *envelope,
    const cog_cli_instruction_resource_t *body);
cog_result_t cli_resource_exchange_receive(cli_resource_exchange_t *exchange,
    const cog_cli_control_message_t *envelope, const void *frame, size_t length);
size_t cli_resource_frame_index(cog_cli_control_kind_t kind);
#endif

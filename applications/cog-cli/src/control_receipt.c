/** @file control_receipt.c @brief Pure fixed-size update receipt payload. */
#include "control_receipt.h"
#include "codec_internal.h"

static bool receipt_valid(const cog_cli_control_receipt_t *r)
{
    if (r->parent_runtime_incarnation == 0U || r->agent_id == 0U ||
        r->execution_epoch == 0U || r->owner_peer == 0U || r->wire_id == 0U ||
        r->disposition < COG_CLI_CONTROL_RECEIPT_ACCEPTED ||
        r->disposition > COG_CLI_CONTROL_RECEIPT_TERMINAL_FIXED) return false;
    bool terminal = r->accepted_terminal_status != COG_AGENT_STATUS_PENDING;
    if (!terminal) {
        if (r->accepted_terminal_outcome != COG_OK) return false;
    } else {
        if (r->parent_accepted_sequence == 0U ||
            r->accepted_terminal_status < COG_AGENT_STATUS_COMPLETED ||
            r->accepted_terminal_status > COG_AGENT_STATUS_TIMEOUT) return false;
        if (r->accepted_terminal_status == COG_AGENT_STATUS_COMPLETED) {
            if (r->accepted_terminal_outcome != COG_OK) return false;
        } else if (!cli_wire_result(r->accepted_terminal_outcome) ||
            r->accepted_terminal_outcome == COG_OK) return false;
    }
    if (r->disposition == COG_CLI_CONTROL_RECEIPT_ACCEPTED && r->parent_accepted_sequence == 0U) return false;
    if (r->disposition == COG_CLI_CONTROL_RECEIPT_TERMINAL_FIXED && !terminal) return false;
    return r->disposition != COG_CLI_CONTROL_RECEIPT_CANCEL_CONSUMED ||
        (!terminal && r->parent_accepted_sequence < UINT64_MAX && r->wire_id < UINT64_MAX);
}
cog_result_t cog_cli_control_receipt_encode(const cog_cli_control_receipt_t *receipt,
    void *output, size_t capacity, size_t *required)
{
    if (receipt == NULL || required == NULL || (output == NULL && capacity != 0U)) return COG_ERROR_NULL_PTR;
    const cli_wire_region_t regions[] = {{receipt, sizeof(*receipt)}, {required, sizeof(*required)}, {output, capacity}};
    if (!cli_wire_regions(regions, 3U) || !receipt_valid(receipt)) return COG_ERROR_INVALID_ARG;
    *required = COG_CLI_CONTROL_RECEIPT_PAYLOAD_BYTES;
    if (capacity < COG_CLI_CONTROL_RECEIPT_PAYLOAD_BYTES) return COG_ERROR_OVERFLOW;
    cli_wire_writer_t w = {output, 0U, true};
    cli_wire_put(&w, COG_CLI_CONTROL_RECEIPT_VERSION, 1U);
    cli_wire_put(&w, (uint64_t)receipt->disposition, 1U);
    cli_wire_put(&w, receipt->parent_runtime_incarnation, 8U);
    cli_wire_put(&w, receipt->agent_id, 8U); cli_wire_put(&w, receipt->execution_epoch, 8U);
    cli_wire_put(&w, receipt->owner_peer, 8U); cli_wire_put(&w, receipt->wire_id, 8U);
    cli_wire_put(&w, receipt->parent_accepted_sequence, 8U);
    cli_wire_put(&w, (uint64_t)receipt->accepted_terminal_status, 1U);
    cli_wire_put(&w, (uint32_t)receipt->accepted_terminal_outcome, 4U);
    return COG_OK;
}
cog_result_t cog_cli_control_receipt_decode(const void *input, size_t input_len,
    cog_cli_control_receipt_t *receipt)
{
    if (input == NULL || receipt == NULL) return COG_ERROR_NULL_PTR;
    if (input_len != COG_CLI_CONTROL_RECEIPT_PAYLOAD_BYTES) return COG_ERROR_PROTOCOL;
    const unsigned char *p = input;
    if (p[0] != COG_CLI_CONTROL_RECEIPT_VERSION) return p[0] == 0U ? COG_ERROR_PROTOCOL : COG_ERROR_UNSUPPORTED;
    if (p[1] < 1U || p[1] > 4U) return p[1] == 0U ? COG_ERROR_PROTOCOL : COG_ERROR_UNSUPPORTED;
    if (!cli_wire_disjoint(input, input_len, receipt, sizeof(*receipt))) return COG_ERROR_PROTOCOL;
    cli_wire_reader_t r = {p, input_len, 2U, true};
    cog_cli_control_receipt_t v = {0};
    v.disposition = (cog_cli_control_receipt_disposition_t)p[1];
    v.parent_runtime_incarnation = cli_wire_get(&r, 8U);
    v.agent_id = cli_wire_get(&r, 8U); v.execution_epoch = cli_wire_get(&r, 8U);
    v.owner_peer = cli_wire_get(&r, 8U); v.wire_id = cli_wire_get(&r, 8U);
    v.parent_accepted_sequence = cli_wire_get(&r, 8U);
    v.accepted_terminal_status = (cog_agent_status_t)cli_wire_get(&r, 1U);
    v.accepted_terminal_outcome = cli_wire_signed(cli_wire_get(&r, 4U));
    if (!receipt_valid(&v)) return COG_ERROR_PROTOCOL;
    *receipt = v;
    return COG_OK;
}

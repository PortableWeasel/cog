/** @file control_receipt.h
 * @brief Private fixed child-update receipt payload; no transport or runtime.
 *
 * This payload belongs inside the application control envelope. The envelope
 * owner supplies authenticated fabric/role/session/connection binding and its
 * payload tag. This header supplies neither worker IDs nor application drive,
 * receipt storage, effects, authentication, or a second top-level protocol.
 * Only the parent issues these receipts for a worker's CGA1 update envelope.
 * Local owner_peer=0 updates and generic SPAWN/CANCEL command receipts are out
 * of scope. All scalar identity checks remain mandatory after decoding.
 *
 * Threading: pure caller-storage codecs, no allocation, state or I/O. Different
 * callers may encode/decode concurrently with independent input/output objects.
 */
#ifndef COG_CLI_CONTROL_RECEIPT_H
#define COG_CLI_CONTROL_RECEIPT_H

#include <cog/agent.h>
#include <cog/ipc.h>
#include <cog/result.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define COG_CLI_CONTROL_RECEIPT_VERSION 1U
#define COG_CLI_CONTROL_RECEIPT_PAYLOAD_BYTES 55U

/** @brief Disposition of one immutable worker update envelope, not a core ACK.
 * Zero is invalid. Successful encode/decode does not carry out the disposition.
 */
typedef enum cog_cli_control_receipt_disposition {
    /** Exact update was semantically accepted; parent independently copied and
     * queued its notification and completed local cog_agent_ack_update. Retire
     * only this matching wire item. parent_accepted_sequence must match its
     * CGA1 sequence. No further effects are authorized by this receipt. */
    COG_CLI_CONTROL_RECEIPT_ACCEPTED = 1,
    /** No final delivery acknowledgement; retain/retry the same immutable wire
     * item. Prefix can already include apply_update while notification copy/
     * local ACK remains blocked. Never infer retirement from prefix alone. */
    COG_CLI_CONTROL_RECEIPT_RETRY = 2,
    /** Parent admitted cancel and rejected fresh RUNNING/COMPLETED n=k+1.
     * Retire this rejected wire item without semantic apply/ACK/advance. After
     * actual drain, a NEW wire ID may carry an admissible terminal at k+1.
     * No accepted terminal is present in this disposition. */
    COG_CLI_CONTROL_RECEIPT_CANCEL_CONSUMED = 3,
    /** Parent's accepted terminal wins; the matching pending wire item is
     * retired without claiming that its payload was accepted. The accepted
     * terminal notification has been independently copied/queued/local-ACKed.
     * Never rewrite that terminal or create a later semantic update. */
    COG_CLI_CONTROL_RECEIPT_TERMINAL_FIXED = 4
} cog_cli_control_receipt_disposition_t;

/** @brief Receipt snapshot for an exact current parent/agent/attempt/wire item.
 *
 * All identity fields except parent_accepted_sequence must be nonzero.
 * owner_peer is the actual authenticated worker peer in the control fabric,
 * never a managed worker ID or a peer from another fabric. The outer envelope
 * binds the session, parent role/incarnation, fabric and native child identity.
 * A matching payload alone grants no authority. Never allocate an identity by
 * decoding one; compare it against already admitted host-owned records.
 *
 * Wire IDs are fresh, increasing and never reused within this parent/attempt
 * identity, independent of CGA1 semantic sequence. CANCEL_CONSUMED permits a
 * new wire ID to fill an unaccepted semantic slot; it cannot modify a retried
 * envelope's bytes or an already accepted update. A cached receipt is immutable
 * on retransmission, including its prefix snapshot; current state may be newer.
 */
typedef struct cog_cli_control_receipt {
    uint64_t parent_runtime_incarnation; /**< Parent process lifetime, not display
       generation, configuration generation, durable owner epoch or session ID. */
    cog_agent_id_t agent_id; /**< Exact previously admitted parent registry ID. */
    uint64_t execution_epoch; /**< Exact immutable remote attempt, never reused. */
    cog_peer_id_t owner_peer; /**< Actual authenticated worker transport peer. */
    uint64_t wire_id; /**< Exact immutable update envelope being answered. */
    uint64_t parent_accepted_sequence; /**< Parent semantic prefix at receipt
       construction. Zero before first accepted CGA1 update. Not a render cursor. */
    cog_cli_control_receipt_disposition_t disposition;
    cog_agent_status_t accepted_terminal_status; /**< PENDING means absent;
       otherwise COMPLETED/FAILED/CANCELLED/TIMEOUT only; never RUNNING. */
    cog_result_t accepted_terminal_outcome; /**< Absent or COMPLETED: OK. Other
       terminal: a known negative cog_result_t, exactly as accepted by the core. */
} cog_cli_control_receipt_t;

/** @brief Encode one validated fixed-size payload, without any envelope/I/O.
 * @param[in] receipt Borrowed scalar snapshot, unchanged.
 * @param[out] output Caller destination; NULL iff capacity is zero.
 * @param[in] capacity Available destination bytes.
 * @param[out] required Exact payload bytes on OK/OVERFLOW, otherwise unchanged.
 * @return OK; OVERFLOW changes only required; NULL_PTR for mandatory NULL or
 * output NULL with positive capacity; INVALID_ARG for invalid scalar/predicate.
 * @pre receipt, required and the output region do not overlap.
 * @post OK writes exactly55 bytes; bytes beyond55 remain unchanged. Every
 * failure leaves output untouched. No allocation, core calls or consumption.
 *
 * Version1 literal layout, no native structs/padding/pointers or magic:
 * offset0 version u8=1; offset1 disposition u8; offset2 parent incarnation u64;
 * offset10 agent ID u64; offset18 attempt epoch u64; offset26 owner peer u64;
 * offset34 wire ID u64; offset42 parent accepted prefix u64;
 * offset50 terminal status u8; offset51 terminal outcome signed i32.
 * Multi-byte fields are big-endian, signed outcome two's complement.
 *
 * Scalar predicates: all identities nonzero; terminal absent iff status=PENDING
 * with outcome=OK. Present status is COMPLETED with OK, or FAILED/CANCELLED/
 * TIMEOUT with a known negative cog_result_t. RUNNING/unknown status/outcome
 * invalid. Present terminal requires nonzero prefix. ACCEPTED requires nonzero
 * prefix; CANCEL_CONSUMED requires absent terminal; TERMINAL_FIXED requires
 * present terminal. RETRY may include an accepted terminal while notification
 * delivery remains blocked, but does not release the pending wire item.
 */
cog_result_t cog_cli_control_receipt_encode(
    const cog_cli_control_receipt_t *receipt, void *output, size_t capacity,
    size_t *required);

/** @brief Decode one complete fixed payload transactionally into scalar storage.
 * @param[in] input Complete borrowed bytes, unchanged.
 * @param[in] input_len Exact payload length, excluding the outer envelope.
 * @param[out] receipt Caller scalar object; unchanged on every failure.
 * @return OK; NULL_PTR for input/receipt NULL; PROTOCOL for wrong length,
 * malformed scalar/predicate, zero disposition or trailing bytes; UNSUPPORTED
 * for an unknown nonzero version or disposition. No allocation or ingestion.
 * @pre input and receipt do not overlap.
 * @post OK reconstructs only the payload above. Caller still verifies current
 * role/fabric/session/incarnation/placement/attempt/exact pending wire identity
 * and compares the prefix/disposition with its retained CGA1 update before any
 * retirement/rebase. Decode is never an ACK, proof of drain or execution lease.
 */
cog_result_t cog_cli_control_receipt_decode(
    const void *input, size_t input_len, cog_cli_control_receipt_t *receipt);

#ifdef __cplusplus
}
#endif
#endif /* COG_CLI_CONTROL_RECEIPT_H */

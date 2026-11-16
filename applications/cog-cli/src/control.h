/**
 * @file control.h
 * @brief Private bounded headless/worker envelope, without transport authority.
 *
 * Single owner, no allocation/I/O/dispatch in codecs. CGL1 is application-private;
 * libraries never include this header. Existing CGA1/CGR1/CGD1/CGH1 and the
 * independently reviewed receipt payload are nested unchanged. Decode is not
 * authentication, command admission, CGA apply/ack, model input or guard proof.
 */
#ifndef COG_CLI_CONTROL_H
#define COG_CLI_CONTROL_H
#include <cog/agent.h>
#include <cog/orchestrator.h>
#include "control_receipt.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum cog_cli_control_kind {
    COG_CLI_CONTROL_UI_HELLO = 1,
    COG_CLI_CONTROL_UI_COMMAND,
    COG_CLI_CONTROL_COMMAND_RESULT,
    COG_CLI_CONTROL_COMMAND_ACK,
    COG_CLI_CONTROL_REPLAY,
    COG_CLI_CONTROL_BOOTSTRAP,
    COG_CLI_CONTROL_BOOTSTRAP_ACK,
    COG_CLI_CONTROL_CHILD_START,
    COG_CLI_CONTROL_CHILD_CANCEL,
    COG_CLI_CONTROL_CHILD_UPDATE,
    COG_CLI_CONTROL_UPDATE_RECEIPT,
    COG_CLI_CONTROL_CHILD_MESSAGE,
    COG_CLI_CONTROL_MESSAGE_RESULT,
    COG_CLI_CONTROL_EXECUTOR_VIEW,
    COG_CLI_CONTROL_VIEW_COMMIT,
    COG_CLI_CONTROL_HANDOFF,
    COG_CLI_CONTROL_DELEGATE_START,
    COG_CLI_CONTROL_DELEGATE_RESULT,
    COG_CLI_CONTROL_DELEGATE_RESULT_ACK,
    COG_CLI_CONTROL_MAILBOX_POLL,
    COG_CLI_CONTROL_MAILBOX_RESULT,
    COG_CLI_CONTROL_EXECUTOR_COMMAND,
    COG_CLI_CONTROL_EXECUTOR_COMMAND_RESULT,
    COG_CLI_CONTROL_POLICY_STATE,
    COG_CLI_CONTROL_RESYNC_VIEW,
    COG_CLI_CONTROL_DELEGATE_RESERVE,
    COG_CLI_CONTROL_DELEGATE_RESERVATION,
    COG_CLI_CONTROL_DELEGATE_RESERVATION_RELEASE,
    COG_CLI_CONTROL_CHILD_START_ACK
} cog_cli_control_kind_t;

/** Independent stop/wait lanes; IDs/cache/watermark are scoped by full identity
 * AND this lane. A sibling's higher wire ID never consumes another receipt. */
typedef enum cog_cli_control_lane {
    COG_CLI_LANE_COMMAND = 1, COG_CLI_LANE_UPDATE, COG_CLI_LANE_VIEW,
    COG_CLI_LANE_POLICY, COG_CLI_LANE_DELEGATE, COG_CLI_LANE_MAILBOX,
    COG_CLI_LANE_PROFILE, COG_CLI_LANE_HANDOFF, COG_CLI_LANE_REPLAY,
    COG_CLI_LANE_CANCEL
} cog_cli_control_lane_t;

/** Every field is copied/validated before ingress can acquire a role. Bind this
 * view to the protected (fabric,actual peer,immutable kernel proof) and retained
 * logical-client or managed-worker record. Numeric equality across fabrics has
 * no meaning. Accepted frames survive socket EOF under their original binding;
 * a later connection never inherits that binding or immutable owner_peer.
 */
typedef struct cog_cli_control_message {
    cog_cli_control_kind_t kind;
    cog_cli_control_lane_t lane;
    uint64_t runtime_incarnation; /**< Fresh nonzero coordinator lifetime, checked
       entropy, never restored from PID/time/store/client claim. UI_HELLO may
       use0 to discover it; all other messages require the known exact value. */
    uint64_t sender_incarnation; /**< Nonzero server-issued UI logical lease,
       coordinator incarnation or worker's managed incarnation, role checked. */
    uint64_t wire_id; /**< Fresh increasing nonzero sender-stream ID, not CGA
       sequence, display cursor, config generation or durable owner epoch. */
    uint64_t reply_to; /**< Exact answered outer envelope, otherwise0. */
    cog_worker_id_t worker; /**< Both0 for UI/local; both nonzero for worker path.
       Never a replacement for the ACTUAL placement below. */
    cog_peer_id_t owner_peer; /**< Exact immutable admitted attempt placement;
       0 for local/UI, authenticated actual worker peer for executor bootstrap.
       Worker CHILD messages require nonzero. */
    cog_agent_id_t agent_id; /**< Both0 with execution_epoch for nonattempt frames. */
    uint64_t execution_epoch; /**< Exact admitted attempt; never reallocated by decode. */
    const char *session_id; /**< UTF-8,1..127 bytes, no NUL/control/separator;
       fixed already-admitted identity, never a store path or existence proof. */
    const void *payload;
    size_t payload_len; /**< Exact constructor-bounded bytes, at most UINT32_MAX. */
} cog_cli_control_message_t;

/** Typed UI/executor command union; unused scalar/pointer fields MUST be0/NULL. */
typedef enum cog_cli_command_kind {
    COG_CLI_COMMAND_SUBMIT = 1, COG_CLI_COMMAND_CONTINUE,
    COG_CLI_COMMAND_POLICY, COG_CLI_COMMAND_CANCEL, COG_CLI_COMMAND_RELOAD,
    COG_CLI_COMMAND_HANDOFF, COG_CLI_COMMAND_SHUTDOWN, COG_CLI_COMMAND_RESYNC,
    COG_CLI_COMMAND_FORK
} cog_cli_command_kind_t;
typedef struct cog_cli_command {
    cog_cli_command_kind_t kind;
    uint64_t observed_generation, observed_sequence;
    const char *text; /**< SUBMIT/CONTINUE/RELOAD only, strict full UTF-8 suffix. */
    size_t text_len;
    const char *target_session_id; /**< POLICY only, root or admitted child. */
    uint64_t node_id, nonce; /**< CONTINUE/FORK node; POLICY node+nonce. */
    uint16_t call_ordinal;
    uint8_t decision; /**< POLICY deny1/allow2. */
    uint8_t reload_field; /**< RELOAD model1/base-url2/credential-ref3. */
    cog_agent_id_t agent_id; /**< CANCEL,0 primary. */
    cog_worker_id_t target; /**< HANDOFF,both0 local. */
} cog_cli_command_t;

/** @brief Encode exact typed command body from HEADLESS_CONTROL's opcode table.
 * @param[in] command Borrowed view. @param[out] output NULL iff capacity0.
 * @param[in] capacity Bytes. @param[out] required Exact bytes.
 * @return OK; NULL_PTR; INVALID_ARG scalar/unused field; LIMIT; OVERFLOW changes
 * required only. All regions nonoverlapping; no other output changes on failure.
 * No allocation/authorization/admission; same prefix/error order as CGL1 codec. */
cog_result_t cog_cli_command_encode(const cog_cli_command_t *command,
    void *output, size_t capacity, size_t *required);
/** @brief Decode command transactionally, strings copied to caller arena.
 * @param[in] input Bytes; @param[in] input_len Exact length.
 * @param[out] command View; @param[out] storage Text+ID NUL strings;
 * @param[in] capacity Bytes, NULL storage iff0; @param[out] required Exact bytes.
 * @return OK; NULL_PTR; PROTOCOL opcode/shape/UTF-8/scalar/trailing; LIMIT;
 * OVERFLOW changes required only. Outputs unchanged otherwise; nonoverlap.
 * Same pointer/shape-before-capacity validation, no callbacks or allocation. */
cog_result_t cog_cli_command_decode(const void *input, size_t input_len,
    cog_cli_command_t *command, void *storage, size_t capacity, size_t *required);

/**
 * @brief Encode one structurally valid immutable CGL1 frame.
 * @param[in] message Caller view; @param[out] output NULL iff capacity0.
 * @param[in] capacity Writable bytes; @param[out] required Exact bytes.
 * @return OK; NULL_PTR; INVALID_ARG scalar/type/length mismatch; LIMIT arithmetic;
 * OVERFLOW changes only required. Other errors change no output, including
 * required. No allocation, secret logging, semantic acceptance or callbacks.
 * Literal layout: magic CGL1(4), kind u8,lane u8, runtime/sender/wire/reply u64, worker
 * incarnation/number u64, owner_peer/agent/attempt u64, session-length u8 then
 * session UTF-8 bytes, payload-length u32 then payload; all integers big-endian.
 * Kind values1..29/lane1..10 above are pinned. No padding/trailing/native pointers.
 * Discovery exception: UI_HELLO alone permits runtime0/session empty(length0),
 * worker/owner/agent/attempt0. Client knows only socket; server's authenticated
 * response supplies actual root session/runtime before any bound command.
 * Fresh HELLO sender is only a client nonce; response assigns a monotonically
 * nonreused server logical lease used as sender by all later UI frames. Known
 * reconnect uses exact session/runtime/lease; retired lease is stale, never a
 * new admission. Client never retargets old command bytes to a new lease.
 * All message/view/required/input/output regions must be nonoverlapping.
 * Codec checks envelope scalars, NOT nested payload or authorization. The static
 * ingress handler validates the exact nested schema before retaining/admitting.
 */
cog_result_t cog_cli_control_encode(const cog_cli_control_message_t *message,
    void *output, size_t capacity, size_t *required);

/** @brief Transactionally decode into caller storage, without borrowed input.
 * @param[in] input Exact bytes; @param[in] input_len Exact frame length.
 * @param[out] message Scalar/pointer view into storage.
 * @param[out] storage Copied session+NUL followed by payload, NULL iff capacity0.
 * @param[in] capacity Writable bytes; @param[out] required Exact storage bytes.
 * @return OK; NULL_PTR; PROTOCOL malformed scalars/UTF-8/length/trailing bytes;
 * PROTOCOL bad prefix, zero/unknown kind or malformed envelope; UNSUPPORTED
 * recognizable CGL version other than1; OVERFLOW changes required only;
 * LIMIT arithmetic. Check pointers, version/prefix, full envelope scalars and
 * lengths, then caller capacity. Other errors leave required unchanged.
 * Every other output is unchanged on all failures. Nonoverlapping regions.
 * No peer query, allocation, dedup cursor advance, dispatch or state mutation.
 */
cog_result_t cog_cli_control_decode(const void *input, size_t input_len,
    cog_cli_control_message_t *message, void *storage, size_t capacity,
    size_t *required);

/** Retired watermark is (runtime,sender,managed worker,session,agent,attempt,LANE)
 * scoped, NOT a CGA semantic prefix. One
 * exact pending frame and last unconsumed final disposition are retained with
 * original bytes; after final-receipt consumption, a higher valid wire retires
 * proof into the watermark. Covered IDs are uniformly stale-rejected before
 * semantic ingress. No unbounded receipt log, ID reuse or guessed byte equality.
 * Reserve fresh final/control IDs before admitting ordinary frames; UINT64_MAX
 * cannot consume required cancel/drain/terminal/receipt identities. Exhaustion
 * freezes admissions and uses already-reserved cleanup identities, never wraps.
 * UPDATE_RECEIPT body follows control_receipt.h's distinct apply/copy/localACK
 * and wire-consume rules; COMMAND_ACK only reclaims copied command receipts.
 */

#ifdef __cplusplus
}
#endif
#endif

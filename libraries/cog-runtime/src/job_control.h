/** @file job_control.h
 * @brief Draft private CGL1 extension for the primary root controller.
 *
 * HELLO v2 requests the job profile using the existing 18-byte body with
 * version=2 and role=controller. On a job-capable primary, the ordinary
 * final HELLO receipt OK and assigned lease confirm the profile. A new
 * coordinator returns UNSUPPORTED when disabled. Malformed or v1-only
 * peers may close; never downgrade or dispatch a remembered job command.
 * Reconnect requires the original lease, peer credential binding and
 * negotiated profile. Limits and opcode numbers below are wire contracts.
 */
#ifndef COG_CLI_JOB_CONTROL_H
#define COG_CLI_JOB_CONTROL_H

#include <cog/result.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Extension opcodes following CGL1 UI_COMMAND opcodes 1..9. */
typedef enum cog_cli_job_opcode {
    COG_CLI_JOB_QUERY = 10, /**< COMMAND lane, key is job ID. */
    COG_CLI_JOB_CANCEL = 11, /**< CANCEL lane, key is job ID. */
    COG_CLI_JOB_APPROVAL_DETAIL = 12 /**< COMMAND lane, key is the published policy nonce. */
} cog_cli_job_opcode_t;

/** @brief Fixed 25-byte command. Big-endian u8 opcode then u64 observed
 * generation, observed sequence and key. All three integers nonzero. CGL1
 * binds the authenticated controller lease, primary session, runtime and lane;
 * worker/agent/epoch/owner_peer fields are zero. Validate before root lookup.
 */
typedef struct cog_cli_job_request {
    cog_cli_job_opcode_t opcode;
    uint64_t observed_generation, observed_sequence, key;
} cog_cli_job_request_t;

/** @brief Encode a complete job command into caller-owned storage.
 * @param[in] request Valid borrowed values. @param[out] dst NULL iff capacity=0.
 * @param[in] capacity Available bytes. @param[out] required Exact bytes on
 * OK/OVERFLOW. @return OK; NULL_PTR; INVALID_ARG bad scalar or overlap;
 * OVERFLOW preserves destination. Pure and allocation-free.
 */
cog_result_t cog_cli_job_request_encode(const cog_cli_job_request_t *request,
    void *dst, size_t capacity, size_t *required);

/** @brief Decode exactly one job command; authorization belongs to coordinator.
 * @param[in] src Borrowed complete bytes. @param[in] bytes Exact length.
 * @param[out] request Copied values, unchanged on failure.
 * @return OK; NULL_PTR; INVALID_ARG overlap; PROTOCOL invalid bytes/opcode.
 * Pure and allocation-free.
 */
cog_result_t cog_cli_job_request_decode(const void *src, size_t bytes,
    cog_cli_job_request_t *request);

/** @brief Controller-private payload appended to the fixed 21-byte FINAL
 * COMMAND_RESULT only on successful QUERY or APPROVAL_DETAIL. Encode opcode
 * u8, key u64, text length u32, exact UTF-8 bytes. All integers big-endian.
 * QUERY text is the bounded canonical status JSON produced by the primary
 * status renderer. DETAIL text is the original validated proposal arguments.
 * The client must match opcode, key, request wire and retained lease before
 * reading the value. No extension on errors, provisional, CANCEL or ACK-of-ACK.
 * The query result belongs to one outbound command receipt and its cache.
 * Ordinary shared projections, replay/GAP, diagnostics and HELLO carry no
 * private extension. No PID, pidfd or durable recovery authority on this wire.
 */
typedef struct cog_cli_job_reply {
    cog_cli_job_opcode_t opcode;
    uint64_t key;
    const char *text;
    size_t bytes;
} cog_cli_job_reply_t;

/** @brief Encode one private reply suffix without its control result prefix.
 * @param[in] reply Borrowed validated status or detail bytes.
 * @param[out] dst NULL iff capacity=0. @param[in] capacity Available bytes.
 * @param[out] required Exact bytes on OK/OVERFLOW.
 * @return OK; NULL_PTR; INVALID_ARG invalid input/overlap; LIMIT wire size;
 * OVERFLOW preserves dst. Pure and allocation-free.
 */
cog_result_t cog_cli_job_reply_encode(const cog_cli_job_reply_t *reply,
    void *dst, size_t capacity, size_t *required);

/** @brief Validate and borrow the private suffix of a matching FINAL result.
 * @param[in] src Complete suffix. @param[in] bytes Exact available bytes.
 * @param[out] reply Text references src until its owner releases it. UTF-8
 * text is length-delimited. Output unchanged on failure.
 * @return OK; NULL_PTR; INVALID_ARG overlap; PROTOCOL invalid bytes/opcode.
 * Pure and allocation-free. Render only after checking outer identity.
 */
cog_result_t cog_cli_job_reply_decode(const void *src, size_t bytes,
    cog_cli_job_reply_t *reply);

#ifdef __cplusplus
}
#endif
#endif

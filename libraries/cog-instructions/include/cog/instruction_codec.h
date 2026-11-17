/**
 * @file instruction_codec.h
 * @brief Bounded portable encoding of pinned instruction manifests.
 *
 * Draft header-first contract. No UI, filesystem, provider or transport calls.
 * Owner-thread operations; inputs remain immutable for the call. CGIM is an
 * explicit byte format, never a dump of native structs. Encoding contains
 * source bodies, so callers treat its buffers as private session data, exclude
 * them from diagnostics/replay, and clear them before reuse across principals.
 */
#ifndef COG_INSTRUCTION_CODEC_H
#define COG_INSTRUCTION_CODEC_H
#include <cog/instructions.h>

#ifdef __cplusplus
extern "C" {
#endif

#define COG_INSTRUCTION_CONTEXT_NAMESPACE "cog.instructions"
#define COG_INSTRUCTION_CONTEXT_FORMAT 1u

/**
 * @brief Encode a complete manifest in canonical CGIM version 1.
 * @param[in] manifest Complete validated snapshot view.
 * @param[out] output Caller bytes; NULL permitted only with capacity zero.
 * @param[in] capacity Output bytes, not including an implicit terminator.
 * @param[out] required Exact encoded bytes, set on OK/OVERFLOW only.
 * @return OK; NULL_PTR; INVALID_ARG inconsistent inputs; UNSUPPORTED version;
 * PROTOCOL malformed manifest; LIMIT arithmetic or a length above UINT32_MAX;
 * OVERFLOW insufficient capacity. Failure changes no output bytes. No overlap
 * with inputs, allocation or callbacks. Measure with NULL/0 before reservation.
 *
 * Format: magic "CGIM", u32 version=1, then manifest fields in declaration order
 * (including manifest.version). u32/u64 are unsigned little endian; size_t fields
 * encode u64. Enums encode u32. Strings encode u32 byte length then UTF-8 bytes,
 * no terminator; UINT32_MAX denotes NULL, zero denotes an empty string. Present
 * strings must be shorter than UINT32_MAX bytes. Both encoded version words
 * must equal 1. Explicit
 * text/text_bytes pairs encode u64 byte length plus exact bytes once. Arrays
 * encode u32 count then records; their native count fields are not repeated.
 * Sources encode kind,root_id,path,scope,owner_skill,precedence,text, then their
 * 32-byte source hash. The source_sha256 array is not separately encoded.
 * Available skill entries encode fields in declaration order including 32 hash
 * bytes. Active skills encode strings. Tools encode name,description,input_schema,
 * policy. Fragments encode origin,source_id,scope,text (u64 length plus bytes).
 * No padding, pointers, unknown fields or trailing bytes. All counts and lengths
 * must fit both host size_t and caller limits on decode. Native enum values are
 * the frozen V1 values; future enum additions require a format revision.
 */
cog_result_t cog_instruction_manifest_encode(
    const cog_instruction_manifest_t *manifest, void *output, size_t capacity,
    size_t *required);

/**
 * @brief Compute SHA-256 of the exact canonical CGIM encoding without allocation.
 * @param[in] snapshot Immutable executable or inspection snapshot.
 * @param[out] digest Exactly 32 caller-owned bytes, unchanged on failure.
 * @return OK; NULL_PTR; LIMIT if canonical encoding exceeds format bounds.
 * @note No output arena or temporary encoded copy is required: stream fields
 * into a stack-owned hash state. Digest is identity/integrity, not authorization.
 * Lifetime is independent of the source owner after the call returns.
 */
cog_result_t cog_instruction_snapshot_fingerprint(
    const cog_instruction_snapshot_t *snapshot, unsigned char digest[32]);

/**
 * @brief Construct a snapshot from an exact bounded CGIM frame.
 * @param[out] out NULL on failure; owned snapshot on OK, match snapshot_free.
 * @param[in] limits Explicit constructor limits for decoded data and descriptors.
 * @param[in] input Complete immutable frame, no transport header.
 * @param[in] input_bytes Exact bytes; truncation/trailing data is PROTOCOL.
 * @param[in] mode EXECUTE or INSPECT, as snapshot_restore.
 * @param[in] bindings Current static descriptors; INSPECT requires NULL/0.
 * @param[in] count Number of bindings.
 * @return OK; NULL_PTR; INVALID_ARG limits/mode; PROTOCOL malformed bytes,
 * hash/source/fragment mismatch; UNSUPPORTED format or descriptor drift;
 * DENIED permission drift; LIMIT reserved ceilings/overflow; NOMEM.
 * @note Validate all count/length arithmetic before allocation or iteration.
 * Constructor allocation is bounded by limits, never attacker-declared lengths
 * alone. Performs snapshot_restore semantic validation; INSPECT does not waive
 * structural/hash validation. Owns all decoded bytes; input may be cleared on
 * return. No credential/descriptor pointers, path reopening or source resolution.
 */
cog_result_t cog_instruction_snapshot_decode(cog_instruction_snapshot_t **out,
    const cog_instruction_limits_t *limits, const void *input, size_t input_bytes,
    cog_instruction_restore_mode_t mode, const cog_prompt_tool_binding_t *bindings,
    size_t count);

#ifdef __cplusplus
}
#endif
#endif /* COG_INSTRUCTION_CODEC_H */

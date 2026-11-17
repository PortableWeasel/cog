/** @file session_store_codec.h
 * @brief Private allocation-free snapshot wire codec, not an installed API.
 */
#ifndef COG_SESSION_STORE_CODEC_H
#define COG_SESSION_STORE_CODEC_H
#include "cog/session_store.h"

/** Descriptor/string workspace borrowed from the explicit store constructor.
 * Reset used=0 before each decode. Nothing allocated/freed by codec. All decoded
 * pointers borrow either input bytes or workspace until either is overwritten.
 */
typedef struct cog_store_codec_workspace {
    unsigned char *data;
    size_t capacity;
    size_t used;
} cog_store_codec_workspace_t;

/** Explicit little-endian v2/v3 snapshot records (no native structs/pointers).
 * Output is raw binary, to be wrapped by the transaction writer in a checksummed
 * immutable generation. All scalars/strings/opaque/ledger/audits and immutable contexts/bindings are preserved.
 * Encode never changes session/snapshot; LIMIT may leave destination partial.
 * out_size set only on success. Caller validates session via snapshot_view.
 */
cog_result_t cog_store_codec_encode(const cog_session_snapshot_t *snapshot,
    unsigned char *output, size_t capacity, size_t *out_size);

/** Strict binary decode into caller-reserved scratch. No STARTED normalization:
 * return the exact committed snapshot for monotonic-diff checks. For public load,
 * construct with cog_session_restore (which validates full tree/ledger semantics
 * and conservatively normalizes STARTED). Do not treat decode as replay admission.
 * Output unchanged on failure; workspace may be consumed. Unknown wire version
 * UNSUPPORTED; malformed/truncated input PROTOCOL; descriptor capacity LIMIT.
 */
cog_result_t cog_store_codec_decode(const unsigned char *input, size_t size,
    cog_store_codec_workspace_t *workspace, cog_session_snapshot_t *snapshot);

/** Strict v1 meta.json/nodes.jsonl migration into public snapshot scratch.
 * Requires exactly metadata.node_count validated complete records. Optional tail
 * recovery discards only one unterminated record AFTER that complete committed
 * prefix; malformed complete lines, missing committed records/bytes always fail.
 * Unidentified tools set inspection_only and replay_safe=false; REPLAY refuses
 * UNSUPPORTED. No disk writes or invented identities. Public load constructs with
 * session_restore. Report fields version/migration/tail facts initialized on OK;
 * transaction writer may supply checkpoint generation separately for v2.
 */
cog_result_t cog_store_codec_decode_v1(const char *session_id,
    const unsigned char *metadata, size_t metadata_size,
    const unsigned char *nodes, size_t nodes_size, cog_store_load_mode_t mode,
    bool recover_tail, cog_store_codec_workspace_t *workspace,
    cog_session_snapshot_t *snapshot, cog_store_load_report_t *report);

#endif

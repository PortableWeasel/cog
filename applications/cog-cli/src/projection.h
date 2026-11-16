/**
 * @file projection.h
 * @brief Private canonical UI state/replay commit; no execution reconstruction.
 *
 * One owner thread, outside executor callbacks. New/free reserve all rings,
 * full-state encode/copy/decode and resync arenas. No runtime allocation,
 * credentials/opaque/config references or replay of model/tool effects.
 */
#ifndef COG_CLI_PROJECTION_H
#define COG_CLI_PROJECTION_H
#include <cog_cli/cli.h>
#include <cog/agent.h>

#ifdef __cplusplus
extern "C" {
#endif
typedef struct cog_cli_projection cog_cli_projection_t;

/** One current meaningful ASK prompt, never a persistent ALLOW grant. */
typedef struct cog_cli_policy_view {
    const char *session_id;
    uint64_t assistant_node, nonce, configuration_generation, deadline_ms;
    uint16_t call_ordinal; /**< Zero-based immutable assistant tool-call order,
       not pending-list order; full literal ID remains in owned session ledger. */
    const char *tool_name; /**< Static schema name copied for wire/UI; no args. */
} cog_cli_policy_view_t;
/** Copied real registry state, not fabricated remote conversation history. */
typedef struct cog_cli_agent_view {
    cog_agent_id_t agent_id;
    uint64_t execution_epoch, group_id, configuration_generation;
    const char *session_id;
    cog_agent_status_t status;
    cog_result_t outcome;
    const char *status_text, *result_text; /**< Bounded copied actual update. */
} cog_cli_agent_view_t;
/** Full replacement of primary display AND current controls at one anchor. */
typedef struct cog_cli_projection_view {
    cog_cli_display_snapshot_t display; /**< Actual primary data, copied with
       COORDINATOR generation/sequence. Never rebuild it from a result string. */
    uint64_t active_configuration, published_configuration;
    const cog_cli_agent_view_t *agents;
    size_t agent_count;
    const cog_cli_policy_view_t *policies;
    size_t policy_count;
} cog_cli_projection_view_t;

/** Every maximum is explicit/nonzero except max_agents may be0. */
typedef struct cog_cli_projection_config {
    const char *session_id;
    uint64_t generation;
    size_t records, replay_bytes, projection_bytes;
    size_t max_display_bytes; /**< Maximum full encoded CGD1 including overhead. */
    size_t max_agents; /**<≤16; exactly one possible PRIMARY policy row. */
    size_t max_status_bytes, max_result_bytes; /**< Per agent including NUL. */
} cog_cli_projection_config_t;

/** @brief Reserve ring+complete projection/send/resync storage for fixed bounds.
 * @param[out] out Owned, NULL on failure, match free.
 * @param[in] config Copied fixed bounds, ID and checked fresh nonzero lifetime.
 * Validate worst-case declared display/control bytes
 * fit ONE CGV1/frame and resync; no successful partial view.
 * @return OK; NULL_PTR; INVALID_ARG zero/bad bounds; LIMIT arithmetic; NOMEM.
 * Underlying existing cog_ipc_replay_new/free owns its bounded ring. */
cog_result_t cog_cli_projection_new(cog_cli_projection_t **out,
    const cog_cli_projection_config_t *config);
/** @brief Consume all owned projection/ring storage; NULL is a no-op.
 * @param[in,out] projection No borrowed view/send scratch remains. */
void cog_cli_projection_free(cog_cli_projection_t *projection);

/** @brief Atomically validate/copy/append and publish one full replacement view.
 * @param[in,out] projection Owner. @param[in] source Actual immutable copied
 * executor text plus real current registry/prompt state; source app anchor is
 * input provenance only and is replaced in the committed projection.
 * @param[out] sequence Assigned nonzero CGR1 sequence on success only.
 * @return OK; NULL_PTR; INVALID_ARG bad source; LIMIT bytes/sequence exhaustion;
 * underlying append error. On error prior committed view/ring/anchor unchanged,
 * caller retains pending actual executor view and freezes ordinary admission.
 * Stage encode with known next sequence, call existing replay_append ONCE, verify
 * exact assigned sequence then infallibly publish prepared pointer/anchor. No
 * display write happens before that commit. Ring eviction is existing explicit
 * GAP semantics, not execution/history truncation. No consumer socket can block
 * commit; every client owns bounded copied sends/resync and may fall behind/GAP.
 */
cog_result_t cog_cli_projection_commit(cog_cli_projection_t *projection,
    const cog_cli_projection_view_t *source, uint64_t *sequence);

/** @brief Borrow exact latest committed full state, no mutation or effects.
 * @param[in] projection Owner. @param[out] view Valid until next commit/free.
 * @return OK; NULL_PTR; AGAIN before initial view; no partial success. */
cog_result_t cog_cli_projection_snapshot(const cog_cli_projection_t *projection,
    cog_cli_projection_view_t *view);

/** @brief Copy existing CGR1 DATA/GAP/CURSOR after client's exact cursor.
 * @param[in] projection Owner; @param[in] generation Client replay lifetime;
 * @param[in] after_sequence Last checked rendered/acknowledged ID.
 * @param[out] record Payload points into caller storage.
 * @param[out] storage Bounded payload bytes; @param[in] capacity Bytes;
 * @param[out] required Exact bytes. @return Existing replay_copy_after errors.
 * Snapshot/resync transfer uses a copied committed CGV1 plus its exact anchor,
 * never app snapshot sampled later with an unrelated cursor. After GAP install
 * snapshot then copy after its anchor; a new GAP repeats resync, no silent skip. */
cog_result_t cog_cli_projection_copy_after(const cog_cli_projection_t *projection,
    uint64_t generation, uint64_t after_sequence, cog_ipc_replay_record_t *record,
    void *storage, size_t capacity, size_t *required);

/** @brief Encode CGV1 full replacement, with exact nested CGD1 anchor.
 * @param[in] view Valid source. @param[out] output NULL iff capacity0.
 * @param[in] capacity Bytes. @param[out] required Exact bytes.
 * @return OK; NULL_PTR; INVALID_ARG; LIMIT; OVERFLOW changes required only.
 * No other output change on failure. Literal body defined in HEADLESS_VIEW.md.
 * No allocation, rendering, state mutation, log or authority grant. */
cog_result_t cog_cli_projection_encode(const cog_cli_projection_view_t *view,
    void *output, size_t capacity, size_t *required);

/** @brief Decode full CGV1 transactionally into caller-provided array/text arenas.
 * @param[in] input Frame; @param[in] input_len Exact bytes.
 * @param[out] view Pointers to caller arrays/storage, unchanged on failure.
 * @param[out] items CGD1 rows; @param[in] item_capacity Rows;
 * @param[out] agents Agent rows; @param[in] agent_capacity Rows;
 * @param[out] policies Prompt rows; @param[in] policy_capacity Rows;
 * @param[out] storage Text; @param[in] capacity Bytes;
 * @param[out] required_items Exact counts on OK/OVERFLOW;
 * @param[out] required_agents Exact count; @param[out] required_policies Count;
 * @param[out] required_storage Exact text bytes.
 * @return OK; NULL_PTR; PROTOCOL invalid status/IDs/duplicate rows/correlation/
 * anchor/UTF-8/NUL/length/trailing; UNSUPPORTED magic; OVERFLOW changes only all
 * required counts. Every other output untouched on error; no allocation.
 * Structural ceilings16 agents/1 PRIMARY policy and caller-declared string bounds
 * are validated by a full nonwriting scan before any nested CGD1 decode writes
 * final caller arrays. Validate full shape/correlations before capacities; use
 * second writing pass only after all checks, preserving outer transactionality.
 */
cog_result_t cog_cli_projection_decode(const void *input, size_t input_len,
    cog_cli_projection_view_t *view, cog_cli_display_item_t *items,
    size_t item_capacity, cog_cli_agent_view_t *agents, size_t agent_capacity,
    cog_cli_policy_view_t *policies, size_t policy_capacity, char *storage,
    size_t capacity, size_t *required_items, size_t *required_agents,
    size_t *required_policies, size_t *required_storage);

#ifdef __cplusplus
}
#endif
#endif

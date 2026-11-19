/**
 * @file tool_job.h
 * @brief Durable identity and outcome for work that outlives a tool call.
 *
 * These are value types and borrowed views. The session owns every referenced
 * byte and is serviced by one owner thread. Never serialize this native layout.
 */
#ifndef COG_TOOL_JOB_H
#define COG_TOOL_JOB_H

#include <cog/tool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Session-local job identity, allocated from the launch STARTED audit
 * sequence. Zero is invalid. The model token is "job-" followed by exactly
 * 16 lowercase hexadecimal digits. The token grants no access on its own.
 */
typedef uint64_t cog_tool_job_id_t;

/** @brief Durable job phase, independent of the launch call ledger. */
typedef enum cog_tool_job_state {
    COG_TOOL_JOB_INTENT = 0, /**< Persisted before invocation. */
    COG_TOOL_JOB_ACCEPTED, /**< Launch receipt committed; outcome still pending. */
    COG_TOOL_JOB_FINISHED, /**< Verified terminal outcome; resources drained. */
    COG_TOOL_JOB_INDETERMINATE, /**< Effect or terminal outcome unverified. */
    COG_TOOL_JOB_ACKNOWLEDGED /**< Operator closed an unknown outcome. */
} cog_tool_job_state_t;

/** @brief Immutable predecessor identity and current durable job outcome.
 * Unique launch association. receipt_node_id is zero until a successful
 * launch receipt is published and never changes thereafter. The origin context
 * binding is the launch assistant's immutable binding. outcome is present
 * only for FINISHED; text is complete bounded UTF-8, NUL-terminated, and
 * carries the truncation flag. A terminal resolution records evidence only if
 * reconciled; ACKNOWLEDGED requires evidence and never fabricates an outcome.
 * Nothing in this view is a process handle, clock deadline or replay grant.
 */
typedef struct cog_tool_job_record {
    cog_tool_job_id_t id;
    uint64_t assistant_node_id;
    const char *call_id;
    uint64_t receipt_node_id;
    cog_tool_job_state_t state;
    const cog_tool_output_t *outcome;
    const char *evidence;
    size_t evidence_bytes;
} cog_tool_job_record_t;

#ifdef __cplusplus
}
#endif

#endif /* COG_TOOL_JOB_H */

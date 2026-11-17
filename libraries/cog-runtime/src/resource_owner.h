#ifndef COG_RUNTIME_RESOURCE_OWNER_H
#define COG_RUNTIME_RESOURCE_OWNER_H
#include <cog/instructions.h>
#include <cog/session_store.h>
#include <stdint.h>
typedef struct runtime_instruction_job runtime_instruction_job_t;
/** Coordinator ownership separate from secret wire buffers. Current association
 * changes only after exact SAVED proof. Per-session high-water survives receipt
 * retirement; all durable historical contexts are scanned on first request.
 * Attempt destruction consumes resolver first, then proof/candidate/current. */
typedef struct cli_resource_owner {
    cog_instruction_snapshot_t *current, *candidate;
    cog_session_t *proof, *saved_proof;
    cog_store_load_report_t report;
    runtime_instruction_job_t *job;
    uint64_t high_water, reply_wire, candidate_id;
    bool prepared;
} cli_resource_owner_t;
cog_result_t cli_resource_owner_new(cli_resource_owner_t **out);
cog_result_t cli_resource_owner_retire(cli_resource_owner_t *owner);
cog_result_t cli_resource_owner_reset(cli_resource_owner_t *owner);
cog_result_t cli_resource_owner_free(cli_resource_owner_t **owner);
#endif

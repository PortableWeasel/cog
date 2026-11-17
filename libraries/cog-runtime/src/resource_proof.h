/** @file resource_proof.h @brief Exact immutable CGIR checkpoint evidence.
 * Constructors load INSPECT only; never authorize execution or mutate/save.
 * Outputs NULL on failure. Caller frees returned proof sessions. Exact current
 * owner must match before and after loading immutable checkpoint. Request matches
 * selected and assistant-bound old capsule, identified call and persisted state;
 * synthetic recovery audit above source watermark is distinguished from durable
 * uncertainty. Caller additionally validates requested operation arguments via
 * cli_runtime_resource_request_matches against the returned identified call.
 * Commit requires newer checkpoint, exact selected candidate, old call identity
 * and bytes, old bindings, preserved old evidence/results and complete no-effect
 * closure of every unstarted member. No proof uses latest-checkpoint guessing.
 */
#ifndef COG_RUNTIME_RESOURCE_PROOF_H
#define COG_RUNTIME_RESOURCE_PROOF_H
#include "instruction_transfer.h"
#include <cog/session_store.h>
#include <cog/instructions.h>
cog_result_t cli_resource_request_prove(cog_store_t *store,cog_store_owner_t owner,
    const char *session_id,const cog_cli_instruction_resource_t *request,
    const cog_cli_instruction_capsule_t *old,const cog_prompt_tool_binding_t *bindings,
    size_t binding_count,const cog_instruction_limits_t *limits,
    cog_session_t **proof_session,cog_store_load_report_t *report);
cog_result_t cli_resource_commit_prove(cog_store_t *store,cog_store_owner_t owner,
    const char *session_id,const cog_cli_instruction_resource_t *request,
    const cog_session_t *old_proof,const cog_store_load_report_t *old_report,
    const cog_cli_instruction_capsule_t *candidate,uint64_t checkpoint_generation,
    cog_session_t **saved_proof);
#endif

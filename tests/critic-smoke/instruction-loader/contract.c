/** @file contract.c @brief Loader-only consumer with no frontend/provider tree. */
#include <cog/instruction_source.h>
#include <cog/instructions.h>
#include <cog/instruction_codec.h>
#include <cog/tool.h>

#if defined(COG_CLI_H) || defined(COG_PROVIDER_VTABLE_H) || defined(COG_NET_H) || defined(COG_IPC_H)
#error "Instruction loading must not pull UI, provider, networking or IPC APIs"
#endif

int main(void)
{
    cog_instruction_source_request_t job = {0};
    cog_instruction_selection_t selection = {0};
    cog_instruction_manifest_t manifest = {0};
    cog_prompt_bundle_t bundle = {0};
    cog_result_t (*view)(const cog_instruction_snapshot_t *, cog_prompt_bundle_t *) =
        cog_instruction_prompt_view;
    job.operation = COG_INSTRUCTION_SOURCE_SCOPE;
    selection.snapshot_id = 1U;
    manifest.version = COG_INSTRUCTION_MANIFEST_V1;
    bundle.snapshot_id = selection.snapshot_id;
    (void)job; (void)manifest; (void)bundle; (void)view;
    return 0;
}

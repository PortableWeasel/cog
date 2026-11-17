/** @file test_instruction_headers.c @brief Compile-only draft API type coverage. */
#include <cog/instruction_source.h>
#include <cog/instructions.h>
#include <cog/provider_prompt.h>

static cog_result_t render(const cog_provider_prompt_input_t *input,
    const cog_provider_prompt_limits_t *limits, void *scratch, size_t scratch_capacity,
    void *storage, size_t capacity, cog_provider_request_t *request)
{
    (void)input; (void)limits; (void)scratch; (void)scratch_capacity;
    (void)storage; (void)capacity; (void)request;
    return COG_ERROR_UNSUPPORTED;
}

int main(void)
{
    cog_result_t (*reader_new)(cog_instruction_source_reader_t **,
        const cog_instruction_source_config_t *, cog_instruction_diagnostic_t *) =
        cog_instruction_source_reader_new;
    cog_result_t (*source_begin)(cog_instruction_source_reader_t *,
        const cog_instruction_source_request_t *) = cog_instruction_source_begin;
    cog_result_t (*source_poll)(cog_instruction_source_reader_t *, bool *) = cog_instruction_source_poll;
    cog_result_t (*source_view)(const cog_instruction_source_reader_t *,
        const cog_instruction_source_item_t **, size_t *) = cog_instruction_source_view;
    cog_result_t (*source_cancel)(cog_instruction_source_reader_t *) = cog_instruction_source_cancel;
    cog_result_t (*source_error)(const cog_instruction_source_reader_t *,
        cog_instruction_diagnostic_t *) = cog_instruction_source_error;
    cog_result_t (*reader_free)(cog_instruction_source_reader_t *) = cog_instruction_source_reader_free;
    cog_result_t (*catalog_new)(cog_instruction_catalog_t **,
        const cog_instruction_limits_t *, uint64_t,
        const cog_instruction_source_t *, size_t, cog_instruction_diagnostic_t *) = cog_instruction_catalog_new;
    cog_result_t (*catalog_view)(const cog_instruction_catalog_t *,
        const cog_instruction_entry_t **, size_t *) = cog_instruction_catalog_view;
    cog_result_t (*snapshot_new)(cog_instruction_snapshot_t **,
        const cog_instruction_catalog_t *, const cog_instruction_limits_t *,
        const cog_instruction_selection_t *, const cog_instruction_source_t *,
        size_t, cog_instruction_diagnostic_t *) = cog_instruction_snapshot_new;
    cog_result_t (*snapshot_view)(const cog_instruction_snapshot_t *,
        cog_instruction_manifest_t *) = cog_instruction_snapshot_view;
    cog_result_t (*snapshot_restore)(cog_instruction_snapshot_t **,
        const cog_instruction_limits_t *, const cog_instruction_manifest_t *,
        cog_instruction_restore_mode_t, const cog_prompt_tool_binding_t *,
        size_t, cog_instruction_diagnostic_t *) = cog_instruction_snapshot_restore;
    cog_result_t (*prompt_view)(const cog_instruction_snapshot_t *,
        cog_prompt_bundle_t *) =
        cog_instruction_prompt_view;
    cog_result_t (*catalog_free)(cog_instruction_catalog_t *) = cog_instruction_catalog_free;
    cog_result_t (*snapshot_free)(cog_instruction_snapshot_t *) = cog_instruction_snapshot_free;
    cog_result_t (*compiler_new)(cog_provider_prompt_compiler_t **,
        const cog_provider_prompt_template_t *, const cog_provider_prompt_limits_t *) =
        cog_provider_prompt_compiler_new;
    cog_result_t (*compile)(cog_provider_prompt_compiler_t *, const cog_provider_vtable_t *,
        const cog_provider_prompt_input_t *, cog_provider_request_t *) = cog_provider_prompt_compile;
    cog_result_t (*compiler_free)(cog_provider_prompt_compiler_t *) = cog_provider_prompt_compiler_free;
    const cog_provider_prompt_template_t format = {
        .id = "compile-fixture", .revision = 1U, .provider = NULL,
        .required_scratch_bytes = 1U, .render = render};
    cog_instruction_source_request_t source = {.operation = COG_INSTRUCTION_SOURCE_SCOPE,
        .root_id = "project", .path = "libraries", .optional = false};
    cog_instruction_selection_t selection = {0};
    cog_instruction_manifest_t manifest = {0};
    cog_provider_prompt_input_t input = {0};
    selection.snapshot_id = 1U;
    manifest.version = COG_INSTRUCTION_MANIFEST_V1;
    input.prompt.template_id = format.id;
    input.prompt.template_revision = format.revision;
    input.prompt.fragment_count = manifest.fragment_count;
    (void)reader_new; (void)source_begin; (void)source_poll; (void)source_view;
    (void)source_cancel; (void)source_error; (void)reader_free; (void)catalog_new; (void)catalog_view;
    (void)snapshot_new; (void)snapshot_view; (void)snapshot_restore; (void)prompt_view;
    (void)catalog_free; (void)snapshot_free; (void)compiler_new; (void)compile;
    (void)compiler_free; (void)format; (void)source; (void)selection; (void)input;
    return 0;
}

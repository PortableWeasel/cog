/** @file test_context_headers.c @brief Compile-only context contract use. */
#include <cog/session.h>
#include <cog/session_store.h>

int main(void)
{
    cog_result_t (*add)(cog_session_t *, const cog_session_context_t *) =
        cog_session_context_add;
    cog_result_t (*publish)(cog_session_t *, const cog_session_context_binding_t *) =
        cog_session_context_publish;
    cog_result_t (*select_context)(cog_session_t *, cog_session_node_t *, uint64_t) =
        cog_session_context_select;
    cog_result_t (*view)(const cog_session_t *, uint64_t, cog_session_context_t *) =
        cog_session_context_view;
    cog_session_config_t config = {0};
    cog_session_snapshot_t snapshot = {0};
    cog_store_config_t store = {0};
    cog_session_context_t context = {0};
    cog_session_context_binding_t binding = {0};
    config.max_contexts = 4U;
    config.max_context_bindings = 8U;
    config.max_context_bytes = 4096U;
    context.context_id = 1U;
    binding.context_id = context.context_id;
    snapshot.selected_context_binding_id = binding.binding_id;
    store.required_capabilities = COG_STORE_CAP_CONTEXT_V3;
    (void)add; (void)publish; (void)select_context; (void)view;
    (void)config; (void)snapshot; (void)store;
    return 0;
}

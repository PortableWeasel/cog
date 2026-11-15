/** @file toolkit_core.h @brief Small real core/session fixture for toolkit tests. */
#ifndef COG_TEST_TOOLKIT_CORE_H
#define COG_TEST_TOOLKIT_CORE_H
#include <cog/core.h>
#include <cog/session.h>
#include <assert.h>
static cog_core_t *toolkit_test_core;
static cog_session_t *toolkit_test_session;
static cog_handler_result_t toolkit_test_handler(cog_core_t *core, const cog_event_t *event)
{
    (void)core; (void)event;
    cog_handler_result_t result = { COG_TRANSITION_STAY, COG_OK };
    return result;
}
static void toolkit_core_create(void)
{
    static const cog_state_t transitions[] = { COG_TRANSITION_END };
    static const cog_dispatch_entry_t dispatch[] = {
        { 0U, COG_EVENT_USER_INPUT, toolkit_test_handler, transitions }
    };
    cog_session_config_t session = {0};
    session.id = "toolkit-test";
    session.max_nodes = 32U;
    assert(cog_session_new(&toolkit_test_session, &session) == COG_OK);
    cog_core_config_t config = {0};
    config.session = toolkit_test_session;
    config.dispatch_table = dispatch;
    config.dispatch_table_size = 1U;
    config.pending_session_capacity = 1U;
    config.pending_messages_per_session = 1U;
    config.pending_message_bytes = 16U;
    config.max_agents = 1U;
    config.max_agent_message_bytes = 16U;
    config.ipc_max_frame_bytes = 128U;
    config.ipc_payload_bytes = 128U;
    assert(cog_core_new(&toolkit_test_core, &config) == COG_OK);
}
static void toolkit_core_destroy(void)
{
    cog_core_free(toolkit_test_core);
    cog_session_free(toolkit_test_session);
}
#endif

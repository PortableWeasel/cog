/** @file app_internal.h @brief Private bounded CLI composition and handlers. */
#ifndef COG_CLI_APP_INTERNAL_H
#define COG_CLI_APP_INTERNAL_H
#include "configuration.h"
#include <cog/workspace.h>
#include <stdatomic.h>
#define CLI_NODES 1024U
#define CLI_CALLS 16U
#define CLI_OPAQUE 16U
#define CLI_INPUTS 8U
#define CLI_ID_BYTES 256U
#define CLI_ARGUMENT_BYTES 65536U
#define CLI_SCHEMA_BYTES 65536U
#define CLI_CODEC_BYTES 1048576U

enum { CLI_IDLE = 0, CLI_MODEL, CLI_BACKOFF, CLI_POLICY, CLI_TOOL };
typedef struct cli_call {
    cog_message_tool_call_t view;
    char id[CLI_ID_BYTES], name[CLI_ID_BYTES];
    char *arguments;
    bool begun, ended;
} cli_call_t;
typedef struct cli_input {
    char *text;
    size_t length;
    uint64_t order;
    bool used, queued;
} cli_input_t;
struct cog_cli_app {
    cog_cli_configuration_t *configuration;
    cog_cli_options_t opts;
    cog_session_t *session;
    cog_provider_t *provider;
    cog_obs_t *obs;
    cog_store_t *store;
    cog_store_owner_guard_t *guard;
    cog_core_t *core;
    cog_ipc_endpoint_t *ipc;
    cog_workspace_t *workspace;
    cog_toolkit_execution_t *execution;
    cog_mock_script_t script;
    cog_mock_step_t *steps;
    size_t step_count;
    char *chunks_copy;
    FILE *out, *log_file;
    cog_obs_sink_t log_sink;
    cog_obs_sink_table_t sink_table;
    atomic_bool stop_requested;
    bool prompt_submitted, shutdown, cancelling, active, provider_active;
    bool delta_ready, event_queued, tool_done, effects, accepted, policy_prompt;
    bool recovery, stdin_eof;
    int signal_fd;
    cog_result_t deferred_error;
    cog_toolkit_policy_decision_t policy_decision;
    uint64_t now, started_at, retry_at, sequence, display_sequence, generation;
    uint32_t retries, rounds;
    char *text, *opaque_storage, *delta_storage, *result_storage, *tools_json;
    size_t text_len, opaque_used, opaque_count, call_count, tool_index;
    size_t delta_capacity, result_capacity, tools_json_len;
    cog_provider_delta_t delta;
    cog_usage_t usage;
    char currency[32];
    cli_call_t calls[CLI_CALLS];
    cog_message_tool_call_t call_views[CLI_CALLS];
    cog_message_opaque_t opaque[CLI_OPAQUE];
    cog_message_t *history;
    cog_cli_display_item_t *display_items;
    cog_session_node_t *assistant, *safe_ancestor;
    cli_input_t inputs[CLI_INPUTS];
    uint64_t input_order;
    char line[4096];
    size_t line_length;
};
const cog_dispatch_entry_t *cog_cli_dispatch_table(size_t *count);
cog_result_t cog_cli_service(void *arg, uint64_t now, uint64_t *deadline);
cog_result_t cog_cli_idle(void *arg, uint32_t max_wait_ms);
cog_result_t cog_cli_save(cog_cli_app_t *app);
cog_result_t cog_cli_begin_request(cog_cli_app_t *app, bool reset);
cog_result_t cog_cli_finish_partial(cog_cli_app_t *app, cog_result_t reason);
cog_result_t cog_cli_tools_step(cog_cli_app_t *app, cog_state_t *next);
cog_result_t cog_cli_tools_cancel(cog_cli_app_t *app);
cog_result_t cog_cli_drain(cog_cli_app_t *app);
cog_result_t cog_cli_clock(uint64_t *now);
cog_result_t cog_cli_tools_json(cog_cli_app_t *app);
bool cog_cli_text_valid(const char *text, size_t length);
#endif

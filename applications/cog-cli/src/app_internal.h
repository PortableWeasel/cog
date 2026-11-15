/** @file app_internal.h @brief Private bounded CLI composition and handlers. */
#ifndef COG_CLI_APP_INTERNAL_H
#define COG_CLI_APP_INTERNAL_H
#include "configuration.h"
#include <cog/workspace.h>
#include <cog/config.h>
#include <stdatomic.h>
#define CLI_NODES 1024U
#define CLI_CALLS 16U
#define CLI_OPAQUE 16U
#define CLI_INPUTS 8U
#define CLI_ID_BYTES 256U
#define CLI_ARGUMENT_BYTES 65536U
#define CLI_SCHEMA_BYTES 65536U
#define CLI_CODEC_BYTES 1048576U
#define CLI_LIST_IDS 1024U
#define CLI_EVIDENCE_BYTES 4096U
#define CLI_RELOAD_ATTEMPTS 64U

enum { CLI_IDLE = 0, CLI_MODEL, CLI_BACKOFF, CLI_POLICY, CLI_TOOL };
enum { CLI_EVENT_RESUME_POLICY = 0x7f00 };
typedef struct cli_call {
    cog_message_tool_call_t view;
    char id[CLI_ID_BYTES], name[CLI_ID_BYTES];
    char *arguments;
    bool begun, ended, resumed_closed;
} cli_call_t;
typedef struct cli_input {
    char *text;
    size_t length;
    uint64_t order;
    bool used, queued, continuation_authorized;
} cli_input_t;
struct cog_cli_app {
    cog_cli_configuration_t *configuration;
    cog_cli_options_t opts;
    /* Active aliases above/below borrow current owner; future is published but
       never used by active turn work. No third generation may be constructed. */
    cog_cli_configuration_t *future_configuration;
    cog_provider_t *future_provider;
    cog_config_tree_t *reload_tree;
    uint64_t configuration_generation, future_configuration_generation;
    unsigned reload_attempts;
    bool reload_in_progress;
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
    bool resuming_policy, recovery_write_failed;
    bool delta_ready, event_queued, tool_done, effects, accepted, policy_prompt;
    bool recovery, stdin_eof, continuation_required;
    bool terminal_result_pending, result_node_added, assistant_pending;
    cog_message_status_t pending_assistant_status;
    cog_result_t assistant_error;
    size_t pending_result_len;
    cog_result_t tool_result_error;
    int signal_fd;
    cog_result_t deferred_error;
    cog_toolkit_policy_decision_t policy_decision;
    uint64_t now, started_at, retry_at, sequence, display_sequence, generation, request_serial;
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
    cog_session_node_t *assistant, *safe_ancestor, *fork_parent;
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
cog_result_t cog_cli_context_check(cog_cli_app_t *app, size_t count);
cog_result_t cog_cli_finish_partial(cog_cli_app_t *app, cog_result_t reason);
cog_result_t cog_cli_tools_step(cog_cli_app_t *app, cog_state_t *next);
cog_result_t cog_cli_tools_cancel(cog_cli_app_t *app);
cog_result_t cog_cli_drain(cog_cli_app_t *app);
cog_result_t cog_cli_clock(uint64_t *now);
cog_result_t cog_cli_tools_json(cog_cli_app_t *app);
bool cog_cli_text_valid(const char *text, size_t length);
/** Initialize the bounded three-key control tree after startup config is valid.
 * app-owned constructor: new_ex/free, limits64 nodes/65536 string bytes/65 txn
 * identities/1 validator; startup consumes one identity. No raw secrets in tree.
 * @return OK or constructor/seed/validation error; cleanup via reload_free. */
cog_result_t cog_cli_reload_init(cog_cli_app_t *app);
/** Activate the single future generation only at a safe idle owner boundary,
 * before input or restored proposal admission. Requires no active app/provider
 * turn, retained delta/event, execution, pending result/assistant, resuming policy,
 * cancellation, shutdown or frozen admission. Otherwise leave future untouched.
 * Uses cog_core_set_provider on a valid core (infallible), updates aliases before
 * old disconnect then secret free. No allocation/callback/wire/session mutation.
 * @return OK; unexpected disconnect contract violation freezes admission and
 * retains any still-borrowed generation, surfaced as the original error. */
cog_result_t cog_cli_reload_activate(cog_cli_app_t *app);
/** Destroy future/current generation resources after core and turn work drain.
 * Disconnect every provider before clearing its config. Safe on partial init;
 * no callback/context ownership transfer; tree_free after all transactions end. */
void cog_cli_reload_free(cog_cli_app_t *app);
/** Handle only /reload model VALUE, /reload base-url VALUE and
 * /reload credential-ref VALUE, preserving the whole suffix as one field.
 * @param app Owner app; @param text Complete UTF-8 line; @param handled Output.
 * Reload commands are checked before policy answers; never reinterpret them as
 * ALLOW/DENY, user prompts or model tool instructions. No secret/value echo.
 * @return reload result; NOT_FOUND/INVALID_ARG for unknown key/missing value;
 * IO only for checked output. Owner thread between dispatches, no callbacks. */
cog_result_t cog_cli_reload_command(cog_cli_app_t *app, const char *text,
    bool *handled);
bool cog_cli_metadata(const cog_cli_app_t *app);
cog_result_t cog_cli_sessions_open(cog_cli_app_t *app);
cog_result_t cog_cli_sessions_prepare(cog_cli_app_t *app);
cog_result_t cog_cli_metadata_run(cog_cli_app_t *app);
cog_result_t cog_cli_session_command(cog_cli_app_t *app, const char *text, bool *handled);
cog_result_t cog_cli_usage(cog_cli_app_t *app, const cog_session_node_t *node, FILE *stream, bool explicit_request);
cog_session_node_t *cog_cli_session_node(cog_cli_app_t *app, uint64_t id);
#endif

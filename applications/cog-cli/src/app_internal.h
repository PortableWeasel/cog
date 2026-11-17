/** @file app_internal.h @brief Private bounded CLI composition and handlers. */
#ifndef COG_CLI_APP_INTERNAL_H
#define COG_CLI_APP_INTERNAL_H
#include "configuration.h"
#include "observation.h"
#include "session_admission.h"
#include <cog/workspace.h>
/* Control-tree ownership is private; only app_reload.c needs config.h. */
typedef struct cog_config_tree cog_config_tree_t;
/* O4 private implementation layout; standalone constructors leave this NULL. */
typedef struct cli_execution_state cli_execution_state_t;
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

/* N headless uses PUBLICATION and PREPARE as explicit static dispatch states;
   standalone behavior keeps original values and paths. No bodies added here. */
enum { CLI_IDLE = 0, CLI_MODEL, CLI_BACKOFF, CLI_POLICY, CLI_TOOL,
       CLI_PUBLICATION, CLI_PREPARE };
enum { CLI_EVENT_RESUME_POLICY = 0x7f00,
       CLI_EVENT_PUBLICATION = 0x7f01, CLI_EVENT_PREPARE = 0x7f02 };
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
    cli_execution_state_t *headless; /* Owned explicit app constructor reservation. */
    /* Optional host bridge borrowed past provider teardown, never net-owned.
       Application timing/error state exists even with an external observer. */
    cog_cli_observer_t *observer;
    cog_result_t observation_error;
    cog_cli_observation_scope_t observation_scope;
    uint64_t observation_request_counter, provider_started_ns, tool_started_ns, tool_assistant_id;
    size_t tool_ordinal;
    const char *observed_tool; /* Static descriptor name or static unknown. */
    cog_obs_span_id_t provider_span, tool_span;
    bool provider_timed, tool_timed, tool_execution_started, tool_observed, provider_usage_seen, observation_retry;
    bool observation_draining;
    char log_buffer[4096]; /* Owned setvbuf storage for an explicitly opened log. */
    cog_cli_configuration_t *configuration;
    cog_cli_options_t opts;
    /* Active aliases above/below borrow current owner; future is published but
       never used by active turn work. No third generation may be constructed. */
    cog_cli_configuration_t *future_configuration;
    cog_provider_t *future_provider;
    cog_cli_configuration_t *retiring_configuration;
    cog_provider_t *retiring_provider;
    cog_config_tree_t *reload_tree;
    uint64_t configuration_generation, future_configuration_generation;
    unsigned reload_attempts;
    bool reload_in_progress, reload_notice_pending, reload_notice_queued;
    cog_result_t reload_cleanup_error; /* first cleanup error; not provider error */
    uint64_t reload_notice_generation;
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
/** Failed repeated ASK prompt write/flush returns IO; never consumes a later
 * allow as successful delivery of the failed prompt. */
cog_result_t cog_cli_idle(void *arg, uint32_t max_wait_ms);
cog_result_t cog_cli_save(cog_cli_app_t *app);
cog_result_t cog_cli_begin_request(cog_cli_app_t *app, bool reset);
cog_result_t cog_cli_context_check(cog_cli_app_t *app, size_t count);
cog_result_t cog_cli_finish_partial(cog_cli_app_t *app, cog_result_t reason);
/** Initial ASK prompt must write/flush successfully before policy_prompt is
 * published. Failure returns IO before execution construction/STARTED; ordinary
 * drain denies the still-PROPOSED call without executing it. */
cog_result_t cog_cli_tools_step(cog_cli_app_t *app, cog_state_t *next);
cog_result_t cog_cli_tools_cancel(cog_cli_app_t *app);
cog_result_t cog_cli_drain(cog_cli_app_t *app);
cog_result_t cog_cli_clock(uint64_t *now);
cog_result_t cog_cli_tools_json(cog_cli_app_t *app);
bool cog_cli_text_valid(const char *text, size_t length);
/** Initialize the bounded three-key control tree after startup config is valid.
 * @param[in,out] app Owner app; metadata modes skip the tree and return OK.
 * app-owned constructor: new_ex/free, limits64 nodes/65536 string bytes/65 txn
 * identities/1 validator; startup consumes one identity. No raw secrets in tree.
 * @return OK; NULL_PTR; constructor/seed/validation error; cleanup via reload_free. */
cog_result_t cog_cli_reload_init(cog_cli_app_t *app);
/** Activate the single future generation only at a safe idle owner boundary,
 * @param[in,out] app Normal owner app with a valid core for activation.
 * before input or restored proposal admission. Requires no active app/provider
 * turn, retained delta/event, execution, pending result/assistant, resuming policy,
 * cancellation, shutdown or frozen admission. Otherwise leave future untouched
 * and return AGAIN; callers defer fresh/queued input without losing its lease.
 * May run at input-handler entry before acquiring any provider reference, or
 * in owner service/idle hooks, including immediately after terminal reload.
 * Uses cog_core_set_provider on a valid core (infallible), updates aliases before
 * old disconnect then secret free. Move old resources to explicit retiring
 * fields only until the destructor call: detach/NULL all provider aliases before
 * disconnect, which consumes its handle on EVERY return. Clear/free matching
 * config after return even on cleanup error; never retain/retry the old handle.
 * Only current+future or current+retiring can coexist; never a third constructed
 * owner. No allocation, arbitrary app/resolver/
 * tool callback, wire or session mutation; provider disconnect is the destructor.
 * @return OK if no future or successfully activated; NULL_PTR; AGAIN while
 * future is blocked; observation checkpoints drain/check before publication or
 * retirement and can return a frozen observation driver error. Consuming
 * disconnect cleanup error records first separate
 * reload_cleanup_error, freezes admission and requests cancel/drain shutdown,
 * then returns that driver error. Commit/new publication are not rolled back;
 * new current/core references remain valid but may not start work. */
cog_result_t cog_cli_reload_activate(cog_cli_app_t *app);
/** Fair service of one retained CONFIG_CHANGED notice.
 * @param[in,out] app Normal owner app; no arbitrary callback recursion.
 * Enqueue uses source_id
 * copied by value, NULL payload, no allocation; OVERFLOW retains pending notice
 * and returns OK for retry on later service. All app states have a self handler
 * that acknowledges only matching generation and clears notice flags. It must
 * not alter the ordinary request event_queued/sequence/policy state.
 * @return OK; NULL_PTR; unexpected enqueue error is a driver failure. */
cog_result_t cog_cli_reload_service(cog_cli_app_t *app);
/** Destroy future/current/retiring owners after core and turn work drain.
 * @param[in,out] app Owner app; NULL is a no-op.
 * Detach/NULL every provider alias before its one consuming disconnect; clear
 * corresponding config after EVERY return. Continue disposing remaining owners
 * on error; no consumed pointer is retried. Safe on partial init; no callback/
 * context ownership transfer; tree_free after transactions end. app_free reports
 * returned cleanup failure in sanitized form without changing this consumption.
 * @return OK or first cleanup error after all owned resources are released. */
cog_result_t cog_cli_reload_free(cog_cli_app_t *app);
/** Handle only /reload model VALUE, /reload base-url VALUE and
 * /reload credential-ref VALUE, preserving the whole suffix as one field.
 * @param[in,out] app Owner app; @param[in] text Complete UTF-8 line;
 * @param[out] handled Whether this is a /reload command, even on refusal.
 * Recognize all /reload-prefixed commands as handled even on refusal. Terminal
 * input prints only sanitized status/generation and keeps old work running on
 * ordinary candidate errors (including NOMEM/IO/LIMIT) when cleanup succeeded.
 * A recorded discard/retirement cleanup error instead reaches the driver/frozen
 * cancel-drain exit; inspect reload_cleanup_error independently of overlapping
 * result codes before ordinary refusal conversion, never conceal that freeze.
 * A failed output write is fatal IO.
 * Reload commands are checked before policy answers; never reinterpret them as
 * ALLOW/DENY, user prompts or model tool instructions. No secret/value echo.
 * @return OK for handled success or sanitized candidate refusal, including
 * unknown key/missing value; NULL_PTR for invalid helper pointers; consuming
 * cleanup error as a fatal driver result; IO for checked output failure.
 * Non-reload input leaves handled=false and returns OK. Owner
 * thread between dispatches; only explicit reload constructor may call resolver. */
cog_result_t cog_cli_reload_command(cog_cli_app_t *app, const char *text,
    bool *handled);
bool cog_cli_metadata(const cog_cli_app_t *app);
cog_result_t cog_cli_sessions_prepare(cog_cli_app_t *app);
cog_result_t cog_cli_metadata_run(cog_cli_app_t *app);
cog_result_t cog_cli_session_command(cog_cli_app_t *app, const char *text, bool *handled);
cog_result_t cog_cli_usage(cog_cli_app_t *app, const cog_session_node_t *node, FILE *stream, bool explicit_request);
cog_session_node_t *cog_cli_session_node(cog_cli_app_t *app, uint64_t id);
#endif

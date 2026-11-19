#ifndef COG_RUNTIME_INTERNAL_H
#define COG_RUNTIME_INTERNAL_H
#include "app_internal.h"
#include "host_runtime.h"
#include "instruction_transfer.h"
#include <cog/instruction_codec.h>
typedef struct runtime_instruction_job runtime_instruction_job_t;
typedef struct runtime_snapshot_cache {
  uint64_t context_id;
  cog_instruction_snapshot_t *snapshot;
} runtime_snapshot_cache_t;
struct cog_runtime {
  cog_cli_app_t *app;
  cog_toolkit_validator_t *validator;
  cog_toolkit_execution_t *background_execution;
  cog_tool_job_id_t background_id;
  char *background_output;
  bool foreground_done, job_save_failed;
  cog_cli_host_runtime_t *host;
  bool host_root_owner; /**< Root facade owns host construction even before host
    output publication; host teardown closes its app without freeing facade. */
  cog_runtime_workers_t workers;
  cog_runtime_config_t config;
  cog_instruction_snapshot_t *instructions;
  runtime_snapshot_cache_t *snapshots;
  size_t snapshot_count, snapshot_capacity;
  bool snapshots_loaded;
  cog_provider_prompt_compiler_t *compiler;
  cog_instruction_source_reader_t *reader;
  cog_runtime_t *source_owner; /**< Borrowed pinned reader/lease owner for local children;
    root outlives children. NULL means this runtime owns its reader. Child jobs
    retain child snapshot/execution identity; source operations lease owner only. */
  cog_prompt_tool_binding_t *bindings;
  cog_runtime_scope_binding_t *scope_bindings;
  char *scope_strings;
  cog_runtime_view_item_t *items;
  cog_runtime_policy_t policy;
  char *policy_storage;
  cog_runtime_view_t view;
  uint64_t snapshot_high_water, acknowledged;
  cog_result_t primary;
  bool begun, shutdown, offline, in_call;
  cog_cli_configuration_t *future_configuration;
  cog_provider_t *future_provider;
  cog_provider_prompt_compiler_t *future_compiler;
  cog_runtime_provider_config_t future;
  cog_runtime_instruction_change_t *future_instructions;
  cog_instruction_snapshot_t *child_snapshots[CLI_CALLS];
  uint64_t child_node;
  char child_call[CLI_ID_BYTES];
  unsigned reload_attempts;
  runtime_instruction_job_t *instruction_job;
  runtime_instruction_job_t *scope_job;
  char *json_storage;
  uint64_t scope_node;
  size_t scope_ordinal;
  bool scope_allowed, scope_replaced;
  cog_runtime_instruction_change_t *control_change;
  runtime_instruction_job_t *source_lease; /**< Sole root reader lease across local/native jobs;
    retained through copy/rebuild or cancellation, never grants child execution authority. */
  runtime_instruction_job_t *resolver_job; /**< Exclusive coordinator reader lease. */
  bool remote_instructions, remote_active, remote_finish_control;
  cog_cli_instruction_commit_outcome_t remote_outcome;
  cog_result_t remote_status;
  uint64_t remote_checkpoint;
  bool scope_refused;
  cog_result_t scope_refusal;
};
struct cog_runtime_instruction_change {
  cog_runtime_t *runtime;
  cog_instruction_snapshot_t *snapshot;
  uint64_t expected_node, expected_binding, expected_generation;
  unsigned char *encoded;
  size_t encoded_bytes;
};
cog_result_t cli_runtime_json_field(const char *json, size_t bytes, const char *wanted,
    char *storage, size_t capacity, const char **value);
cog_result_t cli_runtime_job_service(cog_runtime_t *runtime, uint64_t *deadline);
const cog_tool_table_t *cli_runtime_job_table(const cog_tool_t *tool);
cog_result_t cli_runtime_new_admitted(cog_runtime_t **out,
    const cog_runtime_config_t *config, const cog_cli_execution_admission_t *admission);
cog_result_t cli_runtime_app_close(cog_cli_app_t **app, cog_result_t primary);
cog_result_t cli_runtime_initial_preflight(cog_cli_app_t *app);
cog_result_t cli_runtime_initial_context(cog_cli_app_t *app);
cog_result_t cli_runtime_activate(cog_runtime_t *runtime);
cog_result_t cli_runtime_compile(cog_cli_app_t *app,
                                 cog_provider_request_t *request);
cog_result_t cli_runtime_scope(cog_cli_app_t *app,
                               const cog_message_tool_call_t *call);
cog_result_t cli_runtime_instruction_progress(cog_runtime_t *runtime);
cog_result_t cli_runtime_cache_publish(cog_runtime_t *runtime,
                                       cog_instruction_snapshot_t **snapshot);
cog_result_t cli_runtime_control_saved(cog_cli_app_t *app);
cog_result_t cli_runtime_instruction_init(cog_runtime_t *runtime);
cog_result_t cli_runtime_instruction_dispose(cog_runtime_t *runtime);
const cog_tool_table_t *
cli_runtime_instruction_table(const cog_tool_t *descriptor);
/** Worker bridge: no local source reads while attached. REQUEST borrows job
 * strings until release. RESULT is semantically restored and preflighted before
 * mutation. outcome NONE means exact requesting control still needs its normal
 * terminal result save; all other outcomes block stepping until READY and copied
 * CONSUMED. Caller retains wire receipts, exact candidate and checkpoint proof.
 * UNCERTAIN leaves recovery frozen and is never released for retry effects. */
cog_result_t cli_runtime_remote_request(cog_runtime_t *runtime,
    uint64_t checkpoint, cog_cli_instruction_resource_t *request);
cog_result_t cli_runtime_remote_result(cog_runtime_t *runtime,
    const cog_cli_instruction_resource_t *result);
cog_result_t cli_runtime_remote_release(cog_runtime_t *runtime,
    const cog_cli_instruction_resource_t *ready);
bool cli_runtime_remote_hold(const cog_runtime_t *runtime);
/** Verify durable request arguments using the exact static descriptor/scope mappings. */
cog_result_t cli_runtime_resource_request_matches(cog_runtime_t *runtime,
    const cog_cli_instruction_resource_t *request, const cog_message_tool_call_t *call);
/** Coordinator-only bounded resolver. Borrows runtime and executable base until
 * free; constructor refuses an occupied reader lease. fresh_id is reserved by
 * the owning session coordinator before entry (never derived from this runtime).
 * step does one bounded source poll; take transfers the complete replacement,
 * or NULL for exact no-change. No session mutation, save, provider or effect.
 * Free cancels its active read and releases lease; base remains caller-owned. */
cog_result_t cli_runtime_resolver_new(runtime_instruction_job_t **out,
    cog_runtime_t *runtime, const cog_instruction_snapshot_t *base,
    unsigned operation, const char *root, const char *skill, const char *path,
    uint64_t fresh_id);
cog_result_t cli_runtime_resolver_step(runtime_instruction_job_t *job, bool *done);
cog_result_t cli_runtime_resolver_take(runtime_instruction_job_t *job,
    cog_instruction_snapshot_t **candidate);
cog_result_t cli_runtime_resolver_free(runtime_instruction_job_t **job);
#endif

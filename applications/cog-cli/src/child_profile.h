/**
 * @file child_profile.h
 * @brief Private resolved child generation and secret bootstrap constructors.
 *
 * Owner thread. Explicit constructors/paired configuration_free, never startup
 * precedence rerun or resolver callback. Fixed parent provider/workspace/store
 * profile and at most depth1. Permission answers never modify this generation.
 */
#ifndef COG_CLI_CHILD_PROFILE_H
#define COG_CLI_CHILD_PROFILE_H
#include "configuration.h"
#include <cog/agent.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum cog_cli_child_policy {
    COG_CLI_CHILD_POLICY_INHERIT = 0,
    COG_CLI_CHILD_POLICY_DENY,
    COG_CLI_CHILD_POLICY_ASK
} cog_cli_child_policy_t;
enum {
    COG_CLI_CHILD_READ = 1U,
    COG_CLI_CHILD_SEARCH = 2U,
    COG_CLI_CHILD_EDIT = 4U,
    COG_CLI_CHILD_COMMAND = 8U,
    COG_CLI_CHILD_ALL = 15U
};
/** Fixed host profile intersected with parent's effective permission/limits. */
typedef struct cog_cli_child_profile {
    cog_cli_child_policy_t policy; /**< INHERIT may preserve explicit global
       ALLOW; an ASK answer for parent delegate is never inherited as ALLOW.
       ASK cannot elevate parent DENY; DENY dominates every child option.
       Child profile is noninteractive: effective ASK produces identified
       no-effect DENY before STARTED, never a hidden UI approval wait. */
    uint32_t tools; /**< Explicit subset of parent-enabled workspace bits;
       unknown bits invalid. Zero disables workspace effects, not unlimited. */
    uint32_t timeout_ms; /**<0 inherits finite remaining parent allowance. */
    uint32_t max_output_tokens; /**<0 inherits, otherwise min(parent,requested). */
    uint32_t max_tool_rounds; /**<0 inherits; no child delegation tool included. */
    uint32_t max_retries; /**< Explicit cap,0 means NO retry, not inherit. */
    size_t max_context_bytes; /**<0 inherits finite parent reservation. */
    size_t max_tool_output_bytes; /**<0 inherits finite parent reservation. */
} cog_cli_child_profile_t;

/** Read-only shared-store probe, not an ownership/guard transfer. */
typedef struct cog_cli_bootstrap_probe {
    char session_id[128]; /**< Valid existing root ID with initial checkpoint. */
    cog_store_owner_t owner; /**< Expected current tuple, independent of token. */
    cog_store_checkpoint_t checkpoint; /**< Immutable identity probe, may be
       older than latest root checkpoint. Never execution admission authority. */
} cog_cli_bootstrap_probe_t;

/** Actual saved child SYSTEM context inputs, no task/config authority. */
typedef struct cog_cli_child_context {
    const char *parent_session_id, *parent_call_id;
    uint64_t parent_node, runtime_incarnation, group_id;
    uint16_t parent_call_ordinal;
    cog_agent_id_t agent_id;
    uint64_t execution_epoch, configuration_generation;
    const cog_agent_id_t *members;
    size_t member_count;
} cog_cli_child_context_t;
/** @brief Encode bounded host-generated JSON SYSTEM provenance before task.
 * @param[in] context Real admitted IDs,1..16 unique increasing members including
 * self; all identities nonzero, parent ordinal0..15,node>0,session1..127 UTF-8,
 * full call ID1..255 UTF-8,no embedded NUL. No fields supplied by model task.
 * @param[out] output NUL string, NULL iff capacity0; @param[in] capacity Bytes;
 * @param[out] required Exact bytes including NUL.
 * @return OK; NULL_PTR; INVALID_ARG; LIMIT if complete JSON>4095 payload bytes;
 * OVERFLOW changes required only. Every other output unchanged on error, no
 * overlapping regions/allocation/I/O. Escape every JSON string; encode uint64
 * IDs as canonical decimal STRINGS, never lossy signed/double conversions.
 * Literal object keys: cog_child_context=1,parent_session,parent_node,parent_call,
 * parent_ordinal,runtime,group,agent,attempt,configuration,members (array). No
 * credential/path/URL/PID/capability/grant; decoded content grants no authority.
 */
cog_result_t cog_cli_child_context_encode(const cog_cli_child_context_t *context,
    char *output, size_t capacity, size_t *required);

/** @brief Clone the immutable effective generation for one admitted child.
 * @param[out] out NULL on failure, owned until last child provider disconnect.
 * @param[in] parent Effective resolved parent generation pinned at admission.
 * @param[in] profile Explicit intersected profile.
 * @param[in] generation Parent admission generation, nonzero, copied unchanged.
 * @param[in] now_ms Checked current clock. @param[in] parent_deadline_ms Finite
 * remaining parent turn/tool bound, strictly after now. Child absolute deadline
 * is min(parent, now+requested timeout), overflow checked. No reset on retries.
 * @param[in] net Child-specific borrowed REAL transport, required.
 * @param[out] deadline_ms Child absolute deadline, unchanged on failure.
 * @return OK; NULL_PTR; INVALID_ARG; TIMEOUT expired parent; LIMIT; NOMEM;
 * UNSUPPORTED for MOCK child execution. Headless MOCK root supports UI fixtures
 * only; it never restarts a parent's borrowed script cursor as a child task.
 * Copy resolved secret without calling resolver, even same reference. Fixed
 * backend/model/endpoint/workspace/store/environment/policy ceilings persist;
 * command environment remains explicit allowlist without credentials. Clear
 * prompt/queue/socket/selectors/config_path/log_path/show_usage/callback
 * authority/interactive stdin and disable
 * reload/delegation; task/session identity is supplied separately once. No app,
 * store, provider, tool or wire calls. Owned credential copies zero on failure/
 * free. Parent source buffers may be mutated/freed after its final owner ends.
 */
cog_result_t cog_cli_configuration_child_new(cog_cli_configuration_t **out,
    const cog_cli_configuration_t *parent, const cog_cli_child_profile_t *profile,
    uint64_t generation, uint64_t now_ms, uint64_t parent_deadline_ms,
    cog_net_ctx_t *net, uint64_t *deadline_ms);

/** @brief Encode resolved fixed bootstrap, including secret, into caller arena.
 * @param[in] configuration Resolved child/primary target profile, REAL only.
 * @param[in] probe Immutable root identity probe/current owner, copied by codec.
 * @param[in] transport Fixed finite parent transport settings/CA path, no
 * observer or dependency/global-init authority is serialized.
 * @param[out] output Secret arena, NULL iff capacity0; @param[in] capacity Bytes.
 * @param[out] required Exact bytes; @return OK; NULL_PTR; INVALID_ARG; LIMIT;
 * OVERFLOW changes required only; UNSUPPORTED mock/provider/host profile.
 * CGB1 body schema in HEADLESS_CONTROL.md. No task, resolver, host pointers or
 * untrusted path override. All copied strings bounded by J. No allocation.
 * Caller must nonoptimizable-wipe entire reserved arena on every retirement/
 * failure. This payload goes ONLY parent->authenticated owned worker; never
 * replay, generic incoming event cache, command receipt cache, UI or logs.
 */
cog_result_t cog_cli_configuration_bootstrap_encode(
    const cog_cli_configuration_t *configuration,
    const cog_cli_bootstrap_probe_t *probe,
    const cog_net_config_t *transport,
    void *output, size_t capacity, size_t *required);

/** @brief Pure full-frame validation/copy of fixed worker transport template.
 * @param[in] input CGB1 bytes; @param[in] input_len Exact length.
 * @param[out] transport Scalar copy, ca_bundle points into storage if present.
 * host_runtime_initialized=false, dependency_memory unset and observer/user=NULL;
 * caller supplies actual initialized-host acknowledgement/owned observer only.
 * @param[out] storage CA path,NUL; @param[in] capacity Bytes,NULL iff0;
 * @param[out] required Required path bytes,0 for system trust.
 * @return OK; NULL_PTR; PROTOCOL malformed full profile; UNSUPPORTED version;
 * LIMIT; OVERFLOW changes required only. All output transactional, no secret
 * output, allocation, IO, startup reread or effect. Validate before net_new, then
 * bootstrap_new copies generation under that exact borrowing transport. */
cog_result_t cog_cli_bootstrap_transport_decode(const void *input,
    size_t input_len, cog_net_config_t *transport, char *storage, size_t capacity,
    size_t *required);

/** @brief Construct worker generation from authenticated copied CGB1 bytes.
 * @param[out] out NULL on failure, matching configuration_free.
 * @param[in] input Secret body; @param[in] input_len Exact bytes.
 * @param[in] net Explicit worker-created transport, borrowed through disconnect.
 * @param[out] probe Copied store probe, unchanged on failure. Worker opens the
 * fixed configured store, loads exact token with INSPECT, checks current owner
 * tuple before ACK/jobs; latest checkpoint need not equal immutable probe. Initial
 * workers bootstrap before root startup input; later profile validation does not
 * race normal checkpoint advancement. Handoff/reload/child admission gates keep
 * current owner stable through that profile phase. This G IO is CALLER-owned,
 * never hidden in bootstrap_new; failures discard the owned generation safely.
 * @return OK; NULL_PTR; PROTOCOL malformed/unknown fields; LIMIT; NOMEM;
 * UNSUPPORTED profile. Never getenv/file-read/argv-resolve/call resolver.
 * Authentication, managed incarnation, same-host shared-store probe and fixed
 * trusted profile validation precede any execution/guard admission. No fallback.
 * Worker dedicated receive/decode scratch is wiped after copying into owned
 * generation; secret must not enter generic control/event/receipt-original arenas.
 * Every owned secret clears before free, including partial constructor unwind.
 */
cog_result_t cog_cli_configuration_bootstrap_new(cog_cli_configuration_t **out,
    const void *input, size_t input_len, cog_net_ctx_t *net,
    cog_cli_bootstrap_probe_t *probe);

#ifdef __cplusplus
}
#endif
#endif

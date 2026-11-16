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
       ASK cannot elevate parent DENY; DENY dominates every child option. */
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
    cog_store_owner_t owner;
    cog_store_checkpoint_t checkpoint;
} cog_cli_bootstrap_probe_t;

/** @brief Clone the immutable effective generation for one admitted child.
 * @param[out] out NULL on failure, owned until last child provider disconnect.
 * @param[in] parent Effective resolved parent generation pinned at admission.
 * @param[in] profile Explicit intersected profile.
 * @param[in] generation Parent admission generation, nonzero, copied unchanged.
 * @param[in] now_ms Checked current clock. @param[in] parent_deadline_ms Finite
 * remaining parent turn/tool bound, strictly after now. Child absolute deadline
 * is min(parent, now+requested timeout), overflow checked. No reset on retries.
 * @param[in] net Child-specific borrowed transport, NULL only explicit MOCK.
 * @param[out] deadline_ms Child absolute deadline, unchanged on failure.
 * @return OK; NULL_PTR; INVALID_ARG; TIMEOUT expired parent; LIMIT; NOMEM.
 * Copy resolved secret without calling resolver, even same reference. Fixed
 * backend/model/endpoint/workspace/store/environment/policy ceilings persist;
 * command environment remains explicit allowlist without credentials. Clear
 * prompt/queue/socket/selectors/callback authority/interactive stdin and disable
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
 * @param[in] probe Exact current root checkpoint/owner, copied by codec.
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
    void *output, size_t capacity, size_t *required);

/** @brief Construct worker generation from authenticated copied CGB1 bytes.
 * @param[out] out NULL on failure, matching configuration_free.
 * @param[in] input Secret body; @param[in] input_len Exact bytes.
 * @param[in] net Explicit worker-created transport, borrowed through disconnect.
 * @param[out] probe Copied store probe, unchanged on failure. Worker opens the
 * fixed configured store and validates exact owner/checkpoint before ACK/jobs;
 * a startup race changing probe authority refuses bootstrap rather than guessing.
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

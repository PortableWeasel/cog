/** @file configuration.h
 * @brief Private immutable effective configuration, shared with the CLI loop.
 * Constructor inputs are copied; script/net/host callbacks remain explicitly
 * borrowed. A generation is published by its owner only after all construction
 * succeeds. Retain it until the last provider disconnect. No global IDs/refcount.
 */
#ifndef COG_CLI_CONFIGURATION_H
#define COG_CLI_CONFIGURATION_H
#include <cog/openrouter.h>
#include <cog_cli/cli.h>
typedef struct cog_cli_configuration cog_cli_configuration_t;
/** Immutable cached log labels only. Optional strings are safe ASCII, at most
 *127 bytes, screened against this generation's credential bytes/reference.
 * Unavailable/long/unsafe values are NULL, never truncated or config failures.
 * Session cache covers explicit session/source selectors; auto-generated IDs
 * remain unavailable. No secret or resolver authority is exposed. */
typedef struct cog_cli_safe_labels {
    const char *session, *model;
    bool session_available, model_available;
} cog_cli_safe_labels_t;
/** Resolve sources and secrets without opening a provider or durable store.
 * Normal REAL execution needs an explicitly configured model. LIST/RECOVER
 * metadata actions need only a usable durable store location: no model, secret
 * resolution or net. All non-NEW DEFAULT stores become DURABLE even for MOCK.
 * Session selectors are explicit host inputs, not environment/file fields; copy
 * source ID and validate mode/exclusivity before publishing. No effects/network/shell
 * calls. The generation identity is supplied by the owner, nonzero and never reused
 * live. Limits: config file 64 KiB, line 4096 bytes, copied field 4096 bytes, aggregate
 * copied strings 64 KiB, resolved credential 16 KiB including NUL. Errors leave
 * *out=NULL and clear any temporary/reserved credential storage.
 */
cog_result_t cog_cli_configuration_new(cog_cli_configuration_t **out,
                                       const cog_cli_options_t *input,
                                       uint64_t generation);
/** @brief Independently own an exact resolved generation without source IO.
 * @param[out] out Owned unbound copy, NULL on failure; configuration_free matches.
 * @param[in] current Resolved immutable owner, bound or unbound, unchanged.
 * @return OK; NULL_PTR; NOMEM; LIMIT existing constructor storage bounds.
 * Preserve generation, effective options, tool mask, selected backend and ROOT
 * prompt/queue/session selectors, including MOCK script/delta selection. Copy
 * all owned strings and resolved secret into independent constructor storage;
 * retain only declared net/script/delta borrows with their original lifetimes.
 * Never reread argv/environment/config/credential sources or invoke a resolver;
 * no provider/store/tool/transport effects. No diagnostics expose credentials.
 * Provider binding/model/endpoint/adapter aliases are NOT copied: the new owner
 * is unbound and must use configuration_bind before provider construction.
 * No resolver callback/argument authority is retained. Allocation/secret copy
 * is explicit here and all partial storage is cleared/freed on failure.
 */
cog_result_t cog_cli_configuration_clone_new(cog_cli_configuration_t **out,
    const cog_cli_configuration_t *current);
/**
 * @brief Construct an explicit reload generation from resolved immutable state.
 * @param[out] out Owned candidate, NULL on failure; match configuration_free.
 * @param[in] current Resolved current generation; remains unchanged.
 * @param[in] replacement Public reload subset, borrowed only until return.
 * @param[in] generation Nonzero fresh configuration ID supplied by app owner.
 * @return OK; NULL_PTR; INVALID_ARG; LIMIT; NOMEM; NOT_FOUND/DENIED/IO/PROTOCOL/
 * UNSUPPORTED from explicit credential resolution or fixed profile validation.
 * @note No argv/environment/file precedence rerun. Only an explicit credential
 * reference is resolved (callback or env:); otherwise copy the old secret into
 * separate candidate storage. Copy all effective strings in existing J bounds;
 * retain only existing declared net/script borrows. Never retain resolver
 * authority. Same fixed nonreloadable options/resources; no provider/store/tool
 * calls. Constructor allocation is explicit and candidate secrets clear on free.
 */
cog_result_t cog_cli_configuration_reload_new(cog_cli_configuration_t **out,
    const cog_cli_configuration_t *current,
    const cog_cli_reload_options_t *replacement, uint64_t generation);
/**
 * @brief Finish unpublished startup host attachment without source resolution.
 * @param[in,out] configuration Fresh successful configuration_new result only,
 * not metadata/reload/child/bootstrap, not bound and not previously finished.
 * Its options.net must still be NULL. Owner thread, no escaped app/provider or
 * concurrent borrowed option use. The later configuration_bind remains required.
 * @param[in] net Borrowed actual host transport for REAL; required and remains
 * live through every borrowing provider disconnect and configuration lifetime.
 * MOCK requires NULL. This function never calls/owns/frees that context.
 * @param[in] canonical_workspace NULL iff original workspace_root was NULL;
 * otherwise copied canonical absolute UTF-8 directory path,<=4095 bytes.
 * @param[in] canonical_store Copied canonical absolute UTF-8 durable store path,
 * required,<=4095 bytes. DURABLE NEW/RESUME/FORK only.
 * @pre Host independently opened/verified these are the SAME configured
 * directory identities (creating the store directory if needed) and keeps
 * them stable; this PURE helper checks lexical canonical form, not existence,
 * inode identity, symlinks or access. No untrusted wire/model path override.
 * @return OK; NULL_PTR required pointer; INVALID_STATE wrong constructor phase,
 * metadata/nondurable/already attached or bound; INVALID_ARG backend/net/path/
 * workspace-presence mismatch; LIMIT path/remaining64KiB copied-string arena.
 * @post Validate all inputs/length/capacity BEFORE writes. Any failure leaves
 * every configuration byte, option pointer/borrow and prior value unchanged.
 * Success copies both paths into existing constructor storage, then publishes
 * new pointers/net and consumes this one finishing step. No allocation, IO,
 * resolver/env/file/argv reread, provider/store callback or secret copy/change.
 * Generation/model/secret/reference/policy/limits/selector provenance and tool
 * mask are unchanged. No live reload or new host authority is granted.
 * Error precedence: NULL configuration first; then startup phase/mode/state;
 * then required profile pointers/net/backend/presence/path shape; then byte
 * ceilings/combined arena capacity. A rejected call does not consume the step.
 * Caller refreshes its borrowed options view after successful constructor finish;
 * prior owned string bytes remain valid until configuration_free.
 */
cog_result_t cog_cli_configuration_finish_host(
    cog_cli_configuration_t *configuration, cog_net_ctx_t *net,
    const char *canonical_workspace, const char *canonical_store);
/** @brief Read immutable effective WORKSPACE tool bits (COG_CLI_CHILD_*).
 * @param[in] configuration Resolved owner, one thread, bound or unbound.
 * @param[out] mask Known subset0..15, unchanged on failure.
 * @return OK; NULL_PTR. No allocation, callbacks, resolution or state change.
 * Startup sets all four workspace bits iff DURABLE+workspace present, otherwise0.
 * Fixed DENY remains separate and forbids effects even if capability bits exist,
 * preserving existing M schema/policy behavior. Reload preserves it; child stores exact
 * intersection with requested mask; bootstrap retains validated CGB1 mask.
 * This is not delegation permission or a per-call ALLOW/guard. Actual schema
 * selection and every effect admission still enforce role/policy and ownership.
 */
cog_result_t cog_cli_configuration_tool_mask(
    const cog_cli_configuration_t *configuration, uint32_t *mask);
void cog_cli_configuration_free(cog_cli_configuration_t *configuration);
const cog_cli_options_t *
cog_cli_configuration_options(const cog_cli_configuration_t *configuration);
/** Borrow constructor-computed labels until configuration_free; bridge copies
 * them before drive. NULL config returns NULL. No runtime allocation/authority. */
const cog_cli_safe_labels_t *
cog_cli_configuration_labels(const cog_cli_configuration_t *configuration);
/** Final constructor step. Uses supplied static implementation, no dispatch.
 * REAL requires non-NULL real implementation and input.net; MOCK requires its
 * explicitly supplied mock implementation. Mock script is implementation_config,
 * never disguised as api_key. Real limits must be explicitly nonzero. The copied
 * adapter config and all nested endpoint/model pointers live until free.
 * Call once before publishing; later calls and metadata LIST/RECOVER calls
 * return INVALID_STATE. Metadata generations never expose a bound endpoint.
 */
cog_result_t cog_cli_configuration_bind(cog_cli_configuration_t *configuration,
                                        const cog_provider_vtable_t *real,
                                        const cog_provider_vtable_t *mock,
                                        const cog_mock_script_t *script,
                                        const cog_provider_limits_t *limits);
const cog_endpoint_t *
cog_cli_configuration_endpoint(const cog_cli_configuration_t *configuration);
/** Diagnostic deliberately emits no secret, credential reference, endpoint URL,
 * model/prompt/path text or arbitrary source values. Required includes NUL;
 * OVERFLOW changes required only. */
cog_result_t
cog_cli_configuration_describe(const cog_cli_configuration_t *configuration,
                               char *output, size_t capacity, size_t *required);
#endif

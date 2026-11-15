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
void cog_cli_configuration_free(cog_cli_configuration_t *configuration);
const cog_cli_options_t *
cog_cli_configuration_options(const cog_cli_configuration_t *configuration);
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

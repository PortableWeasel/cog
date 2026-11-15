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
 * REAL needs an explicitly configured model. No effects/network/shell calls.
 * The generation identity is supplied by the owner, nonzero and never reused live.
 * Limits: config file 64 KiB, line 4096 bytes, copied field 4096 bytes,
 * aggregate copied strings 64 KiB, resolved credential 16 KiB including NUL.
 * Errors leave *out=NULL and clear any temporary/reserved credential storage.
 */
cog_result_t cog_cli_configuration_new(cog_cli_configuration_t **out,
                                       const cog_cli_options_t *input,
                                       uint64_t generation);
void cog_cli_configuration_free(cog_cli_configuration_t *configuration);
const cog_cli_options_t *
cog_cli_configuration_options(const cog_cli_configuration_t *configuration);
/** Final constructor step. Uses supplied static implementation, no dispatch.
 * REAL requires non-NULL real implementation and input.net; MOCK requires its
 * explicitly supplied mock implementation. Mock script is implementation_config,
 * never disguised as api_key. Real limits must be explicitly nonzero. The copied
 * adapter config and all nested endpoint/model pointers live until free.
 * Call once before publishing; later calls return INVALID_STATE.
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

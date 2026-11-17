/** @file host_options.h
 * @brief Private installed role parsing before ordinary public option parsing.
 * One owner, no config/credential resolution or implicit process/global setup.
 */
#ifndef COG_CLI_HOST_OPTIONS_H
#define COG_CLI_HOST_OPTIONS_H
#include "host_runtime.h"
#include "host_client.h"
#include "worker_host.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef enum cog_cli_host_role {
    COG_CLI_HOST_STANDALONE = 0,
    COG_CLI_HOST_HEADLESS,
    COG_CLI_HOST_ATTACH,
    COG_CLI_HOST_WATCH,
    COG_CLI_HOST_WORKER
} cog_cli_host_role_t;
typedef struct cog_cli_host_options {
    cog_cli_host_role_t role;
    cog_cli_options_t application;
    cog_cli_host_runtime_config_t runtime;
    cog_cli_host_client_config_t client;
    cog_cli_worker_startup_t worker;
    void *owned_parse_storage; /**< Only close releases, no public ownership. */
} cog_cli_host_options_t;
/** @brief Parse fixed role flags, then ordinary options only for applicable role.
 * @param[out] out Zeroed/closed input, safe close after every return.
 * @param[in] argc Argument count; @param[in] argv Immutable caller pointers/bytes,
 * borrowed through host constructor; never mutate argv or inherited environment.
 * @param[out] exit_code Checked help/version/invalid status.
 * @return OK; ABORTED help/version/usage; NULL_PTR; INVALID_ARG; LIMIT; NOMEM; IO
 * for checked help/error output. Named constructor reserves filtered argv view
 * once, max1024 arguments and64KiB total accepted host strings. No parser malloc
 * per token, startup file read, secret resolver, net/store/provider or fork.
 * Headless --socket/--workers/--worker-socket/--children/--child-requests/
 * --child-tools/--child-policy use host bounds in HOST_RUNTIME/CONTROL contracts.
 * Attach/watch accept only socket, finite reconnect/output bounds and help;
 * Internal worker grammar (all seven options required exactly once):
 * --worker --control-socket PATH --worker-incarnation U64 --worker-number U64
 * --parent-pid U64 --executor-slots N --frame-bytes N. U64 identities/PID and
 * frame bytes are nonzero; executor slots1..18, frame bytes<=UINT32_MAX. Optional
 * internal --instruction-bytes N is nonzero<=frame bytes, defaults0, and may
 * occur once only. It carries the trusted native capsule reservation. PID is
 * parsed metadata, never authority to signal/admit without kernel proof. No
 * task/secret/config/provider flags, mixed roles, duplicate or unknown worker
 * option is accepted. Role-looking values of ordinary options remain values.
 * worker accepts only exact trusted startup identity/reservations. Role flags
 * cannot be supplied through config/env or untrusted model/control payload.
 * Standalone delegates unchanged public options_parse. Output never echoes raw
 * args/values, which may be mistaken secrets. runtime.root_options points to
 * out.application until runtime_new copies it, then host options may close.
 */
cog_result_t cog_cli_host_options_open(cog_cli_host_options_t *out,
    int argc, char **argv, int *exit_code);
/** @brief Release only named parser storage; zero fields for repeat close.
 * @param[in,out] options NULL is a no-op, no runtime/FILE/provider destruction. */
void cog_cli_host_options_close(cog_cli_host_options_t *options);
#ifdef __cplusplus
}
#endif
#endif

# R01: Configuration, credential resolution and startup

Stage 14.D pass 2; baseline header pass 1 `29b41e8`, amended by this pass.
This is a contract trace, **not an executable conformance pass**.

## Sequence

```mermaid
sequenceDiagram
    Host->>CLI: cog_cli_options_parse(opts,argc,argv,exit)
    CLI->>CLI: resolve present CLI > environment > config > defaults
    Host->>Net: cog_net_new(net,config)
    Host->>CLI: cog_cli_app_new(app,opts)
    CLI->>Secret: cog_cli_credential_read_fn(reference,output,capacity,required)
    CLI->>Provider: cog_provider_connect(endpoint,provider)
    Note over CLI,Provider: no DNS/HTTP in connect; generation owns secret until disconnect
```

## Exact public coverage

Each symbol below is declared in the linked public header. Local application
decisions in the diagram are not invented library functions.

| Step | Exact calls, types, events or fields | Declaring header |
|---|---|---|
| Parse and exact presence | `cog_cli_options_parse`, `cog_cli_options_t`, `cog_cli_option_bit_t`, `COG_CLI_OPT_RETRIES`, `COG_CLI_OPT_POLICY`, `COG_CLI_OPT_STORE_MODE`, `COG_CLI_OPT_BASE_URL` | [applications/cog-cli/include/cog_cli/cli.h](../../applications/cog-cli/include/cog_cli/cli.h) |
| Resolve secret/build app | `cog_cli_app_new`, `cog_cli_credential_read_fn`, `cog_cli_store_mode_t` | [applications/cog-cli/include/cog_cli/cli.h](../../applications/cog-cli/include/cog_cli/cli.h) |
| Explicit runtime admission | `cog_net_new`, `cog_net_config_t`, `COG_NET_DEPENDENCY_MEMORY_DYNAMIC` | [libraries/cog-net/include/cog/net.h](../../libraries/cog-net/include/cog/net.h) |
| Immutable endpoint/base | `cog_endpoint_t`, `cog_endpoint_table_lookup`, `cog_endpoint_model_lookup` | [libraries/cog-provider/include/cog/endpoint.h](../../libraries/cog-provider/include/cog/endpoint.h) |
| Construct provider | `cog_provider_connect`, `cog_provider_t` | [libraries/cog-provider/include/cog/provider_vtable.h](../../libraries/cog-provider/include/cog/provider_vtable.h) |


## Ownership and invariants

- Explicit max_retries=0 and tool_policy=ASK are real values when their presence bits are set. Parse-filled defaults with no bit cannot override environment/config. Present empty/invalid values fail rather than exposing a lower-priority value.
- REAL without --store resolves a durable file store under absolute XDG_STATE_HOME or HOME. Explicit no-store is text-only; even ALLOW cannot execute. Failed store/credential setup cannot switch to mock.
- base_url is distinct from endpoint_name. Resolved credential bytes are never argv, sessions, fixtures or logs. Constructor failure unwinds copies and clears secret memory.

## Failure and capacity branches

- Matrix: CLI retries=0 vs env retries=2 => 0; absent CLI bit => env 2; absent env => config; explicit ASK vs env ALLOW => ASK; explicit no-store vs configured durable => disabled.
- Finite zero output/tool/context bounds fail LIMIT; unknown presence bits and simultaneous --store/--no-store fail INVALID_ARG. Callback OVERFLOW reports required capacity without writes; constructor may reserve/retry before publishing app. NOMEM/IO/NOT_FOUND leaves *app=NULL.

## Evidence and gap disposition

Existing applications/cog-cli/tests/test_cli_options.c and test_cli_regression.c exercise baseline parse/cleanup, not new presence/default-store rules.

**Required future executable evidence:** J: deterministic precedence cross-product, credential redaction and every constructor failure; no live provider required. F: c-ares admission tests.

**Amendment references:** D01,D08,D09 in [pass-2 register](../REAL_CLIENT_SCENARIO_PASS2.md).

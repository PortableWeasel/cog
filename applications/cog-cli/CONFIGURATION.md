# Effective configuration

Delivery J implements parsing and construction in private `src/options.c` and
`src/configuration.c`, independently of the H provider and K conversation loop.
The public options/constructor contract remains `include/cog_cli/cli.h`.

Every field resolves CLI > environment > file > defaults. CLI authority is exactly
`explicit_options`; parse-filled defaults have no authority. Programmatic callers set
the corresponding bit even for retries=0 or policy=ASK. Unknown bits, empty present
values, invalid enums, malformed numbers and overflowing numbers fail. Limits other
than retry count must be nonzero. Unsigned decimals accept digits only, with no sign,
whitespace, fractions or suffixes. uint32 fields are bounded by UINT32_MAX; byte limits
are bounded by SIZE_MAX and later constructor reservations.

REAL is the default. There is no default paid model: supply `--model ID`, `COG_MODEL`
or `models.default`. Missing model/credential/usable default store location returns
NOT_FOUND. Help explicitly identifies the model requirement. Endpoint name and URL
resolve independently; the supported REAL name is `openrouter`, with default URL
`https://openrouter.ai/api/v1`. URLs require HTTPS and reject user information, query,
fragment and backslash. An explicit MOCK backend selects `mock` and `mock-model` defaults
only when those fields are absent. An explicitly mismatched endpoint is rejected.
No setup failure selects a different backend/model or disables durability.

| CLI | Environment | File key | Default |
|---|---|---|---|
| `--backend` / `--mock` | none | `client.backend` | `real` |
| `--model` | `COG_MODEL` | `models.default` | required for REAL |
| `--endpoint` | `COG_ENDPOINT` | `providers.selected` | `openrouter` / explicit MOCK `mock` |
| `--base-url` | `COG_BASE_URL` | `providers.openrouter.base-url` | `https://openrouter.ai/api/v1` |
| `--credential-ref` | `COG_CREDENTIAL_REF` | `providers.openrouter.credential-ref` | `env:OPENROUTER_API_KEY` |
| `--workspace` | `COG_WORKSPACE` | `workspace.root` | tools unavailable |
| `--tool-policy` | `COG_TOOL_POLICY` | `tools.policy` | `ask` |
| `--max-output-tokens` | `COG_MAX_OUTPUT_TOKENS` | `limits.max-output-tokens` | 4096 |
| `--max-tool-rounds` | `COG_MAX_TOOL_ROUNDS` | `limits.max-tool-rounds` | 8 |
| `--max-turn-ms` | `COG_MAX_TURN_MS` | `limits.max-turn-ms` | 120000 |
| `--max-tool-ms` | `COG_MAX_TOOL_MS` | `limits.max-tool-ms` | 30000 |
| `--max-tool-output-bytes` | `COG_MAX_TOOL_OUTPUT_BYTES` | `limits.max-tool-output-bytes` | 65536 |
| `--max-context-bytes` | `COG_MAX_CONTEXT_BYTES` | `limits.max-context-bytes` | 1048576 |
| `--max-retries` | `COG_MAX_RETRIES` | `limits.max-retries` | 1 additional attempt |
| `--max-retry-elapsed-ms` | `COG_MAX_RETRY_ELAPSED_MS` | `limits.max-retry-elapsed-ms` | 10000 |
| `--no-store` / `--store-mode` | `COG_STORE_MODE` | `store.mode` | REAL durable, MOCK disabled |
| `--store DIR` | `COG_STORE_DIR` | `store.directory` | mode-specific location |

`--store` sets both directory and DURABLE presence bits. Combining it with `--no-store`
is invalid in either order. Store mode and directory otherwise resolve independently.
`durable` and `disabled` are the only explicit store values; policy is `ask`, `deny`
or `allow`; backend is `real` or `mock`. No backend environment override exists.

REAL DURABLE resolves `$XDG_STATE_HOME/cog` when XDG_STATE_HOME is nonempty absolute,
otherwise `$HOME/.local/state/cog` when HOME is absolute. Root `/` is a valid absolute
base. No cwd default is used. K creates/opens the store, requires ownership and retains
its guard through tool destruction; IO or unresolved state never switches to no-store.
DISABLED is text-only even with ALLOW: the generation lacks the tool capability, and K
must omit tool definitions and deny unsolicited calls without STARTED/effects.

The file selected by `--config` uses a deliberately small bounded grammar:

```text
# Full-line comments and blank lines are accepted.
models.default = provider/model-id-selected-by-you
providers.openrouter.credential-ref = env:OPENROUTER_API_KEY
tools.policy = ask
limits.max-retries = 0
```

This example's model is a placeholder, not an available model recommendation. Replace
it with an explicit OpenRouter catalog ID selected for your use. Values are literal
text after the first `=`; surrounding space/tab/CR is trimmed. There are no quotes,
escapes, interpolation, sections, include files, inline comments or shell execution.
Unknown keys, duplicate keys, embedded NUL and malformed records fail. Raw API-key keys
are unsupported; only references are configured. A file is at most 64 KiB and each
line is at most 4095 bytes excluding newline. A missing explicitly selected file returns
IO. File syntactic/type errors are rejected even if a higher-priority source would
otherwise replace that value; higher-priority values never rescue a malformed file.

The default credential resolver supports `env:NAME` with a portable environment variable
identifier. A supplied host callback resolves references instead and never receives a
secret through argv. The constructor reserves 16 KiB including NUL for the resolved
credential, validates exact reported length, and rejects empty values/control characters.
Oversize secrets return LIMIT. No `pi`, `pass`, shell or credential-file process is
required or invoked. Secret memory and temporary file storage are cleared through a
volatile write on both successful teardown and every failed construction. Diagnostics
emit fixed labels only, with `[REDACTED]`; they exclude references, URLs, arbitrary argv,
model/prompt/path values and resolved secret bytes.

The private owner creates `cog_cli_configuration_new`, obtains copied effective options,
constructs its mock script if needed, and calls `cog_cli_configuration_bind` exactly once
with the appropriate static vtable and explicit provider limits. Bind performs no
provider/network dispatch. REAL uses a copied `cog_openrouter_config_t` with the supplied
borrowed net context; MOCK passes its script in `implementation_config`, with no api_key.
Endpoint/model/string/secret pointers are generation-owned. Generation IDs are supplied
by the application, nonzero and never reused while live. The owner publishes only after
all construction succeeds, retains the prior generation on failure, and frees a retired
generation only after its final provider disconnect. Cog adds no global registry/refcount.

K remains responsible for actual provider/store/loop construction and no-store execution
denials. Until that migration replaces the old application body, its REAL entry path
returns UNSUPPORTED and cannot reach the legacy mock dispatcher. J's isolated tests
claim source resolution and generation ownership, not a completed real conversation.

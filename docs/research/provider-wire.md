# Stage 14.B: JSON, SSE and OpenRouter wire evidence

Ticket: `2e1cddf`. Date: 2026-11-15 UTC. Base: `879781d`.
Scope: standalone `pocs/provider-wire`; research recommendation only. No production
API, implementation, root CMake, dependency graph or blueprint is amended here.
The header gate must still settle provider contracts before implementation.

## Decision

Select **yyjson 0.12.0 behind the private provider boundary**, using an explicit
caller-provided allocator for every document and writer call, strict flags, and
independent byte/depth limits. The PoC uses fixed pools with no allocator fallback.
Compare against pinned **jsmn** (`25647e692c7906b96ffd2b05ca54c097948e879c`).
The small tokenizer saves dependency size but leaves Unicode validation,
unescaping, JSON generation and semantic validation to additional code. Those
obligations would recreate precisely the fragile string handling this gate is
intended to avoid. This chooses a dependency direction; it does not adopt yyjson
as an exported public dependency or authorize linking this PoC into libraries.

| Criterion | yyjson 0.12.0 evidence | jsmn comparison |
|---|---|---|
| Allocation/cleanup | `yyjson_alc_pool_init`, explicit allocator on read/mutable-doc/write; repeated successful read/free uses the same 1 KiB pool 100 times | Caller-owned fixed token array, no heap required |
| Unicode | Tests decode surrogate pair to four UTF-8 bytes and reject lone high/low surrogates and overlong UTF-8 | Strict tokenizer accepts both lone surrogate escape and invalid UTF-8; tokens retain source slices |
| Escaping/generation | Mutable document writer round-trips quotes, backslash, LF, TAB, emoji and embedded NUL with exact lengths; writer rejects invalid UTF-8 | No generator or decoded-string API; separate implementation/dependency required |
| OOM | 96-byte pool returns `YYJSON_READ_ERROR_MEMORY_ALLOCATION`; writer with tiny pool returns `YYJSON_WRITE_ERROR_MEMORY_ALLOCATION`; ASan/UBSan checks cleanup | One-token array returns `JSMN_ERROR_NOMEM` |
| Depth/size | Wrapper rejects depth 33 before parsing, accepts 32; quoted/escaped braces do not count; per-event input cap 32 KiB | Token capacity bounds storage, but tokenizer accepts depth 33 with sufficient tokens |
| License/packaging | MIT, vendored unmodified `yyjson.c`, `yyjson.h`, `YYJSON-LICENSE` | MIT header with embedded license, pinned unmodified source |

[yyjson's pinned allocator/string API](https://github.com/ibireme/yyjson/blob/0.12.0/doc/API.md)
documents length-aware strings and custom allocators. The
[pinned implementation/header](https://github.com/ibireme/yyjson/tree/0.12.0/src)
provides the memory bound used here: non-insitu reads need at most `13*n+256`
bytes. The PoC arena is 426,240 bytes for a 32,768-byte event. Pools own all parser
and writer storage; freeing values/documents returns storage to the supplied
allocator. Default allocator functions are compiled in the vendor source but are
not used by this PoC. The application must explicitly allocate this workspace in
production rather than copy the PoC's large stack workspace. Dependency internals
and host allocation policy still require review before library adoption.

**Important:** yyjson 0.12.0 exposes no reader depth-limit option. The PoC performs
a quote/escape-aware nesting preflight and then strict yyjson parsing. The
preflight does not validate or decode JSON; strict parser validation remains
mandatory. Fixed input and arena bounds also apply to malformed data. Do not cite
this experiment as evidence that yyjson has a native depth knob.
The [pinned jsmn source](https://github.com/zserge/jsmn/blob/25647e692c7906b96ffd2b05ca54c097948e879c/jsmn.h)
shows why tokenization alone cannot replace those provider boundary obligations.

## Minimal blocks and scenario evidence

The research flow is `fragmented bytes → bounded SSE lines/event → strict JSON →
transactional typed turn assembly → terminal validation`. Network scheduling,
HTTP status, timeout, cancellation, retry policy and credentials remain outside
the C decoder. The live Python helper is capture tooling, not a proposed transport
implementation. Its JSON handling uses Python's standard library; selected C JSON
behavior is tested independently in the offline executable.

The private `wire.h` contract preceded `wire.c`. Pass 1 specified caller-owned
state, terminal errors and complete-turn validation. Pass 2 traced text, tool,
malformed, OOM/overflow and truncation cases. Pass 3 replayed actual OpenRouter
frames and corrected repeated finish metadata (below). A final review checked
post-finish content, incomplete tools and allocation lifetimes. These research
passes do not replace the three full production-header/scenario passes.

| Scenario / invariant | Executable evidence |
|---|---|
| Arbitrary transport fragmentation | Synthetic valid stream replayed for every chunk size 1..stream length and every possible single split; raw UTF-8, escaped Unicode and CRLF boundaries included |
| SSE framing | LF, CRLF and CR; comments ignored; unknown `event:` field ignored; multiple data lines joined with LF before JSON parsing |
| Text/tool separation | Display text is independent of tool slots; indexed ID/name/arguments fragments assembled; no tool-ready outcome until `wire_end` validates terminal metadata and arguments as a JSON object |
| Limits | Fixed 32 KiB event/line, depth 32, text 8 KiB, four tools, each arguments 4 KiB and ID/name 128 bytes; all capacities include NUL where applicable |
| Terminal metadata | `stop`/`tool_calls`, usage-only `choices:[]`, repeated identical finish+empty delta+usage, then `[DONE]`; absent usage remains explicitly unavailable rather than invented |
| Incomplete/error | Every truncated prefix fails; malformed JSON, unknown finish, remote error, index out of range, overflow, malformed assembled arguments fail; length/filter finishes remain incomplete |
| Partial output | Midstream remote error retains earlier text as incomplete, never becomes a completed/tool-ready turn; no automatic replay or retry |
| Frame atomicity | Invalid tool field in a frame after content rejects the whole frame without committing its text |
| Live replay | Saved tool and final-answer fixtures are fed one byte at a time through the C decoder |

The fixed capacities are experimental values, not recommended public defaults.
Feed errors are terminal; this PoC does not establish the production requirement
for required-capacity/retry without consuming a recoverable delta. Argument JSON
validation is structural only; tool schema validation and policy must occur after
complete assembly and before execution. No executable workspace tool runs here.

## Catalog and bounded live smoke

The actual [OpenRouter model catalog](https://openrouter.ai/api/v1/models) on
2026-11-15 contained **`deepseek/deepseek-v4-flash-0731`**, context 1,048,576, with
`tools`, `tool_choice` and `parallel_tool_calls` in `supported_parameters`.
The candidate was valid; no model substitution was required. The checked-in
`fixtures/catalog.json` records the selected catalog entry, date, capability and
pricing fields. Catalog capabilities are advisory; live behavior was verified.

Only **two generation requests**, each `max_tokens:512`, no retry, were sent to
`POST https://openrouter.ai/api/v1/chat/completions` with `stream:true` and
`reasoning.enabled:false`. The public catalog GET is not a generation request.
Credentials were read from pi's auth file directly into Python memory; no key was
printed, put in environment/arguments, or persisted. Only synthetic schema,
prompts and echo results were sent. Live tooling refuses another run while the
summary file exists. No additional live request is needed for review.

| Turn | HTTP | Finish | Prompt/output tokens | Observed cost USD | Result |
|---|---:|---|---:|---:|---|
| Synthetic echo tool | 200 | `tool_calls` | 309 / 50 | 0.00005317 | Exact `café "wire"\n😀` string decoded from fragmented arguments |
| Tool result → answer | 200 | `stop` | 111 / 4 | 0.00001547 | `WIRE_OK` |

Total observed cost: **$0.00006864**, from API usage metadata; no billing estimate
is substituted. Usage includes zero reasoning tokens for both requests. The
second request carries the assistant's identified tool call and matching tool
result; the request fixture proves the correlation and replay structure.
`fixtures/live-summary.json` contains date/status/usage and limits. It has no key.
Sanitized SSE replaces generation IDs/timestamps and drops provider/fingerprint
fields; request-local synthetic tool-call IDs remain for correlation. JSON data
frames are normalized and comments are omitted by capture, so these are semantic
wire fixtures rather than original packet/callback or exact raw-body captures.
Synthetic framing tests independently cover comments and arbitrary byte splits.

**Empirical correction:** both live streams sent a finish frame and then another
frame repeating the same `finish_reason` with empty content/role plus usage and
`choices:[...]`. The first decoder expected only a later `choices:[]` usage frame
and rejected this. It now accepts repeated identical terminal metadata with no
new content/tool deltas. Changed finish or post-finish nonempty content fails.
Completion is not committed until `[DONE]` and EOF checks succeed.
The [OpenRouter streaming guide](https://openrouter.ai/docs/api_reference/streaming)
documents comment heartbeats and stream errors; its current examples also include
Responses API events. This PoC freezes **Chat Completions** wire fixtures and never
mixes in Responses event types.

## Reasoning and opaque continuation

The experiment explicitly disabled reasoning generation. Neither live stream
contained `reasoning`, `reasoning_details` or another opaque continuation field;
no such field was needed to replay this observed tool turn. Zero usage reasoning
tokens support that narrow observation. The synthetic C decoder never merges
reasoning into visible text and currently skips unrecognized delta fields.

That skipping is **not** a production continuation strategy. A provider/model
that requires `reasoning_details` or opaque signatures for tool-turn continuation
must retain them losslessly in owned structured assistant history and replay them
as required, separate from visible text. Do not request or expose model reasoning
to manufacture this metadata. The production header gate must define typed
visible content vs. provider-owned opaque continuation lifetime/serialization,
limits and persistence, and include a fixture proving that round trip. This
PoC does not prove continuation support for reasoning-enabled models or providers.

## Validation and limitations

GCC 14.2.0, C11. `provider-wire` compiles with `-Wall -Wextra -Wpedantic
-Wconversion -Wshadow -Wstrict-prototypes -Wmissing-prototypes -Werror`.
Vendored headers use system include treatment; upstream jsmn triggers conversion
warnings under these local flags, so no claim is made that it is warning-free.
Three offline CTest cases pass in default and Release builds and with ASan+UBSan,
leak detection and halt-on-error (assertions explicitly remain enabled in Release). Parser/writer OOM, repeated pool reuse, Unicode, depth, framing,
terminal states and the live semantic fixtures are covered. No Cog-wide build is
needed because no production code/build configuration changed.

This is not a production adapter: it supports one completion choice, four tool
slots, no SSE BOM, no HTTP error-header parsing and no schema validation. Unknown
finish reasons fail closed. JSON duplicate-key policy, adversarial fuzzing,
provider-specific opaque metadata, cancellation and recoverable delta capacity
semantics remain header/implementation gates. Bounded buffers prevent unbounded
storage but do not prove acceptable runtime for every adversarial input. Real
429/midstream-error/OOM behavior was exercised synthetically; no paid requests
were spent provoking service errors. No ticket closure before independent review.

Whitespace review: `git diff --check` reports the required blank final SSE event
separator and two whitespace-only lines in the unmodified pinned jsmn header.
Those format/provenance exceptions are intentional; authored code has no other
whitespace findings. Vendor checksums are recorded in `vendor/SHA256SUMS`.

# Runtime implementation contract

`cog::runtime` owns cooperative execution, durable admission, instruction context
publication, and coordinator/child lifetimes. Public interfaces are
`cog/runtime.h` and `cog/runtime_instructions.h`. It builds with `COG_BUILD_CLI=OFF`.
The existing executor transition tables and STARTED/result/publication barriers
remain authoritative; the public runtime does not introduce another agent loop.

The runtime copies resolved scalar configuration, model names, original descriptor
bindings, scope metadata, and immutable instruction snapshots. Endpoint/template
objects and their nested provider state remain caller borrows through close.
Source readers are independent bounded objects constructed from explicit roots.
Input, views, decisions, reloads and shutdown are explicit calls. No terminal
reader, renderer, FILE API, argv parser, environment discovery, or UI endpoint is
part of the embedding interface. The optional native coordinator uses the existing
worker lifecycle and authenticated transport, with separate secret instruction
arenas. A native workspace directory is explicit and independent of source roots.

Legacy `cog_cli_*` private engine names and symbols remain to preserve fixture and
CLI compatibility. CLI-only terminal/argv/client adapters are in the application;
old private include paths forward to the library. The private fixture provider
remains linked for legacy configuration compatibility; it is not a public runtime
dependency. A later split of the legacy resolver can remove that implementation.

Each local child has a separate instruction/compiler owner and an explicitly
reserved core-owned session. The child borrows that session until executor cleanup;
only then may the coordinator return/release it. Immutable role selection precedes
STARTED and intersects effective parent capabilities. Child ASK denies; recursive
delegation is unavailable. Original static descriptor identity determines builtin
bridges; a custom tool with the same name never acquires runtime authority.

Instruction changes are prepared outside reader/tool poll functions. Exact CGIM
identities, context reservations, original bindings and provider preflight are
checked before publication. Control result, sibling no-effect results and context
binding are saved together. Scope checks follow capability/policy admission and
precede effects. Native transfer additionally requires exact checkpoint/call proof
and REQUEST/RESULT/COMMIT/READY/CONSUMED barriers; wire acceptance is not durability.

Validation requires both compilers and standalone header checks, CLI-OFF builds,
all existing CLI/scenario tests, installed external consumers, and sanitizer runs.
Runtime tests cover terminal isolation, pinned resume/activation, ASK identity,
local named delegation, child session reservations, native framing/capsules,
source resolution, receipt ownership and no-change remote scope publication.
The actual-process TLS fixture exercises both local and native named children
through activation, resource loading, changed-scope refusal and unchanged-scope
retry. Failure-path tests cover root-constructor cleanup and descriptor identity.
Local and native source work shares the coordinator reader under one exclusive
lease; children borrow its owner until their executor cleanup completes.

Native V1 export preserves resolved OpenRouter URL, credential, selected model,
static template, effective instruction bindings and context/output/round budgets.
It never resolves credentials from environment. The worker uses its fixed adapter
buffer/service profile derived from transmitted context budgets; arbitrary root
adapter buffer reservations are not serialized. Only STREAMING|TOOLS models,
the builtin template, and the guarded file store are supported. Native session
ceilings are 1024 nodes/16 calls, payload 16 times context budget, 16 context
records/bindings and context arena 16 times instruction capacity; the parent must
reserve at least those ceilings. Smaller configurations fail before launch.
Native builtin scope rows are fixed and checked before launching: read/edit use
required file path, search optional path and command optional cwd defaulting to
root. Unpublished configuration clones can replace their copied capsule only
before any receipt/provider/publication borrow; retained original bytes remain
unchanged. Host canonical directory identity is copied into the native profile.

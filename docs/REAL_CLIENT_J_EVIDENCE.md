# Stage 14.J implementation evidence

Package `32db464`; implementation children `194734c` (bounded config) and `a43e409`
(CLI resolution), review `f47fd57`, integration `bd8ac08`, finalize `4fb9ab5`.
This record includes writer evidence and the independent J package acceptance below.
It does not claim full application or real-client release acceptance.
The branch merges main's integrated G/Q state through `e35dc9f`.

The public headers are unchanged. The approved E07 pools are implemented in
`libraries/cog-config/src/config.c`. All running/candidate nodes and strings share
aggregate constructor ceilings. Retained finalized handles have a fixed identity
ceiling; begin failure consumes none. Strings use stable contiguous slices with
trial occupancy admission, so unrelated snapshots retain borrowed pointers. Set
stages aliased path/value inputs before reusing old bytes. Recursive traversal and
runtime malloc/realloc are absent. Constructor size products and their total are
checked; nine allocation-failure sites release every partial reservation.

`applications/cog-cli/src/options.c` implements public parse/usage/result names.
`configuration.c` is a private independently testable constructor for copied effective
options, resolved credential storage and one immutable endpoint/model/adapter generation.
The owner supplies a nonzero generation identity and static provider implementations;
bind performs no provider dispatch. It accepts one publication only. Old generations
stay owned until the application disconnects every borrower. Secret storage and
constructor file scratch are cleared before release on success and failure.

[Effective configuration](../applications/cog-cli/CONFIGURATION.md) documents the
bounded literal dotted-key grammar and every option/environment/file mapping.
CLI explicit presence, including retries=0, ASK and no-store, wins over other
sources; absent presence does not confer authority to parse-filled defaults. An
explicit real model is required. REAL defaults to durable XDG/HOME storage; no-store
is text-only and removes the generation's tool capability. Invalid setup never
selects mock or disables durability. No pi/pass/shell command is invoked. Default
credentials use env references; host callbacks support other references explicitly.
Secret, reference, URL, model/path/prompt and arbitrary argv values are excluded from
diagnostic descriptions and parser errors.

The private boundary for K/H is `applications/cog-cli/src/configuration.h`:
new -> options -> app script/workspace/store preparation -> bind -> endpoint ->
provider connect -> publication. On reload failure, free the failed new generation
and retain the old one. On shutdown disconnect providers before configuration_free.
MOCK script belongs in implementation_config; api_key is NULL. REAL adapter config
copies explicit limits and borrows the host net context. There is no successful
provider stub or mock fallback. The legacy application body fails REAL with
UNSUPPORTED until K replaces it.

## Reproducible validation

Configure separate directories with `COG_ENABLE_WERROR=ON`: `build-j` (GCC),
`build-j-clang` (`CMAKE_C_COMPILER=clang`), and `build-j-asan` (Clang plus
`COG_ENABLE_ASAN=ON`). In each:

```sh
cmake --build BUILD --target test_config test_config_bounded test_cli_options test_configuration test_configuration_memory -j4
ctest --test-dir BUILD -R '^(unit-config|unit-config-bounded|cli-options|cli-configuration|cli-configuration-memory)$' --output-on-failure
```

ASan used `ASAN_OPTIONS=detect_leaks=1:halt_on_error=1` and
`UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`.
All five tests pass in all three builds (15 executable passes). The baseline
`unit-config` target now links the real G/Q core/store library. The isolated
bounded/memory tests compile the same production config source. CLI configuration
is a separate static library included by cog::cli, with no provider/network dispatch.

Coverage includes baseline path/get/set/delete/list/validator ordering and repeated
snapshots, exact aggregate node/string/identity ceilings, stale handles, preserved
borrowed pointers, atomic set/begin rejection, aliased replacement, rejected reset
LIMIT finalization, precedence for file/env/CLI, every environment field including
present-empty rejection, numeric ranges and zero bounds, explicit retries=0/ASK,
default durable location, missing model/key/location, unsupported credential scheme,
malformed/duplicate/unknown/oversize/NUL file input, unsafe base URL, oversized and
header-injecting secrets, two independently owned generations, failed new generation
retaining old credentials, repeat-bind refusal, transactional diagnostic overflow,
redaction in descriptions and captured parser stderr, constructor allocation failure,
and zeroed memory before credential-owning free.

The Linux GNU/Clang linker-local memory test wraps direct malloc/calloc/free calls:
each of nine config constructor failures returns NOMEM with zero outstanding
allocations. It arms the next allocation to fail across begin/set/get/list/delete/
commit/rollback/validator registration and verifies the count never changes. The
CLI constructor's allocation failure and credential resolver failure clear/release
all state; the free wrapper checks every byte of its owned allocation is zero.
These wrappers add no production allocator hooks or global state.

Full application, installed link closure, real provider request-generation mismatch,
store ownership, no-store unsolicited-call denial, and request/tool execution remain
H/K/P integration obligations. These tests do not claim those scenarios passed and
make no authenticated requests. The config arena can fail LIMIT on contiguous-slice
fragmentation; capacity is never expanded or silently compacted across borrowers.

## Independent-review revision 1

Critic `1978933` added permanent protocol-input regressions after rejecting the
initial writer tip for malformed HTTPS authority and model UTF-8 acceptance. The
focused correction validates the resolved generation before publication: nonempty
ASCII hostname or bracketed IPv6 parsed with inet_pton, decimal optional port in
1..65535, and strict model UTF-8 scalar encodings. No URI framework, path Unicode
requirement or public header amendment is introduced. Provider-side validation
remains H's independent defense.

Regression coverage exercises each rejected URL/model through CLI presence,
environment and literal config-file sources. Valid domain/absolute-domain/IPv4,
bracketed IPv6 (including embedded IPv4), explicit ports 1/65535 and leading-zero
443 remain accepted. Valid two/three/four-byte Unicode boundary scalars including
U+0080, U+D7FF/U+E000 and U+10FFFF remain accepted; malformed native filesystem
bytes remain accepted as paths. Failed constructors return INVALID_ARG and NULL.
The five J tests pass again in strict GCC, strict Clang and separate Clang
ASan/UBSan with leak detection (15 passes). Independent rereview passed below.


## Independent acceptance and integration

Independent review `f47fd57` accepted corrected tip `acd16cc` after one rejection
cycle. The critic reproduced all five J tests under GCC WERROR, Clang WERROR and
separate Clang ASan/UBSan with leak detection: 15/15 passed. An independent
20,000-case byte generator matched model admission against Python strict UTF-8
decoding and checked 23 further HTTPS authority cases. The first review also
matched 18,000 deterministic config operations, with concurrent snapshots and
commit/delete/prune/rollback, against an independent map oracle. Separate sanitized
probes checked callback protocol/overflow failures, numeric/presence rules and exact
64 KiB file / 4095-byte line boundaries. Only synthetic credentials were used.

Integration `bd8ac08` merged the approved branch into main as `1367a6f`, without
conflicts. Root CMake and F/Q/G changes are retained; public headers are unchanged.
The app target propagates toolkit, net and cli-config in its PUBLIC link interface.
Generated export inspection and undefined-symbol inspection confirm cli-config
uses only its own objects and libc (including inet_pton); this is not installed
full-app runtime acceptance.

Fresh main builds `/tmp/cog-j-main-gcc`, `/tmp/cog-j-main-clang` and
`/tmp/cog-j-main-asan` repeated the five-target build and anchored CTest expression
above: all 15 post-merge executions passed. The sanitizer run used leak detection
and halt-on-error for ASan/UBSan. Main has no tracked modifications after this
record; the preexisting untracked `build_user/` directory was preserved.

J is accepted for bounded configuration, precedence/credential resolution and the
private generation boundary. H/K still own provider construction, store guards,
actual no-store execution denial, request-generation matching and installed full-app
closure. The old REAL application entry remains UNSUPPORTED until K. The known
legacy mock vtable strict-build failure belongs to H migration. No authenticated
request or claim of a complete real conversation is part of J acceptance.

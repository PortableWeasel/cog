# O6 resolved profiles and installed roles

Review source: `configuration.c` (resolved child/CGB1/finish_host/mask) and
`host_options.c` (fixed installed roles). Header amendment10dde8a independently
passed at5585a89; fixed worker argv was declared at277b004 before parser bodies.
No shared application CMake or O4 runtime glue is edited here.

Standalone, actual production-source component verification:

```sh
cmake -S applications/cog-cli/tests/headless-profiles -B /tmp/cog-o6-gcc \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=gcc
cmake --build /tmp/cog-o6-gcc -j4
ctest --test-dir /tmp/cog-o6-gcc --output-on-failure

cmake -S applications/cog-cli/tests/headless-profiles -B /tmp/cog-o6-clang \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang
cmake --build /tmp/cog-o6-clang -j4
ctest --test-dir /tmp/cog-o6-clang --output-on-failure

cmake -S applications/cog-cli/tests/headless-profiles -B /tmp/cog-o6-asan \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DPROFILES_SANITIZERS=ON
cmake --build /tmp/cog-o6-asan -j4
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
  ctest --test-dir /tmp/cog-o6-asan --output-on-failure
```

All five tests pass on GCC14.2 and Clang19.1.7 in all three profiles. Strict
C11/warnings/Werror and executed Release assertions apply to each compiled source.
The isolated fixture needs only public type declarations and libc; no dependency
profile, transport, provider, store or child executor is instantiated. Opaque net
pointer tokens test ONLY explicit borrowing, never stand in for a runtime. O4
still owns actual offline TLS/process/task integration and the final full build
with the accepted shipped curl/c-ares profile. These are component results, not
original03/07/09/10/11/17 or whole-O runtime acceptance.

Coverage:

- `o6-profiles`: REAL resolved copies, policy DENY/ASK/ALLOW intersection, tool
  mask distinct from policy, inherited/narrowed finite limits/deadline (including
  near-u64 exhaustion), cleared startup selectors/input/authority, MOCK refusal;
  one-shot canonical host attachment and error transactionality; CGB1 fixed-byte
  golden prefix, complete encode/decode/re-encode including exact generation and
  secret, explicit transport defaults/no imported lifecycle authority, every
  truncated frame, trailing bytes, version/profile errors, control-byte secret
  rejection, malformed paths/references/limits and oversized real fields. CA
  capacity and malformed profile failure preserve scalar/text/probe outputs.
- `o6-host-options`: ordinary role-looking option values are preserved; fixed
  role flags/ranges/duplicates/mixed-role rejection, all worker identity fields,
  no client config/credential parsing, no raw value echo and checked help failure.
- `o6-memory`: observable allocation and source-read positive controls at O3;
  exact named constructor allocations/OOM outputs and deadline/probe canaries;
  no parser/clone/codec/finalizer source or resolver reads after resolution;
  whole-configuration-byte failure preservation and whole-owner zero-before-free
  for actual parent, child and decoded secret owners. Wire may be overwritten
  after decode. Parser has exactly one filtered argv allocation and repeat close.
- Existing `test_configuration.c` and `test_configuration_memory.c` run unchanged
  for J/R12 precedence, explicit one-call resolver authority, pointer lifetime,
  saved secret copies, failure cleanup and optimizer-sensitive allocation oracles.

O4 registration handoff:

1. Keep existing `configuration.c` on `cog-cli-config`. Add `src/host_options.c`
   to the real CLI library/installed-host composition when approved O4 link/include
   closure (`cog::orchestrator`) is ready. Do not create fake runtime implementations
   merely to link the parser.
2. Register these THREE new fixtures additively; the standalone CMakeLists is an
   exact working recipe. Suggested integrated names: `cli-headless-profiles`,
   `cli-host-options`, `cli-headless-profile-memory`. Profile/options tests may
   link the actual `cog::cli` once host_options is present. The memory test should
   compile the three production C sources directly (as this recipe does), with
   target-specific `-fno-builtin-malloc/calloc/realloc/free` and wrappers for
   malloc/calloc/realloc/free/getenv/fopen. Preserve production optimization.
   Supply existing public module includes; no test stub/vtable is needed.
3. Run existing configuration/memory tests via their existing targets; do not
   register duplicate existing tests in the final application suite.

CGB1 is a pure resolved profile, not the outer BOOTSTRAP purpose/counter envelope.
R12 failed-attempt import belongs to O4's validated outer BOOTSTRAP
`attempts_used` field and executor adoption. It is intentionally NOT silently
invented inside CGB1: generation is copied unchanged, constructors never re-read
sources, and no bootstrap/probe acquires a store guard or grants execution. The
host separately verifies canonical SAME-directory identities and the immutable
shared-store checkpoint/current-owner probe before ACK/tasks. This component
cannot claim those caller-owned IO/runtime oracles.

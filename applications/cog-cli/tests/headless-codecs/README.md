# O5 pure codec receipt

Base: approved O integration `0ae8213ac43e231e31890a094f2cbd69600c043c`.
Scope: `control.c`, `control_receipt.c`, `projection_codec.c`, `child_context.c`
and private `codec_internal.h` (header stage `fa0175a` before bodies). No app
runtime/configuration/CMake edits, dispatch, provider calls or transport activity.

The four codecs have no constructor, hidden allocation, I/O or mutable global.
Constructor/runtime ownership and authenticating/role-checking ingress remain O4.
`nm -u` on each Release implementation object has no malloc/calloc/realloc/free
import. GCC imports only memcpy/memcmp and actual CGD1 encode/decode. The runner
checks allocation imports on every profile. Dependent allocator behavior is not
claimed from this static evidence.

```
python3 applications/cog-cli/tests/headless-codecs/run.py --cc gcc --build-dir /tmp/cog-o5-codecs-gcc
python3 applications/cog-cli/tests/headless-codecs/run.py --cc clang --build-dir /tmp/cog-o5-codecs-clang
python3 applications/cog-cli/tests/headless-codecs/run.py --cc clang --sanitize --build-dir /tmp/cog-o5-codecs-asan
```

All profiles use C11, strict warnings/Werror, Release/O3 and NDEBUG. Sanitizers
use ASan+UBSan with leak detection and halt-on-error. Tests use always-evaluated
CHECK rather than removable assertions; each executable is separately run with
a deliberate failed CHECK and must exit unsuccessfully. Four positive fixtures
and four negative assertion controls pass in each profile. This is focused
standalone evidence, not full O application/scenario/installed acceptance.

The runner compiles the actual existing `app_display.c`, `cli.c` and core
`message.c` with section GC so CGV1 tests call the real CGD1 codec/message validator
without invoking unrelated app functions. There are no replacement validators,
mock CGD bodies or successful runtime stubs. No libcurl/profile install or
`build_user` files are involved.

| Fixture | Exact oracle |
|---|---|
| receipt | independently literal55-byte negative-consume vector, big-endian negative outcome, all meaningful status/outcome pairs, identity/prefix/wire exhaustion predicates, unknown version/disposition precedence, every truncation/trailing byte, overlap/no-write and exact capacity |
| control | independently literal CGL1 bytes and all9 command suffixes, owned session/payload/text copies, all structural kinds/lanes without implying role acceptance, HELLO discovery, full uint64/127-byte ID bounds, malformed UTF-8/IDs/lengths/version, unused fields, every truncation/trailing byte, canaries/overlap/capacity |
| projection | independent CGV1+CGD1+agent+policy vector, active/published semantics, meaningful agent outcomes/ordered IDs/16-agent ceiling, exact policy root/config/ordinal/name bounds, every truncation/trailing byte and late malformed policy, all caller arenas untouched on late failure or any independent shortage; standalone CGD sequence0 remains valid while CGV rejects it |
| child context | literal escaped JSON/full call-ID bytes, lossless UINT64_MAX decimal strings, max16 sorted members including self, worst-case escaped legal127/255-byte strings, UTF-8 and scalar bounds, exact NUL capacity, overlap and error canaries |

Writer self-review found and fixed checked `text_len+1` accumulation so a 32-bit
size_t cannot wrap a decoded command storage requirement. No supported public
header was narrowed. Architect confirmed literal clarification: typed commands
require nonzero observed generation/sequence for all opcodes (initial0 belongs
HELLO); CGV nested through_sequence is nonzero committed DATA/resync, policy
ordinal0..15/tool-name1..255 bytes. Current role/name/config/owner authority is
still checked by ingress; pure decode never performs semantic acceptance.

Shared integration owner must add the four sources to the real CLI library and
register these fixtures only after its transitive orchestrator dependency is
correct. Suggested additive tests/CMakeLists fragment (not applied by O5):

```cmake
foreach(codec IN ITEMS receipt child_context control projection)
    add_executable(test_cli_codec_${codec} headless-codecs/test_${codec}.c)
    target_include_directories(test_cli_codec_${codec} PRIVATE ../src)
    target_link_libraries(test_cli_codec_${codec} PRIVATE cog::cli)
    target_enable_warnings(test_cli_codec_${codec})
    cog_enable_test_assertions(test_cli_codec_${codec})
    add_test(NAME cli-codec-${codec} COMMAND test_cli_codec_${codec})
    set_tests_properties(cli-codec-${codec} PROPERTIES TIMEOUT 20 LABELS "cli;codec")
endforeach()
```

The standalone runner also runs negative assertion controls; CMake integration
may register those under the maintained expected-failure fixture policy rather
than treating their exit as a successful codec operation. No runtime skip or
allocation policy change is required.

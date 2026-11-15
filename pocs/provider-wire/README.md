# Provider wire research

Standalone Stage 14.B PoC, ticket `2e1cddf`. Nothing links into Cog; root CMake,
production headers and blueprint are unchanged. Read the decision and evidence in
[`docs/research/provider-wire.md`](../../docs/research/provider-wire.md).

```sh
cmake -S pocs/provider-wire -B pocs/provider-wire/build
cmake --build pocs/provider-wire/build -j4
ctest --test-dir pocs/provider-wire/build --output-on-failure

cmake -S pocs/provider-wire -B pocs/provider-wire/build-asan -DWIRE_SANITIZE=ON
cmake --build pocs/provider-wire/build-asan -j4
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
ctest --test-dir pocs/provider-wire/build-asan --output-on-failure
```

All tests are offline. `wire-contract` compares yyjson and jsmn, then tests bounded
SSE framing/turn assembly. The two fixture tests replay the sanitized live tool
and final text responses one byte at a time. Warnings apply strictly to PoC code;
vendored headers are system includes and remain unmodified. Assertions stay on
in Release builds. Vendor versions and license locations are in `vendor/SOURCES.md`.

`live_smoke.py --live` is a separate explicit authenticated experiment. It refuses
to overwrite the committed summary, checks the configured catalog model and tool
capabilities, and attempts exactly two generation requests (no retries), each
limited to 512 output tokens. It reads pi's OpenRouter key directly into process
memory. It never places the key in arguments, environment, payloads, fixtures or
logs. It sends only synthetic prompts/schema/tool results, disables reasoning,
and executes a synthetic echo in memory. Do not delete the summary and rerun as
part of review: the authorized smoke has already used two requests.

Fixtures contain synthetic content only. Generation IDs/creation times and
provider fingerprint fields are sanitized; request-local tool-call IDs remain to
show correlation across the tool round. Data frame JSON is normalized, comment
frames are omitted by capture, and original transport fragmentation is not
retained. Synthetic tests cover comments and fragmentation independently.

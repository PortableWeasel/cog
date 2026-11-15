# R12 bounded application reload: writer evidence

Design `dffc59e`, package `af6956a`, app child `881cb81`, independent runtime
review `faaf1ea`, integration `7ad3bf9`. Managed worktree
`/home/user/.codex/worktrees/impl-live-reload/cog`, branch `codex/impl-live-reload`.
Approved headers are `4984d1b`; independently approved core/config implementations
were integrated before app work at `eb76050`. Reviewed main net observation
delivery `2cfd1b6` is incorporated through `bd7d76a`, with both DESIGN paragraphs
preserved. No provider/core/config/net production implementation was changed by
this app child. The later application observability bridge remains separate.

## Delivery and correction

Source checkpoint `254f713` implements the fixed real config transaction, one
future owner beside current, pinned request identities, deferred activation,
retained CONFIG_CHANGED notice, terminal routing and consuming cleanup semantics.
Full static CLI linkage now includes `cog::config`. Every generation disconnects
before its corresponding configuration/secret free; consumed handles are never
retained or retried, including cleanup errors. Candidate discard retains the
primary API error while separately freezing admission on cleanup failure.

Independent source review found one grouped runtime defect: known EOF during an
old active turn still allowed future activation after that turn finished with no
remaining input. `3ff7980` adds the narrow activation guard: known EOF requires an
existing eligible input slot. Otherwise the future remains idle until teardown.
Fresh/final-line input and K's incomplete-turn authorization remain authoritative.
The critic's independent permanent pipe-EOF and64-successful-generation tests
(`0e34e8c`) are included as `7955a04`. The final executable fixture checkpoint is
`92b25f1`; subsequent writer edits only add this evidence and contract prose.

## Targeted behavior and oracles

| Contract rows | Executable evidence |
|---|---|
| 1,6,11: idle/busy/input/notice | Two actual providers, unchanged active getters at publication, next-input generation2, saturated64-event queue and copied input ahead of the retained notice; stale notices do not change request serial |
| 2: retained output | Actual next OVERFLOW before/after preparation has identical required capacity; old request serial/sequence/time/error snapshot remains unchanged |
| 3: retries | Old request and retry both use generation1/model; next turn and its retry both use generation2/model |
| 4: tools and restored proposals | A real command starts once under the old turn; old tool continuation stays generation1. Durable PROPOSED restore preflights generation1, candidate preflights generation2, old continuation remains1, fresh next input uses2, effect file is exactly `once` |
| 5: policy terminal route | Actual pipe-fed /reload during ASK publishes without changing the old undecided policy; subsequent EOF denies that call and creates no effect |
| 7,18,20: resolver/admission | REAL explicit equal reference resolves once, per-call recursive reload/drive/input is refused, old secret survives, new secret is independently owned; component config tests cover omitted reference, malformed secrets and fixed fields |
| 8: constructors | Provider-construction NOMEM preserves current provider/tree; actual allocation-failure iteration covers every candidate constructor allocation and unchanged generation output |
| 9: real commit failures | Injected unknown key triggers the actual static validator. A shrunk candidate plus held transaction fills the actual fixed64-node pool so rejected reset returns LIMIT and finalizes that exact transaction; old running values remain intact |
| 10: queue saturation | A full real core queue retains the pending notice without an enqueue error escaping or a second publication; eventual by-value notice clears independently |
| 12,13: selected compatibility | REAL foreign namespace and malformed known encrypted-data type refuse before commit/wire. Explicit safe fork prefix excludes incompatible unselected history; an incomplete selected prefix refuses |
| 14: future bytes | Old complete text plus the new input exceeds the unchanged context ceiling after publication; new send refuses LIMIT, preserving complete old text with no truncation or retry of old work |
| 15: failed old work | Publication plus queued input cannot authorize replay of old partial text; only fresh explicit input sends under generation2, with the failed branch preserved. Existing K save/retained-output faults and source guards remain active |
| 16: EOF/cleanup | Unterminated /reload publishes without a model turn or idle activation at EOF; unterminated fresh input activates and executes. Critic pipe EOF during delayed old work leaves current1/future2 until free |
| 17: exact bound | Failed attempts consume identities; last eligible attempt publishes65. Independent64-successful-reload test proves every commit/activation/retirement, peak two owners,65 total constructed/consumed providers and no sends |
| 19: retirement | Core/app getters agree after activation, including cleanup error. Source updates both before destruction. Configuration-free probes require matching provider consumption, primary candidate error is retained, and no handle is destroyed twice |

`test_configuration`/`test_configuration_memory` are the independently reviewed
resolved-constructor prerequisites, not a substitute for these actual provider
composition cases. The reload memory extension uses the existing corrected
observable allocation oracle, symbol-specific no-builtin flags and positive
controls; production optimization is unchanged. Runtime after publication rejects
all allocator attempts while activation, notification and the next text turn pass.

Original08 retains actual config snapshot/validator/rollback/event checks and
adds actual app sends at generations1/2 with preserved history. The installed
runtime test performs terminal reload, then fresh input, and verifies generation
status without echoing supplied values; an EOF reload command produces no text.

## Reproduction and scoped results

All complete target builds use Release/O3, WERROR and the audited c-ares profile:

```sh
cmake -S . -B build-r12 -DCMAKE_BUILD_TYPE=Release -DCOG_ENABLE_WERROR=ON \
  -DCOG_NET_CURL_BUILD_DIR=/tmp/cog-net-maintained/build \
  -DCOG_NET_CURL_LIBRARY=/tmp/cog-net-maintained/build/lib/libcurl.so \
  -DCURL_LIBRARY=/tmp/cog-net-maintained/build/lib/libcurl.so \
  -DCURL_INCLUDE_DIR=/tmp/cog-net-maintained/curl-8.23.0/include
cmake --build build-r12 -j4
ctest --test-dir build-r12 \
  -R '^(cli-|unit-core(-service)?$|unit-config(-bounded)?$|integration-core-toolkit$|scenario-test_(01|02|04|05|06|08|12|15|16|17|20))' \
  --output-on-failure -j4
```

Separate `build-r12-clang` selects Clang. Separate `build-r12-asan` selects Clang
and `COG_ENABLE_ASAN=ON`, also retaining Release/O3; runs enable
`ASAN_OPTIONS=detect_leaks=1:halt_on_error=1` and
`UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`. Fresh caches in all writer
profiles have no stale CURL_LIBRARY_RELEASE override. `ldd` resolves libcurl to
the admitted maintained path and c-ares to its explicit dependency path.

At `92b25f1`, strict GCC, strict Clang and Clang ASan/UBSan/LSan all compile every
target and pass the exact38-executable selection above,38/38 in each profile.
Those are21 CLI tests, four core/config components, one core/toolkit integration
and12 original scenarios. This count does not mean P's38 scenario traces passed.
A separate GCC/Debug `build-r12-tsan` enables `COG_ENABLE_TSAN=ON` and passes
`^cli-app-(stop|limits)$`2/2 with `TSAN_OPTIONS=halt_on_error=1`.

The REAL compatibility cases use synthetic credentials/models and an audited net
context pointed at loopback; no request starts. Existing actual local-TLS
`cli-app-https` passes its tool/edit/opaque continuation fixture. **No paid/live
generation call was made.** Installed tests use the actual executable installed
through its runtime component. Full installed static consumer/transport dependency
closure remains the independent integration/packaging obligation, not an inference
from this component install.

Independent final package review and authorized integration remain required.
M/P keep their applicable acceptance/full-suite/trace requirements; remote agent,
headless routing and the application observation bridge retain their own tickets.

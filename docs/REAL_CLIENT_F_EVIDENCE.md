# Stage 14.F transport delivery evidence

Package `8f55808`, independent review `c1f8d6c`, integration `308e2f8`.
Reviewed revision `100649e3ece14232099b016b944c839aec45940b` was merged without
conflicts as `e5b27acfd299a851a62ebb95f4c9062a20eb3ad9` on 2026-11-15.
Independent review passed on cycle 2 after one revision.

The initial transport profile uses audited c-ares-only curl 8.23.0 and c-ares
1.34.9, explicit host dependency initialization, and explicit DYNAMIC memory
acknowledgement. Cog reserves its own storage at construction; dependency handles
may allocate during service. Total dependency memory caps remain unsupported.
The [dependency profile](../libraries/cog-net/cmake/README.md) records provenance
and deployment requirements. Unknown or mismatched curl artifacts fail closed.

Each request uses a fresh HTTP/1.1 connection, with reuse forbidden and ALPN,
implicit proxy, netrc and redirects disabled. This intentionally excludes pooling
and HTTP/2: maintained curl can automatically replay a consumed POST after a
reused connection closes before response headers. A prerequisite guard also
rejects a second HTTP attempt. One-effect POST/drop, 401, 307 and 407 regression
oracles pass, including hostile proxy environment settings. Dependency upgrades
require source and behavioral revalidation of this no-replay profile.

Completion is captured and detached even when callback work exhausts the quantum;
terminal publication remains budgeted and later timeout/cancel cannot replace an
already captured outcome. The [quantum amendment](../libraries/cog-net/contracts/QUANTUM_REVIEW.md)
was independently accepted under `62993d0`: it bounds accepted work and driven
slots, not dependency-internal CPU/syscall counts or hard elapsed time.

## Independent validation

All 11 registered `unit-net` tests pass in strict GCC, strict Clang and
ASan/UBSan builds (33 passes), including the two critic-authored regressions that
failed before the revision. Coverage includes retained views, shared-consumer
backpressure, quantum-1 fairness, cancellation, capacity, TLS verification,
HEAD, dependency/Cog allocation failure, completion/error/cancel precedence and
absence of automatic HTTP replay. A fresh main build after merging passed all
11 again:

```sh
cmake -S . -B /tmp/cog-net-main-critic -DCOG_ENABLE_WERROR=ON \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCOG_NET_CURL_BUILD_DIR=/tmp/cog-net-maintained/build \
  -DCOG_NET_CURL_LIBRARY=/tmp/cog-net-maintained/build/lib/libcurl.so \
  -DCURL_LIBRARY=/tmp/cog-net-maintained/build/lib/libcurl.so \
  -DCURL_INCLUDE_DIR=/tmp/cog-net-maintained/curl-8.23.0/include
cmake --build /tmp/cog-net-main-critic --target \
  test_net test_net_oom test_net_cog_oom test_net_regressions -j4
ctest --test-dir /tmp/cog-net-main-critic -R '^unit-net' --output-on-failure
```

Additional direct Cog DNS testing used a test-only c-ares initialization shim
pointing channels at a local UDP blackhole. Thirty contexts (10 each cancel,
deadline, active release/free) emitted 60 observed queries; expected terminal
outcomes and descriptor/task baselines (4/1) were restored after every run.
Observed cancel/release/free peak was 1 ms on this host, not a latency guarantee.
Local reproduction sources and commands are recorded on the review ticket.
TLS fixture keys/certificates were temporary. No provider or paid calls ran.

## Packaging boundary

A net-only install, manually supplied `cog/result.h`, was moved to a different
prefix. An external CMake consumer linked only `cog::net`, initialized curl,
constructed/destroyed a context, and cleaned up successfully. Export CMake files
contained no build/worktree paths. This is limited net-target evidence, **not**
a full project install or complete release dependency-closure proof.

Gate P (`77adfec`) must build/install all migrated sibling packages, stage the
shared result header through normal install rules, and verify the complete
external consumer and shipped dependency profile. Other Stage 14 implementation
and runtime gates remain pending.

# O1 authenticated IPC writer evidence

Parent e7c11c7 / source7b5592d, source checkpoint `fa921ac`. Based on the approved
O integration0ae8213, not main. Private layout passed independent review at
b7485c2 after three writer passes and the explicit first-proof ordering amendment
in [the layout record](REAL_CLIENT_IPC_O1_LAYOUT.md). Final independent source
review, O integration and component finalization remain separate tickets.

## Implementation and actual oracles

Only ipc.c, its private endpoint layout, one focused unit fixture and its additive
unit CMake registration changed. Existing ipc_replay/core/app/orchestrator code
and public headers remain unchanged. Native Linux SO_PEERCRED is captured once
at native connection establishment, separate from the wire identity prefix;
public peer_info performs no syscall and changes caller output only on success.
Exact directed send resolves the owning listener's actual live accepted peer,
requires complete handshake and delegates to existing copied queue admission.
No temporary route, role authority or additional runtime allocation is added.

The private handshake service unit still returns before subsequent payload/EOF/
TX processing. O2 must cache proof at that boundary irrespective of its event
queue capacity; aggregate public service is not a substitute. These IPC tests
prove the boundary and retained frames; actual core event saturation/lease proof
remains O2's acceptance obligation, not inferred from this component test.

With wipe_buffers, volatile stores clear whole retired TX/RX slots, abandoned TX
and incomplete RX, and all allocated payload storage at explicit disconnect,
free and failed construction. Complete readable frames survive physical EOF or
TX failure and receive OVERFLOW retries. Without wipe_buffers the previous
payload behavior remains. Accepted endpoints remain separately caller-owned
when the listener is freed; no ownership change is introduced.

The new `unit-ipc-authenticated` uses real Unix sockets and a forked connector:

- Native parent/child PID and UID/GID, immutable proof despite misleading payload,
  listener/connected query, no query syscall, exact fabric/link lookup, unknown
  and retired IDs, explicit unbound/handshake/null/zero errors, unchanged outputs.
- Credential syscall error, malformed result length and nonpositive native PID
  fail closed while optional generic IPC remains usable. The compile-time
  no-SO_PEERCRED branch returns UNSUPPORTED; this Linux run does not claim
  empirical validation on an unsupported operating system.
- Two live accepted peers prove directed delivery is not broadcast. Copied source
  bytes survive caller mutation; full TX queue returns AGAIN without mutation;
  over-limit input is refused; short writes retain the unsent cursor/secret.
- Complete frame plus partial next frame followed by EOF, and complete/partial
  or completely full RX plus TX EPIPE, preserve complete payload/proof copy.
  Receive OVERFLOW preserves bytes; successful copy clears only the retired slot.
  EINTR does not abandon queued data. Explicit disconnect clears full buffers.
- Allocation wrappers have positive call-count/payload controls; each of six
  constructor allocation failures produces NULL/NOMEM, and actual free observes
  zeroed payload allocations, including untouched capacity initially filled with
  a canary. Release test compilation retains assertions and disables relevant
  allocator builtins. Wrappers are confined to the fixture, not production.

## Scoped build and runtime results

GCC14.2 and Clang19.1.7 strict Release builds use normal project warnings/Werror.
Separate Clang ASan+UBSan uses leak detection and halt-on-error; separate GCC TSan
uses Debug and halt-on-error. All use the admitted frozen curl configuration from
[M offline evidence](REAL_CLIENT_M_OFFLINE_EVIDENCE.md), matching CURL_LIBRARY
and COG_NET_CURL_LIBRARY to the installed admitted libcurl and its actual audited
build directory. No dependency installation or runtime-byte change was needed.

Each strict GCC, strict Clang and ASan/UBSan/LSan profile passed **6/6**:
unit-ipc, unit-ipc-critic, unit-ipc-nonblocking, unit-ipc-replay,
unit-ipc-replay-critic and unit-ipc-authenticated. Separate TSan passed **4/4**:
unit-ipc, unit-ipc-critic, unit-ipc-nonblocking and unit-ipc-authenticated, including
the existing independent-endpoint threaded fixtures. This is not a full O build
or scenario pass; sibling runtime sources and six M exceptions remain pending.

Builds/logs are `/tmp/cog-o1-{gcc,clang,asan,tsan}` and matching
`-config.log`, `-build.log`, `-tests.log` files. Focused command:

```sh
cmake --build BUILD --target test_ipc_authenticated test_ipc_nonblocking \
  test_ipc test_ipc_critic test_ipc_replay test_ipc_replay_critic -j2
ctest --test-dir BUILD -R '^unit-ipc' --output-on-failure
```

Four separate temporary O3 negative controls were rejected: omit volatile wipe,
falsify native PID, omit RX retirement wipe, and choose the wrong directed peer.
Each fails an actual assertion; `/tmp/cog-o1-negatives` retains the mutated copies
and diagnostic logs. Production source was never replaced for these controls.
Initial writer iteration also caught an extra service-progress result from
credential capture; immediately successful connect now captures in connect,
preserving existing nonblocking handshake expectations without weakening tests.

| Final test log | SHA256 |
|---|---|
| cog-o1-gcc-tests.log | 7d4c016e995c12ebe71e0e01578d3f17a0a090fb67552631f74b11ec74a501c8 |
| cog-o1-clang-tests.log | e50a61cd864eea024d06154b547bcaba4780032b81d5869c05c75bf7f3a7bbb1 |
| cog-o1-asan-tests.log | ce8da38f4d5c79fc2cfbdf4d0655f9e21946b75406ded35fc2abd79a4edf8d20 |
| cog-o1-tsan-tests.log | d9c8c3a7db15efa5d56e2c1445d9db428ba61593bea02fa7591b115fd50de3cf |

No paid requests, source/main integration edits or release-gate closure occurred.

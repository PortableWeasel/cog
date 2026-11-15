# HTTPS lifecycle research PoC

Standalone research for `c5671b5`. It does not implement or link Cog, change
public headers, or participate in the root build. Evidence, limitations and
the conditional adoption recommendation are in
[the research report](../../docs/research/https-lifecycle.md).

Requires Linux (`/proc/self/fd` and a saturated TCP accept queue fixture), a
C11 compiler, CMake >= 3.18, pkg-config, libcurl >= 7.85.0 with asynchronous
DNS, Python >= 3.10, and the OpenSSL command line. The tested libcurl uses
OpenSSL; other TLS backends are not validated. No internet, credentials,
privileged network setup, checked-in private keys, or production files are
needed. Certificates and keys are generated in a temporary directory and
removed when the fixture exits.

```sh
cmake -S pocs/https-lifecycle -B /tmp/cog-https-lifecycle-build
cmake --build /tmp/cog-https-lifecycle-build
ctest --test-dir /tmp/cog-https-lifecycle-build --output-on-failure -V

cmake -S pocs/https-lifecycle -B /tmp/cog-https-lifecycle-asan -DPOC_SANITIZERS=ON
cmake --build /tmp/cog-https-lifecycle-asan
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
ctest --test-dir /tmp/cog-https-lifecycle-asan --output-on-failure -V
```

The one CTest fixture runs 12 transport cases and 80 allocation-failure
cases in separate child hosts. Each host explicitly initializes libcurl,
creates contexts, services transfers, removes/destroys all handles, and
then performs global cleanup. The multi-context case destroys one context
and submits a further request on its surviving peer. The failure sweep
rejects any nonzero child status or untriggered fault injection.

The C file's mutable atomic allocator observers belong to the experimental
host. libcurl's global allocator callbacks have no userdata argument; this
instrumentation is not a proposal for Cog global state. Counts exclude
allocations made directly by TLS/resolver dependencies. ASan/LSan supplements
the curl counts. File-descriptor counts must return to their initial value.

The 10ms wait budget and 150ms test ceiling measure this machine, not a hard
real-time guarantee. The fairness case uses a zero-wait pump alongside an
always-nonempty synthetic event queue. It does not exercise cog_core_run.

# HTTPS lifecycle research: libcurl multi

Research ticket `c5671b5`, 2026-11-15. Baseline `879781d`.
Scope: standalone [PoC](../../pocs/https-lifecycle/README.md), no production
implementation, header amendment, root CMake change, or provider request.

## Decision from evidence

libcurl multi is a viable transport candidate with **explicit host-owned
process initialization**, separate context-owned multi handles, verified
TLS, and event-loop cancellation. Recommend continuing to the header gate
with libcurl as a **conditional candidate**, not declaring the embedding
contract satisfied. There are two adoption gates:

1. Runtime allocations are real: one successful request made 72 observed
   curl allocation calls during polling, after context/request setup. A
   constructor-only bounded-workspace/no-hidden-allocation contract is not
   met. The architect must explicitly specify third-party allocations under
   explicit per-request constructor/destructor-owned dependency handle lifetimes,
   distinguish these from Cog-owned allocations, and define OOM and memory-limit
   semantics, or reject this backend. A constructor lifetime alone does not
   establish a byte bound. No silent weakening of the existing rule.
2. The installed threaded-resolver build **fails** the DNS cancellation
   target: a controlled two-second libc lookup made remove_handle wait
   1.893 seconds. The DNS followup below demonstrates a c-ares-only build
   cancelling actual blackholed DNS queries promptly without leftover
   threads. Require a verified c-ares primary resolver for the initial
   supported transport profile; ASYNCHDNS alone is insufficient.

No Cog global initialization flag/reference counter or lazy curl init is
needed or proposed. The host must initialize the dependency before any Cog
transport context, coordinate with other curl users in the process, and
clean it up only after every such user/context is finished. The PoC uses
curl_global_init_mem explicitly so its host can observe curl allocations;
ordinary hosts can use curl_global_init. Both are process-scoped operations.
See the [official lifecycle contract](https://curl.se/libcurl/c/curl_global_init.html)
and [allocator callback contract](https://curl.se/libcurl/c/curl_global_init_mem.html).

## Ownership and execution trace

```mermaid
sequenceDiagram
    participant H as Embedding host
    participant A as Context A / CURLM
    participant B as Context B / CURLM
    participant T as Local TLS fixtures
    H->>H: curl_global_init_mem (explicit, once)
    H->>A: create multi + easy handles
    H->>B: create multi + easy handle
    loop event-loop service budget
        H->>A: multi_perform / multi_poll(10ms) / multi_perform
        A->>T: verified HTTPS and body callbacks
        H->>B: multi_perform / multi_poll(10ms) / multi_perform
        B->>T: independent verified HTTPS
    end
    H->>A: remove easy handles / easy_cleanup / multi_cleanup
    H->>B: submit another request; service to completion
    H->>B: remove easy handles / easy_cleanup / multi_cleanup
    H->>H: curl_global_cleanup (all contexts gone)
```

The callback copies into a fixed 128-byte caller-owned array. Overflow
returns zero and produces CURLE_WRITE_ERROR. It does not silently truncate
or claim a retryable consumed chunk. The PoC does no retry, parsing, or
provider adaptation. An application-level 100ms no-data timer demonstrates
stall detection independently of libcurl's overall/connect timeouts.
Cancellation calls remove_handle outside curl callbacks, then easy_cleanup;
other handles remain valid under the
[documented removal contract](https://curl.se/libcurl/c/curl_multi_remove_handle.html).
All helper interfaces are private and sketched at the top of lifecycle.c
before their bodies; there is no public PoC API or changed Cog contract.

| Flow | Calls used | Declaring contract |
|---|---|---|
| Host start/stop | curl_global_init_mem, curl_global_cleanup | curl/curl.h, host-owned lifecycle |
| Context start/stop | curl_multi_init/cleanup | curl/curl.h, one multi per context |
| Request start/stop | curl_easy_init/setopt/cleanup, curl_multi_add/remove_handle | curl/curl.h, easy owned until cleanup |
| Service and completion | curl_multi_perform/poll/info_read, curl_easy_getinfo | curl/curl.h, message read before removal |
| Data and capacity | CURLOPT_WRITEFUNCTION/WRITEDATA | curl/curl.h, caller-owned bounded callback destination |

## Reproduced tests and measurements

Environment: Linux 6.12.96+deb13-amd64, GCC 14.2.0, libcurl 8.14.1
(Debian package build; runtime and pkg-config versions agree), OpenSSL
3.5.7, Python 3.10.15. libcurl reports ASYNCHDNS and THREADSAFE. curl-config
reports --enable-threaded-resolver and --with-openssl. Minimum chosen is
7.85.0 for CURLOPT_PROTOCOLS_STR; this is not a release-wide support claim.

Commands are in the PoC README. Strict C11 warning-as-error build passed.
CTest passed all 12 transport cases plus 80 injected allocation failures in
both the ordinary and ASan/UBSan builds. LSan was enabled with detect_leaks=1;
there were no sanitizer reports. The ordinary run took about 3.5 seconds;
the sanitizer run about 4.7 seconds. These are single-machine observations.

| Case | Deterministic fixture / assertion |
|---|---|
| success | Temporary self-signed localhost cert explicitly trusted; HTTP 200 and exact fragmented SSE-shaped body |
| untrusted | Same endpoint with system trust only; CURLE_PEER_FAILED_VERIFICATION, no body accepted |
| hostname | Trust cert, URL uses 127.0.0.1 while cert SAN contains only localhost; same TLS failure, no body |
| cancel-connect | Linux listener backlog filled without accepting; at 100ms getpeername is ENOTCONN and connect timestamp zero; remove before completion |
| cancel-tls | TCP accepted, no ServerHello; getpeername confirms TCP peer and app-connect timestamp zero; remove at 100ms |
| cancel-stall | Verified TLS and partial declared HTTP body; user-cancel trigger after 100ms total elapsed |
| stall-timeout | Same body fixture; application timer removes request after 100ms without received data |
| connect-timeout | No ServerHello; CURLE_OPERATION_TIMEDOUT after configured 800ms, asserted within 700–1200ms |
| multi | Three simultaneous requests across two contexts; cancel stalled request; complete its peer; destroy A; submit and complete another request on B |
| teardown | Destroy context with active stalled request; matching easy and multi cleanup |
| overflow | 1024-byte body into 128-byte destination; CURLE_WRITE_ERROR, cleanup |
| fairness | Zero-wait transport service after batches of 16 perpetually queued synthetic events; full body completes |
| OOM sweep | Fail each of first 80 curl allocation calls after explicit host init; setup/perform failures cleanly release partially constructed state |

Latest measured ordinary maximum service step was 11.277ms, cancel/remove
or active teardown up to 0.314ms; sanitizer maximum step was 14.408ms and
active teardown up to 0.938ms. Every non-OOM case asserts each service step
and cancel/active teardown below 150ms. Connect cancellation also asserts
that its decision occurs before 250ms. This is an empirical regression
ceiling, not an OS scheduling guarantee. The
[multi_poll contract](https://curl.se/libcurl/c/curl_multi_poll.html) bounds
its wait to the supplied budget or shorter curl timer; it does not bound
TLS CPU work, callbacks, allocation latency, resolver cleanup, or scheduling.

Curl observer counts return to zero blocks/bytes after cleanup in every
host, including all injected failures. /proc/self/fd counts also return to
the starting value in every case. A successful transfer peaks near 44.7KB
of observed curl allocations; the multi-context case near 97.6KB. Observers
count curl's allocator callbacks only, excluding independent OpenSSL,
resolver, libc, and system allocations. Realloc is instrumented by allocate,
copy, free, so transient peaks include the observer's replacement copy.
ASan/LSan checks process-wide allocations but system libcurl/OpenSSL binaries
are not themselves compiled with sanitizers. The first 80 faults do not
exhaust every allocation site or dependency failure path. No universal
memory ceiling or exhaustive OOM proof is claimed.

The sole mutable global state in lifecycle.c is **host test observation**:
atomic counters and a fault-injection selector required because curl's
process-global memory callbacks lack userdata. Backend contexts own all
transfer state; production Cog must not inherit these observer globals.
No initialization constructors, hidden reference count, or per-context
global init/cleanup calls exist.

## net.h compatibility and header-gate gaps

This is a mapping assessment, not ABI approval or a backend implementation.

| Existing surface | Fit / required decision |
|---|---|
| cog_net_new/free | Context-owned CURLM maps naturally; document host dependency precondition and cleanup ordering. Expose constructor failure/feature mismatch. |
| cog_net_config.max_connections | Bound easy slots explicitly; also specify curl connection/cache limits. A transfer bound alone does not prove total memory bound. |
| request(method/url/headers/body) | Curl easy handle supports methods/headers/body; define copied asynchronous ownership. Request body/options not exercised by this GET-only PoC. |
| poll(response) | Need bounded/nonblocking service semantics and a documented response lifetime; current call has no wait/deadline argument. Polling may allocate in curl. |
| response.chunk/headers | Define queue/buffer ownership and overflow/pause/retry semantics. PoC aborts overflow; does not prove lossless caller-capacity retry. |
| cancel(id) / ABORTED | Removing easy works for measured phases; define one terminal result with request identity and callback/cancel ordering. |
| connect_timeout/stall_timeout | Curl timeout covers connection including TLS; byte inactivity is an application timer. Header stall wording currently does not define phases/clock. |
| max_retries/backoff | Keep real-client blueprint's single application-owned retry budget; disable transport replay. Resolve the older net.h autonomous-retry contract. |
| HTTP vs transport failure | HTTP status and CURLcode differ. Need provider-neutral error metadata distinguishing TLS, DNS, connect, timeout, overflow and cancellation. |
| active_count/ids/status | Map slots/IDs per context; specify completion/cancel retention and unknown IDs. |
| cog_net_fd | A CURLM manages multiple sockets plus timers; there is no single libcurl fd to return. Wake-only fd is insufficient to drive sockets/timers. |

Core service fairness is a prerequisite (delivery `bcd9c0a`). Service the
transport nonblockingly on **every bounded core-loop service cycle**, even
when the core event queue remains nonempty. Then wait only within the next
transport/core deadline, including cancellation, stall and retry timers.
The synthetic fairness case demonstrates that service ordering; it does
not validate the present core implementation. Do not put all net work only
on the queue-empty/wait branch or wait for a wake-only eventfd to advance
curl. For multiple contexts, two blocking 10ms pumps accumulate latency;
use zero-wait service per context and one host wait. An eventual socket/timer
integration may use
[curl_multi_socket_action](https://curl.se/libcurl/c/curl_multi_socket_action.html)
with an application-owned poll/epoll aggregation, or bounded multi_poll with
extra core wake fds. That integration requires a header decision and tests.

## Dependencies, licenses and packaging

The PoC dynamically links system libcurl via pkg-config's imported target;
no vendoring or new root-build dependency is introduced. libcurl is under
the [curl license](https://curl.se/docs/copyright.html), SPDX curl (MIT-like,
retain required notices). The tested TLS backend is OpenSSL 3.5.7 under
Apache-2.0, confirmed by /usr/share/doc/libssl3t64/copyright. Python and the
openssl executable are test-only. Production package metadata must declare
libcurl development/runtime requirements and preserve static link closure
if static builds are supported; this PoC validates dynamic linking only.

The distribution libcurl brings a larger optional dependency closure than
HTTPS alone: ldd showed nghttp2/nghttp3, idn2/unistring, rtmp, ssh2, psl,
OpenSSL, GSSAPI/Kerberos, LDAP/SASL, zstd, brotli and zlib, with further
GnuTLS/nettle/GMP/p11-kit/ffi dependencies. Their licenses vary and must be
included in a binary redistribution inventory; curl's license does not
cover that closure. No claim of a fully audited distributable bundle.
Inspect the actual shipped configuration and each installed package's
copyright file rather than inferring a universal curl dependency set.

## Research passes and limits

Pass 1 surveyed lifecycle, allocator and header boundaries before writing
private experiment interfaces. Pass 2 traced verified success, two TLS
failures, TCP/TLS/body stalls, multiple contexts and active teardown into
executable fixtures. Pass 3 added bounded service, synthetic queue fairness,
capacity overflow, allocation faults, descriptor accounting and sanitizer
cleanup. Final self-review separates observations from guarantees. These
are research passes, not the required three public-header review passes.

Still unproven: DNS behavior across untested resolver/build profiles, all dependency allocations,
hard memory caps, TLS certificate expiry/revocation policy, proxies,
HTTP/2 behavior, SIGPIPE behavior on all TLS builds, static packaging,
request-body ownership, curl pause/resume lossless streaming, provider
protocols, and real cog_core_run fairness. No live provider/API call was
made. The local backlog fixture is Linux-specific and explicitly fails if
it cannot establish saturation. Timing may fail on severely loaded hosts;
that failure should be investigated rather than recorded as a pass.

[Thread safety guidance](https://curl.se/libcurl/c/threadsafe.html) covers
asynchronous resolver and signal requirements. Global cleanup does not wait
for resolver threads under the
[cleanup contract](https://curl.se/libcurl/c/curl_global_cleanup.html), so
plugin unloading and DNS shutdown cannot be certified by these localhost
measurements. The DNS followup adds a concrete default-threaded-resolver
counterexample and a c-ares-only supported candidate. Keep both research
tickets open until independent review, and carry these adoption gates into
the shared blueprint/header work.


## DNS followup: c-ares required for the supported profile

Child research ticket `23ae6c8`. Reproducible probe sources, pinned dependency
hashes, authenticated setup commands and offline runner are in
[pocs/https-lifecycle/dns](../../pocs/https-lifecycle/dns/README.md).
No system packages or resolver configuration were changed. Downloads and
custom build stayed in /tmp; no downloaded source or binary is committed.

The installed curl 8.14.1 reports ASYNCHDNS but ares=NULL. Its DNS_SERVERS
option returns 48 (CURLE_UNKNOWN_OPTION). In the isolated test host, an
LD_PRELOAD getaddrinfo shim sleeps two seconds then returns EAI_AGAIN. At
100ms the multi pump remains responsive, but default removal **joins the
resolver thread**, blocking cancellation for another 1.893 seconds. This is
not a real wire-DNS experiment: it deliberately controls the libc resolver
latency without touching system configuration. It directly disproves a
universal bounded-cancel claim for this backend. The pinned
[curl threaded resolver source](https://github.com/curl/curl/blob/curl-8_14_1/lib/asyn-thrdd.c)
explains why shutdown waits unless quick_exit is enabled.

A separately built curl 8.14.1 with ENABLE_ARES=ON and
ENABLE_THREADED_RESOLVER=OFF uses extracted Debian c-ares 1.34.9. A local
UDP server receives DNS requests but sends no answers. Three cancellations
and one configured 800ms timeout all complete with one host thread throughout,
including immediately after easy/multi/global cleanup. Final descriptor counts
match baseline. This fixture never performs an external DNS/provider request.

| Probe | Latest observed result |
|---|---|
| Distro threaded resolver, cancel around 107ms | remove_handle 1893.377ms; remaining cleanup prompt after resolver joined |
| Same resolver with QUICK_EXIT | remove_handle 0.043ms; **two threads remain after global cleanup**; one thread only after explicit 2.2s settling delay |
| c-ares, three actual pending-query cancels | remove_handle 0.045–0.082ms; easy cleanup 0.017–0.030ms; multi cleanup 0.012–0.027ms; global cleanup 0.002–0.004ms |
| c-ares, real blackhole DNS timeout | CURLE_OPERATION_TIMEDOUT at 800.807ms; no residual thread or descriptor |

The same c-ares-only build also passed all 12 existing TLS/transport cases
and the 80 curl allocation-failure cases. DNS-specific dependency OOM paths
remain outside that earlier allocator sweep.

The offline suite observes eight blackholed DNS packets, checks every
c-ares service/cancel/cleanup step under 150ms, and verifies no resolver
thread remains immediately after cleanup. These are measured regression
ceilings on this host, not universal latency guarantees or full dependency
allocation/OOM coverage. c-ares cancellation and destruction synchronously
notify outstanding queries and release channel resources under its
[ares_cancel](https://c-ares.org/docs/ares_cancel.html) and
[ares_destroy](https://c-ares.org/docs/ares_destroy.html) contracts. Callbacks,
allocator latency, scheduler delays and config/filesystem operations remain
outside a hard real-time guarantee.

### Backend checks and lifecycle contract

For the initial supported package, record curl's **actual build options**:
ENABLE_ARES=ON and ENABLE_THREADED_RESOLVER=OFF. Require runtime ASYNCHDNS,
a nonnull curl_version_info().ares identifying the tested c-ares version,
and successful c-ares-only DNS_SERVERS option support on a temporary easy
handle (reset to system defaults before real use). For the tested curl
8.14.1 source, DNS_SERVERS is compiled only for CURLRES_ARES. The
[option documentation](https://curl.se/libcurl/c/CURLOPT_DNS_SERVERS.html)
also states that it needs c-ares. Fail context construction clearly if the
supported profile cannot be established; never silently fall back.

A nonnull ares string alone is insufficient: curl can build c-ares alongside
a **threaded primary A/AAAA resolver** for HTTPS resource records. Reject
such combined builds (reported as asyn-rr where available), retain build
provenance, and rerun the controlled DNS cancellation suite for every shipped
configuration. c-ares 1.34.9 and curl 8.14.1 are the reproduced versions, not
a claim that every version above them has identical latency. Production
must use maintained security-patched packages; the unpatched upstream curl
archive here is for local isolated experimentation only.

The host explicitly initializes curl before contexts and cleans it up after
all contexts, thereby also owning curl's dependency initialization; Cog
adds no global resolver pointer or allocator/refcounter workaround. Keep
request and context ownership explicit. The c-ares experiment adds no Cog
state, callback registration system, or implicit startup.

Do **not** use
[CURLOPT_QUICK_EXIT](https://curl.se/libcurl/c/CURLOPT_QUICK_EXIT.html) as an
embedding fix: its documented purpose permits abandoned cleanup when the
program is about to exit. The experiment confirms the resolver thread can
outlive easy/multi/global cleanup. Keeping allocator callbacks and code loaded
until arbitrary DNS returns would undermine the clean destructor contract.

If a host cannot supply the supported resolver, the smallest honest fallback
contract is to refuse DNS-dependent starts, or accept explicit host-supplied
resolved routes through
[CURLOPT_RESOLVE](https://curl.se/libcurl/c/CURLOPT_RESOLVE.html) while preserving the URL hostname
for TLS/SNI verification. That alternative must disable implicit proxy,
redirect or other fallback resolution and make TTL/refresh ownership explicit;
it is not implemented or validated here. Another honest contract could
separate immediate **logical cancel** from potentially unbounded resolver
**drain/free**, but that does not satisfy this release's bounded reusable
shutdown goal. A worker process could isolate blocking DNS, at substantial
extra lifecycle complexity. Prefer the c-ares requirement.

### Dependency verification

Curl source archive signature was verified with GPG against fingerprint
27EDEAF22F3ABCEB50DB9A125CC908FDB71E12C2 from the
[official verification page](https://curl.se/docs/verify.html); GPG emitted
VALIDSIG for that fingerprint. Its SHA256 matched the archive used for the
exploratory build. The exact c-ares .deb files were fetched with apt-get
download, and their SHA256 values match apt's authenticated repository
metadata. Source/package URLs, digests and minimal reconstruction commands
are preserved in the DNS README/SHA256SUMS. c-ares uses the MIT license,
curl uses SPDX curl, and OpenSSL uses Apache-2.0. This does not replace the
complete binary dependency/license inventory required for packaging.

# Relocatable admitted dependency profile: writer evidence

Ticket `ba50799`, baseline `b0da8a6`, branch `codex/build-relocatable-curl`.
The R18 author found that the earlier development curl RUNPATH and pkg-config
metadata referenced `/tmp/cog-dns-research`; ordinary install changed the curl
bytes that Cog had admitted. This delivery supplies a genuine normal-install and
relocation profile without changing net admission, public headers, app code or
system installations. Independent profile review and final P acceptance remain
pending.

The [recipe](../libraries/cog-net/cmake/relocatable/README.md) and its explicit
source-template patches build authenticated curl 8.23.0 and c-ares 1.34.9.
Curl archive signature verification uses pinned signer
`27EDEAF22F3ABCEB50DB9A125CC908FDB71E12C2`; the archive SHA-256 is
`8dadd7d2536a314f982ad05a651b713f426bf7d02cc2c76cffb9796f2218e6c5`.
C-ares' pristine upstream archive SHA-256 is
`9364b70a12eb59a06830fb488d9357f90100dff0d98b23ea3593a183d43e6f2f`.
It was fetched using `apt-get source --download-only` for exact version
`1.34.9-1~deb13u1`, with no apt update/install. The recipe independently verifies
the existing Debian InRelease signature, its Valid-Until, its complete Sources
index hash, and all source-package digests against that record. Upstream c-ares/
maintainer signatures are not separately claimed; authenticated repository hashes
provide that source authentication. Corrupt curl/archive and Sources-index probes
both fail before creating an output/build directory.

The [unaltered generated receipt](../libraries/cog-net/cmake/relocatable/recorded-profile.json)
records those checks, every installed-file digest, compiler/platform, source patch
and recipe digests, actual build-cache hashes and artifact locations. Initial
build `.../final` and fresh notice-bearing `.../shipped` use different output roots
and produce identical library bytes:

| Final installed object | SHA-256 |
|---|---|
| libcurl.so.4.8.0 | `ae1a9e77488254d744c9064972b025d8eb7210e73ea900cd5f3632995bb29d18` |
| libcares.so.2.19.8 | `b4d2377241c646dc2054e864e403f162d4b5969823bdd86ec5ff49ecf7e79f53` |

The exact admitted artifact is
`/tmp/cog-relocatable-profile/shipped/curl-build/install/lib/libcurl.so`.
Its genuine audited build/cache is `/tmp/cog-relocatable-profile/shipped/curl-build`;
c-ares is normally installed from `/tmp/cog-relocatable-profile/shipped/cares-build`.
That first prefix is naturally inside the real curl build, satisfying existing
Cog configure-time provenance. No finished library/cache is copied back to satisfy
the ancestry check. BUILD_WITH_INSTALL_RPATH=ON and `$ORIGIN` are set at link time;
ordinary installs into new prefixes preserve every installed byte, including the
normal-installed COPYING/LICENSE.md notices. No patchelf, chrpath or post-install
binary rewrite is used.

The source patches make pkg-config prefixes and curl-config relative to their
installed location, replace private build-prefix curl metadata with this fixed
shared profile's c-ares/OpenSSL targets/library directory, and install upstream
notices. They do not change resolver/transport C code. ENABLE_ARES stays ON,
ENABLE_THREADED_RESOLVER OFF and OpenSSL selected. Unused curl LDAP/HTTP2 and
zlib/brotli/zstd integrations are disabled; net still uses its approved HTTP/1.1
fresh-connection/no-replay profile. C-ares thread-safety support is enabled but
curl's c-ares resolver produces no resolver/event thread in the probes.

## Installed closure and DNS evidence

The [recorded audit](../libraries/cog-net/cmake/relocatable/recorded-audit.json)
comes from a new normal install of both real builds, actual directory relocation,
complete installed-file digest comparisons and metadata/path audits. Standalone
CMake and pkg-config consumers use only relocated dependency headers/packages;
no development include paths or manually staged headers are supplied. Actual
loader mappings select relocated curl/c-ares, with library RUNPATH exactly
`$ORIGIN` and executable paths `$ORIGIN/../lib`. No LD_LIBRARY_PATH or LD_PRELOAD
is used. The genuine final bytes construct/destroy an actual Cog net context;
a separately labeled test copy with altered trailing bytes returns UNSUPPORTED
under unchanged admission. Original artifacts/provenance are never modified.

Ten real pending-query cancellations and three 800 ms timeouts send 26 packets
to an offline local DNS blackhole. Every existing probe assertion passes:
ASYNCHDNS, c-ares 1.34.9, DNS_SERVERS support, no asyn-rr, one host thread before,
during and after cleanup, restored descriptor count 4, and measured service/
cancel/cleanup ceilings. Timings are recorded observations on this host, not a
hard real-time guarantee. There is no external DNS or provider traffic.

This is a relocatable dependency bundle with explicit host requirements. Direct
curl ELF dependencies are libssl.so.3, libcrypto.so.3, bundled libcares.so.2 and
libc.so.6; c-ares directly needs libc.so.6. Actual symbol requirements include
GLIBC_2.38 and OPENSSL_3.2.0. Tested host OpenSSL 3.5.7 further loads host libz.so.1
and libzstd.so.1. The report records their paths/hashes; the host supplies these
OS/TLS libraries and `/etc/ssl/certs/ca-certificates.crt`. They are not bundled,
made relocatable or security-maintained by this recipe. Curl's existing runtime
hash check pins curl, while receipt/path audits record the bundled c-ares bytes;
trusted loader/filesystem deployment remains the host's responsibility.

## Focused actual Cog checks

Release/O3 WERROR GCC, Release/O3 WERROR Clang, and separate Clang ASan/UBSan builds
against **final installed** curl each pass 29 focused tests: **87/87**. Sanitizers
use `detect_leaks=1:halt_on_error=1` and UBSan halt-on-error/stack traces. Dependency
shared libraries remain ordinary Release builds, as in the accepted F/H profile;
this is not a claim that all third-party code is sanitizer-instrumented.

The selection includes all unit-net admission/HTTPS/OOM/observer/completion/cancel/
replay/auth/redirect/proxy tests; generic provider/preflight/mock/actual OpenRouter;
actual offline provider HTTPS and core-provider integration; and actual CLI loop,
HTTPS, stop, memory, save, limits, session UX and both reload suites. The build
logs/caches are `/tmp/cog-relocatable-profile/cog-{gcc,clang,asan}`. Configuration
uses the installed prefix/CURL config and `COG_NET_CURL_LIBRARY` above, with the
real curl build/cache. No development curl or c-ares RUNPATH is involved.

Reproduce the CTest set after building its named targets:

```sh
ctest --test-dir BUILD --output-on-failure -j4 \
  -R '^(unit-net.*|unit-provider|unit-provider-critic|unit-provider-preflight-critic|unit-openrouter|unit-mock-provider|integration-core-provider|integration-openrouter-https|cli-app-(loop|https|stop|memory|save|limits|reload|reload-critic)|cli-session-ux)$'
```

The R18 author separately reports the full normal Cog install, two prefix
relocations, installed C11 API consumer and installed CLI passing under those
three compiler/sanitizer profiles using these same dependency bytes. Its fixture
`66527d0` and independent review remain separate from this recipe's review; this
writer record does not close R18, packaging, or release gates. No authenticated or
paid request, credential access or global installation occurred.

## Independent profile review

Different critic review of complete checkpoint `d7db7e4` passes **cycle 1**.
The nine-file scope contains recipe, metadata patches, provenance and evidence
only; no runtime/header/admission/app/root-CMake change is included.

Independent checks revalidated the curl detached signature against the pinned
signer and the fresh Debian InRelease -> complete Sources index -> exact four
source-package input chain. Corrupt curl archive, curl signature, c-ares archive
and Sources-index controls were rejected. Comparing all 4,515 curl and 532 c-ares
regular archive files with the built source trees found differences only in the
six declared metadata/install-template files; resolver/transport source is
unchanged. Recipe/profile/patch/cache digests and all 205 installed regular-file
hashes match the frozen receipt. Both notices equal their authenticated source
copies, all four installed symlinks resolve inside the prefix, and the two
separate output roots contain byte-identical curl/c-ares libraries.

The critic used a private copy of the complete, already normally installed
prefix for a relocation replay, preserving symlinks and every recorded byte.
The two dependency-install commands were intentionally skipped in that replay;
it does not claim another genuine dependency install. All subsequent audit
checks ran: ELF/RUNPATH/host closure, relocated CMake/pkg-config consumers,
actual loaded curl/c-ares paths, unchanged Cog admission, altered-test-copy
rejection, and thirteen local DNS cancellation/timeout probes with 26 queries.
SHA-256 contents and nanosecond modification times of all 5,979 frozen regular
files matched before/after this replay. No candidate file was changed by it.
Receipt `/tmp/dependency-profile-critic-audit.json` has SHA-256
`66485318e3755eede0d5bdb8ab1df237511e078885f408d3736a1818c5c59043` and
explicitly records the private-copy staging mode.

The separate R18 critic independently completed genuine normal installs and
relocations under GCC, Clang and sanitizers at fixture `66527d0`, with matching
library identities and loader receipts
`/tmp/cog-r18-critic-{gcc,clang,asan}-report.json`. Ordinary `cmake --install`
updates generated dependency `install_manifest.txt` bookkeeping; it does not
reconfigure/relink or alter the accepted source receipts, caches, recipes or
runtime bytes. Profile freeze refers to those acceptance identities, rather
than immutability of installer bookkeeping. The read-only review avoided further
manifest races with R18.

No profile blocker was found. Existing host OS/OpenSSL/trust-store requirements
remain explicit; this is not a self-contained OS bundle or an admission-policy
change. Main integration, the R18 join and full P/O validation remain separate.

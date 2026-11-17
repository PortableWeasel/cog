# Curl dependency profile

Default Cog builds use the [pinned vendor wrappers](../../../vendor/README.md)
for curl, c-ares and OpenSSL. The external recipes below apply when configuring
Cog with `-DCOG_USE_VENDORED_LIBS=OFF`. Runtime admission is the same in both
modes.

Cog requires an authenticated, security-maintained curl build with the c-ares
resolver selected and the threaded resolver disabled. Select a patched version
appropriate for the deployment; the experimental 8.14.1 archive in the DNS
research is not a production recommendation. Authenticate source and c-ares
packages before building, retain their provenance and rerun the DNS cancellation
and descriptor/thread tests in `pocs/https-lifecycle/dns/README.md` on the exact
artifacts shipped. The profile alone is configuration, not proof that the
runtime-loaded library is the one that was configured.

## Workspace defaults

In external mode, Cog automatically uses `build-deps/curl` at the checkout root when that directory
contains a configured curl build. Build dependencies on disk in this gitignored
folder, using authenticated sources and the profile below. For example, with
`CURL_SOURCE` and `CARES_PREFIX` pointing to prepared inputs:

```sh
cmake -S "$CURL_SOURCE" -B "$PWD/build-deps/curl" \
  -C "$PWD/libraries/cog-net/cmake/curl-cares-profile.cmake" \
  -DCARES_INCLUDE_DIR="$CARES_PREFIX/include" \
  -DCARES_LIBRARY="$CARES_PREFIX/lib/libcares.so"
cmake --build build-deps/curl --parallel 2
cmake -S . -B build -DCOG_ENABLE_WERROR=ON -DCOG_USE_VENDORED_LIBS=OFF
cmake --build build --parallel 2
```

The default library is `<curl-build>/lib/libcurl.so`; matching headers come from
`CURL_SOURCE_DIR` in its cache. For another layout, set
`COG_NET_CURL_BUILD_DIR`, `COG_NET_CURL_LIBRARY`, and optionally
`COG_NET_CURL_INCLUDE_DIR`. The selected artifact controls both FindCURL linking
and runtime admission, including when reconfiguring a build that previously
selected system curl. Nonempty explicit Cog settings take precedence; empty
build/library settings request automatic defaults. Explicit paths are cached for subsequent builds; automatic defaults are
recomputed so changing the selected curl build also selects its matching headers.

Without a local dependency or explicit selection, configuration reports that
live transport is disabled; mock/offline builds remain available. Resolver flags,
artifact containment and the loaded-library digest are still checked. This
selection neither downloads dependencies nor authenticates source on your behalf.

A reproducible external configuration (paths must be absolute) is:

```sh
cmake -S "$CURL_SOURCE" -B "$CURL_BUILD" \
  -C "$COG_SOURCE/libraries/cog-net/cmake/curl-cares-profile.cmake" \
  -DCMAKE_INSTALL_PREFIX="$CURL_PREFIX" \
  -DCARES_INCLUDE_DIR="$CARES_PREFIX/include" \
  -DCARES_LIBRARY="$CARES_PREFIX/lib/libcares.so"
cmake --build "$CURL_BUILD" -j4
cmake --install "$CURL_BUILD"
```

Inspect `CMakeCache.txt` for `ENABLE_ARES=ON` and
`ENABLE_THREADED_RESOLVER=OFF`; retain the authenticated source identity,
cache, compiler settings, installed library digest and DNS test results as
part of the dependency provenance. Verify the deployed loader resolves that
same artifact; reporting `ASYNCHDNS` or a c-ares version alone cannot rule out
a hybrid resolver. The host owns curl global initialization and cleanup.

## Recorded candidate, 2026-11-15

The [curl release table](https://curl.se/docs/releases.html) identifies 8.23.0
as the latest release at this date; the [curl security table](https://curl.se/docs/security.html)
records the preceding release's published fixes. The
[c-ares download page](https://c-ares.org/download/) identifies 1.34.9 as the
latest release; its [changelog](https://c-ares.org/changelog.html) describes
its security fixes. Recheck both advisories before each deployment. Use patched
successors if either candidate becomes vulnerable.

The upstream `curl-8.23.0.tar.xz` SHA-256 is
`8dadd7d2536a314f982ad05a651b713f426bf7d02cc2c76cffb9796f2218e6c5`.
Authenticate its detached signature as described in the DNS research recipe;
the observed signing fingerprint was
`27EDEAF22F3ABCEB50DB9A125CC908FDB71E12C2`. The tested c-ares was the
1.34.9 Debian security package authenticated as described in that recipe.
These digests do not replace signature and package-repository verification.

To enable the backend with an external build, configure Cog with
`COG_NET_CURL_BUILD_DIR` and `COG_NET_CURL_LIBRARY` pointing to the audited
build cache and its shared curl artifact. Matching FindCURL settings are selected
automatically; `COG_NET_CURL_INCLUDE_DIR` can select installed headers. Cog checks the cache resolver flags,
hashes that artifact at configure time and compares the actually loaded curl
object to the recorded digest at context construction. A missing profile,
mismatch, or unverifiable loaded object fails closed. Deployments must protect
the library path against modification during process startup and construction;
this local digest check is not a substitute for a trusted loader/filesystem.
Installed exported targets contain no build-tree dependency paths and find the
consumer's curl and OpenSSL; the runtime curl must match the audited digest.

For the recorded candidate, the curl archive and signature can be acquired and
verified as follows, using the signing key authenticated independently from
[curl's verification page](https://curl.se/docs/verify.html):

```sh
curl -fL --proto '=https' https://curl.se/download/curl-8.23.0.tar.xz -o curl-8.23.0.tar.xz
curl -fL --proto '=https' https://curl.se/download/curl-8.23.0.tar.xz.asc -o curl-8.23.0.tar.xz.asc
printf '%s  %s\n' 8dadd7d2536a314f982ad05a651b713f426bf7d02cc2c76cffb9796f2218e6c5 curl-8.23.0.tar.xz | sha256sum -c -
gpg --status-fd 1 --verify curl-8.23.0.tar.xz.asc curl-8.23.0.tar.xz > verify.status
rg '^\[GNUPG:\] VALIDSIG 27EDEAF22F3ABCEB50DB9A125CC908FDB71E12C2 ' verify.status
tar -xf curl-8.23.0.tar.xz
```

Use the authenticated Debian c-ares package and checksums in the DNS research
recipe, or independently authenticate an updated package. Run the same DNS
probe against the resulting curl and c-ares libraries. The probe passed for the
recorded candidate on this host, including cancellation, timeout, descriptor
and thread checks; it is not a guarantee for a different build or host.

The standalone net export depends on the shared public `cog/result.h` installed
by cog-core. Net-only staging for development must provide that header as well;
it is not evidence of a full project install. Full packaging is validated once
sibling packages are migrated.

## Final installed/relocatable profile

Gate-P dependency delivery `ba50799` adds a [normal-install relocatable recipe](relocatable/README.md)
for these exact authenticated source versions. Use its final installed curl bytes
and real audited build/cache for admission. The older development paths above
alone do not establish a shipped runtime closure: their RUNPATH/pkg-config paths
and install-time ELF changes motivated this separate delivery. The new recipe
sets deliberate `$ORIGIN` paths at link time, packages adjacent c-ares and source
notices, audits every normal-installed byte after relocation, and records host TLS/
OS requirements. [Writer evidence](../../../docs/REAL_CLIENT_DEPENDENCY_PROFILE_EVIDENCE.md)
separates those results from independent acceptance and the remaining P/R18 gates.

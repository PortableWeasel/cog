# Curl dependency profile

Cog requires an authenticated, security-maintained curl build with the c-ares
resolver selected and the threaded resolver disabled. Select a patched version
appropriate for the deployment; the experimental 8.14.1 archive in the DNS
research is not a production recommendation. Authenticate source and c-ares
packages before building, retain their provenance and rerun the DNS cancellation
and descriptor/thread tests in `pocs/https-lifecycle/dns/README.md` on the exact
artifacts shipped. The profile alone is configuration, not proof that the
runtime-loaded library is the one that was configured.

A reproducible local configuration (paths must be absolute, outside the source
tree) is:

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

To enable the backend, configure Cog with `CURL_INCLUDE_DIR`, `CURL_LIBRARY`,
`COG_NET_CURL_BUILD_DIR`, and `COG_NET_CURL_LIBRARY` pointing to the audited
build cache and its shared curl artifact. Cog checks the cache resolver flags,
hashes that artifact at configure time and compares the actually loaded curl
object to the recorded digest at context construction. A missing profile,
mismatch, or unverifiable loaded object fails closed. Deployments must protect
the library path against modification during process startup and construction;
this local digest check is not a substitute for a trusted loader/filesystem.
Installed exported targets contain no build-tree dependency paths and find the
consumer's curl and OpenSSL; the runtime curl must match the audited digest.

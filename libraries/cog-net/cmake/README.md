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

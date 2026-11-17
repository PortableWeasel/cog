# Vendored dependencies

Each dependency has a pinned, pristine Git submodule at
`vendor/<upstream-name>/upstream` and Cog-owned build rules alongside it.

| Dependency | Pinned release | Purpose |
| --- | --- | --- |
| curl | 8.23.0 | HTTP/TLS transport with the c-ares resolver |
| c-ares | 1.34.9 | Non-threaded DNS integration for curl |
| openssl | 3.5.7 | TLS and runtime artifact hashing |
| yyjson | 0.12.0 | Private, separately namespaced provider/toolkit JSON |

Initialize sources with `git submodule update --init --recursive`. Configuration
and builds use local source; they never download or advance dependency revisions.
Review and update the gitlinks explicitly when upgrading dependencies.

## Build contract and design review

The networking dependencies build out of tree under the active Cog build's
`vendor/` directory. Their common install prefix is `vendor/curl-build/install`;
the curl binary directory is the real `vendor/curl-build`. This preserves the
existing requirement that the admitted curl artifact comes from inside its
actual configured build. The normal Cog configure prepares these dependencies
before hashing the final curl shared object. Subsequent configures reuse build
stamps. Public Cog headers and runtime ownership contracts do not change.

The default build uses these dependencies. An explicit system/external mode
retains the previous audited dependency selection for integrators. No runtime
admission check is relaxed: the actual loaded curl digest, c-ares resolver and
no-thread configuration must still match. yyjson is built twice from one source
revision, with separate private symbol prefixes.

Pass 1 identified source/build/install ownership. Pass 2 traced configure ->
dependency build -> exact artifact hash -> Cog link -> installed CLI -> live
query, including clean checkout, missing submodules and reconfigure cases.
Pass 3 independently reviewed installed runtime closure, cached dependency
selection, source-change invalidation and build regressions. The existing R02 turn trace covers the runtime calls; no new public
API is introduced. Missing sources must produce an actionable error.

## Usage

```sh
git submodule update --init --recursive
cmake -S . -B build -DCOG_ENABLE_WERROR=ON
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure -j4
cmake --install build --prefix "$PWD/install/stage"
```

The first configure builds the networking dependencies and may take several
minutes; later configures reuse the build. Logs live in `build/vendor/`. Set
`COG_VENDOR_JOBS` to control dependency parallelism. Build prerequisites include
a C compiler, CMake, Python 3, Perl, make, Git and patch. The admitted vendored
network profile currently supports Linux with `CMAKE_INSTALL_LIBDIR=lib` and
host trust data at `/etc/ssl`; it ships shared libraries with relative loader
paths. `-DCOG_USE_VENDORED_LIBS=OFF` selects the previous external networking
configuration; yyjson remains private vendored source in either mode.

OpenSSL and c-ares build from their upstream pins. Curl and c-ares packaging-only
patches are applied to build-tree copies; the submodules stay pristine. Retained
licenses accompany installed dependencies. System C/POSIX runtime, compiler
runtime and trust certificates remain host dependencies.

## Validation

The initial migration passed all 187 workspace tests, including installed
consumer relocation and public header smoke, plus a fresh Release/Werror build.
The relocated CLI and OpenSSL executable resolve the bundled shared libraries.
A live text-only CLI query succeeded with tools and retries disabled. Missing
submodules fail configuration with initialization instructions. Private yyjson
exports match the prior provider/toolkit copies; the prefix-map check is part
of CTest.

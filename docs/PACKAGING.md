# CPack distributions

The supported binary profile is Linux on the build architecture with the
vendored shared curl, c-ares and OpenSSL dependencies. The archive includes
Cog CLI, public headers, static libraries, CMake exports, shared dependencies
and their installed license notices. Runtime shared libraries use relative
search paths. System libc and a host CA certificate store remain required.
Binary portability is limited by the build host libc/architecture; build on
the oldest supported distribution. No live provider requests are packaging tests.

```sh
git submodule update --init --recursive
cmake -S . -B build-dist -DCMAKE_BUILD_TYPE=Release \
  -DCOG_USE_VENDORED_LIBS=ON -DCOG_ENABLE_WERROR=ON
cmake --build build-dist --parallel 4
ctest --test-dir build-dist --output-on-failure --parallel 4
cpack --config build-dist/CPackConfig.cmake -B build-dist/packages
cpack --config build-dist/CPackSourceConfig.cmake -B build-dist/packages
```

TGZ is the default; override with `cpack -G ZIP` or `-DCPACK_GENERATOR=...`
at configure time. SHA256 sidecars accompany archives. Binary archive names
include version, OS and architecture. Extract the binary archive into a prefix
and run `PREFIX/bin/cog-cli --mock --no-store --prompt smoke --chunks installed`.
A separately packaged Cog TUI may be extracted into the same prefix.

The source archive contains populated pinned upstream sources and
`vendor/source-revisions.cmake`; Git metadata and local build/install/agent
state and other untracked checkout files are excluded. An extracted archive builds with the same commands above,
without the submodule initialization step. In Git checkouts, upstream sources
must still be clean submodules. Regenerate the revision map when updating pins.
Archive checksums establish transport integrity, not authenticity; sign release
artifacts through the release process if required.

When `COG_USE_VENDORED_LIBS=OFF`, packaging follows the caller's dependency and
RPATH policy and does not bundle external dependencies. Such an archive needs
its own dependency/relocation validation and is outside the verified bundle profile.
Optional DEB/RPM/STGZ generators have no maintained native dependency policy.

## Licensing limitation

This repository currently has no project-level LICENSE file. These packages
are technical distribution candidates; the project owner must establish Cog's
redistribution terms before a public release. Dependency license notices do
not license Cog. CPack installs a project LICENSE automatically if one is
provided in the future; no license terms or contact address are invented here.

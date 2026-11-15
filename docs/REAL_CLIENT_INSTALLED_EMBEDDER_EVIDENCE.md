# Permanent installed C11 embedder fixture

Ticket `8f13685`, based on R12-integrated main `b0da8a6`. The permanent
[integration fixture](../tests/external-consumer/README.md) owns a separate CMake
project, public C11 host and normal-install/relocation driver. Independent
acceptance remains required. Final profile approval is tracked by `ba50799` and
integration join `b61fb1b`; this evidence does not close release gate P.

## Concrete packaging defects and approved scope

A genuine consumer of the **full normal project install** initially failed CMake
generation: the documented `cog::config` target was absent, while the export and
CLI transitive graph used `cog::cog-config`. Child `4d68a50` corrects only
`EXPORT_NAME config` (`16a4f19`); the installed/relocated consumer now links the
canonical name. No application source or public header changed.

The application owner supplied the source-level Linux installed CLI relative
runtime default under `7527e7d` (`f2e2a0b` plus `b5d589d`, incorporated as
`5ab8867`/`9a337f5`). The followup preserves explicit caller RPATH/skip/link-path
policy and legitimate absolute/parent GNUInstallDirs configurations. Only a
suitable relative layout with no caller policy receives the relative default.
The strict bundled fixture rejects unsuitable layouts separately; normal builds
do not acquire a blanket installation restriction. Application-owner default,
nested-libdir, custom/empty policy and absolute-layout proofs remain that
child's evidence.

The historical development curl artifact was **not** counted as shipped
acceptance. Its build RUNPATH points into a foreign `/tmp` c-ares prefix, its
normal installed bytes differ after RPATH stripping, and its pkg-config metadata
contains temporary paths. Those findings triggered the separate dependency
profile task rather than environment overrides or binary/header staging.

## Normal install and relocation checks

The fixture normally installs c-ares, curl and all Cog install rules into one
fresh prefix, including dependency notices. It audits installed CMake,
pkg-config and curl-config metadata, then renames the prefix. A separate CMake
process consumes only installed package configs/raw target exports and public
headers; its source is copied into a temporary directory with no private Cog
headers. The provider/net package configs must supply CURL/OpenSSL imports;
Threads is explicitly imported for the existing raw toolkit export. All eleven
public/imported component targets are checked, including the header-only result
and CLI configuration targets. No package-config framework was added.

The standalone host is normally installed and the entire prefix is renamed a
second time. ELF runtime paths must be relative and remain inside the relocated
prefix. Actual loader paths, imported target locations and compile commands are
audited; both curl and c-ares must load from that final prefix. The driver clears
loader, compiler include/library and package-prefix environment overrides, and
never sets LD_LIBRARY_PATH. Platform OpenSSL/libc/zlib/zstd remain recorded system
dependencies. An audit JSON is written into each producer build directory.

The host implements all nine custom provider callbacks through the public
first-member base, including a bounded text-only preflight profile, missing
callback rejection before connect, copied inputs, retained overflow, typed
terminal/cancellation/error, legacy receive and matching disconnect. Pending
tool history is explicitly unsupported by that fixture profile and never sent.
Two caller-owned store contexts receive their exact identities; public snapshot
and restore round trips survive source-session destruction. Wrapper/context
reverse destruction is checked while the other store remains usable.

Two independent net contexts each own local request ID1. Reverse cancel, retained
terminal acknowledgement, release and free preserve the other context. Requests
are never serviced, so this lifecycle fixture sends no DNS/HTTP request. Linker
wrappers verify exactly one explicit host curl initialization and one final
cleanup, with none hidden inside Cog. Basic supported config/IPC/obs/toolkit and
mock CLI lifecycles also link and run. Orchestrator is symbol/argument-error
closure only; pending O async/handoff/runtime APIs are not claimed.

## Candidate-profile validation

The final notice-bearing installable profile is:

- curl build/cache: `/tmp/cog-relocatable-profile/shipped/curl-build`
- c-ares build: `/tmp/cog-relocatable-profile/shipped/cares-build`
- admitted final prefix: `curl-build/install` inside that curl build
- runtime curl8.23.0 SHA256:
  `ae1a9e77488254d744c9064972b025d8eb7210e73ea900cd5f3632995bb29d18`
- runtime c-ares1.34.9 SHA256:
  `b4d2377241c646dc2054e864e403f162d4b5969823bdd86ec5ff49ecf7e79f53`

The dependency recipe/manifest and source authentication are owned by `ba50799`;
R18 does not duplicate or waive that review. Cog's normal runtime provenance
check accepts the relocated final curl bytes in both net contexts.

Strict GCC Release, Clang Release and Clang Release with ASan/UBSan full producer
builds pass. The installed C11 host and installed CLI fixture pass under each
profile after two relocations; sanitizer runs enable leak detection and
halt-on-error. Checks remain active under Release through the host's CHECK macro.
No manual header staging, loader override, paid/live request or private header is
used. The fixture is registered only for the declared installable dependency
inputs; absence of that registration is not passing release evidence.

Original20/all R01-R18, standalone C++ headers, broad sparse-ID/audit restore edge
cases, final O APIs and full release P remain separate obligations.

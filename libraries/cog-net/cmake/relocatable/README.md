# Relocatable admitted curl/c-ares profile

Ticket `ba50799`. This Linux/ELF shared-library recipe builds the already selected
curl 8.23.0 and c-ares 1.34.9 from authenticated archives. It does not install
system packages, download during build, change Cog admission, or modify any
finished binary. The recorded build uses GCC 14.2.0, CMake 3.31.6, glibc 2.41 and
host OpenSSL 3.5.7 on x86_64 Debian. Different toolchains/hosts need their own
recorded artifact and checks.

`build_profile.py` verifies the pinned curl archive digest and detached signature
against the independently authenticated curl signer. For c-ares it verifies the
Debian InRelease signature using the supplied archive keyring, checks its current
Valid-Until and the complete Sources-index digest, then checks all four exact
source-package input digests against that authenticated record. It builds the
pristine upstream `c-ares_1.34.9.orig.tar.gz`. Debian's two packaging patches affect
tests/docs only and are not applied; no independent maintainer `.dsc` or upstream
c-ares signature verification is claimed. The signed repository record authenticates
those bytes. Keep the archives, signatures, trusted public keyrings and signed
repository inputs with the generated build receipt.

The source-template patches are explicit and hashed in `profile.json`:

- `libcurl.pc` and `libcares.pc` derive their prefix from `${pcfiledir}/../..`;
- `curl-config` derives its prefix from its installed `bin` directory;
- curl's private metadata uses the bundled library directory and imported
  c-ares/OpenSSL targets, avoiding build-prefix linker flags;
- normal CMake installs preserve curl's COPYING and c-ares' LICENSE.md notices.

They change packaging metadata/install rules only. Resolver and transport source
are unchanged. Curl keeps ENABLE_ARES=ON, ENABLE_THREADED_RESOLVER=OFF and OpenSSL;
unused LDAP, HTTP2, zlib/brotli/zstd curl integrations are disabled. The public net
profile still forces fresh HTTP/1.1 connections and its existing no-replay rules.
C-ares thread-safety support remains enabled; curl uses its non-threaded c-ares
resolver. Actual DNS tests, not that build flag alone, check residual thread/FD state.

Both libraries are linked with `BUILD_WITH_INSTALL_RPATH=ON` and installed RUNPATH
exactly `$ORIGIN`, with shared curl and c-ares adjacent under `lib`. Normal install
never rewrites those loader bytes. The first genuine install prefix is a child of
the genuine curl build directory. Consequently unchanged Cog build provenance
accepts the final installed library inside its actual audited build; no cache or
library is copied back to fake that relationship. Subsequent ordinary
`cmake --install --prefix` operations and directory relocation preserve the bytes.

## Build from prepared authenticated inputs

Reuse the recorded curl archive, detached signature and trusted public keyring
from the [existing profile](../README.md). If necessary, obtain the exact c-ares
source package using the configured authenticated Debian security repository:

```sh
mkdir -p /tmp/cog-relocatable-profile/source-inputs
cd /tmp/cog-relocatable-profile/source-inputs
apt-get source --download-only c-ares=1.34.9-1~deb13u1
```

No apt update/install is needed for the recorded host. Supply the matching signed
InRelease and uncompressed `main/source/Sources` index; their names below are this
host's apt-cache paths. Fresh metadata must still publish the exact version. An
aged-out package/index needs separately authenticated provenance, never a silent
version/hash substitution or disabled verification.

From the Cog checkout, with a **fresh, nonexistent** output directory:

```sh
python3 libraries/cog-net/cmake/relocatable/build_profile.py \
  --curl-archive /tmp/cog-net-maintained/curl-8.23.0.tar.xz \
  --curl-signature /tmp/cog-net-maintained/curl-8.23.0.tar.xz.asc \
  --curl-keyring /tmp/cog-dns-research/gnupg/pubring.kbx \
  --cares-inputs /tmp/cog-relocatable-profile/source-inputs \
  --debian-inrelease /var/lib/apt/lists/security.debian.org_debian-security_dists_trixie-security_InRelease \
  --debian-sources /var/lib/apt/lists/security.debian.org_debian-security_dists_trixie-security_main_source_Sources \
  --debian-keyring /usr/share/keyrings/debian-archive-keyring.gpg \
  --output /tmp/cog-relocatable-profile/shipped --jobs 8
```

C/CMake/OpenSSL development tools, tar, patch, gpgv, pkg-config and readelf must
already be installed. The tested recipe uses ordinary absolute paths without
whitespace. Ambient loader, compiler/include/library and package-prefix overrides
are cleared for dependency construction. SOURCE_DATE_EPOCH and compiler prefix
maps are fixed; `profile.json` records source/patch/recipe/cache/compiler/platform
and final installed-file digests. Two separate output roots produced identical
curl and c-ares library digests on this toolchain.

Recorded outputs:

```text
curl build/cache: /tmp/cog-relocatable-profile/shipped/curl-build
c-ares build:    /tmp/cog-relocatable-profile/shipped/cares-build
first prefix:   /tmp/cog-relocatable-profile/shipped/curl-build/install
receipt:        /tmp/cog-relocatable-profile/shipped/profile.json
```

Configure Cog with that prefix/CURL config and **installed** curl bytes:

```sh
cmake -S . -B build-admitted -DCOG_ENABLE_WERROR=ON \
  -DCMAKE_PREFIX_PATH=/tmp/cog-relocatable-profile/shipped/curl-build/install \
  -DCURL_DIR=/tmp/cog-relocatable-profile/shipped/curl-build/install/lib/cmake/CURL \
  -DCOG_NET_CURL_BUILD_DIR=/tmp/cog-relocatable-profile/shipped/curl-build \
  -DCOG_NET_CURL_LIBRARY=/tmp/cog-relocatable-profile/shipped/curl-build/install/lib/libcurl.so
```

Cog hashes these installed bytes and still checks the actual loaded curl object,
ASYNCHDNS, nonempty c-ares version and DNS_SERVERS support. The deployment owns
trusted loader/filesystem contents and explicit host initialization/cleanup.
A later strip, binary edit or RPATH modification creates a different artifact;
it requires matching fresh provenance/configuration and repeated checks.

## Normal install and relocation audit

```sh
python3 libraries/cog-net/cmake/relocatable/audit_profile.py \
  --manifest /tmp/cog-relocatable-profile/shipped/profile.json \
  --cog-net-archive build-admitted/libraries/cog-net/libcog-net.a \
  --report /tmp/cog-relocatable-profile/shipped/relocation-audit.json
```

The optional Cog archive is an ordinary, nonsanitized actual net archive built
against this profile. This admission probe is separate from full installed Cog
consumer coverage. Without that option the dependency audit still runs completely.

The audit normally installs both dependency builds into a new prefix, checks every
recorded installed-file digest, relocates that prefix, audits CMake/pkg-config/
curl-config metadata and ELF dependencies/RUNPATH, compiles and runs standalone
CMake **and** pkg-config consumers, and confirms actual `ldd` curl/c-ares paths.
It uses no LD_LIBRARY_PATH/LD_PRELOAD. Installed executables receive deliberate
`$ORIGIN/../lib` loader paths through their build configuration. The optional Cog
probe accepts final bytes and refuses a clearly labeled altered test copy while
the genuine final artifacts stay untouched.

It then compiles the existing DNS PoC probe against installed headers/libraries,
runs ten cancellations and three 800 ms timeouts against a local UDP blackhole,
and requires real packets plus existing thread/FD/cleanup/timing assertions.
No external DNS, provider request or resolver-system change occurs. Measured
ceilings remain host regression observations, not hard scheduler guarantees.

For another packager's prefix, ordinary installs are sufficient:

```sh
cmake --install /tmp/cog-relocatable-profile/shipped/cares-build --prefix /desired/prefix
cmake --install /tmp/cog-relocatable-profile/shipped/curl-build --prefix /desired/prefix
```

Normal install must also stage the complete Cog package and deliberately configure
its executable install RPATH; the separate R18 fixture owns that full-project check.
No manually supplied headers, static curl/cares archives, TLS library or CA store are
part of this shared profile. The recorded artifact needs glibc symbol versions
through GLIBC_2.38 (from c-ares) and OpenSSL versions through OPENSSL_3.2.0. The tested
host OpenSSL additionally loads system libz/libzstd. Its CA bundle remains
`/etc/ssl/certs/ca-certificates.crt`; host OpenSSL, its transitive OS libraries,
loader and trust-store maintenance remain deployment requirements. Installed
notices cover the bundled source projects; host packages retain their own notices.

[Recorded receipt](recorded-profile.json), [relocation/admission/DNS audit](recorded-audit.json)
and delivery evidence (`tix show ba50799`)
identify the exact tested bytes. Independent review and the broader packaging gate
remain required before release acceptance.

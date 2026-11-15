# DNS cancellation followup

Research child ticket `23ae6c8` under `c5671b5`. No Cog code or public API.
The existing distro curl uses a threaded resolver. A two-second synthetic
getaddrinfo delay demonstrates that remove_handle can wait for its thread.
QUICK_EXIT demonstrates fast removal with a **residual resolver thread after
global cleanup**. It is evidence against using QUICK_EXIT for embedding.
The c-ares-only build sends real DNS queries to a local UDP blackhole and
checks cancellation, timeout, cleanup latency, descriptor accounting and
absence of resolver threads.

Only the setup below uses the network. The runner is offline: no external
DNS, provider traffic, system package installation, network namespace,
resolver configuration changes, or committed binaries/dependencies.

## Offline replay with prepared dependencies

Prerequisites: Linux/glibc, C11 compiler, Python >= 3.10, pkg-config and
system curl >= 8.14.1 headers/runtime with a threaded resolver. Also provide
a curl >= 8.14.1 build with c-ares enabled and threaded resolver disabled,
and its c-ares runtime. The recorded profile is curl 8.14.1 + c-ares 1.34.9.

```sh
python3 pocs/https-lifecycle/dns/run.py \
  --curl-lib /tmp/cog-dns-research/curl-build/lib \
  --cares-lib /tmp/cog-dns-research/deps/usr/lib/x86_64-linux-gnu
```

These are the prepared paths retained during this research session. The
runner compiles both hosts and the preload shim in a temporary directory,
selects the isolated curl with executable rpath and per-child LD_LIBRARY_PATH,
then deletes its own compiler outputs. It does not rewrite system libraries.
Checks deliberately reject the wrong resolver backend rather than skipping.
The shim affects every getaddrinfo in its isolated host; its 2-second delay
is a controlled libc resolver stall, not a real DNS packet-timeout test.

## Recreate the recorded dependency profile (network required)

This is a Debian trixie amd64 recipe using a fresh scratch directory. Run
from repository root first to record the probe source path. System packages
needed for building include CMake, pkg-config and OpenSSL development
headers. No apt install or apt update is performed.

```sh
set -eu
dns_source="$(pwd)/pocs/https-lifecycle/dns"
dns_work=/tmp/cog-dns-research-repro
mkdir -p "$dns_work"
cd "$dns_work"
curl -fL --proto '=https' https://curl.se/download/curl-8.14.1.tar.xz -o curl-8.14.1.tar.xz
curl -fL --proto '=https' https://curl.se/download/curl-8.14.1.tar.xz.asc -o curl-8.14.1.tar.xz.asc
curl -fL --proto '=https' \
  'https://keyserver.ubuntu.com/pks/lookup?op=get&search=0x27edeaf22f3abceb50db9a125cc908fdb71e12c2' \
  -o daniel-key.asc
mkdir -m700 -p gnupg
gpg --homedir "$dns_work/gnupg" --batch --import daniel-key.asc
gpg --homedir "$dns_work/gnupg" --batch --status-fd 1 \
  --verify curl-8.14.1.tar.xz.asc curl-8.14.1.tar.xz > verify.status
rg '^\[GNUPG:\] VALIDSIG 27EDEAF22F3ABCEB50DB9A125CC908FDB71E12C2 ' verify.status
apt-get download libc-ares-dev=1.34.9-1~deb13u1 libcares2=1.34.9-1~deb13u1
sha256sum -c "$dns_source/SHA256SUMS"
mkdir -p deps
dpkg-deb -x libc-ares-dev_1.34.9-1~deb13u1_amd64.deb deps
dpkg-deb -x libcares2_1.34.9-1~deb13u1_amd64.deb deps
tar -xf curl-8.14.1.tar.xz
cmake -S curl-8.14.1 -B curl-build \
  -DENABLE_ARES=ON -DENABLE_THREADED_RESOLVER=OFF \
  -DCARES_INCLUDE_DIR="$dns_work/deps/usr/include" \
  -DCARES_LIBRARY="$dns_work/deps/usr/lib/x86_64-linux-gnu/libcares.so" \
  -DCURL_USE_OPENSSL=ON -DBUILD_TESTING=OFF -DBUILD_CURL_EXE=OFF \
  -DBUILD_SHARED_LIBS=ON -DBUILD_STATIC_LIBS=OFF \
  -DCURL_USE_LIBPSL=OFF -DUSE_LIBIDN2=OFF -DCURL_USE_LIBSSH2=OFF
cmake --build curl-build -j4
python3 "$dns_source/run.py" --curl-lib "$dns_work/curl-build/lib" \
  --cares-lib "$dns_work/deps/usr/lib/x86_64-linux-gnu"
```

Stop if signature, fingerprint or digest verification fails. The fingerprint
comes from [curl's official verification page](https://curl.se/docs/verify.html).
In the recorded run GPG emitted VALIDSIG for that exact fingerprint; its
unknown-owner-trust warning reflects an isolated scratch trustdb. The source
archive's signature was verified after the initial exploratory build; its
hash matched the archive used to build. SHA256SUMS records that archive and
the exact Debian packages, whose hashes also match apt-cache show metadata
from the authenticated configured Debian security repository. The dependency
packages were downloaded using apt-get, not arbitrary unverified URLs.

Recorded package source URLs (derived from apt's Filename fields):

- [libc-ares-dev](https://security.debian.org/debian-security/pool/updates/main/c/c-ares/libc-ares-dev_1.34.9-1~deb13u1_amd64.deb)
- [libcares2](https://security.debian.org/debian-security/pool/updates/main/c/c-ares/libcares2_1.34.9-1~deb13u1_amd64.deb)
- [curl source archive](https://curl.se/download/curl-8.14.1.tar.xz) and
  [detached signature](https://curl.se/download/curl-8.14.1.tar.xz.asc)

Versions may age out of live apt repositories. Reproduction then needs an
authenticated Debian snapshot or separately verified replacements, with a
new evidence record; do not disable verification or silently substitute.
The old unpatched upstream curl archive is a **local blackhole experiment**,
not a production-version recommendation. Production must use maintained
security-patched dependencies and rerun these cases for its actual build.

curl uses SPDX curl; c-ares is under the
[MIT license](https://c-ares.org/license.html). Installed notices are in the
extracted packages' usr/share/doc directories and curl's COPYING. The tested
OpenSSL backend is Apache-2.0. No dependency source/library is redistributed
in this commit. The wider optional dynamic dependency closure still needs
its own packaging/license inventory.

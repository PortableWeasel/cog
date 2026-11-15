#!/usr/bin/env python3
"""Build authenticated curl8.23.0/c-ares1.34.9 with relocatable Linux ELF metadata.
No downloads, global installs, binary patching or Cog admission changes.
"""
import argparse
import datetime
import email.utils
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess

CURL_SHA = "8dadd7d2536a314f982ad05a651b713f426bf7d02cc2c76cffb9796f2218e6c5"
CURL_SIGNER = "27EDEAF22F3ABCEB50DB9A125CC908FDB71E12C2"
CARES_INPUTS = {
    "c-ares_1.34.9-1~deb13u1.dsc": "497dd2e71ac7ddb45cf6c540916e693db8767433c18f6ddd8e5c7653623c225a",
    "c-ares_1.34.9.orig.tar.gz": "9364b70a12eb59a06830fb488d9357f90100dff0d98b23ea3593a183d43e6f2f",
    "c-ares_1.34.9.orig.tar.gz.asc": "47c70be87138c8b67c0d7dd5b14557a3f545a7047bd7c28ed5aab1b66aae2f84",
    "c-ares_1.34.9-1~deb13u1.debian.tar.xz": "85c330a08d892a3b91d9678954512763fa2022395a67ee11c30f74fd3e9166be",
}


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def checked_sha(path, expected):
    actual = sha(path)
    if actual != expected:
        raise RuntimeError(f"source digest mismatch: {path}")
    return actual


def clean_environment():
    env = os.environ.copy()
    for key in ("LD_PRELOAD", "LD_LIBRARY_PATH", "PKG_CONFIG_PATH", "PKG_CONFIG_LIBDIR",
                "CMAKE_PREFIX_PATH", "CFLAGS", "CPPFLAGS", "LDFLAGS", "LIBRARY_PATH",
                "CPATH", "C_INCLUDE_PATH", "CPLUS_INCLUDE_PATH", "CC", "CXX"):
        env.pop(key, None)
    env["SOURCE_DATE_EPOCH"] = "1791936000"
    return env


def run(command, env, log, cwd=None):
    with log.open("a") as stream:
        stream.write("COMMAND " + json.dumps([str(x) for x in command]) + "\n")
        stream.flush()
        result = subprocess.run(command, cwd=cwd, env=env, stdout=stream, stderr=subprocess.STDOUT)
    if result.returncode:
        raise RuntimeError(f"command failed ({result.returncode}), inspect {log}")


def verify_sources(args, env):
    curl_sha = checked_sha(args.curl_archive, CURL_SHA)
    verification = subprocess.run(["gpgv", "--status-fd", "1", "--keyring", str(args.curl_keyring),
                                   str(args.curl_signature), str(args.curl_archive)],
                                  env=env, check=True, capture_output=True, text=True)
    if f"[GNUPG:] VALIDSIG {CURL_SIGNER} " not in verification.stdout:
        raise RuntimeError("curl signature does not match the independently pinned signer")
    release_check = subprocess.run(["gpgv", "--status-fd", "1", "--keyring", str(args.debian_keyring),
                                    str(args.debian_inrelease)], env=env, check=True,
                                   capture_output=True, text=True)
    release = args.debian_inrelease.read_text()
    valid_until = re.search(r"^Valid-Until: (.+)$", release, re.M)
    if valid_until is None or email.utils.parsedate_to_datetime(valid_until[1]) < datetime.datetime.now(datetime.timezone.utc):
        raise RuntimeError("Debian signed metadata expired/missing Valid-Until; refresh authenticated metadata first")
    sources_sha = sha(args.debian_sources)
    if re.search(r"^ " + sources_sha + r"\s+" + str(args.debian_sources.stat().st_size) + r" main/source/Sources$", release, re.M) is None:
        raise RuntimeError("Debian Sources index does not match the signed InRelease")
    sources = args.debian_sources.read_text()
    records = [r for r in sources.split("\n\n") if r.startswith("Package: c-ares\n") and "\nVersion: 1.34.9-1~deb13u1\n" in r]
    if len(records) != 1:
        raise RuntimeError("signed source index lacks the exact c-ares source release")
    inputs = {}
    for name, expected in CARES_INPUTS.items():
        if re.search(r"^ " + expected + r"\s+\d+ " + re.escape(name) + "$", records[0], re.M) is None:
            raise RuntimeError("c-ares archive is not authenticated by the pinned source index")
        inputs[name] = checked_sha(args.cares_inputs / name, expected)
    return {"curl_archive_sha256": curl_sha, "curl_signature_sha256": sha(args.curl_signature),
            "curl_validsig": verification.stdout.splitlines(), "cares_source_inputs": inputs,
            "debian_inrelease_sha256": sha(args.debian_inrelease), "debian_sources_sha256": sources_sha,
            "debian_validsig": release_check.stdout.splitlines(),
            "debian_source_record": records[0]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("curl-archive", "curl-signature", "curl-keyring", "cares-inputs",
                 "debian-inrelease", "debian-sources", "debian-keyring", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--cc", default="cc")
    parser.add_argument("--jobs", type=int, default=4)
    args = parser.parse_args()
    if platform.system() != "Linux" or args.jobs < 1:
        parser.error("this profile requires Linux and a positive job count")
    for name, value in vars(args).items():
        if isinstance(value, Path):
            setattr(args, name, value.resolve())
    env = clean_environment()
    provenance = verify_sources(args, env)
    if args.output.exists():
        raise RuntimeError("output must be a fresh directory; never reuse a possibly different cache")
    args.output.mkdir(parents=True)
    recipe = Path(__file__).resolve().parent
    source = args.output / "sources"
    source.mkdir()
    log = args.output / "build.log"
    run(["tar", "-xf", args.curl_archive, "-C", source], env, log)
    run(["tar", "-xf", args.cares_inputs / "c-ares_1.34.9.orig.tar.gz", "-C", source], env, log)
    curl_source, cares_source = source / "curl-8.23.0", source / "c-ares-1.34.9"
    patches = [(curl_source, recipe / "curl-8.23.0-relocatable-metadata.patch"),
               (cares_source, recipe / "c-ares-1.34.9-relocatable-metadata.patch")]
    for tree, patch in patches:
        run(["patch", "--batch", "--fuzz=0", "-p1", "-i", patch], env, log, cwd=tree)
    curl_build, cares_build = args.output / "curl-build", args.output / "cares-build"
    # Genuine installed artifact is inside the genuine audited curl build; no
    # copied-back library/cache is used to satisfy Cog's build-prefix check.
    prefix = curl_build / "install"
    common = [f"-DCMAKE_C_COMPILER={shutil.which(args.cc)}", "-DCMAKE_BUILD_TYPE=Release",
              f"-DCMAKE_INSTALL_PREFIX={prefix}", "-DCMAKE_INSTALL_LIBDIR=lib",
              "-DCMAKE_BUILD_WITH_INSTALL_RPATH=ON", "-DCMAKE_INSTALL_RPATH=$ORIGIN",
              "-DCMAKE_INSTALL_RPATH_USE_LINK_PATH=OFF", "-DCMAKE_BUILD_RPATH_USE_ORIGIN=ON",
              f"-DCMAKE_C_FLAGS=-ffile-prefix-map={args.output}=/cog-dependencies -fmacro-prefix-map={args.output}=/cog-dependencies"]
    run(["cmake", "-S", cares_source, "-B", cares_build, *common, "-DCARES_SHARED=ON",
         "-DCARES_STATIC=OFF", "-DCARES_THREADS=ON", "-DCARES_BUILD_TOOLS=OFF",
         "-DCARES_BUILD_TESTS=OFF"], env, log)
    run(["cmake", "--build", cares_build, "--parallel", str(args.jobs)], env, log)
    run(["cmake", "--install", cares_build], env, log)
    profile = recipe.parent / "curl-cares-profile.cmake"
    run(["cmake", "-S", curl_source, "-B", curl_build, "-C", profile, *common,
         f"-DCMAKE_PREFIX_PATH={prefix}", "-DCURL_USE_CMAKECONFIG=ON", "-DCURL_USE_PKGCONFIG=OFF",
         "-DCURL_ZLIB=OFF", "-DCURL_BROTLI=OFF", "-DCURL_ZSTD=OFF", "-DUSE_NGHTTP2=OFF",
         "-DCURL_DISABLE_LDAP=ON", "-DCURL_DISABLE_LDAPS=ON", "-DBUILD_EXAMPLES=OFF",
         "-DBUILD_LIBCURL_DOCS=OFF", "-DBUILD_MISC_DOCS=OFF"], env, log)
    run(["cmake", "--build", curl_build, "--parallel", str(args.jobs)], env, log)
    run(["cmake", "--install", curl_build], env, log)
    for tree in (curl_build, cares_build):
        run(["readelf", "-d", prefix / "lib" / ("libcurl.so" if tree == curl_build else "libcares.so")], env, log)
    provenance.update({"recipe_sha256": sha(__file__), "patches_sha256": {p.name: sha(p) for _, p in patches},
                       "profile_sha256": sha(profile), "compiler": subprocess.check_output([args.cc, "--version"], text=True).splitlines()[0],
                       "platform": platform.platform(), "curl_build": str(curl_build), "cares_build": str(cares_build),
                       "prefix": str(prefix), "curl_cache_sha256": sha(curl_build / "CMakeCache.txt"),
                       "cares_cache_sha256": sha(cares_build / "CMakeCache.txt"),
                       "curl_sha256": sha(prefix / "lib/libcurl.so"), "cares_sha256": sha(prefix / "lib/libcares.so"),
                       "runtime_host_requirements": "Linux/glibc and system OpenSSL3 runtime; CA trust from host; no bundled TLS/CA store",
                       "installed_files_sha256": {str(p.relative_to(prefix)): sha(p) for p in sorted(prefix.rglob("*")) if p.is_file() and not p.is_symlink()}})
    manifest = args.output / "profile.json"
    manifest.write_text(json.dumps(provenance, indent=2) + "\n")
    print(json.dumps({"manifest": str(manifest), "curl_build": str(curl_build), "cares_build": str(cares_build), "prefix": str(prefix), "curl_sha256": provenance["curl_sha256"], "cares_sha256": provenance["cares_sha256"]}, indent=2))


if __name__ == "__main__":
    main()

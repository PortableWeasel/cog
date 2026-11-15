#!/usr/bin/env python3
"""Normal-install, relocate and audit the final dependency bytes without loader overrides."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import shlex
import socket
import subprocess
import tempfile
import threading


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def command(args, env, **kwargs):
    return subprocess.check_output([str(x) for x in args], env=env, text=True, **kwargs)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--cc", default="cc")
    parser.add_argument("--cog-net-archive", type=Path,
                        help="Optional actual Cog net archive for admitted/altered-byte checks")
    args = parser.parse_args()
    profile = json.loads(args.manifest.read_text())
    curl_build, cares_build = Path(profile["curl_build"]), Path(profile["cares_build"])
    source = Path(__file__).resolve().parents[4]
    env = os.environ.copy()
    for key in ("LD_LIBRARY_PATH", "LD_PRELOAD", "PKG_CONFIG_PATH", "PKG_CONFIG_LIBDIR", "CMAKE_PREFIX_PATH"):
        env.pop(key, None)
    for tree, field in ((curl_build, "curl_cache_sha256"), (cares_build, "cares_cache_sha256")):
        if sha(tree / "CMakeCache.txt") != profile[field]:
            raise RuntimeError("audited build cache changed")
    cache = (curl_build / "CMakeCache.txt").read_text()
    for setting in ("ENABLE_ARES:BOOL=ON", "ENABLE_THREADED_RESOLVER:BOOL=OFF"):
        if setting not in cache.splitlines():
            raise RuntimeError("audited curl profile flag missing: " + setting)
    if re.search(r"^CMAKE_BUILD_WITH_INSTALL_RPATH:(?:BOOL|UNINITIALIZED)=ON$", cache, re.M) is None:
        raise RuntimeError("audited curl build lacks installed-RPATH construction")
    report = {"manifest_sha256": sha(args.manifest), "audit_recipe_sha256": sha(__file__),
              "loader_overrides": False}
    with tempfile.TemporaryDirectory(prefix="cog-relocated-dependencies-") as directory:
        work = Path(directory)
        installed, relocated = work / "installed", work / "relocated"
        for tree in (cares_build, curl_build):
            command(["cmake", "--install", tree, "--prefix", installed], env)
        # Byte identity is checked before and after actual directory relocation.
        for name, field in (("libcurl.so", "curl_sha256"), ("libcares.so", "cares_sha256")):
            if sha(installed / "lib" / name) != profile[field]:
                raise RuntimeError("normal install changed the admitted dependency bytes")
        installed.rename(relocated)
        for notice in ("share/licenses/curl/COPYING", "share/licenses/c-ares/LICENSE.md"):
            if not (relocated / notice).is_file():
                raise RuntimeError("normal installation omitted the upstream license notice: " + notice)
        for rel, expected in profile["installed_files_sha256"].items():
            if sha(relocated / rel) != expected:
                raise RuntimeError("normal installation/relocation changed " + rel)
        for path in list(relocated.rglob("*.cmake")) + list(relocated.rglob("*.pc")) + [relocated / "bin/curl-config"]:
            text = path.read_text()
            if str(curl_build.parent) in text or "/tmp/cog-dns-research" in text or "/tmp/cog-net-maintained" in text:
                raise RuntimeError("foreign build/development path in installed metadata: " + str(path))
        report["elf"] = {}
        report["host_libraries_sha256"] = {}
        for name in ("libcurl.so", "libcares.so"):
            dynamic = command(["readelf", "-d", relocated / "lib" / name], env)
            needed = set(re.findall(r"\(NEEDED\).*\[([^]]*)\]", dynamic))
            expected = {"libssl.so.3", "libcrypto.so.3", "libcares.so.2", "libc.so.6"} if name == "libcurl.so" else {"libc.so.6"}
            if needed != expected:
                raise RuntimeError("dependency closure changed from the supported shared profile: " + name)
            paths = re.findall(r"\((?:RUNPATH|RPATH)\).*\[([^]]*)\]", dynamic)
            if paths != ["$ORIGIN"]:
                raise RuntimeError("dependency loader path is not exactly $ORIGIN: " + name)
            versions = command(["readelf", "-V", relocated / "lib" / name], env)
            report["elf"][name] = {"sha256": sha(relocated / "lib" / name),
                                    "dynamic": dynamic.splitlines(),
                                    "symbol_versions": sorted(set(re.findall(r"(?:GLIBC|OPENSSL)_[0-9.]+", versions)))}
        pkg_env = env.copy()
        pkg_env["PKG_CONFIG_PATH"] = str(relocated / "lib/pkgconfig")
        report["pkgconfig"] = {}
        for module in ("libcurl", "libcares"):
            prefix = command(["pkg-config", "--variable=prefix", module], pkg_env).strip()
            if Path(prefix).resolve() != relocated:
                raise RuntimeError("pkg-config still names an old prefix")
            report["pkgconfig"][module] = command(["pkg-config", "--cflags", "--libs", "--static", module], pkg_env).strip()
            if str(curl_build.parent) in report["pkgconfig"][module]:
                raise RuntimeError("foreign linker flag survived relocation")
        if Path(command([relocated / "bin/curl-config", "--prefix"], env).strip()) != relocated:
            raise RuntimeError("curl-config did not relocate")
        report["curl_config"] = command([relocated / "bin/curl-config", "--libs", "--cflags"], env).splitlines()
        consumer_source = work / "consumer-source"
        consumer_source.mkdir()
        (consumer_source / "consumer.c").write_text('''#include <curl/curl.h>
#include <ares.h>
#include <stdio.h>
#include <string.h>
int main(void) {
    if(curl_global_init(CURL_GLOBAL_ALL)!=CURLE_OK) return 1;
    const curl_version_info_data *v=curl_version_info(CURLVERSION_NOW);
    if(v==NULL || strcmp(v->version,"8.23.0")!=0 || v->ares==NULL || strcmp(v->ares,"1.34.9")!=0 ||
       (v->features&CURL_VERSION_ASYNCHDNS)==0 || strcmp(ares_version(NULL),"1.34.9")!=0) return 2;
    CURL *easy=curl_easy_init(); if(easy==NULL) return 3;
    CURLcode result=curl_easy_setopt(easy,CURLOPT_DNS_SERVERS,"127.0.0.1");
    printf("curl=%s cares=%s DNS_SERVERS=%d\\n",v->version,v->ares,(int)result);
    curl_easy_cleanup(easy); curl_global_cleanup(); return result==CURLE_OK ? 0 : 4;
}
''')
        dns_source = source / "pocs/https-lifecycle/dns/probe.c"
        (consumer_source / "CMakeLists.txt").write_text('''cmake_minimum_required(VERSION 3.16)
project(relocated_dependencies C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_EXTENSIONS OFF)
find_package(CURL 8.23.0 EXACT CONFIG REQUIRED)
find_package(c-ares 1.34.9 EXACT CONFIG REQUIRED)
add_executable(curl-consumer consumer.c)
target_link_libraries(curl-consumer PRIVATE CURL::libcurl c-ares::cares)
add_executable(dns-probe "''' + str(dns_source) + '''")
target_link_libraries(dns-probe PRIVATE CURL::libcurl)
foreach(target curl-consumer dns-probe)
  target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror)
  set_target_properties(${target} PROPERTIES INSTALL_RPATH "$ORIGIN/../lib" BUILD_WITH_INSTALL_RPATH ON)
endforeach()
install(TARGETS curl-consumer dns-probe RUNTIME DESTINATION bin)
''')
        consumer_build = work / "consumer-build"
        command(["cmake", "-S", consumer_source, "-B", consumer_build, "-DCMAKE_BUILD_TYPE=Release",
                 f"-DCMAKE_C_COMPILER={shutil.which(args.cc)}", f"-DCMAKE_PREFIX_PATH={relocated}",
                 f"-DCMAKE_INSTALL_PREFIX={relocated}"], env)
        command(["cmake", "--build", consumer_build, "--parallel", "2"], env)
        command(["cmake", "--install", consumer_build], env)
        report["consumer"] = command([relocated / "bin/curl-consumer"], env).strip()
        pkg_flags = shlex.split(command(["pkg-config", "--cflags", "--libs", "libcurl", "libcares"], pkg_env))
        command([args.cc, "-std=c11", "-Wall", "-Wextra", "-Wpedantic", "-Werror",
                 consumer_source / "consumer.c", *pkg_flags, "-Wl,-rpath,$ORIGIN/../lib",
                 "-o", relocated / "bin/pkg-consumer"], env)
        report["pkg_consumer"] = command([relocated / "bin/pkg-consumer"], env).strip()
        report["ldd"] = {}
        for executable in ("curl-consumer", "pkg-consumer", "dns-probe"):
            loaded = command(["ldd", relocated / "bin" / executable], env)
            if "not found" in loaded or str(curl_build.parent) in loaded:
                raise RuntimeError("runtime loader relied on an original/build dependency")
            for library in ("libcurl.so.4", "libcares.so.2"):
                match = re.search(re.escape(library) + r" => (\S+)", loaded)
                if match is None or Path(match[1]).resolve().parent != relocated / "lib":
                    raise RuntimeError("runtime did not select the relocated bundled " + library)
            report["ldd"][executable] = loaded.splitlines()
            for path in re.findall(r" => (/\S+)", loaded):
                library = Path(path).resolve()
                if library.parent != relocated / "lib":
                    report["host_libraries_sha256"][str(library)] = sha(library)
        if args.cog_net_archive is not None:
            admission = consumer_source / "admission.c"
            admission.write_text("""#include <cog/net.h>
#include <curl/curl.h>
#include <stdio.h>
int main(int argc, char **argv) {
    (void)argv;
    if(curl_global_init(CURL_GLOBAL_ALL)!=CURLE_OK) return 1;
    cog_net_ctx_t *ctx=NULL;
    cog_net_config_t c={.host_runtime_initialized=true,.dependency_memory=COG_NET_DEPENDENCY_MEMORY_DYNAMIC};
    cog_result_t rc=cog_net_new(&ctx,&c);
    bool expected=argc==1 ? rc==COG_OK && ctx!=NULL : rc==COG_ERROR_UNSUPPORTED && ctx==NULL;
    cog_net_free(ctx); curl_global_cleanup();
    if(!expected) return 2;
    puts(argc==1 ? "PASS unchanged Cog admission accepts final installed curl" :
                 "PASS unchanged Cog admission rejects altered curl bytes");
    return 0;
}
""")
            command([args.cc, "-std=c11", "-Wall", "-Wextra", "-Wpedantic", "-Werror",
                     "-I" + str(source / "libraries/cog-net/include"),
                     "-I" + str(source / "libraries/cog-core/include"),
                     admission, args.cog_net_archive.resolve(), *pkg_flags, "-lcrypto", "-ldl",
                     "-Wl,-rpath,$ORIGIN/../lib", "-o", relocated / "bin/cog-admission"], env)
            report["admission"] = command([relocated / "bin/cog-admission"], env).strip()
            rejected = work / "explicit-negative-fixture"
            (rejected / "bin").mkdir(parents=True)
            (rejected / "lib").mkdir()
            shutil.copy2(relocated / "bin/cog-admission", rejected / "bin/cog-admission")
            shutil.copy2((relocated / "lib/libcurl.so.4").resolve(), rejected / "lib/libcurl.so.4")
            shutil.copy2((relocated / "lib/libcares.so.2").resolve(), rejected / "lib/libcares.so.2")
            # Deliberately altered test COPY, never admitted or substituted for
            # a final/provenance artifact. ELF ignores the appended marker.
            with (rejected / "lib/libcurl.so.4").open("ab") as output:
                output.write(b"EXPLICIT NEGATIVE HASH FIXTURE\n")
            report["admission_rejected_copy"] = command([rejected / "bin/cog-admission", "reject"], env).strip()
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.bind(("127.0.0.1", 0))
        sock.settimeout(0.1)
        stop = threading.Event()
        queries = []

        def blackhole():
            while not stop.is_set():
                try:
                    packet, _ = sock.recvfrom(4096)
                    queries.append(len(packet))
                except TimeoutError:
                    pass

        thread = threading.Thread(target=blackhole)
        thread.start()
        report["dns"] = []
        try:
            for mode in ["cares-cancel"] * 10 + ["cares-timeout"] * 3:
                before = len(queries)
                result = command([relocated / "bin/dns-probe", mode,
                                  f"127.0.0.1:{sock.getsockname()[1]}"], env, timeout=4)
                if len(queries) <= before:
                    raise RuntimeError("DNS probe did not send a real query to the offline fixture")
                report["dns"].append(result.strip())
        finally:
            stop.set()
            thread.join()
            sock.close()
        report["dns_queries"] = len(queries)
        for name, field in (("libcurl.so", "curl_sha256"), ("libcares.so", "cares_sha256")):
            if sha(relocated / "lib" / name) != profile[field]:
                raise RuntimeError("dependency bytes changed during consumer/probe execution")
    args.report.write_text(json.dumps(report, indent=2) + "\n")
    print(f"PASS normal install, all installed-file hashes, relocation, ELF/pkg-config/CMake/runtime closure, 13 DNS probes ({report['dns_queries']} queries); report={args.report}")


if __name__ == "__main__":
    main()

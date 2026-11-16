"""R18: normal installs, two relocations and a standalone installed C11 host.

This fixture requires an installable admitted curl/c-ares profile, not the
historical development artifact with foreign RUNPATH/pkg-config paths.
"""
import argparse
import hashlib
import json
import os
import pathlib
import re
import shutil
import subprocess
import tempfile


def run(command, env, cwd=None):
    result = subprocess.run([str(x) for x in command], env=env, cwd=cwd,
                            text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if result.returncode:
        raise RuntimeError("command failed: " + " ".join(str(x) for x in command) + "\n" + result.stdout)
    return result.stdout


def cache(path):
    values = {}
    for line in (path / "CMakeCache.txt").read_text().splitlines():
        if not line.startswith(("#", "//")) and "=" in line and ":" in line.split("=", 1)[0]:
            key, value = line.split("=", 1)
            values[key.split(":", 1)[0]] = value
    return values


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def reject_paths(text, forbidden, owned=None):
    # The current relocated prefix may live below a forbidden producer/source
    # ancestor. That exception must not hide explicitly forbidden old install
    # prefixes within the fixture itself.
    scrubbed = re.sub(re.escape(str(owned)) + r"(?=/|$|[\s;\"'])", "<fixture>", text) if owned is not None else text
    for path in forbidden:
        checked = scrubbed if owned is not None and pathlib.Path(path) in pathlib.Path(owned).parents else text
        if str(path) in checked:
            raise RuntimeError("nonrelocatable source/build/install reference: " + str(path))
    # A temporary *current relocated prefix* is allowed, never a foreign /tmp tree.
    if re.search(r"/tmp/|/home/[^\s;\"']+/(?:ws-|\.codex/worktrees/)", scrubbed):
        raise RuntimeError("foreign temporary/worktree path in installed metadata")


def loader(binary, prefix, env, forbidden):
    dynamic = run(["readelf", "-d", binary], env)
    reject_paths(dynamic, forbidden, prefix)
    for line in dynamic.splitlines():
        if "RPATH" in line or "RUNPATH" in line:
            entries = line.split("[", 1)[1].split("]", 1)[0].split(":")
            for entry in entries:
                variable = "${ORIGIN}" if entry.startswith("${ORIGIN}") else "$ORIGIN"
                if not entry.startswith(variable) or entry[len(variable):len(variable) + 1] not in ("", "/"):
                    raise RuntimeError("nonrelative installed runtime path: " + line)
                location = (binary.parent / entry[len(variable):].lstrip("/")).resolve()
                if location != prefix and prefix not in location.parents:
                    raise RuntimeError("runtime path escapes relocated prefix: " + line)
    dependencies = run(["ldd", binary], env)
    reject_paths(dependencies, forbidden, prefix)
    if "not found" in dependencies:
        raise RuntimeError("unresolved installed dependency: " + dependencies)
    paths = [str(pathlib.Path(path).resolve())
             for path in re.findall(r"=> (/[^\s]+)", dependencies)]
    for path in paths:
        if not path.startswith((str(prefix) + "/", "/lib/", "/lib64/", "/usr/lib/")):
            raise RuntimeError("undeclared runtime dependency: " + path)
    return paths


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for argument in ("build", "curl-build", "curl-library", "cares-build", "compiler"):
        parser.add_argument("--" + argument, required=True, type=pathlib.Path)
    parser.add_argument("--asan", choices=("0", "1"), required=True)
    args = parser.parse_args()
    args.build = args.build.resolve()
    producer = cache(args.build)
    curl = cache(args.curl_build)
    if curl.get("ENABLE_ARES") != "ON" or curl.get("ENABLE_THREADED_RESOLVER") != "OFF":
        raise RuntimeError("not the admitted c-ares-only curl configuration")
    bindir = pathlib.Path(producer.get("CMAKE_INSTALL_BINDIR", "bin"))
    libdir = pathlib.Path(producer.get("CMAKE_INSTALL_LIBDIR", "lib"))
    if bindir.is_absolute() or libdir.is_absolute() or ".." in (*bindir.parts, *libdir.parts):
        raise RuntimeError("fixture requires relative installed bin/lib directories")
    for dependency in (args.cares_build, args.curl_build):
        if pathlib.Path(cache(dependency).get("CMAKE_INSTALL_LIBDIR", "lib")) != libdir:
            raise RuntimeError("bundle dependency/Cog libdir settings must match")
    fixture = pathlib.Path(__file__).resolve().parent / "project"
    env = {key: value for key, value in os.environ.items()
           if not key.startswith("COG_") and key not in
           ("LD_LIBRARY_PATH", "LD_PRELOAD", "OPENROUTER_API_KEY", "CMAKE_PREFIX_PATH",
            "CMAKE_TOOLCHAIN_FILE", "CMAKE_INCLUDE_PATH", "CMAKE_LIBRARY_PATH",
            "PKG_CONFIG_PATH", "PKG_CONFIG_LIBDIR", "PKG_CONFIG_SYSROOT_DIR",
            "CPATH", "C_INCLUDE_PATH", "CPLUS_INCLUDE_PATH", "LIBRARY_PATH")}
    report = {}
    scratch = args.build / "installed-consumer-tmp"
    scratch.mkdir(exist_ok=True)
    env["TMPDIR"] = str(scratch)
    tsan = producer.get("COG_ENABLE_TSAN") == "ON"
    with tempfile.TemporaryDirectory(prefix="cog-r18-", dir=scratch) as directory:
        work = pathlib.Path(directory)
        original, relocated, final = (work / name for name in ("stage", "relocated", "final"))
        source, build = work / "consumer-source", work / "consumer-build"
        shutil.copytree(fixture, source)
        for dependency_build in (args.cares_build, args.curl_build, args.build):
            run(["cmake", "--install", dependency_build, "--prefix", original], env)
        for notice in ("share/licenses/curl/COPYING", "share/licenses/c-ares/LICENSE.md"):
            if not (original / notice).is_file():
                raise RuntimeError("missing normally installed dependency notice: " + notice)
        forbidden = [args.build, producer["CMAKE_HOME_DIRECTORY"], args.curl_build, args.cares_build, original]
        # Reject stale dependency metadata even if this CMake consumer happens
        # not to use pkg-config. Do not hide a development artifact behind RPATH.
        for pattern in ("**/*.cmake", "**/*.pc"):
            for path in (original / libdir).glob(pattern):
                reject_paths(path.read_text(), forbidden)
        curl_config = original / bindir / "curl-config"
        if curl_config.exists():
            reject_paths(curl_config.read_text(), forbidden)
        original.rename(relocated)
        run(["cmake", "-S", source, "-B", build,
             "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_C_COMPILER=" + str(args.compiler),
             "-DCMAKE_PREFIX_PATH=" + str(relocated), "-DCMAKE_INSTALL_PREFIX=" + str(relocated),
             "-DCMAKE_INSTALL_BINDIR=" + str(bindir), "-DCMAKE_INSTALL_LIBDIR=" + str(libdir),
             "-DEMBEDDER_ASAN=" + ("ON" if args.asan == "1" else "OFF"),
             "-DEMBEDDER_TSAN=" + ("ON" if tsan else "OFF")], env)
        run(["cmake", "--build", build, "--parallel", "2"], env)
        for path in (build / "imports.txt", build / "compile_commands.json"):
            reject_paths(path.read_text(), forbidden, work)
        imports = dict(line.split("=", 1) for line in (build / "imports.txt").read_text().splitlines())
        if any(not value.startswith(str(relocated) + "/") for value in imports.values()):
            raise RuntimeError("consumer imported an uninstalled Cog/curl artifact")
        run(["cmake", "--install", build, "--prefix", relocated], env)
        relocated.rename(final)
        config = work / "empty-config"; config.write_text("")
        env["XDG_CONFIG_HOME"] = str(work / "xdg-config")
        libraries = list((final / libdir).glob("libcurl.so.*"))
        actual_curl = next(path for path in libraries if path.is_file())
        if digest(actual_curl) != digest(args.curl_library):
            raise RuntimeError("normal installed curl bytes differ from admitted final artifact")
        runtime = {}
        for binary in (final / bindir / "installed_embedder", final / bindir / "cog-cli", actual_curl):
            runtime[binary.name] = loader(binary, final, env, [*forbidden, relocated])
        expected_cares = next(path for path in (final / libdir).glob("libcares.so.*") if path.is_file())
        cares_prefix = pathlib.Path(cache(args.cares_build)["CMAKE_INSTALL_PREFIX"])
        admitted_cares = next(path for path in (cares_prefix / libdir).glob("libcares.so.*") if path.is_file())
        if digest(expected_cares) != digest(admitted_cares):
            raise RuntimeError("normal installed c-ares bytes differ from the declared dependency profile")
        loader(expected_cares, final, env, [*forbidden, relocated])
        for program in ("installed_embedder", "cog-cli"):
            paths = runtime[program]
            if not any(path.startswith(str(final / libdir)) and "/libcurl.so" in path for path in paths):
                raise RuntimeError("host did not load the relocated admitted curl")
            if not any(path.startswith(str(final / libdir)) and "/libcares.so" in path for path in paths):
                raise RuntimeError("host did not load relocated c-ares")
        report["embedder_output"] = run([final / bindir / "installed_embedder", config], env, work)
        run([final / bindir / "cog-cli", "--mock", "--no-store", "--config", config, "--prompt", "embedding"], env, work)
        report.update(curl_sha256=digest(actual_curl), cares_sha256=digest(expected_cares),
                      compiler=str(args.compiler), sanitizer=args.asan == "1", tsan=tsan,
                      runtime={key: [p.replace(str(final), "<relocated-prefix>") for p in values]
                               for key, values in runtime.items()})
    (args.build / "installed-embedder-report.json").write_text(json.dumps(report, indent=2) + "\n")
    print("R18 normal install, public C11 consumer, relocation and lifecycle checks passed")


if __name__ == "__main__":
    main()

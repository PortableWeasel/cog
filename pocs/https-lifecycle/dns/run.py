#!/usr/bin/env python3
"""Offline Linux DNS cancellation comparison. Never downloads dependencies."""
import argparse
import os
from pathlib import Path
import shlex
import socket
import subprocess
import tempfile
import threading


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--curl-lib", type=Path, required=True, help="c-ares-only curl build lib directory")
    parser.add_argument("--cares-lib", type=Path, required=True, help="extracted libcares runtime directory")
    parser.add_argument("--cc", default="cc")
    args = parser.parse_args()
    source = Path(__file__).resolve().parent
    flags = ["-std=c11", "-Wall", "-Wextra", "-Wpedantic", "-Wconversion", "-Wshadow", "-Werror"]
    system_flags = shlex.split(subprocess.check_output(["pkg-config", "--cflags", "--libs", "libcurl"], text=True))
    curl_lib, cares_lib = args.curl_lib.resolve(strict=True), args.cares_lib.resolve(strict=True)
    environment = os.environ.copy()
    environment.pop("LD_PRELOAD", None)
    environment.pop("LD_LIBRARY_PATH", None)
    with tempfile.TemporaryDirectory(prefix="cog-dns-probes-") as directory:
        root = Path(directory)
        shim, system_probe, cares_probe = root / "delay.so", root / "system-probe", root / "cares-probe"
        subprocess.run([args.cc, *flags, "-fPIC", "-shared", str(source / "delay_getaddrinfo.c"), "-o", str(shim)], check=True)
        subprocess.run([args.cc, *flags, str(source / "probe.c"), *system_flags, "-o", str(system_probe)], check=True)
        subprocess.run([args.cc, *flags, str(source / "probe.c"), "-L" + str(curl_lib),
                        "-Wl,-rpath," + str(curl_lib), "-Wl,-rpath-link," + str(cares_lib),
                        "-lcurl", "-o", str(cares_probe)], check=True)
        delayed_environment = environment.copy()
        delayed_environment["LD_PRELOAD"] = str(shim)
        for mode in ("threaded-wait", "threaded-quick"):
            result = subprocess.run([str(system_probe), mode, "unused"], env=delayed_environment,
                                    text=True, capture_output=True, timeout=6)
            print(result.stdout, end="", flush=True)
            print(result.stderr, end="", flush=True)
            if result.returncode or "controlled getaddrinfo entered" not in result.stderr:
                raise RuntimeError(f"{mode} did not reproduce expected resolver behavior")
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
        cares_environment = environment.copy()
        cares_environment["LD_LIBRARY_PATH"] = str(curl_lib) + ":" + str(cares_lib)
        try:
            for mode in ("cares-cancel", "cares-cancel", "cares-cancel", "cares-timeout"):
                before = len(queries)
                subprocess.run([str(cares_probe), mode, f"127.0.0.1:{sock.getsockname()[1]}"],
                               env=cares_environment, check=True, timeout=4)
                if len(queries) <= before:
                    raise RuntimeError(f"{mode} sent no actual DNS query to local fixture")
            print(f"PASS threaded cleanup blocker + quick-exit residual thread + 4 c-ares runs; DNS queries={len(queries)}", flush=True)
        finally:
            stop.set()
            thread.join()
            sock.close()


if __name__ == "__main__":
    main()

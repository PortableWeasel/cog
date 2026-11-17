"""Install a CLI-disabled build and run an external public runtime consumer."""
import pathlib
import subprocess
import sys
import tempfile

build = pathlib.Path(sys.argv[1]).resolve()
compiler = sys.argv[2]
source = pathlib.Path(__file__).resolve().parent / "runtime-only"
values = {}
for line in (build / "CMakeCache.txt").read_text().splitlines():
    if not line.startswith(("#", "//")) and "=" in line and ":" in line.split("=", 1)[0]:
        name, value = line.split("=", 1)
        values[name.split(":", 1)[0]] = value
if values.get("COG_BUILD_CLI") != "OFF":
    raise RuntimeError("runtime isolation proof requires COG_BUILD_CLI=OFF")

def run(command):
    result = subprocess.run([str(arg) for arg in command], text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if result.returncode:
        raise RuntimeError(" ".join(str(arg) for arg in command) + "\n" + result.stdout)

with tempfile.TemporaryDirectory(prefix="runtime-installed-", dir=build) as temporary:
    root = pathlib.Path(temporary)
    prefix, consumer = root / "prefix", root / "consumer"
    run(["cmake", "--install", build, "--prefix", prefix])
    if (prefix / "include" / "cog_cli").exists():
        raise RuntimeError("CLI headers present in CLI-disabled install")
    relocated = root / "relocated"
    prefix.rename(relocated)
    configure = ["cmake", "-S", source, "-B", consumer,
                 "-DCMAKE_PREFIX_PATH=" + str(relocated), "-DCMAKE_C_COMPILER=" + compiler]
    if values.get("COG_ENABLE_ASAN") == "ON":
        configure += ["-DCMAKE_C_FLAGS=-fsanitize=address,undefined -fno-omit-frame-pointer",
                      "-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined"]
    if values.get("COG_ENABLE_TSAN") == "ON":
        configure += ["-DCMAKE_C_FLAGS=-fsanitize=thread -fno-omit-frame-pointer",
                      "-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=thread"]
    run(configure)
    run(["cmake", "--build", consumer, "--parallel", "2"])
    run([consumer / "installed_runtime"])
print("CLI-disabled relocated installed runtime consumer passed")

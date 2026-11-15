"""Replay this audit's optimized negative controls against existing Make builds.

No repository/build files are changed. Variants link the unchanged production
archives. Linux/GNU-ld and CMake Makefiles are required for this diagnostic.
"""
import argparse
import pathlib
import resource
import shlex
import subprocess
import tempfile

CASES = {
    "test_net_cog_oom": "tests/unit/test_net_cog_oom.c",
    "test_session_allocations": "tests/unit/test_session_allocations.c",
    "test_store_fencing": "tests/unit/test_store_fencing.c",
    "test_ipc_replay": "tests/unit/test_ipc_replay.c",
    "test_ipc_replay_critic": "tests/unit/test_ipc_replay_critic.c",
    "test_toolkit_resources": "tests/unit/test_toolkit_resources.c",
    "test_configuration_memory": "applications/cog-cli/tests/test_configuration_memory.c",
    "test_app_memory": "applications/cog-cli/tests/test_app_memory.c",
    "test_mock_provider": "applications/cog-cli/tests/test_mock_provider.c",
    "test_mock_provider_contract": "applications/cog-cli/tests/test_mock_provider.c",
}


def no_core():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))


def replay(root, build, variant, temporary):
    for target, relative in CASES.items():
        flags_path = next(p for p in build.rglob("flags.make")
                          if p.parent.name == target + ".dir")
        flags = dict(line.split(" = ", 1) for line in flags_path.read_text().splitlines()
                     if " = " in line)
        link = shlex.split((flags_path.parent / "link.txt").read_text())
        stem = temporary / (variant + "-" + target)
        source = stem.with_suffix(".c")
        text = (root / relative).read_text()
        if variant == "nonvolatile":
            text = text.replace("static volatile ", "static ")
        source.write_text(text)
        options = shlex.split(flags["C_FLAGS"])
        if variant == "builtin":
            options = [p for p in options if not p.startswith("-fno-builtin-")]
        subprocess.run([link[0], *shlex.split(flags["C_DEFINES"]),
                        *shlex.split(flags["C_INCLUDES"]), *options,
                        "-iquote", str((root / relative).parent),
                        "-c", str(source), "-o", str(stem.with_suffix(".o"))], check=True)
        for i, value in enumerate(link):
            if value.endswith("/" + pathlib.Path(relative).name + ".o"):
                link[i] = str(stem.with_suffix(".o"))
            if value.startswith("--dependency-file="):
                link[i] = "--dependency-file=" + str(stem.with_suffix(".d"))
            if i and link[i - 1] == "-o":
                link[i] = str(stem)
        subprocess.run(link, cwd=flags_path.parent.parent.parent, check=True)
        result = subprocess.run([str(stem)], text=True, capture_output=True,
                                timeout=30, preexec_fn=no_core)
        # Session's escaped counter already works with only the symbol flags;
        # its unnecessary volatile changes were deliberately omitted.
        expected_pass = variant == "nonvolatile" and target == "test_session_allocations"
        correct = result.returncode == 0 if expected_pass else (
            result.returncode != 0 and "allocation_oracle_control" in result.stderr)
        print(f"{variant} {target}: exit={result.returncode} expected={'pass' if expected_pass else 'oracle failure'}",
              flush=True)
        if not correct:
            raise RuntimeError(result.stderr or "negative control unexpectedly passed")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--gcc-build", type=pathlib.Path, required=True)
    parser.add_argument("--clang-build", type=pathlib.Path, required=True)
    args = parser.parse_args()
    root = pathlib.Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory(prefix="cog-oracle-negative-") as path:
        replay(root, args.gcc_build.resolve(), "nonvolatile", pathlib.Path(path))
        replay(root, args.clang_build.resolve(), "builtin", pathlib.Path(path))

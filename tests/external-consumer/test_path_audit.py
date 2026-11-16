"""Independent relocation audit negatives; imports runner without executing it."""
import importlib.util
import pathlib
import sys
import unittest

sys.dont_write_bytecode = True
spec = importlib.util.spec_from_file_location("consumer_runner", pathlib.Path(__file__).with_name("run.py"))
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)


class PathAudit(unittest.TestCase):
    def test_owned_and_forbidden_paths(self):
        producer = pathlib.Path("/home/user/ws-2611/cog/build-review")
        work = producer / "installed-consumer-tmp/cog-r18-example"
        original = work / "stage"
        forbidden = [producer, original]
        runner.reject_paths(str(work / "relocated/lib/libcog-core.a"), forbidden, work)
        for path in (original / "lib/libcog-core.a",
                     producer / "libraries/libcog-core.a",
                     pathlib.Path(str(work) + "-foreign") / "lib/libcog-core.a",
                     pathlib.Path("/tmp/foreign/libcog-core.a")):
            with self.subTest(path=str(path)):
                with self.assertRaises(RuntimeError):
                    runner.reject_paths(str(path), forbidden, work)


if __name__ == "__main__":
    unittest.main()

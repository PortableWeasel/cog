# Test assertion policy

`cog_configure_test_assertions()` runs after the root has created all targets.
With `BUILD_TESTING=ON`, test executables receive a PRIVATE compiler option to
undefine `NDEBUG` in every configuration (GCC/Clang `-UNDEBUG`, MSVC `/UNDEBUG`).
Installed libraries, `cog-cli`, ordinary PoC demos and test-disabled builds keep
their existing assertion policy. No option is exported to consumers.

The current `test_*`, `run_cli_smoke`, `aux_core` and explicit PoC test targets
are classified centrally. A new target outside that convention can call
`cog_enable_test_assertions(target)` after creation. The registration test reads
the generated CTest files and checks project executable paths, including script
arguments, against the generated target manifest. Unclassified registered
executables fail the check; the explicit ordinary-program exceptions preserve
testing of actual production settings. External interpreters are not modified.

`test_assertions` refuses to compile if `NDEBUG` remains defined, verifies an
assertion side effect, and exposes an intentional failure. A bounded subprocess
check requires the reachability marker, assertion diagnostic and unsuccessful
termination. It does not use CTest's `WILL_FAIL`, which cannot invert aborts.

Configure a Release build, build `test_assertions`, then run:

```sh
ctest --test-dir build-release -C Release -R '^build-test-' --output-on-failure
```

These checks verify test-build policy, not full application/runtime acceptance.

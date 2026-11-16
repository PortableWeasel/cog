# Independent O5 codec review

Source checkpoint: `86f73b4d8744ccee3b3510c7e25104494b58256e` on approved
O base `0ae8213`. Reviewed after the private helper stage `fa0175a`. No source,
public header, app glue, configuration, main or CMake change by this critic.

Source review checks literal CGL1/command, fixed55-byte receipt, escaped child
context and CGV1 formats against approved N. Validations precede caller writes;
CGV1 validates actual nested CGD1 without writing before later rows or capacities.
UTF-8 rejects NUL/overlong/surrogate/out-of-range forms, widths and additions are
checked, signed outcomes preserve known two's-complement values, and alias
guards precede write passes. These pure modules grant no role/store/effect
authority and perform no I/O or secret logging. Their object imports and the
actual retained CGD1/message validation path contain no allocation operation.
Nonzero private command/CGV anchors preserve public standalone CGD1 sequence0.

The authored four golden/malformed fixtures and four always-evaluated CHECK
negative controls pass independently under strict GCC O3/NDEBUG, Clang O3/NDEBUG,
and Clang O3 ASan+UBSan+LSan. The separate critic fixture adds120000 deterministic
seed mutations in each profile: truncation, trailing bytes, single/multiple byte
corruption, unchanged controls and zero/sufficient storage capacity. It checks
all metadata/arenas unchanged on errors, required counts only on OVERFLOW, unused
successful arena/array canaries and byte-exact re-encoding of every accepted
frame. Its always-evaluated CHECK negative control also fails as intended.

Reproduce both authored and critic checks:

```sh
python3 applications/cog-cli/tests/headless-codecs/run_critic.py --cc gcc --build-dir /tmp/cog-o5-critic-gcc
python3 applications/cog-cli/tests/headless-codecs/run_critic.py --cc clang --build-dir /tmp/cog-o5-critic-clang
python3 applications/cog-cli/tests/headless-codecs/run_critic.py --cc clang --sanitize --build-dir /tmp/cog-o5-critic-asan
```

The critic runner builds the exact existing runner's actual objects first; no
runtime stub or substitute nested codec is linked. Total independent profile
evidence:15 positive fixture runs,15 intentional CHECK-failure controls, and
360000 mutation cases. This is focused codec evidence, not whole app/runtime,
installed consumer, full O or P acceptance. No paid requests were made.

The integration owner must retain this executable in addition to the four
authored fixtures. Add `critic_mutations` to the README's codec target loop after
the four real sources join `cog::cli`; register its negative CHECK control under
the maintained expected-failure policy. The standalone runner remains usable
before unfinished app/orchestrator sources link. No test skip is authorized.

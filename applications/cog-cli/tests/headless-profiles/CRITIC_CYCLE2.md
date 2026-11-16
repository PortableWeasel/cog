# O6 independent cycle2 review

**SOURCE/RUNTIME PASS** at exact author
`526e3be05f0d444a96cd2182623fbc1989c4ebf5`, resolving blocker228a05c from
cycle1 review1416a5b. Different cycle2 critic, separate managed
review-headless-profiles-cycle2 tree; no production/public/header/CMake edits.
Only production difference after the earlier broad independent review is the
five-line B_SESSION validator fix. All previous ownership/parser/source findings
were already PASS; this review independently checks the exact correction and
regression surface rather than inventing a new ID grammar.

`bootstrap_valid` now uses actual `cog_cli_session_options_validate` RESUME
source-ID path. NEW only checks conflicting selectors; it was not a valid ID
predicate. Existing L source IDs reject empty, dot/dotdot, .cog-domain.lock,
slash/backslash, ASCII controls/DEL, malformed UTF-8 and >127 bytes. G's actual
file-store valid_id also rejects its reserved DOMAIN/control/separator set;
CGB1's fixed128-byte probe and existing L contract deliberately support127
payload bytes, not an invented narrower punctuation/Unicode rule. Ordinary
spaces/punctuation and full Unicode scalar UTF-8 remain accepted.

All three CGB1 entry points reach this SAME validator:

- local bootstrap_encode builds a view then validates before capacity/write;
  malformed local probe maps PROTOCOL to INVALID_ARG, required/output unchanged;
- bootstrap_new first clears only its documented *out=NULL, then parses and
  validates full immutable input before calloc, output probe or publication;
- bootstrap_transport_decode parses/validates full input before capacity/copy or
  scalar/storage/required writes. Invalid data cannot be hidden by tiny storage.

Mandatory NULL/profile error precedence remains unchanged. Known malformed ID
returns PROTOCOL inbound; declared excessive128-byte wire ID returns LIMIT.
The validator does no G/store probe, source lookup, resolver or network callback.
Actual canonical directory/current-owner/checkpoint inspection stays caller-owned.

Fresh independent strict Release/O3 builds passed all5 focused tests under GCC,
Clang and Clang ASan+UBSan+LSan (15/15, leak detection and halt-on-error).
Existing J/R12 tests and optimizer-safe whole-owner wipe/allocation/source-read
positive controls execute. No paid request, real provider/net or store opened by
these component fixtures. Opaque net tokens are checked borrows only.

Re-ran the ORIGINAL cycle1 actual-source reproduction:
`/tmp/o6_probe_ids.c` SHA256
677ce6c3533150993e6402a05eae2402ef98eb0809929a614d82e2a1cbc91b41.
Against exact1416a5b all six slash/dotdot/separator/control/reserved cases give
encode=OK/bootstrap_new=OK. Against exact526e3be all six give INVALID_ARG at
encode. Logs /tmp/cog-o6-cycle2-repro/{before,after}.log, no secret printed.

New independent retained `test_probe_ids_critic.c` strengthens direct entry joins:
127 interior ASCII byte cases are compared against the actual L selector
validator; exact reserved vs legal prefixed/suffixed names, punctuation, UTF-8
scalar boundaries/noncharacters, overlong/surrogate/>10FFFF,127/128-byte boundary
and embedded NUL are also checked. Each ID independently rewrites the wire
session field in a VALID CGB1 frame, without relying on malformed encoder output.
Both inbound APIs must match expected result. Invalid input leaves every probe,
transport/storage/required byte unchanged, *out=NULL and no allocation/source
reads; memcmp snapshots explicitly use memcpy so struct padding is meaningful.
Valid constructor calls prove allocation wrappers observe exactly one allocation,
and decode owns/copies the exact identity. Encoder rejection is allocation-free
with unchanged required/full output. No fabricated executor/store behavior.

Independent fixture passed GCC/Clang O3/NDEBUG and Clang ASan+UBSan+LSan.
Its runner then compiles the SAME fixture against actual1416a5b production
configuration.c (temporary external copy); all three profiles reject that old
source at the intended invalid-ID assertion. This proves the regression catches
the original failure with optimized CHECK execution, not a target-name pass.

```sh
python3 applications/cog-cli/tests/headless-profiles/run_probe_ids_critic.py --cc gcc --build-dir /tmp/cog-o6-cycle2-extra-gcc
python3 applications/cog-cli/tests/headless-profiles/run_probe_ids_critic.py --cc clang --build-dir /tmp/cog-o6-cycle2-extra-clang
python3 applications/cog-cli/tests/headless-profiles/run_probe_ids_critic.py --cc clang --sanitize --build-dir /tmp/cog-o6-cycle2-extra-asan
```

Integration owner may add this ONE fixture to the component/real test recipe,
compiling actual configuration.c+options.c with existing public includes, strict
warnings and the memory-fixture allocator builtin controls and wrappers
malloc/calloc/realloc/free/getenv/fopen. No host_options/runtime stub is required.
Runner is an independent standalone reproduction, not shared CMake mutation.

| Fresh focused log | SHA256 |
|---|---|
| /tmp/cog-o6-cycle2-gcc.log | f5437504f1c2b07e5f24991bcc66e20d68b3cbe6bab24e215c591a7977168aa1 |
| /tmp/cog-o6-cycle2-clang.log | b5d6a0a9be7a0f596cca8fe9378f45af63420de6c813dd2128ddf8ed093b761f |
| /tmp/cog-o6-cycle2-asan.log | 2cccadaec36e4ae9724de35d809342583698ef33aa56c4af99b57ac2ca0ba607 |

No residual blocker found in this exact correction. Root owns serial O integration
and ticket closure; O4 layout/app bodies and whole O/P/runtime gates are separate.

## Approved O6-only integration

Root-authorized no-ff60eb927 retains approved526e3be source/history; ONLY critic
70c7f75 retained via6dc7994 (tests/runner/evidence). Fresh postmerge strict Release
GCC/Clang and Clang ASan+UBSan+LSan each pass5/5 focused profile/role/config/memory
CTest targets, plus the retained independent ID fixture and actual pre-fix source
negative control. No shared app CMake/source registration was added; this is
component acceptance, not installed role/runtime dispatch. Existing snippets
remain mandatory for O4 integration. Source checkpoint6dc7994bfbbc8196496f96a63a46d5194e324bbd
precedes this evidence-only update. MainMdacbd199/profile/build_user unchanged.
Root-authorized O6 source/review/integration/finalize/parent and ID bug closed;
private10dde8a header gate was already DONE and is now retained in O history.

| Postmerge log | SHA256 |
|---|---|
| /tmp/cog-o6-join-gcc.log | 1422975d25ee32824d014adc2c24f795aaa60eafb71d8c9fdbea8d43163b2170 |
| /tmp/cog-o6-join-clang.log | c26b13ee07296e8c234e3aca4ce715f8f149e9d98767e54dc4800b3e8172585f |
| /tmp/cog-o6-join-asan.log | 35e73d24d4fad550297e777750473f5943c049057e2f69ce595b4b78087a0628 |

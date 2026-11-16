# Scoped R1 receipt review

Source reviewed: 5e21e0f42f993c610b706afac43d13745db0dc0a.
Scoped PASS for 0bc5868 and c1f4dde only. Prior review document records historical
FAIL, superseded for these two defects by this R1 result.

The retained command search includes lane before byte-for-byte duplicate checks.
Fresh logical lease admission clears both directional lane metadata and lengths,
retaining constructor-owned buffer pointers/capacities. It resets HELLO watermark
only for a fresh lease. Same-lease reconnect bypasses this reset. Existing native
UID/peer proof and connected logical lease routing reject stale messages before
command admission. No source edit made by reviewer.

Independent exact-source real leaf tests: admission, group, receipt pressure,
stream identity: PASS 4/4 strict GCC and 4/4 strict Clang. The reviewer added:
- Same-key changed command bytes still yield PROTOCOL.
- Fresh lease retains buffer address/capacity but clears old final/pending proof.
- Actual UI ingress rejects old logical lease and wrong physical peer NOT_FOUND.
- Actual UI ingress rejects wrong native UID DENIED.
- All rejected inputs leave fresh lease/retained request intact.
Author same-lease HELLO receipt/watermark preservation regression also passes.

Clang ASan/UBSan/LSan: PASS 4/4 with production host_runtime.c and test translation
units instrumented. Actual stable dependency archives are NOT instrumented, so
this is owned-source sanitizer coverage only. Strict flags: C11 Wall Wextra
Wpedantic Wconversion Werror; function/data sections and linker section GC.
Dependencies are the immutable actual A archive snapshot under
review-worker-delegation/build-worker-review/joined, inventory archive-sha256.txt
beside it (A close fix f504c01, B65e73db/C7b0fcc8/D7d1afe0, client07d74a4).
Public helper tests explicitly compile exact reviewed host_runtime.c; private
helper tests include that source. No stubs. Logs/TMPDIR are checkout-local
build-host-review; no heavy build.

This closes no full B/O4 source gate. Native integration, reload failure contract,
and repeated handoff remain separate work. D serial reset 8dab83f requires another
independent reviewer because this reviewer authored the original handoff source.

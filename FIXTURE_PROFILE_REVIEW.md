# Independent fixture profile review

Reviewed fixture-only commits bc43972 and578460e, corrected path audit7f4d613,
final maintained audit teste35cde0. Production source unchanged by these patches.

Initial578460e audit FAIL: scrubbing all ownedwork before explicit forbidden
checks hid obsolete work/stage references and prefix-sibling work-foreign paths.
Independent regression6b9114e reproduced both. Final audit checks raw text for
forbidden descendants/siblings, permits the owned subtree only against forbidden
ancestors, and requires a trailing path-component boundary. Both negatives now
fail closed; actual owned relocated paths remain allowed.

Independent actual installed consumer PASS for GCC, Clang, ASan+UBSan with leak
detection, and TSan, serial consumer builds maxj2. Exact7f4d613 runner snapshot
was exercised against A stable build-o4-{gcc,clang,asan,tsan}, production6fce3a8.
Logs: build-fixture-review/installed-{gcc,clang,asan,tsan}.log. The runner performs
normal installs/two relocations, imported-artifact checks, dynamic-loader relative
path and digest checks, actual embedder lifecycle and installed CLI MOCK run.
No live provider calls. External admitted curl remains uninstrumented.

Sanitizer selection matches actual producer TSan cache and CTest ASan argument;
producer configuration forbids both modes together. LD_LIBRARY_PATH/PRELOAD,
credential and build search-path scrubbing remains intact. Compiler scratch now
uses build-local explicit directories/TMPDIR. Allocation-negative diagnostic and
optional live-client workspace paths reviewed statically only; no paid call or
full allocation diagnostic run is claimed. Core IPC correction was separately
reviewed under3c8aa99. This closes onlycc9759b, not wholeO4 acceptance.

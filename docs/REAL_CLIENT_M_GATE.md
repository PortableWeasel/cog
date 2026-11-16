# M single-process gate decision

Gate `d0f910f` passes at integration `0f70e9d841e4e91f30d3a714f1cd18595591bb1c`.
All eight explicit predecessors and both acceptance children are complete.
This accepts the single-process coding client; it does not close N, O or P.

- [Independent offline evidence](REAL_CLIENT_M_OFFLINE_EVIDENCE.md) and its
  [exact manifest](REAL_CLIENT_M_TEST_MANIFEST.json): full strict GCC/Clang
  target builds; 129 applicable tests pass in each GCC, Clang and separate
  ASan/UBSan/LSan profile; 13 separate concurrency checks pass under TSan.
  All twelve retained log hashes were revalidated before this decision.
- [Live evidence](REAL_CLIENT_M_LIVE_EVIDENCE.md) and
  [sanitized machine receipt](REAL_CLIENT_M_LIVE_RECEIPT.json): exactly three
  generation requests, split two initial and one resumed, each capped at512.
  The genuine installed public application consumer read, edited and built
  the disposable program from40 to42, recorded one successful effect, then
  resumed in a new process from four to six messages without replaying it.
  This is explicitly a public application consumer, not a literal CLI cap claim.
- Independent final review passed source5605775 plus evidencec2ff753. After
  the no-ff merge, Python syntax, receipt invariants and installed executable
  hashes passed. Local TLS rehearsal passed again with two initial requests,
  one resume and one offline-only unsolicited-tool guard. The registered
  strict header gate passed1/1. No further paid requests were made.

The six remaining registered tests are O obligations: `unit-orchestrator`,
`integration-orchestrator-ipc`, `integration-orchestrator-stress`,
`scenario-test_07_aux_crash`, `scenario-test_09_subagent_messaging` and
`scenario-test_10_fan_out_fan_in`. Their actual failures and missing nonblocking
orchestration integration remain recorded in the offline evidence. The total
is135 registered tests, not a135-test pass. Actual authenticated remote child,
headless replay and quiescent migration composition remain N/O work. All38
original/revised scenarios and the complete suite remain mandatory at P.

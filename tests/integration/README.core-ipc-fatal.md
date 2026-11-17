# Fatal IPC integration oracle

Ticket 3c8aa99 corrects the obsolete malformed-frame disconnect expectation at
5e24284. No production code changes.

`core.h` cog_core_step returns/latches the first fatal IPC error with done=true.
The archived O2 private-layout record on `tix show 99c42ec` distinguishes retained
ordinary EOF event ordering from fatal Q PROTOCOL/LIMIT. Independently accepted
O2 commit 35c7049 and its evidence on the same ticket retire the exact endpoint
before further queue admission/dispatch, while preserving already-copied event
owners. The IPC_DISCONNECTED enum names an event; it does not promise a newly
synthesized notification after fatal retirement.

The integration case now checks LIMIT/done, zero dispatch in that fatal slice,
latched drive_end result, actual raw-peer EOF/reset after draining server handshake
bytes, no message/disconnect fabrication, preserved CONNECTED descriptor across
explicit rebegin, no rediscovered failure, and listener path removal on free.
CONNECTED may have dispatched before the fatal slice or remain queued until
rebegin; both valid cases are checked. The listener/attached/full-queue ordinary
EOF cases still require DISCONNECTED exactly once, unchanged.

Direct strict GCC and Clang compilation and actual-library integration runs pass.
Evidence: fix-core-ipc-fatal-fixture/build-fatal-fixture/check.log. An uncommitted
build-only core mutation suppressing the fatal return still retires the endpoint;
the corrected test fails its LIMIT/done/latch assertions, proving socket closure
alone cannot pass. Evidence: build-fatal-fixture/negative.log. No shared build,
production change, sanitizer/full-suite claim or independent acceptance here.

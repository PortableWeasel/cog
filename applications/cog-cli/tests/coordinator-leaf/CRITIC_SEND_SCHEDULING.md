# Independent coordinator scheduling delta review

Scoped source PASS exact2fb74ea versus previously reviewed5e21e0f. Includes
114b793/9bd03c8/ec2f337/c92c5fa/aba19b7 plus final UI-drain correction2fb74ea.
This is not full B/O4 or native-failure acceptance.

Reviewed every replaced native send producer: dedicated PROFILE, forwarded
commands/ACKs, child start/cancel, UPDATE replies, mailbox/delegate/publication
replies, and addressed HANDOFF request. Separate pending_reply_wire retains
firstreply AGAIN and cooldown duplicate demand independently of semanticpending.
Retained fair pass services owed exactresponse and reports eligibility; unknown
selector failsclosed. Final supersedes olderprovisional with freshwire; copied
clock doesnotreset on duplicate or pressure. No operationdeadline is repurposed.
Actual retirement clears selectors withreceiptproof, key reset clears scalarstate.

DEGRADED returnsAGAIN only at transportadmission. Existing placement requires
HEALTHY and profile readiness; remotechild admission also checksHEALTHY. No
new task/guard/route authority is granted. Native service continues its existing
health escalation and operation deadline; no nativefailover/rebind is introduced.

Final shutdown change stops the control core early only when local executions
and exact registryborrows, nativeprocesses and handoff ownership are drained,
with no live UI record or protected commandrecord. r.done still requires actual
control_drive_ended on a subsequent drive slice. Existing UI deadline drain
remains when any UI owner/receipt exists. Actual fault/shutdown/native handoff
oracles remain A integration obligations; no lightweight claim substitutes them.

Independent exact-source leaves PASS5/5 GCC,5/5 Clang,5/5 owned-source Clang
ASan/UBSan/LSan: actual orchestrator/Q scheduling, admission, realregistrygroup,
receiptpressure, streamidentity. New reviewer negatives prove degraded freshwire
acceptsno copy, rejected higherrequest capacity failure preserves owedreply and
cooldown, orphan replyselector failsclosed without sending an unrelatedframe.
Existing schedule oracle covers one-frame queuepressure, exactlydue1000ms,
one-waycopystop, overflow, provisional/final replacement and independent earlier
deadline. Synthetic managedscalar record is explicit: scheduling fixture drives
real Q/orchestrator sends but does notclaim spawnedprocess nativeadmission.

Stable actual libraries copied atA260ab25 under
review-o4-app/build-app-review/joined, adjacent SHA inventory. Reviewed host source
compiled directly; archiveddependencies uninstrumented. Owned sanitizer scope
excludes dependency/wholejoinedlayout compatibility. No stubs/heavybuild/author
source edits. Logs/TMPDIR local build-send-review. No runtimebug tickets closed.

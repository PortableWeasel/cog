# Scoped worker copied-send review

Independent source PASS at7b9db0b (productioneeead6a) versus prior13ee548.
Only C worker/delegation scheduling reviewed, not B integration or original D.

All application producers route through retained_send: UPDATE, generic outgoing
requests, incoming COMMAND/HANDOFF/POLICY receipts, dedicated PROFILE ACK,
handle-owned DELEGATE/MAILBOX, one-way result-save ACK and terminal confirmation.
Native CGW lifecycle retains its existing distinct send contract. New wire is
eligible independently of prior copied marker; same accepted wire defers1000ms;
AGAIN changes neither copied marker nor due time. Checked future-time arithmetic
precedes request/reply send; one-way copies need no future timestamp. Original
operation/native deadlines remain separate. Replies retain incoming delivery
pending across cooldown; PROFILE preserves its own flag/schedule on rearm.
Attempt retirement clears clocks with key/proof and preserves actual arenas and
outbound identity counters. Same-key handoff reuse preserves retained lane state.

Independent actual-source tests PASS4/4 GCC,4/4 Clang,4/4 owned-source Clang
ASan/UBSan/LSan. Actual one-frame IPC tests cover fullTX/AGAIN, zero repeated
accepted copies duringcooldown, exact1000ms eligibility, overflowbeforecopy,
one-way copiedstop, PROFILE duplicatecooldown, retained genericreply demand,
and actual retirement. Added reviewer checks: a fresh wire sends immediately
inside its predecessor cooldown; production worker_step reports an unselected
retained lane's exact futuredue and reports immediate eligibility/maxwait0 after
that due timestamp is reached, without another incoming duplicate.

Stable actual A libraries copied at260ab25 are under
review-o4-app/build-app-review/joined, adjacent archive-sha256.txt. Owned C source
is compiled directly by fixtures; dependencies remain uninstrumented. Sanitizer
claims cover owned source/fixtures only. Header-private metadata changed, so no
claim is made for unrelated archived B bodies interpreting new lane storage;
these tests do not drive that joined host path. No stubs or heavybuild.
Logs and TMPDIR are localbuild-worker-leaf. Source unchanged by reviewer.

Scoped source PASS only. fcda02c/7e3eb45 stay open until actual joined5s native
cancellation and15s repeated handoff oracles pass with B scheduling integrated.
No full C/O4 final gate or runtime bug closure.

# O2 accepted-endpoint allocation clarification

Ticket b55dd9e; dependent source tickets e55f22c/959efdc, parent 99c42ec.
Recovered body commit 64386bc is preserved, but its pre-OOM test reports are
not current acceptance evidence. This amendment changes comments and design
only; independent header review precedes further dependent body changes.

## Pass 1 — resource ownership

Goal: reconcile cooperative step with the existing Q/M core configuration and
IPC constructor contracts, without adding storage or changing capacities.
Existing core listener service is the sole affected block; no new diagram node,
API, PoC, dependency or dispatch path is required. The existing IPC/core boundary
continues to own accepted endpoints.

```text
host -> core_new: reserve tracking/queues/arena and transfer listener
host -> drive_begin -> step (or run -> same engine)
core -> endpoint_accept: construct one endpoint only below peer capacity
core -> reserved peer record: retain endpoint and copied connection proof
core -> endpoint_free: retire endpoint once, release reserved record
```

| Call/resource | Header contract | Bound/owner |
|---|---|---|
| core_new / ipc_endpoint | cog/core.h | successful construction transfers listener; fixed records/arena |
| core_step / core_run | cog/core.h | same bounded listener service; no driver or per-message growth |
| endpoint_accept | cog/ipc.h | explicit inherited bounded framing workspaces |
| endpoint_free | cog/ipc.h | core pairs each successful accepted constructor with disposal |
| agent execution borrow/return | cog/agent.h | existing exact session only; no allocation |

Gap fixed: step's blanket allocation prohibition contradicted its own config
contract. The named accepted-endpoint constructor remains admitted in manual
and run modes; `ipc_peer_capacity` caps live accepted endpoints, not the number
of connections over the entire core lifetime. Full capacity cannot grow tables.

## Pass 2 — constructor failure and bounded pressure

```text
step -> endpoint_accept -> endpoint_new_ex: framing reservation fails (NOMEM)
endpoint_accept -> accepted socket: close; no endpoint published
step -> info: done=true; latch first NOMEM
host -> drive_end: return NOMEM, consume driver ownership
host -> explicit external-work cleanup -> core_free: release retained owners
```

`endpoint_accept` is declared in cog/ipc.h; `core_step`, `drive_end` and
`core_free` are declared in cog/core.h. Existing IPC construction cleans partial
workspace reservations and closes the just-accepted socket on failure. The core
adds a tracking record only after success. Existing accepted peers/listener and
queued work retain their owners on driver failure until normal explicit cleanup;
no successful connection event may be invented for the failed constructor.
The amendment makes NOMEM propagation explicit rather than disguising it as
idle/AGAIN or retrying inside a step. An earlier first error remains first.

At full peer capacity, admission cannot construct another tracked endpoint.
At full message/arena capacity, complete frames remain endpoint-owned until a
reserved descriptor and payload slice are available; there is no per-message
allocation fallback. Proof acquisition stays independent of event pressure.
Allocation instrumentation must distinguish the admitted named constructor's
inherited fixed workspaces from forbidden peer-table, message, driver or borrow
growth. Inject failure at each constructor allocation and verify no leaked
socket/workspace, published peer, event or second effect. These are acceptance
requirements, not claims of new runtime tests in this header-only pass.

## Pass 3 — shutdown, rebegin and consistency

```text
core accepts endpoint -> handshake proof -> retained frame/event
host -> stop -> step(done) -> drive_end: release only driver ownership
host -> drive_begin -> step: reuse endpoint, proof and reserved tracking slot
core -> peer retirement: endpoint_free once; clear matching record
host -> stop/end -> external work drained and child borrows returned
host -> core_free: free remaining accepted endpoints before owned listener
```

End/begin does not discard a live connection or construct a replacement.
Retirement and final core destruction own the matching accepted destructor;
record compaction moves endpoint/proof/publication state together. Event proof
copies survive endpoint retirement for their bounded handler lease. No borrowed
child tree, driver state or queued payload gains a constructor exception.

Rechecked the resource table against core.h's config, run/begin/step/end/free,
ipc.h's accept/new_ex/free, and agent.h's exact borrow/return contracts. All20
scenario traces in REAL_CLIENT_CORE_O2_LAYOUT.md retain their existing meaning;
04/05 cover cold start/shutdown, 07/11 cover peer failure/reconnect, 16 covers
first-error propagation and 03/10/17 preserve exact child lifetime. No API,
capacity, type, source body or CMake change is needed for this clarification.
Final self-review found no remaining allocation-contract gap in this scope.

Fresh post-recovery validation: 12/12 standalone/repeated public include checks
passed for core.h, ipc.h and agent.h using GCC/Clang C11 and G++/Clang++ C++11,
strict project warning flags applicable to each language with Werror. Commands
and output are retained in build-o2-header-recovery/headers.log. Diff whitespace
check passed. No heavy build, live model call or source runtime validation was
performed. Independent clarification review remains required; recovered body
64386bc still needs fresh runtime, allocation-failure and sanitizer evidence.

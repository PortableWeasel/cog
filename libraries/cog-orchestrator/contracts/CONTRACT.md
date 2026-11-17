# cog-orchestrator implemented contract

The authoritative interface is [orchestrator.h](../include/cog/orchestrator.h).
The approved [N contract](../../../docs/DESIGN.md#headless-control-children-and-handoff) and
O3 layout (`tix show 21a4a04`) replace the
historical synchronous text HELLO/PID and self-connected worker profile.

- `new` reserves all worker/link/event/route/ingress/path storage and a fresh
  nonzero incarnation. It validates child-reap disposition; it never forks.
- `spawn` publishes an owned native reservation asynchronously. Worker identity,
  immutable fork PID and actual authenticated fabric peer are distinct. Fixed
  argv and minimal environment carry no model/task/credential configuration.
- `service` rotates bounded native/link/listener units without blocking. Native
  proof is captured at the handshake barrier; exact owned PID/UID and managed
  HELLO precede READY. Proof-only silent links time out without gaining authority.
- `stop`, `kill`, `request_shutdown` and the legacy `shutdown` wrapper preserve
  exact PID ownership. Every signal follows exact-PID WNOHANG; ECHILD is permanent
  ownership loss. Only legacy shutdown waits. Free requires disposed children.
- `copy_event`/`ack_event` retain copied classed events, with three independent
  control reservations per worker and held mandatory IDs. Ordinary pressure
  cannot prevent native cleanup. Event copies never borrow endpoint memory.
- `send`, heartbeat, health, list and placement operate on authenticated bounded
  state. Connection loss ends the managed attempt; no rebind, task replay or
  durable ownership is inferred from health or process death.
- `release_worker` refuses native, retained message/control, link and route
  references. Successful retirement frees a slot but never reuses its identity.
- `publish_owner`, `session_owner`, `forget_owner` and sessions expose advisory
  routes. Exact durable store tuple/checkpoint validation does not itself grant
  a guard or activate an executor. Private registration hooks are diagnosis only.
- CGW1 and CGH1 codecs are pure, scalar, transactional and allocation-free. They
  cannot authenticate, signal, reap, publish routes or execute handoff phases.

Single owner thread; no global state, constructors or dynamic dispatch registry.
Named accepted endpoints retain their explicit bounded constructor/destructor
profile. Frame/event/payload/path storage does not grow in the hot path.

Writer evidence (`tix show d5280b7`) records
real process/failure/resource tests and separates component checks from final
independent review and O/P integration acceptance.

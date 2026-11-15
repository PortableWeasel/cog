# Transport service quantum correction (F / 62993d0)

The prior `net.h` promised that the quantum bounded CPU, syscall and dependency
work. libcurl's nonblocking `curl_multi_perform` can perform TLS, DNS and OS
operations internally without exposing a countable preemption boundary. A
finite timeout of zero avoids a positive wait but does not make that stronger
claim enforceable. This correction does not change any declaration or ABI.

1. **Contract pass.** The quantum limits accepted framing/completion units and
   request slots driven in one service call. Scanning slot bookkeeping remains
   bounded by the configured slot count, and retained input by configured
   capacities. The contract no longer purports to cap third-party CPU or syscall
   counts. No hard elapsed-time guarantee follows.
2. **Scenario pass.** In R08, bounded rotating net service still allows the core
   hook to return and dispatch cancellation even with ready traffic; cancellation
   retains queued items and adds a terminal result. In R16, shutdown detaches
   active c-ares transfers without waiting for a peer and leaves explicit release
   ownership. The core's fair hook must still be called despite queued events;
   the quantum correction does not establish hard OS scheduling latency. Deadline
   exhaustion reports an immediate next deadline until pending work is processed.
3. **Edge/consistency pass.** Each request slot has its own multi driver, so one
   curl multi call cannot invoke prerequisite callbacks for every simultaneous
   transfer. Rotating selection prevents one slot monopolizing the driver budget;
   a full ring pauses that transfer. Header/body callbacks consume accepted-unit
   budget; terminal completions and timeout publication also consume it. Callback
   attempts rejected with PAUSE and dependency-internal work are not countable
   accepted units. The local HTTPS fixture concurrently services two requests with
   quantum one and checks progress, a full retained ring and independent peek.
   The DNS probe tests cancellation/timeout on the selected dependency binary;
   neither fixture proves a hard elapsed-time guarantee under arbitrary load.

This is a targeted author retrace, not independent acceptance. The independent
reviewer must recheck the corrected wording, service/terminal accounting, the
all-ready and shutdown cases, and the relationship to the core fairness hook.

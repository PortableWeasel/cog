# Installed host output responsiveness

Bug `02678a1` under acceptance `83b30aa`, baseline `84e2272`. The bounded pipe
probe establishes a full4096-byte pipe and an actual blocked write to stdout or
stderr with SIGINT/SIGTERM masked for signalfd. All four stream/signal cases hang
until an external reader releases backpressure. This is a host output bug; no
nonblocking-output precondition exists for the executable.

## Pass 1: private interface before bodies

`host_output.h` declares a main-owned, zero-initialized adapter lifetime. Explicit
GNU FILE constructors supply matching checked fclose destruction. The standard
FILE bindings change only inside this executable lifetime, with no application
library/public FILE behavior change and no new mutable global context.

Each regular-file adapter duplicates the inherited descriptor without reopening
or seeking, so shared offset and O_APPEND remain intact. Socket output uses a
single send with MSG_DONTWAIT. FIFO/character output independently reopens
/proc/self/fd with O_NONBLOCK/O_NOCTTY; it never F_SETFLs an inherited description.
If an independent nonblocking endpoint cannot be obtained, construction fails
closed. Fixed stdout buffering and unbuffered stderr add no output queue/thread
or later adapter allocation. The adapter performs one system write per callback.
Short writes, saturation or other errors latch IO and prevent replay/retry.
Regular filesystem I/O remains subject to the filesystem's ordinary behavior.

Saturation is an output failure, rather than a request to wait indefinitely for
a slow consumer. Checked app output failure handling cancels/drains, saves partial and
identified tool outcomes, reaps tools and releases owners. A prior work error
retains precedence. Final diagnostics also use the bounded adapter, so a full
stderr cannot block cleanup. An initial adapter construction failure returns
failure without trying a fallback write through the inherited blocked stream.

## Pass 2: scenario trace before bodies

| Case | Private sequence and invariant |
|---|---|
| full stdout | independent FIFO open; app checked flush gets EAGAIN/IO; normal drain; no wait for consumer |
| full stderr | unbuffered adapter gets immediate IO; constructor/log/app error reaches checked host exit; final diagnostic also fails promptly |
| INT/TERM | existing signalfd service/cleanup remains reachable, or output IO has already safely ended the app; no masked blocking FIFO/socket write |
| inherited shared flags | no F_SETFL; parent and inherited descriptions preserve exact flags before/after child |
| regular shared offset | duplicate original description; actual writes advance inherited offset; no reopen-to-zero or lseek restoration |
| partial nonblocking write | latch IO after accepted prefix, never attempt remainder or replay prefix on flush/close |
| help/invalid options | adapters installed before parsing; checked close catches buffered option/help output failures |
| constructor unwind | build both private streams before publishing; close only new descriptors/FILEs on any failure; standard bindings unchanged |
| final teardown | checked stdout close; app/net/runtime cleanup; final bounded stderr diagnostic; checked adapter close; signal disposition/mask restoration |
| embedding | helper is executable-only source; cog::cli and caller-provided FILE behavior do not change |

FIFO reopen includes NONBLOCK on the open itself, so missing readers produce
immediate failure rather than an open wait. Its extra writer does not extend EOF
beyond the original inherited writer, which main still owns until process exit.
TTY reopen includes NOCTTY, so setup cannot acquire a new controlling terminal.
Private socket sends include NOSIGNAL and the host keeps SIGPIPE ignored through
the final pipe/character diagnostic close. Original FILEs have no buffered bytes:
main calls this constructor before any parse/log/output. The adapter neither
flushes nor closes those originals; later process exit sees only their original
empty buffers. Every adapter-only descriptor closes on setup/final failure.

The existing public error/lifetime contract supports this implementation; no
new public API, store/tool policy or network behavior is required. Runtime and
independent-review evidence will follow in this document.

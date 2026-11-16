# O6 private constructor completion (10dde8a)

N final7eb24f1/O integration0ae8213 remain authoritative. This narrow private
amendment fills two constructor joins found while implementing O6: startup host
must attach its newly constructed net/canonical paths AFTER resolving J once,
and workers/O4 must read the CGB1 child tool mask from opaque owned configuration.
No public API, live reload scope, directory selection or execution authority is
expanded. Dependent bodies wait for three targeted passes and different review.

## Pass1 — interface and ownership

configuration.h declares finish_host and immutable tool_mask getter. The finalizer
is an optional named startup constructor step before bind/app publication; legacy
single-process config/bind callers need not use it. It consumes no generation or
secret, allocates nothing, copies canonical representations of SAME caller-proven
directories into remaining64KiB string storage and attaches one borrowed net.
Wrong origin/phase, bound state, missing/mismatched paths/net or insufficient
capacity leaves the complete configuration byte-for-byte unchanged.

Private storage will distinguish STARTUP/RELOAD/CHILD/BOOTSTRAP origin, one
host-finished flag and effective workspace mask. Metadata is never eligible.
Getter exposes only four immutable workspace bits, not policy grants, resolver
authority or provider/tool pointers. Startup computes base mask, reload preserves
it, child construction intersects it and CGB1 decode retains the validated mask.
All storage remains covered by existing whole-owner zero-before-free cleanup.

Path lexical profile: '/' root or slash-separated nonempty components, no '.'
or '..' component/repeated/trailing slash, full valid UTF-8/non-NUL bytes,≤4095
payload. Spaces/Unicode/literal punctuation are not silently rewritten. Host owns
real path/open/identity/permission verification before this pure call and directory
lifetime afterward; helper cannot claim that strings prove filesystem identity.

Pass2 scenario retrace and pass3 consistency review are pending. No new bodies.

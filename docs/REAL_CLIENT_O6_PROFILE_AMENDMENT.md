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

## Pass2 — three targeted end-to-end traces

```mermaid
sequenceDiagram
 Host->>J: configuration_new(root input net=NULL,G1), resolve once
 Host->>Host: open/create/verify SAME configured directories; canonical paths
 Host->>Net: explicit initialized-host net_new
 Host->>J: finish_host(owned unbound cfg,borrowed net,canonical paths)
 Host->>J: tool_mask; configuration_bind
 Host->>Executor: execution_new consumes finished resolved G1
```

REAL needs a nonNULL actual net; MOCK needs NULL. Finalizer preserves generation,
secret/reference/model/limits/policy and startup selectors exactly. Source env/file/
argv/resolver cannot be accessed twice; net/path verification is caller-owned
before the call. After bind, all rebinding/finalization attempts are INVALID_STATE.
Provider disconnect precedes configuration cleanup; host destroys net afterward.

```mermaid
sequenceDiagram
 Parent->>J: configuration_child_new(parent mask,requested subset)
 Parent->>J: tool_mask(child) then bootstrap_encode
 Worker->>J: bootstrap_new(exact CGB1), no source resolution
 Worker->>J: tool_mask(decoded immutable profile)
 Worker->>Toolkit: original descriptor subset; fixed policy/guard before STARTED
```

Startup mask describes workspace capability, independent of per-call policy:
DURABLE+workspace gets four bits, otherwise0. Child stores parent-mask AND
requested-mask; reload preserves mask; CGB1 preserves validated known bits. DENY
still dominates execution regardless of mask; an ASK answer is not inherited
ALLOW. This avoids inventing a new wire predicate DENY=>mask0 or changing M's
advertised schema behavior. Getter cannot reveal credentials or elevate policy.

```mermaid
sequenceDiagram
 Caller->>J: metadata/already-bound/reload/child/finished context
 J-->>Caller: INVALID_STATE, original bytes/pointers unchanged
 Caller->>J: unbound startup,invalid net/path/presence/capacity
 J-->>Caller: NULL_PTR/INVALID_ARG/LIMIT before any copy/publication
 Caller->>J: free constructor failure; separate net cleanup
```

Normal standalone config+bind with an input net remains valid and never calls
the optional finalizer. LIST/RECOVER remains offline; no finalizer needed or
allowed. Canonical workspace is absent iff original workspace was absent, and
store must already be DURABLE. Capacity checks include both new path strings
and terminators before any unused-arena bytes are written. No side effect can
precede a rejected argument, repeated call or exhausted arena.

Pass3 full NULL/ordering/lifetime/compile consistency review remains pending.
No dependent implementation bodies have been written.

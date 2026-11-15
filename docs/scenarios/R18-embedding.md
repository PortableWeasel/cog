# R18: Install, external provider, independent net contexts and cleanup

Stage 14.D pass 2; baseline header pass 1 `29b41e8`, amended by this pass.
This is a contract trace, **not an executable conformance pass**.

## Sequence

```mermaid
sequenceDiagram
    Host->>Host: explicit curl/dependency global initialization
    Host->>Net: cog_net_new(netA,explicit_profile_config)
    Host->>Net: cog_net_new(netB,explicit_profile_config)
    Host->>External: cog_provider_connect(endpoint_with_static_vtable,provider)
    External->>External: first-member cog_provider_t base initialized before publication
    Host->>Provider: cog_provider_send(provider,request); poll/next/cancel
    Host->>Provider: cog_provider_disconnect(provider)
    Host->>Net: cog_net_free(netA); cog_net_free(netB)
    Host->>Host: dependency cleanup after all non-Cog users also finish
```

## Exact public coverage

Each symbol below is declared in the linked public header. Local application
decisions in the diagram are not invented library functions.

| Step | Exact calls, types, events or fields | Declaring header |
|---|---|---|
| Public external base/dispatch | `cog_provider_t`, `cog_provider_vtable_t`, `cog_provider_connect`, `cog_provider_send`, `cog_provider_poll`, `cog_provider_next`, `cog_provider_cancel`, `cog_provider_get_error`, `cog_provider_disconnect` | [libraries/cog-provider/include/cog/provider_vtable.h](../../libraries/cog-provider/include/cog/provider_vtable.h) |
| Immutable composition | `cog_endpoint_t` | [libraries/cog-provider/include/cog/endpoint.h](../../libraries/cog-provider/include/cog/endpoint.h) |
| Explicit lifecycle/memory contract | `cog_net_new`, `cog_net_free`, `cog_net_config_t`, `COG_NET_DEPENDENCY_MEMORY_DYNAMIC` | [libraries/cog-net/include/cog/net.h](../../libraries/cog-net/include/cog/net.h) |
| OpenRouter net usage requirements | `cog_openrouter_config_t`, `cog_openrouter_provider_vtable` | [libraries/cog-provider/include/cog/openrouter.h](../../libraries/cog-provider/include/cog/openrouter.h) |


## Ownership and invariants

- External provider embeds cog_provider_t FIRST and uses only installed public headers. All eight static callbacks are required; no src/provider_internal.h dependency or guessed memory prefix.
- Host validates actual shipped c-ares-only provenance (ENABLE_ARES=ON/ENABLE_THREADED_RESOLVER=OFF), runtime ASYNCHDNS/ares and DNS_SERVERS probe. Threaded/hybrid/unknown builds fail UNSUPPORTED. No QUICK_EXIT abandoned thread workaround.
- Two net contexts have independent slots, IDs, rings and cleanup. Freeing A cannot invalidate B, and never calls dependency-global cleanup. Shared context providers use id-local peek/ack, not accidental global draining.
- DYNAMIC explicitly permits third-party handle-owned service-time allocation; no total memory bound is promised. Nonzero dependency_memory_limit is UNSUPPORTED. Gate E must accept/reject this ownership profile.

## Failure and capacity branches

- Mixed/reverse public-header standalone C11/C++17 compilation and out-of-tree static-vtable consumer must pass. Installed provider target must export the new net usage requirements; core retains only forward dependencies.
- Allocation/backend failure unwinds local handles without cleaning the host runtime. Maintained shipped dependency build repeats the controlled DNS cancellation suite; research versions are not permanent version approval.

## Evidence and gap disposition

Pass1/transport research compile and lifecycle evidence exists; implementation/exported-target migration remains unimplemented.

**Required future executable evidence:** F/H/P: isolated installed consumers, link closure, public external provider fixture, two-context cancellation and shipped dependency-profile tests.

**Amendment references:** D02,D10 in [pass-2 register](../REAL_CLIENT_SCENARIO_PASS2.md).

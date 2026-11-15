# cog-net transport contract

The normative contract is [`include/cog/net.h`](../include/cog/net.h), together
with the accepted real-client design and scenario traces. The former socket
client's automatic retry, HTTP URLs and poll-driven ephemeral responses are no
longer the contract. The host explicitly owns process-level curl initialization
and cleanup, and acknowledges dynamic dependency allocations. Cog validates a
c-ares-only build and the loaded curl artifact; see the
[dependency profile](../cmake/README.md).

Each context reserves Cog-owned request, response and header storage at
construction. A successful request owns an easy handle and a slot until explicit
release; cancellation preserves already queued items and adds one terminal
outcome. Per-request peek/ack gives consumers independent retained views; global
peek rotates after acknowledgement. HTTP errors remain HTTP responses and no
request is retried. HTTPS verification is always enabled; proxies and redirects
are disabled. Service uses nonblocking curl multi work and reports deadlines.

`test_net` covers admission, lifecycle, retained terminal results and input
validation. `run_https.py` creates an ephemeral loopback CA/server to check HTTPS,
capacity failure, untrusted certificates and absence of replay. `test_net_oom`
injects failures into curl allocator callbacks during setup and request creation.
The independent DNS probe covers cancel/timeout and thread/descriptor cleanup for
the actual audited dependency build. Further adversarial coverage and independent
review are required before package F is complete.

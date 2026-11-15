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
are disabled. Each transfer uses a fresh HTTP/1.1 connection with reuse forbidden
and ALPN disabled. This intentionally gives up pooling and HTTP/2 for the
initial no-replay profile. Netrc and implicit proxy use are disabled; URL
userinfo is rejected and no curl credentials are configured. The prerequisite
callback rejects any unexpected second HTTP attempt. Completion is captured and
detached before later local deadlines or cancellation can replace its outcome;
publication remains subject to the service quantum. Service uses nonblocking
curl multi work and reports deadlines.

`test_net` covers admission, lifecycle, retained terminal results and input
validation. `run_https.py` creates an ephemeral loopback CA/server to check HTTPS,
capacity failure, untrusted certificates and absence of replay. `test_net_oom`
injects failures into curl allocator callbacks during setup and request creation.
The independent DNS probe covers cancel/timeout and thread/descriptor cleanup for
the actual audited dependency build. Further adversarial coverage and independent
review are required before package F is complete.

The audited curl 8.23.0 source has separate automatic replay paths for a dead
reused connection and HTTP/2 stream refusal/downgrade. Fresh connections and
HTTP/1.1 with ALPN disabled remove those triggers; no URL credentials, netrc,
proxy or redirect following are enabled. The prerequisite callback additionally
rejects a second attempt before an HTTP request can be sent. The regression
fixture verifies one server-observed POST after response loss, and one POST each
for authentication challenge, redirect and proxy-auth challenge with hostile
proxy environment settings. New dependency versions require the same source
and behavioral revalidation; this profile is not a generic promise about every
possible curl build or protocol.

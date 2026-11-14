# PoC 3 — HTTP streaming + connection tracking

This proof-of-concept validates the **Networking** and **Observability**
functional blocks from `docs/DESIGN.md`: streaming LLM-style HTTP responses
with connection lifecycle tracking, stall detection, and retry on
transient failure. It also directly answers the ticket's comparison
question — *libcurl vs. a minimal hand-rolled HTTP client* — by running
both transports against the same mock server and the same scenarios.

## What it proves

| Acceptance criterion | How it is proven |
|---|---|
| Connect to a (mock) server and read SSE-style chunks | `http-mock-server` streams `text/event-stream` bodies using HTTP/1.1 `Transfer-Encoding: chunked`; both clients decode chunks and the demo parses `data:` events |
| Maintain a connection ID, start time, and last-chunk timestamp | `conn.h` tracker: `conn_id`, `start_ms`, `last_chunk_ms`, plus byte/chunk counters and a lifecycle state; snapshots render all of it |
| Detect a stalled connection when no chunk arrives within N seconds | `/stall` scenario: the server goes silent for 2 s, both clients classify the connection `stalled` at the 700 ms threshold |
| Retry on transient failure | `/flaky` scenario: the server answers the client's first request with `503`, the transports retry (up to `--max-retries`) and finish the stream |
| Compare libcurl vs. a minimal hand-rolled client | Both transports share one contract in `http_common.h`; the demo runs each scenario with `--client hand`, `--client curl`, or `--client both` |

## Layout

```
conn.h / conn.c         Connection tracker (opaque type, CLOCK_MONOTONIC
                        timestamps, stall check, status snapshots)
sse.h / sse.c           Incremental SSE (text/event-stream) event parser
http_common.h/.c        Shared request/result contract + URL splitting
http_hand.c             Hand-rolled HTTP/1.1 client (POSIX sockets only)
http_curl.c             libcurl client using the multi interface
mock_server.c           Mock streaming server (/stream, /stall, /flaky)
demo.c                  Scenario harness asserting the acceptance criteria
demo-test.sh            CTest wrapper: server lifecycle + 3 scenarios
```

## Framing: three layers

The wire format is the usual stack used by real LLM providers
(OpenAI-compatible APIs, etc.):

```
TCP stream
  └─ HTTP/1.1 Transfer-Encoding: chunked     (size-prefixed body chunks)
       └─ text/event-stream                  (SSE: "data: ...\n\n" events)
            └─ connection tracker            (Observability, not on the wire)
```

1. **HTTP chunked framing** is decoded by the transports. The hand-rolled
   client implements the full grammar: hex size line, payload, CRLF,
   terminal `0` chunk, and trailer headers ending in a blank line. It was
   ~150 lines and the trickiest part was a small buffered reader
   (`struct hread`) so that line parsing never over-reads into payload
   bytes. libcurl does the same decoding internally.
2. **SSE framing** is a separate, transport-independent layer
   (`sse.c`): feeds arbitrary byte slices, emits complete `data:`
   events. Both transports deliver raw chunk bytes to the same callback,
   so event counting is identical across clients even though TCP and
   chunk boundaries split differently.
3. **The connection tracker** observes from above: bytes and chunks
   increment as the transports consume the stream; timestamps drive stall
   detection; `conn_tracker_snapshot()` provides the "queryable
   connection-status interface" from the Observability block.

## Connection tracking

```c
typedef enum conn_state {
    CONN_STATE_IDLE, CONN_STATE_CONNECTING, CONN_STATE_STREAMING,
    CONN_STATE_STALLED, CONN_STATE_DONE, CONN_STATE_ERROR
} conn_state_t;
```

Each request gets an opaque `conn_tracker_t` (caller-allocated via
`conn_tracker_create()`, freed with `conn_tracker_destroy()` — the project
requires every allocation to be visible). Fields:

- `conn_id` — a monotonic ID per logical request (the DESIGN.md "request
  ID"). On a retry the tracker is *reset*, keeping the ID but re-basing
  the timestamps so the numbers always describe the final attempt.
- `start_ms`, `last_chunk_ms` — `CLOCK_MONOTONIC`, immune to wall-clock
  jumps. `last_chunk_ms` is updated by `conn_tracker_note_chunk()` from
  inside the transports.
- `bytes`, `chunks` — running counters.
- `state` — transitioned by the transports and by stall detection.
- `conn_tracker_snapshot()` renders e.g.:
  `id=1 state=stalled age=902ms last_chunk=701ms_ago bytes=57 chunks=3`

## Stall detection: two mechanisms, one source of truth

- **Hand-rolled client**: every socket read goes through
  `poll(fd, timeout = stall_ms)`; on timeout the tracker is marked
  `STALLED` and the transfer aborts. This is the classic "recv with a
  deadline" pattern.
- **libcurl**: the multi interface wakes the driver loop at least every
  100 ms; each wake calls `conn_tracker_check_stall()`, which compares
  `now - last_chunk_ms` against the threshold and aborts via handle
  removal. libcurl's *built-in* alternative is
  `CURLOPT_LOW_SPEED_LIMIT` + `CURLOPT_LOW_SPEED_TIME` (abort when average
  throughput drops below a floor), which needs no application loop but is
  coarser and cannot feed the per-chunk tracker — that is why the tracker
  drives detection here.

The tracker is the single source of truth for *when* a stall happened;
each transport just enforces the deadline its architecture allows.

## Retry policy

Transports treat any `5xx` as transient: close the connection, sleep a
fixed 50 ms backoff, and re-issue the request, up to
`cfg.max_retries`. `4xx` and transport-level errors never retry. The
result records `retries`, and the demo asserts `retries >= 1` for the
flaky scenario. (An exponential backoff with jitter would be the obvious
production refinement; a fixed sleep keeps the PoC deterministic.)

## libcurl vs. hand-rolled: notes from the comparison

| Aspect | Hand-rolled (`http_hand.c`) | libcurl (`http_curl.c`) |
|---|---|---|
| Dependencies | POSIX sockets only | libcurl >= 7.66 (`curl_multi_poll`) |
| Code for GET + chunked decoding | ~300 lines incl. buffered reader | ~10 lines of options |
| Stall detection | `poll()` deadline per read | tracker-driven in the multi loop |
| Live status during transfer | between chunks | every ~300 ms from the driver loop |
| TLS, proxies, redirects, HTTP/2 | not implemented | all free |
| Error taxonomy | our own `http_err_t` | map `CURLcode` + status code onto `http_err_t` |
| Retry visibility | full control | full control (start a new easy handle) |

Conclusions for the DESIGN.md "Network Transport" block:

- libcurl is the right production base (TLS alone settles it: a hand-rolled
  `https` client means OpenSSL plumbing + certificate handling).
- The *framing* (SSE) and *observability* (conn tracker) layers should
  stay ours and transport-independent — they are exactly the parts the
  demo proved work identically over either transport.
- The tracker-driven stall detection proven here (threshold + monotonic
  last-chunk timestamp) is portable to a single-threaded agent loop, which
  is Cog's default execution model.

## Building and running

```bash
cmake -B build -DCOG_BUILD_POC_HTTP_STREAMING=ON
cmake --build build
ctest --test-dir build -R http-streaming --output-on-failure   # or:
bash pocs/http-streaming/demo-test.sh \
    build/pocs/http-streaming/http-mock-server \
    build/pocs/http-streaming/http-demo
```

Manual run (two terminals, or background the server):

```bash
./build/pocs/http-streaming/http-mock-server --port 18765
./build/pocs/http-streaming/http-demo --client both --scenario stream
./build/pocs/http-streaming/http-demo --client both --scenario stall
./build/pocs/http-streaming/http-demo --client both --scenario flaky
```

The demo exits `0` only when the scenario expectation is met for every
requested client: stream must finish `done` with ≥ 5 events, stall must
end `stalled`, flaky must finish `done` with ≥ 1 retry.

Server endpoints (all on `127.0.0.1`):

- `/stream` — 8 SSE events, one per 120 ms chunk.
- `/stall?events=3&stall=2000&delay=100` — 3 events, then 2000 ms of
  intentional silence, then a final event.
- `/flaky` — alternating `503` / stream, so every client's *first* request
  fails transiently; retries always succeed.

## Decisions worth recording

1. **Mock server, not a real provider** — deterministic scenarios, no
   credentials, no network; the framing is identical to what a real
   provider sends.
2. **`Transfer-Encoding: chunked` chosen over `Content-Length`** — it is
   what streaming responses actually use (an SSE body has no known length
   up front); read-until-close is supported as a fallback in the
   hand-rolled client.
3. **Fork-per-connection mock server** — a stall handler sleeps for its
   whole silence period; a single-threaded accept loop would block every
   subsequent client, which confuses the back-to-back two-client demo. The
   `/flaky` counter lives in an anonymous `MAP_SHARED` mapping so it stays
   consistent across children.
4. **curl_multi over curl_easy_perform** — the demo needs live tracker
   snapshots and tracker-driven stall aborts *during* the transfer; that
   requires the driver loop only the multi interface offers.
5. **Opaque tracker + explicit create/destroy** — matches the project's
   "opaque typedefs, visible allocations" conventions; the tracker could
   move into a `library` later without changing callers.
6. **Timestamps in monotonic ms** — `CLOCK_MONOTONIC`; wall-clock `time()`
   would break stall detection across NTP steps.

Known limitations (fine for a PoC, noted for the library stage):
hand-rolled client has no TLS, no IP-literal (v6) URLs, cap of 1 MiB per
chunk, and fixed rather than exponential retry backoff.

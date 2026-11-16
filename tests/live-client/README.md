# Explicit installed application live smoke

Ticket `80e782e`. This is a standalone C11 consumer of the **installed public
application library**, not a replacement executable implementation and not a
claim that the literal cog-cli binary itself enforces the paid request cap.
Literal installed CLI operation is covered by the offline M fixtures. Root
approved this consumer so a test-only pre-send boundary can enforce a stronger
cross-restart budget than the available CLI flags.

The consumer links genuine installed `cog::cli` and its exported dependencies,
uses public headers only, owns curl/net lifecycle explicitly, and drives the real
configuration/provider/net/tool/store/application implementations. The sole send
wrapper validates the exact model, stream and max_tokens512, reserves an attempt
before the real send, and refuses excess attempts without calling the provider.
It never replaces a response, alters a request or reports a fabricated success.
Initial coding permits two sends; a fresh resumed process permits one. Both use
zero retries and one tool round. No automatic rerun is permitted after live failure.

The synthetic coding task asks for exactly one run_command call to check child
isolation, read main.c, replace return40 with return42, compile/test, and write
one effect marker. Assertions check exact source bytes, compiled executable
output42, saved test result, marker `applied`, one SUCCEEDED identified tool
ledger and a complete two-request conversation. A fresh process resumes the
same session, proves the earlier call/result were restored, exposes no new tool
capability, and admits one text response. No model wording is asserted.

`run.py` defaults to local TLS rehearsal and does not read pi credentials. It
checks positive coding2+resume1 HTTP requests, then a separate offline unsolicited
tool response: the unavailable tool is denied, an extra follow-up is refused
before send, and the effect marker stays unchanged. Direct wrong-model/token and
zero-budget controls prove refusal does not reach a provider at all. Successful
phases additionally call the exhausted wrapper directly after teardown; receipt
`refused=1` for those phases is this explicit negative control, not another app
or HTTP request.

Build and install Cog plus the accepted dependencies normally into a new prefix,
then relocate it. See the [dependency recipe](../../libraries/cog-net/cmake/relocatable/README.md)
and [installed-consumer fixture](../external-consumer/run.py). Configure this
standalone project with only the relocated prefix, build, and normally install:

```sh
cmake -S tests/live-client -B /tmp/cog-live-client-consumer-build \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=/tmp/cog-live-client-installed \
  -DCMAKE_INSTALL_PREFIX=/tmp/cog-live-client-installed
cmake --build /tmp/cog-live-client-consumer-build -j4
cmake --install /tmp/cog-live-client-consumer-build
python3 tests/live-client/run.py --prefix /tmp/cog-live-client-installed
```

Only an explicit separately authorized invocation may add `--live`. That mode
checks the public catalog for exact model `deepseek/deepseek-v4-flash-0731` and
tool support, then reads the already authorized OpenRouter entry from
`~/.pi/agent/auth.json` into process memory. The key goes to the constructor
callback through one anonymous pipe; it is never argv, environment, a tool FD,
printed output or persisted fixture data. The callback consumes and closes it
before tool construction. Host checks original pipe identity, and the actual
command verifies absent credential environment and original pipe inode. Captured
model/log output remains in memory and is checked for the key before discard;
workspace/store bytes are also screened. Only numeric/boolean summaries and
fixed model/endpoint labels enter optional receipts.

No LD_PRELOAD/LD_LIBRARY_PATH is used. The driver audits the installed consumer's
relative loader path, actual relocated curl/c-ares closure and exact frozen
runtime hashes before any phase. Main source/runtime defects or unsupported
provider output must be reported honestly; this fixture does not relax parsing,
retry on a different model, or spend additional requests to make a failure pass.

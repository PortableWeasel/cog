# O8 projection and attach-client owner fixtures

These fixtures compile the actual O8 sources, O5 codecs, original CGD1/text
validator, and cog-ipc endpoint/replay implementations directly. The native
Unix peer is a protocol oracle, not a substitute host runtime or an O4 integration
claim. The projection fixture wraps only replay_append for explicit fault
injection; successful calls always use the real ring.

Run from the checkout root (no CMake/dependency recovery required):

```
python3 applications/cog-cli/tests/headless-view/run.py --cc gcc --build-dir build-o8-gcc
python3 applications/cog-cli/tests/headless-view/run.py --cc clang --build-dir build-o8-clang
python3 applications/cog-cli/tests/headless-view/run.py --cc clang --sanitize --build-dir build-o8-asan
```

The runner enforces a checkout-local build* path and sets TMPDIR to its tmp
subdirectory. Each profile uses all project strict warnings with Werror, O3 and
NDEBUG, plus always-evaluated fixture checks and deliberate negative assertion
controls. Sanitizers enable leak detection and halt on ASan/UBSan findings.
Socket/output files remain under that build directory. No credentials, provider
or installed-client calls occur. Source ownership remains O8; no shared CMake
file is changed.

Covered: constructor bounds/invalid IDs, deep copied views, rollback on actual
append refusal, invalid staged view, canonical counters distinct from source,
ring eviction/generation GAP, snapshot re-commit and sequence exhaustion;
native proof/discovery/HELLO cleanup, rendered-cursor ACK, final unterminated EOF
input and final-result/ACK-of-ACK drain, exact old command bytes across higher-wire
reconnect HELLO, lost initial HELLO cleanup and original nonce retry, typed
continue/fork/cancel/reload/handoff/shutdown, unknown slash and stale policy
refusal, watcher mutation refusal, GAP full replacement/RESYNC, escaped terminal
control bytes and actual /dev/full output failure with unchanged cursor/view;
server-incarnation change with pending input, expired unknown/known detach
deadlines, independently progressing CANCEL, read-batch anchors across a newer
view, and late older results that cannot overwrite the last input outcome.
Independent critic controls additionally cover retained valid frames followed by
fatal partial framing, malformed/negative timespec fields, millisecond multiply
and add overflow, backward time, clock syscall failure and normal clock progress.
Clock faults preserve the previous clock/deadline/view and latch IO through close.
The clock wrapper is linked only into the client fixture; the projection-only
fixture has no clock wrapper (including sanitizer runtime references).

The client renders complete JSON replacement records (also preceded by a fixed
clear/home sequence for terminal mode). Decimal 64-bit IDs are JSON strings.
Compact row arrays preserve fixed schema order: items=[node,role,status,text];
agents=[agent,attempt,group,configuration,session,status,outcome,status_text,result];
policies=[session,node,ordinal,nonce,configuration,deadline,tool]. /tools renders
only policies, /history the complete current view. Command result records contain
input serial and Cog result; they do not claim model/task completion.

Additive O4 registration, when the owning shared build lane is ready:

```
target_sources(cog-cli-lib PRIVATE src/projection.c src/host_client.c)
target_include_directories(cog-cli-lib PRIVATE
    ${PROJECT_SOURCE_DIR}/libraries/cog-ipc/private)
```

Use the actual existing library target name if the shared lane changes it. The
private IPC include supplies O1's single-unit proof boundary/readiness queries;
no endpoint layout or public API changes are required. Register the Python
runner as a scoped test with a build* directory inside the checkout, or port its
source list and linker fault wrapper into an owned fixture target. Full O4
runtime/client installation and scenario gates remain outstanding.

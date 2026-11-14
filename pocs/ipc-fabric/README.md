# PoC 1 — IPC fabric over Unix domain sockets

This proof-of-concept validates the many-to-many message bus that will
connect UI processes, the main Cog core, and auxiliary core processes.

## What it proves

- A single server process can bind a Unix domain socket and accept
  multiple concurrent clients.
- Messages are framed so boundaries are unambiguous on a stream socket.
- The server can route a reply back to the client that sent the request.
- A client can disconnect and reconnect without restarting the server.

## Framing format

Each message is a single length-prefixed frame:

```
0                   1                   2                   3
0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                      payload length (uint32be)                |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                                                               |
+                          payload bytes                        +
|                                                               |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

- `payload length` is the number of bytes following the header,
  encoded in **big-endian** (network byte order).
- A length of zero is valid and denotes an empty message.
- The maximum payload size is `IPC_MAX_PAYLOAD` (65535 bytes in this PoC).

This framing scheme was chosen because:

1. It is unambiguous — there are no sentinel bytes to escape.
2. It is simple to parse with a small fixed-size header.
3. It maps naturally to a stream transport such as a Unix domain socket
   or TCP connection.

## Multiplexing approach

The server uses `poll(2)` to monitor the listening socket and all connected
client sockets in a single thread. When `poll` reports activity:

- **Listening socket readable** → `accept(2)` a new client, assign it an
  integer client ID, and add its socket to the poll set.
- **Client socket readable** → read as many bytes as available, buffering
  them until a complete frame is received, then dispatch the message.
- **Client socket error/hangup** → remove the client from the poll set and
  close its socket. The client may reconnect later.

`poll` was chosen over `epoll` for this PoC because it is POSIX and keeps the
demonstration small. For a production Cog core that needs to scale to many
thousands of auxiliary cores, `epoll` (or `kqueue` on BSD/macOS) would be a
straightforward replacement with the same framing layer.

## Building

From the repository root:

```bash
cmake -B build -DCOG_BUILD_POC_IPC_FABRIC=ON
cmake --build build
```

The build produces two executables in `build/pocs/ipc-fabric/`:

- `ipc-server`
- `ipc-client`

## Running

1. Start the server:
   ```bash
   ./build/pocs/ipc-fabric/ipc-server
   ```
   The server binds to `/tmp/cog-ipc-poc.sock` by default. Use the
   `COG_IPC_SOCKET` environment variable to override the path.

2. In one or more other terminals, run clients:
   ```bash
   ./build/pocs/ipc-fabric/ipc-client
   ```
   Each client sends a few numbered messages, prints the replies, then
   disconnects and reconnects to prove recovery.

3. Press `Ctrl-C` in the server terminal to shut it down. The server
   removes the socket path on exit.

## Acceptance check

- [x] One server process binds a Unix domain socket.
- [x] Multiple clients connect, send framed messages, and receive routed replies.
- [x] Reconnection after client disconnect works.
- [x] Framing format and multiplexing approach are documented.

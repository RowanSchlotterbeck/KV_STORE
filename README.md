# KV_STORE

A small in-memory key-value store with a text-based TCP protocol, built on
[Asio](https://think-async.com/Asio/) (standalone, non-Boost) for
single-threaded async networking.

## Layout

```
include/        Public headers (engine, parser, kv_store, wal)
network/        Server/client headers
src/            Implementation + entry points (kv_server, kv_client)
bench/          Benchmark programs
```

- `kv_store.{h,cpp}` — `execute()`: applies a parsed command to the
  in-memory `unordered_map<string, string>`.
- `parser.{h,cpp}` — `input_parser()`: tokenizes a line and validates
  command shape (see Protocol below).
- `engine.{h,cpp}` — `process_line()`: parses + executes a single line
  against the process-wide store; this is what the server calls per request.
- `wal.{h,cpp}` — `PersistentLog`: append-only, fsync-per-write command
  log. Writes are logged before they are applied; the server replays it on
  startup (`kv_server [log_path]`, default `data/kvstore.log`). Each entry
  is framed as `[u32 length][u32 CRC32C][payload]` (format in `wal.h`); a
  torn or zero-filled tail is truncated on replay, while a checksum mismatch
  stops startup.
- `crc32c.{h,cpp}` — CRC32C (Castagnoli), hardware-accelerated when the
  target supports it, with a table-driven fallback.
- `rpc_frame.{h,cpp}` — framing for node-to-node Raft RPCs:
  `[u32 length][u8 type tag][protobuf payload]` (format in `rpc_frame.h`).
- `peer_config.{h,cpp}` — static cluster config (`<node_id> <host>
  <peer_port>` per line; see `config/cluster.example.conf`).
- `network/peer_transport.h`, `src/peer_transport.cpp` — peer RPC
  listener (`PeerServer`) and connect-per-RPC client
  (`ConnectPerRpcClient`), both on the server's single `io_context`.
- `raft_node.{h,cpp}` — `RaftNode`: Raft leader election and heartbeats
  (Figure 2) on the same `io_context`, driven by two Asio timers. Election
  only: no log replication or commit index yet.
- `network/status_server.h`, `src/status_server.cpp` — `StatusServer`: a
  minimal read-only HTTP listener for the `/status` endpoint (below), on the
  same `io_context`.
- `src/server.cpp` / `src/client.cpp` — Asio TCP server and a simple
  interactive client, both built on `kv_core`.

## Building

Requires CMake 3.16+, a C++20 compiler, and standalone Asio headers.

```
cmake -S . -B build
cmake --build build -j
```

By default the build looks for Asio at `/opt/homebrew/opt/asio/include`
(Homebrew on Apple Silicon). If your Asio headers live elsewhere, install
via Homebrew (`brew install asio`) or edit the `target_include_directories`
paths in `CMakeLists.txt`.

This produces:

- `kv_server` — the TCP server, listening on port `5555`.
- `kv_client` — an interactive line-based client.
- `kv_bench_engine` — in-process engine benchmark (no networking).
- `kv_bench_network` — network round-trip benchmark against a running
  `kv_server`.

## Running

```
./build/kv_server &
./build/kv_client
```

With `--cluster <file> --node-id <id>` the server runs as one node of a Raft
cluster: it listens for peer RPCs on its `peer_port` from the config, takes
part in leader election, and replicates the leader's log, logging what it
does to stderr. Client writes are committed through Raft before they are
answered:

- **Writes** (`PUT`, `DEL`) go to the leader, which appends them to its log
  in its current term, replicates them, and replies (`OK`, `NOT_FOUND`, ...)
  only once the entry is committed on a majority and applied.
- **Reads** (`GET`) are served by the leader from its applied state,
  once it has applied everything committed before its term. They are not
  linearizable (no ReadIndex): a leader that has just been deposed without
  knowing it can serve a stale read.
- A **non-leader** answers either kind with `REDIRECT <leader_id> <host>:<client_port>`
  (just `REDIRECT <leader_id>` if the config doesn't list the leader's client
  port), or `ERR_NO_LEADER` if it doesn't know one.
- A request in progress when the leader steps down gets `ERR_LEADERSHIP_LOST`;
  one that waits more than 5s gets `ERR_TIMEOUT`. In both cases a write may
  or may not have committed: retry it (`PUT` is idempotent; a retried `DEL`
  may answer `NOT_FOUND`).

Each node keeps `currentTerm`, `votedFor` and its commit index in
`<log_path>.raftstate`, fsynced before it acts on them, so a restarted node
keeps its vote and applies only entries known committed; a corrupt state
file stops startup. Client ports come from the config's optional fourth
column, or `--client-port <port>` (default 5555; it must agree with the
config if both are given). Without `--cluster` it runs single-node, exactly
as before.

```
# three terminals
./build/kv_server data/n1.log --cluster config/cluster.example.conf --node-id 1
./build/kv_server data/n2.log --cluster config/cluster.example.conf --node-id 2
./build/kv_server data/n3.log --cluster config/cluster.example.conf --node-id 3
```

### Status endpoint

`--status-port <port>` (cluster mode only; off by default) starts a read-only
HTTP listener on `127.0.0.1:<port>` for dashboards. It must differ from this
node's client port and from every peer/client port the config lists for the
same host; a clash, or a port already in use, stops startup. Without the
flag nothing extra listens and the node behaves exactly as before.

```
./build/kv_server data/n1.log --cluster config/cluster.example.conf --node-id 1 --status-port 8081
curl http://127.0.0.1:8081/status
```

- `GET /status` → `200 application/json`
- `GET /healthz` → `200 ok`
- any other method → `405`; unknown path → `404`; malformed request → `400`

The `/status` body is one JSON object:

| Field | Type | Meaning |
|-------|------|---------|
| `node_id` | uint | this node |
| `role` | string | `"follower"`, `"candidate"` or `"leader"` |
| `current_term` | uint | current term |
| `leader_id` | uint | leader known for this term, `0` if none |
| `log_last_index` | uint | index of the last entry in this node's log |
| `commit_index` | uint | highest index known committed |
| `last_applied` | uint | highest index applied to the store |
| `peers` | array | leader: `{"id", "nextIndex", "matchIndex"}` per follower; `[]` on other roles |
| `seq` | uint | count of `/status` responses from this process (tells polls apart) |
| `timestamp_ms` | uint | wall-clock Unix time of the response, in ms |

```
{"node_id":2,"role":"leader","current_term":1,"leader_id":2,"log_last_index":2,"commit_index":2,"last_applied":2,"peers":[{"id":1,"nextIndex":3,"matchIndex":2},{"id":3,"nextIndex":3,"matchIndex":2}],"seq":2,"timestamp_ms":1792615816600}
```

It is purely an observer. It runs on the node's single event loop, so each
response is a consistent snapshot taken between Raft steps. It reads only
in-memory state: no disk, no RPCs, no waiting. Each connection carries one
request, and a client that doesn't finish within 2s is dropped. `/status`
sends `Access-Control-Allow-Origin: *` so a browser dashboard on another
origin can poll it. That is intended for local demos: the data is read-only,
but there is no authentication.

Or talk to it directly with `nc`:

```
nc 127.0.0.1 5555
```

## Protocol

One command per line, terminated by `\n`. Keys and values must be
double-quoted; the command name is not. Commands are case-insensitive on
input and normalized to uppercase.

| Command                  | Reply                          |
|---------------------------|---------------------------------|
| `PUT "<key>" "<value>"`   | `OK`                            |
| `GET "<key>"`              | the value, or `NOT_FOUND`       |
| `DEL "<key>"`              | `OK`, or `NOT_FOUND`            |
| `EXIT`                    | `EXIT` (client disconnects)     |

Malformed input (wrong quoting, oversized key/value, unknown command)
returns `INPUT_ERROR` or `ERR_UNKNOWN_CMD`.

Example session:

```
$ nc 127.0.0.1 5555
PUT "foo" "bar"
OK
GET "foo"
bar
DEL "foo"
OK
GET "foo"
NOT_FOUND
```

## Benchmarks

Two benchmark binaries live under `bench/`:

- **`kv_bench_engine`** calls `process_line()` (and the lower-level
  `execute()`) directly in-process, so it isolates parser + map
  performance from any networking cost.

  ```
  ./build/kv_bench_engine [num_ops]   # default 200000
  ```

- **`kv_bench_network`** drives a real TCP connection against a running
  `kv_server`, using the same synchronous request/response pattern as
  `kv_client`. This captures real round-trip cost (syscalls, Asio,
  Nagle/TCP effects), not just engine throughput.

  ```
  ./build/kv_server &
  ./build/kv_bench_network [host] [port] [num_ops]   # default 127.0.0.1 5555 20000
  ```

Both report throughput (ops/sec) and p50/p99 latency per command type
(`PUT`, `GET` hit, `GET` miss). Sample output on Apple Silicon (M-series):

```
$ ./build/kv_bench_engine 100000
process_line PUT                 100000 ops      21.83 ms       4580775 ops/sec  p50=0.21us  p99=0.38us
process_line GET (hit)           100000 ops      10.12 ms       9878079 ops/sec  p50=0.08us  p99=0.12us
execute PUT                      100000 ops       1.97 ms      50641706 ops/sec  p50=0.00us  p99=0.04us
execute GET (hit)                100000 ops       1.89 ms      53017378 ops/sec  p50=0.00us  p99=0.04us

$ ./build/kv_bench_network 127.0.0.1 5555 5000
PUT                          5000 ops     146.26 ms       34185 ops/sec  p50=26.2us  p99=58.5us
GET (hit)                    5000 ops     132.48 ms       37740 ops/sec  p50=26.2us  p99=34.4us
```

The gap between `process_line` and `execute` numbers shows parsing
(quote scanning, uppercasing, length checks) accounts for a meaningful
share of per-command engine cost. The much larger gap between in-process
and network numbers shows the current single connection/synchronous
round trip, not the engine, is the bottleneck for real client traffic.

## Known limitations

- Cluster mode has no log compaction or snapshots (the log grows forever
  and a restart reads all of it), no membership changes, and no
  linearizable reads. A follower far behind catches up one heartbeat per
  missing index before entries flow, and every probe carries entries even
  while it is being rejected.
- Every write is fsynced individually (no batching), so
  write throughput is bounded by disk sync latency.
- No authentication, TLS, or multi-database support.
- The server handles one line per read/write cycle per connection with
  no pipelining.

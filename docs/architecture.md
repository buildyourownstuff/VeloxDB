# Architecture

VeloxDB is split into focused modules:

- `net`: nonblocking TCP server, acceptor, worker event loops, connection buffers.
- `protocol`: RESP2 parser and response encoder.
- `commands`: command registry and command implementations.
- `storage`: sharded in-memory string engine with TTL metadata.
- `persistence`: append-only file and snapshot helpers.
- `config`, `metrics`, `util`: runtime support.

VeloxDB's standard default TCP port is `7379`. Redis compatibility remains a protocol goal, but
VeloxDB does not bind to Redis's `6379` default unless explicitly configured to do so.

## Networking

The MVP uses a single acceptor loop and multiple worker event loops. Accepted sockets are set
nonblocking and dispatched round-robin to workers. On Linux, the acceptor and workers use `epoll`.
On other platforms, VeloxDB falls back to the portable `poll()` backend. Workers own their client
connections, parser buffers, and write buffers.

The design keeps the event-loop boundary explicit so an `io_uring` backend can be introduced later
without changing command execution or storage interfaces.

## Command Execution

Requests are parsed as RESP arrays and executed through `CommandRegistry`. Command names are
case-insensitive. Unsupported commands return Redis-style errors.

Command execution is currently shared-state concurrent: each command directly accesses the sharded
storage engine. Future work can route single-key commands to owning shards and coordinate multi-key
commands explicitly.

## Storage

`StorageEngine` owns configurable shards. Each shard has its own `std::unordered_map` and
`std::shared_mutex`. Values store:

- owned string bytes
- optional absolute expiry timestamp
- version counter
- last access timestamp

The MVP uses approximate memory accounting and a safe no-eviction policy: writes fail when
`storage.max_memory` would be exceeded.

## Expiration

Expiration happens in two ways:

- Lazy expiration when a key is accessed.
- Active expiration in a background loop that samples bounded work per shard.

This avoids large stop-the-world cleanup passes.

## Persistence

AOF stores write commands as RESP arrays. Startup recovery replays complete commands and ignores a
partial final command. Expiry is persisted with absolute millisecond timestamps (`PXAT`/`PEXPIREAT`)
to avoid relative-TTL drift on restart.

`BGREWRITEAOF` compacts the AOF by materializing the current live keyspace into a temporary RESP
stream, fsyncing it, atomically replacing the active file, and reopening the append descriptor.
Write commands hold a shared rewrite barrier while they mutate memory and append to AOF; compaction
takes the exclusive side so replay order remains correct for non-idempotent commands such as `INCR`.

`SAVE` writes a RESP-command snapshot through a temporary file and atomic rename. When AOF is
enabled, it records a manifest containing the AOF byte offset covered by the snapshot. Startup loads
the snapshot only if the manifest still matches the current AOF and then replays the AOF tail from
that offset. AOF rewrite removes old manifests because compaction changes byte offsets.

## Observability

The MVP exposes basic metrics through `INFO`: uptime, connected clients, commands processed,
approximate memory, expired keys, keyspace counts, and AOF status. Logs are structured text emitted
to stderr with component and level fields.

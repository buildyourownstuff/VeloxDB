# Architecture

VeloxDB is split into focused modules:

- `net`: nonblocking TCP server, acceptor, worker event loops, connection buffers.
- `protocol`: RESP2 parser and response encoder.
- `commands`: command registry and command implementations.
- `storage`: sharded in-memory string engine with TTL metadata.
- `persistence`: append-only file and snapshot helpers.
- `config`, `metrics`, `util`: runtime support.

## Networking

The MVP uses a single acceptor loop and multiple worker event loops. Accepted sockets are set
nonblocking and dispatched round-robin to workers. Workers use `poll()` and own their client
connections, parser buffers, and write buffers.

The design keeps the event-loop boundary explicit so a Linux `epoll` or `io_uring` backend can
replace the current portable `poll()` backend later.

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

`SAVE` writes a RESP-command snapshot through a temporary file and atomic rename. In the MVP, AOF is
the recovery source when AOF is enabled.

## Observability

The MVP exposes basic metrics through `INFO`: uptime, connected clients, commands processed,
approximate memory, expired keys, keyspace counts, and AOF status. Logs are structured text emitted
to stderr with component and level fields.

# Roadmap

## Phase 1: MVP Hardening

- Linux epoll backend. Done.
- First-party `velox-cli` client. Done.
- More integration tests with `redis-cli`.
- Parser fuzz target.
- AOF rewrite and compaction.
- Snapshot/AOF manifest with offsets.
- Prometheus metrics endpoint.
- Per-command counters and latency histograms.
- Slowlog.

## Phase 2: Security and Compatibility

- AUTH.
- ACLs.
- RESP3.
- TLS and mTLS.
- More string commands: `GETDEL`, `GETEX`, `RENAME`, `TYPE`, `SCAN`.
- Safer `KEYS` with explicit warnings.

## Phase 3: Data Structures

- Lists.
- Hashes.
- Sets.
- Sorted sets.
- Streams.

## Phase 4: Replication

- Primary-replica replication.
- Replication offsets.
- Partial resync.
- Replica catch-up.
- Read replica mode.

## Phase 5: Clustering

- Hash slots.
- Cluster metadata.
- Client redirection.
- Resharding and rebalancing.

## Phase 6: Advanced Capabilities

- Transactions.
- Scripting with Lua or WASM.
- Vector search.
- JSON documents.
- Time-series module.
- Pub/sub.
- Hybrid memory/disk storage engine.
- Raft-based consensus option.
- Kubernetes operator and Helm chart.

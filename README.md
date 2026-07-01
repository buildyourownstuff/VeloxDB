# VeloxDB

VeloxDB is an early-stage, Redis-inspired in-memory key-value database written in C++20.
The current codebase is an MVP foundation: sharded in-memory storage, RESP2 command handling,
nonblocking TCP networking, append-only persistence, tests, benchmarks, and documentation.

VeloxDB is not production-safe yet. The goal is to build toward predictable latency, efficient
memory use, multi-core scaling, safe persistence, observability, and a cleaner modular database
architecture.

## Current Status

Implemented:

- RESP2 parser and encoder with incremental parsing and bounded request buffers.
- Linux epoll networking backend with portable poll fallback, acceptor dispatch, and worker event
  loops.
- Sharded string key-value storage with TTL, lazy expiration, and active expiration.
- Append-only file persistence with `always`, `everysec`, and `no` fsync policies.
- `INFO` metrics, structured stderr logging, config file/env/CLI loading.
- Deterministic unit tests and internal benchmark executable.

Not yet production-ready:

- No AUTH, ACLs, TLS, replication, clustering, or background snapshot coordination.
- Snapshot `SAVE` exists and uses a manifest with AOF offsets, but background snapshotting is planned.
- `MSET` can partially apply if a later key hits max-memory in the MVP.

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DVELOXDB_BUILD_TESTS=ON
cmake --build build -j
```

Useful CMake options:

```text
VELOXDB_BUILD_TESTS=ON/OFF
VELOXDB_BUILD_INTEGRATION_TESTS=ON/OFF
VELOXDB_BUILD_BENCHMARKS=ON/OFF
VELOXDB_BUILD_FUZZERS=ON/OFF
VELOXDB_ENABLE_ASAN=ON/OFF
VELOXDB_ENABLE_UBSAN=ON/OFF
VELOXDB_ENABLE_TSAN=ON/OFF
VELOXDB_ENABLE_LTO=ON/OFF
```

## Run

VeloxDB's standard default port is `7379`, intentionally separate from Redis's default `6379`.

```bash
./build/veloxdb --config ./veloxdb.toml
```

Or override values directly:

```bash
./build/veloxdb --host 0.0.0.0 --port 7379 --workers 4
```

## Test

```bash
ctest --test-dir build --output-on-failure
```

When `redis-cli` is available, CTest also runs an integration smoke test that starts VeloxDB,
exercises supported Redis-compatible commands, restarts the server, and verifies AOF recovery. The
integration test is skipped automatically when `redis-cli` is not installed.

Convenience:

```bash
./scripts/run-tests.sh
```

RESP parser fuzzing is available with Clang/libFuzzer:

```bash
./scripts/run-fuzz.sh 60
```

See [docs/fuzzing.md](docs/fuzzing.md).

## Redis CLI Examples

VeloxDB has a first-party CLI that defaults to VeloxDB's `7379` port:

```bash
velox-cli PING
velox-cli SET name dev
velox-cli GET name
velox-cli --raw GET name
```

See [docs/cli.md](docs/cli.md) for setup and usage.

Redis-compatible clients can still be used for the supported RESP2 command surface:

```bash
redis-cli -p 7379 PING
redis-cli -p 7379 SET name dev
redis-cli -p 7379 GET name
redis-cli -p 7379 INCR counter
redis-cli -p 7379 EXPIRE name 10
redis-cli -p 7379 TTL name
redis-cli -p 7379 INFO
```

## Supported Commands

`PING`, `ECHO`, `COMMAND`, `INFO`, `DBSIZE`, `FLUSHDB`, `SET`, `GET`, `DEL`,
`EXISTS`, `MGET`, `MSET`, `INCR`, `DECR`, `APPEND`, `STRLEN`, `EXPIRE`,
`PEXPIRE`, `PEXPIREAT`, `TTL`, `PTTL`, `PERSIST`, `CONFIG GET`, `CONFIG SET`,
`SAVE`, `BGSAVE`, `BGREWRITEAOF`, `SHUTDOWN`.

See [docs/commands.md](docs/commands.md) for compatibility notes.

## Persistence

AOF is enabled by default:

```toml
[persistence]
aof_enabled = true
aof_path = "./data/veloxdb.aof"
fsync_policy = "everysec"
snapshot_path = "./data/veloxdb.snapshot"
manifest_path = "./data/veloxdb.manifest"
```

Recovery replays complete RESP commands and ignores a partially written final command. If a valid
snapshot manifest exists, VeloxDB loads the snapshot and replays only the AOF tail after the recorded
byte offset. See [docs/persistence.md](docs/persistence.md) for crash-safety limitations.

## Benchmarks

Internal smoke benchmark:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DVELOXDB_BUILD_BENCHMARKS=ON
cmake --build build -j
./build/benchmarks/veloxdb_bench
```

Redis comparison workflow:

```bash
redis-server --port 6379
./build/veloxdb --port 7379
redis-benchmark -p 7379 -t get,set -n 100000 -c 50
redis-benchmark -p 6379 -t get,set -n 100000 -c 50
```

Do not treat benchmark results as comparable unless machine specs, build type, persistence settings,
dataset size, client count, command mix, throughput, and p50/p95/p99/p999 latency are recorded.

## Docker

Pull from GitHub Container Registry:

```bash
docker pull ghcr.io/buildyourownstuff/veloxdb:latest
docker run --name veloxdb --rm -d -p 7379:7379 -v veloxdb-data:/data ghcr.io/buildyourownstuff/veloxdb:latest
```

Published VeloxDB images include the matching `velox-cli` binary:

```bash
docker exec -it veloxdb velox-cli PING
docker stop veloxdb
```

Build locally:

```bash
docker build -t veloxdb:dev -f docker/Dockerfile .
docker run --rm -p 7379:7379 veloxdb:dev
```

Docker Compose:

```bash
docker compose -f docker/docker-compose.yml up --build
```

Docker Compose from GHCR:

```bash
docker compose -f docker/docker-compose.ghcr.yml up
```

See [docs/packaging.md](docs/packaging.md) for GHCR tags and publishing details.

## Releases

Every push to `main` updates the GitHub prerelease `VeloxDB Continuous (main)`.

VeloxDB uses semantic Git tags like `v0.1.0`. GitHub Releases are named `VeloxDB v0.1.0`
and include source archives plus checksums. Container package publishing is manual:

```bash
make package-release PACKAGE_TAG=0.1.0 PACKAGE_REF=v0.1.0
```

See [docs/release.md](docs/release.md).

## Contributing

Commit messages should use conventional commit-style prefixes such as `feat:`, `fix:`, `docs:`,
`ci:`, `build:`, `test:`, `perf:`, `refactor:`, or `chore:`. See
[docs/contributing.md](docs/contributing.md).

## Roadmap

Near-term work is focused on io_uring experiments, AUTH/ACL, Prometheus metrics,
slowlog, richer latency histograms, more Redis string command coverage, and background persistence
coordination. See [docs/roadmap.md](docs/roadmap.md).

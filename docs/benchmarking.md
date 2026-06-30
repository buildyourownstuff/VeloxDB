# Benchmarking

VeloxDB includes an internal benchmark executable and can also be exercised by `redis-benchmark`.

## Internal Benchmarks

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DVELOXDB_BUILD_BENCHMARKS=ON
cmake --build build -j
./build/benchmarks/veloxdb_bench
```

Current internal benchmark groups:

- RESP parser SET command parsing
- Storage SET throughput
- Storage GET throughput
- Mixed command execution SET/GET loop

These are smoke benchmarks, not product claims.

## Redis Comparison

```bash
redis-server --port 6379
./build/veloxdb --port 7379
redis-benchmark -p 7379 -t get,set -n 100000 -c 50
redis-benchmark -p 6379 -t get,set -n 100000 -c 50
```

Record all of the following before comparing:

- CPU model and core count
- RAM and storage type
- OS and kernel
- Compiler and version
- Build type and flags
- VeloxDB config including workers, shards, max memory, and persistence
- Redis version and config
- Dataset size
- Client count
- Command mix
- Throughput
- p50, p95, p99, and p999 latency when available

Do not claim VeloxDB is faster than Redis without reproducible data.

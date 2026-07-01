# Fuzzing

VeloxDB includes a libFuzzer target for the RESP parser:

```text
veloxdb_resp_parser_fuzzer
```

The fuzzer feeds arbitrary bytes to both value parsing and command parsing paths, including
incremental chunking, malformed requests, multiple commands per buffer, and encoder round-tripping
for successfully parsed commands.

## Requirements

- Clang with libFuzzer support.
- compiler-rt/libFuzzer runtime, for example `libclang-rt-18-dev` on Ubuntu 24.04.
- CMake.

## Quick Run

```bash
./scripts/run-fuzz.sh 60
```

The optional argument is the maximum runtime in seconds. The script creates a small seed corpus in
`fuzz/corpus/resp` if one does not already exist.

## Manual Build

```bash
CXX=clang++ cmake -S . -B build-fuzz \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DVELOXDB_BUILD_FUZZERS=ON \
  -DVELOXDB_BUILD_TESTS=OFF \
  -DVELOXDB_BUILD_BENCHMARKS=OFF
cmake --build build-fuzz -j --target veloxdb_resp_parser_fuzzer
./build-fuzz/fuzz/veloxdb_resp_parser_fuzzer fuzz/corpus/resp -max_total_time=60
```

`VELOXDB_FUZZ_SANITIZERS` defaults to `address,undefined`.

## Corpus and Findings

Crash artifacts and minimized reproducers produced by libFuzzer should be checked into a dedicated
regression corpus only after the underlying parser bug is fixed and covered by a deterministic unit
test.

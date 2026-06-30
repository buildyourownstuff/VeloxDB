#!/usr/bin/env bash
set -euo pipefail

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DVELOXDB_BUILD_BENCHMARKS=ON
cmake --build build -j
exec ./build/benchmarks/veloxdb_bench

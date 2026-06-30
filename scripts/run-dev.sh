#!/usr/bin/env bash
set -euo pipefail

cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DVELOXDB_BUILD_TESTS=ON -DVELOXDB_BUILD_BENCHMARKS=ON
cmake --build build -j

if [ "$#" -eq 0 ]; then
  exec ./build/veloxdb --config ./veloxdb.toml
fi

exec ./build/veloxdb "$@"

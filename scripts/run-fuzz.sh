#!/usr/bin/env bash
set -euo pipefail

duration="${1:-30}"
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
corpus_dir="${VELOXDB_FUZZ_CORPUS:-$root/fuzz/corpus/resp}"

if [[ -z "${CXX:-}" ]]; then
  export CXX=clang++
fi

mkdir -p "$corpus_dir"

if [[ ! -e "$corpus_dir/ping.resp" ]]; then
  printf '*1\r\n$4\r\nPING\r\n' >"$corpus_dir/ping.resp"
fi
if [[ ! -e "$corpus_dir/set_get.resp" ]]; then
  printf '*3\r\n$3\r\nSET\r\n$4\r\nname\r\n$3\r\ndev\r\n*2\r\n$3\r\nGET\r\n$4\r\nname\r\n' \
    >"$corpus_dir/set_get.resp"
fi
if [[ ! -e "$corpus_dir/inline.resp" ]]; then
  printf 'PING hello\r\n' >"$corpus_dir/inline.resp"
fi

cmake -S "$root" -B "$root/build-fuzz" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DVELOXDB_BUILD_FUZZERS=ON \
  -DVELOXDB_BUILD_TESTS=OFF \
  -DVELOXDB_BUILD_BENCHMARKS=OFF
cmake --build "$root/build-fuzz" -j --target veloxdb_resp_parser_fuzzer

exec "$root/build-fuzz/fuzz/veloxdb_resp_parser_fuzzer" "$corpus_dir" \
  -max_total_time="$duration" \
  -print_final_stats=1

#!/usr/bin/env bash
set -euo pipefail

skip() {
  echo "SKIP: $*" >&2
  exit 77
}

fail() {
  echo "FAIL: $*" >&2
  if [[ -n "${log_file:-}" && -f "$log_file" ]]; then
    echo "--- veloxdb log ---" >&2
    cat "$log_file" >&2
  fi
  exit 1
}

veloxdb_bin="${1:-${VELOXDB_BIN:-}}"
if [[ -z "$veloxdb_bin" || ! -x "$veloxdb_bin" ]]; then
  skip "veloxdb binary is not executable"
fi

redis_cli="${VELOXDB_REDIS_CLI:-redis-cli}"
if ! command -v "$redis_cli" >/dev/null 2>&1; then
  skip "redis-cli not found"
fi

choose_port() {
  if command -v python3 >/dev/null 2>&1; then
    python3 - <<'PY'
import socket

sock = socket.socket()
sock.bind(("127.0.0.1", 0))
print(sock.getsockname()[1])
sock.close()
PY
    return
  fi
  echo "${VELOXDB_TEST_PORT:-17379}"
}

tmpdir="$(mktemp -d)"
port="$(choose_port)"
host="127.0.0.1"
aof_path="$tmpdir/veloxdb.aof"
snapshot_path="$tmpdir/veloxdb.snapshot"
log_file="$tmpdir/veloxdb.log"
server_pid=""

cleanup() {
  if [[ -n "$server_pid" ]] && kill -0 "$server_pid" >/dev/null 2>&1; then
    kill "$server_pid" >/dev/null 2>&1 || true
    wait "$server_pid" >/dev/null 2>&1 || true
  fi
  rm -rf "$tmpdir"
}
trap cleanup EXIT

redis_raw() {
  "$redis_cli" --raw -h "$host" -p "$port" "$@"
}

wait_until_ready() {
  for _ in $(seq 1 80); do
    if ! kill -0 "$server_pid" >/dev/null 2>&1; then
      fail "veloxdb exited before accepting connections"
    fi
    if redis_raw PING >/dev/null 2>&1; then
      return
    fi
    sleep 0.05
  done
  fail "veloxdb did not become ready on $host:$port"
}

start_server() {
  "$veloxdb_bin" \
    --host "$host" \
    --port "$port" \
    --workers 2 \
    --aof-enabled true \
    --aof-path "$aof_path" \
    --fsync-policy always \
    --snapshot-path "$snapshot_path" \
    --log-level warn >>"$log_file" 2>&1 &
  server_pid="$!"
  wait_until_ready
}

stop_server() {
  if [[ -n "$server_pid" ]] && kill -0 "$server_pid" >/dev/null 2>&1; then
    kill "$server_pid" >/dev/null 2>&1 || true
    wait "$server_pid" >/dev/null 2>&1 || true
  fi
  server_pid=""
}

assert_eq() {
  local expected="$1"
  local actual="$2"
  local name="$3"
  if [[ "$actual" != "$expected" ]]; then
    fail "$name: expected '$expected', got '$actual'"
  fi
}

assert_contains() {
  local haystack="$1"
  local needle="$2"
  local name="$3"
  if [[ "$haystack" != *"$needle"* ]]; then
    fail "$name: expected output to contain '$needle', got '$haystack'"
  fi
}

assert_int_between() {
  local actual="$1"
  local min="$2"
  local max="$3"
  local name="$4"
  if [[ ! "$actual" =~ ^-?[0-9]+$ || "$actual" -lt "$min" || "$actual" -gt "$max" ]]; then
    fail "$name: expected integer in [$min, $max], got '$actual'"
  fi
}

start_server

assert_eq "PONG" "$(redis_raw PING)" "PING"
assert_eq "hello" "$(redis_raw PING hello)" "PING message"
assert_eq "hello" "$(redis_raw ECHO hello)" "ECHO"

assert_eq "OK" "$(redis_raw SET name dev)" "SET"
assert_eq "dev" "$(redis_raw GET name)" "GET"
assert_eq "1" "$(redis_raw EXISTS name)" "EXISTS present"
assert_eq "0" "$(redis_raw EXISTS missing)" "EXISTS missing"
assert_eq "1" "$(redis_raw DEL name)" "DEL"
assert_eq "0" "$(redis_raw EXISTS name)" "EXISTS after DEL"

assert_eq "OK" "$(redis_raw MSET mk1 one mk2 two)" "MSET"
assert_eq $'one\ntwo' "$(redis_raw MGET mk1 mk2)" "MGET"
assert_eq "1" "$(redis_raw INCR counter)" "INCR"
assert_eq "0" "$(redis_raw DECR counter)" "DECR"

assert_eq "3" "$(redis_raw APPEND append_key abc)" "APPEND create"
assert_eq "6" "$(redis_raw APPEND append_key def)" "APPEND update"
assert_eq "6" "$(redis_raw STRLEN append_key)" "STRLEN"

assert_eq "1" "$(redis_raw EXPIRE mk1 20)" "EXPIRE"
assert_int_between "$(redis_raw TTL mk1)" 1 20 "TTL"
assert_eq "1" "$(redis_raw PERSIST mk1)" "PERSIST"
assert_eq "-1" "$(redis_raw TTL mk1)" "TTL after PERSIST"

assert_contains "$(redis_raw INFO)" "veloxdb_version:" "INFO"
assert_eq "4" "$(redis_raw DBSIZE)" "DBSIZE before FLUSHDB"
assert_eq "OK" "$(redis_raw FLUSHDB)" "FLUSHDB"
assert_eq "0" "$(redis_raw DBSIZE)" "DBSIZE after FLUSHDB"

assert_eq "OK" "$(redis_raw SET persistent_key persistent_value)" "persistent SET"
stop_server

start_server
assert_eq "persistent_value" "$(redis_raw GET persistent_key)" "AOF recovery after restart"

echo "redis-cli integration smoke passed"

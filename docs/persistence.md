# Persistence

VeloxDB's MVP persistence system is append-only logging.

## AOF

Write commands are appended as RESP arrays:

```text
*3\r\n$3\r\nSET\r\n$4\r\nname\r\n$3\r\ndev\r\n
```

Supported fsync policies:

- `always`: fsync after each appended command.
- `everysec`: write commands immediately and fsync from a background loop.
- `no`: rely on the operating system.

The default is `everysec`.

## AOF Rewrite

`BGREWRITEAOF` rewrites the append-only file into a compact RESP command stream representing the
current live keyspace. Deleted keys, overwritten values, expired keys, and historical non-idempotent
commands such as `INCR` are materialized away into `SET` plus `PEXPIREAT` where needed.

The current implementation is synchronous: it takes an AOF rewrite barrier, writes a temporary file,
fsyncs it, atomically renames it over the active AOF, and reopens the append descriptor. Normal write
commands use a shared side of the same barrier so compaction has a clean ordering boundary without
serializing ordinary multi-worker writes outside rewrite windows.

## Recovery

On startup, VeloxDB replays complete AOF commands. A partially written final command is ignored.
Malformed complete commands stop recovery and are reported as errors.

Expiration is logged with absolute millisecond timestamps (`PXAT` or `PEXPIREAT`) so a key that
expired while the server was down is not resurrected.

## SAVE Snapshot

`SAVE` writes a RESP-command snapshot to a temporary file and atomically renames it into place. When
AOF is enabled, `SAVE` takes the exclusive AOF checkpoint barrier, records the current AOF byte
offset, writes the snapshot, and then writes a small manifest as the final commit marker.

The manifest contains:

```text
format_version=1
snapshot_path=./data/veloxdb.snapshot
aof_path=./data/veloxdb.aof
aof_offset=12345
created_unix_ms=...
```

With AOF enabled, startup only trusts a snapshot when this manifest is present, matches the current
snapshot and AOF paths, and points to an offset within the current AOF. Recovery then loads the
snapshot and replays only the AOF tail after `aof_offset`. If the manifest is missing or stale,
VeloxDB ignores the snapshot and replays the AOF from the beginning.

`BGREWRITEAOF` invalidates the snapshot manifest because AOF compaction changes byte offsets. After
rewrite, recovery uses the compacted AOF as the primary source until the next `SAVE`.

## Known Crash-Safety Limits

- AOF rewrite is synchronous; background rewrite and incremental copy-on-write buffering are planned.
- No checksummed segment format yet.
- Snapshot manifests do not yet include checksums or AOF generation IDs.
- A command can mutate memory and then report an AOF append error if the append fails.
- Multi-command persistence windows are minimized for SET with TTL by using absolute SET options,
  but broader transactional AOF grouping is future work.

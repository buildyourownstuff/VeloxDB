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

## Recovery

On startup, VeloxDB replays complete AOF commands. A partially written final command is ignored.
Malformed complete commands stop recovery and are reported as errors.

Expiration is logged with absolute millisecond timestamps (`PXAT` or `PEXPIREAT`) so a key that
expired while the server was down is not resurrected.

## SAVE Snapshot

`SAVE` writes a RESP-command snapshot to a temporary file and atomically renames it into place.

Current limitation: when AOF is enabled, AOF remains the primary recovery source. Snapshot/AOF
manifest coordination and AOF truncation after snapshot are planned future work.

## Known Crash-Safety Limits

- No AOF rewrite or compaction yet.
- No checksummed segment format yet.
- No manifest file tying snapshots to AOF offsets yet.
- A command can mutate memory and then report an AOF append error if the append fails.
- Multi-command persistence windows are minimized for SET with TTL by using absolute SET options,
  but broader transactional AOF grouping is future work.

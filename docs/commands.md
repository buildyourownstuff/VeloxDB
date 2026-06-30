# Commands

Compatibility is intentionally partial. VeloxDB implements Redis-compatible behavior where useful,
but it does not try to expose the whole Redis surface in the MVP.

| Command | Status | Notes |
| --- | --- | --- |
| PING | Done | RESP2 compatible |
| ECHO | Done | Bulk string response |
| COMMAND | Partial | Returns command names, not full Redis metadata |
| INFO | Partial | VeloxDB metrics sections |
| DBSIZE | Done | Counts non-expired keys |
| FLUSHDB | Done | Destructive, persisted to AOF |
| SET | Partial | Supports basic, EX, PX, EXAT, PXAT, NX, XX |
| GET | Done | String values only |
| DEL | Done | Multi-key |
| EXISTS | Done | Multi-key |
| MGET | Done | String values only |
| MSET | Partial | Sequential apply; rollback on max-memory failure is planned |
| INCR | Done | 64-bit signed integer semantics |
| DECR | Done | 64-bit signed integer semantics |
| APPEND | Done | Clears prior TTL like Redis string writes |
| STRLEN | Done | Missing key returns 0 |
| EXPIRE | Done | Seconds |
| PEXPIRE | Done | Milliseconds |
| PEXPIREAT | Done | Used by AOF; available to clients |
| TTL | Done | `-2` missing, `-1` no expiry |
| PTTL | Done | Millisecond TTL |
| PERSIST | Done | Removes expiry |
| CONFIG GET | Partial | Static config plus safe runtime values |
| CONFIG SET | Partial | `logging.level`, `metrics.enabled` only |
| SAVE | Partial | Writes snapshot file |
| BGSAVE | Planned | Returns an error in MVP |
| SHUTDOWN | Protected | Disabled unless `admin.shutdown_enabled=true` |

Unsupported commands return:

```text
-ERR unknown command 'COMMANDNAME'\r\n
```

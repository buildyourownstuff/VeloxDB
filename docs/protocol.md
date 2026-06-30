# Protocol

VeloxDB supports RESP2 for the MVP.

Incoming commands should be RESP arrays:

```text
*2\r\n$4\r\nPING\r\n$5\r\nhello\r\n
```

Inline commands such as `PING\r\n` are accepted for convenience, but Redis-compatible clients should
prefer RESP arrays.

## Parser Properties

- Incremental parsing for partial reads.
- Multiple commands per packet.
- Bounded request buffer.
- Bounded array element count.
- Bounded bulk string size.
- Safe error reporting for malformed CRLF, invalid lengths, null command args, and nested command
  arrays.

## Supported RESP Types

- Simple strings
- Errors
- Integers
- Bulk strings including null bulk
- Arrays including null arrays at the value layer

Command arguments are accepted as bulk strings, simple strings, errors, or integers. Nested arrays
are rejected as command arguments.

## Response Encoding

Examples:

```text
PING        -> +PONG\r\n
PING hello  -> $5\r\nhello\r\n
GET missing -> $-1\r\n
INCR x      -> :1\r\n
```

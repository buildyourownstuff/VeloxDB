# Contributing

## Commit Messages

VeloxDB uses conventional commit-style messages.

Use one of these common types:

- `feat:` for user-visible features.
- `fix:` for bug fixes.
- `docs:` for documentation-only changes.
- `ci:` for GitHub Actions and automation.
- `build:` for build system or packaging changes.
- `test:` for tests.
- `perf:` for performance work.
- `refactor:` for behavior-preserving code changes.
- `chore:` for maintenance.

Examples:

```text
ci: publish continuous GitHub release on push
fix: reject oversized RESP arrays before allocation
docs: document VeloxDB default port
refactor: isolate shard expiration cursor
```

Avoid vague messages like `update`, `changes`, or `stuff`.

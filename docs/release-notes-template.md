Early VeloxDB release.

## Docker

```bash
docker pull ghcr.io/buildyourownstuff/veloxdb:latest
docker run --rm -p 7379:7379 -v veloxdb-data:/data ghcr.io/buildyourownstuff/veloxdb:latest
```

## Quick Check

```bash
redis-cli -p 7379 PING
```

## Notes

VeloxDB is early-stage software and is not production-safe yet. Review the README and persistence
documentation before using it for important data.

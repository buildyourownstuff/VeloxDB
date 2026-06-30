# Docker Package Publishing

VeloxDB publishes container images to GitHub Container Registry.

Image:

```text
ghcr.io/buildyourownstuff/veloxdb
```

## Pull and Run

```bash
docker pull ghcr.io/buildyourownstuff/veloxdb:latest
docker run --rm -p 7379:7379 -v veloxdb-data:/data ghcr.io/buildyourownstuff/veloxdb:latest
```

Then connect with:

```bash
redis-cli -p 7379 PING
```

Docker Compose without a local build:

```bash
docker compose -f docker/docker-compose.ghcr.yml up
```

## Manual Package Release

Container publishing is manual. Normal pushes to `main` do not publish a new package.

Publish a released package from a tag:

```bash
make package-release PACKAGE_TAG=0.1.0 PACKAGE_REF=v0.1.0
```

Publish a released package and also update `latest`:

```bash
make package-release PACKAGE_TAG=0.1.0 PACKAGE_REF=v0.1.0 PUBLISH_LATEST=true
```

Publish `latest` from `main`:

```bash
make package-latest
```

The workflow publishes:

- The requested `PACKAGE_TAG`.
- `MAJOR.MINOR` when `PACKAGE_TAG` is semantic, for example `0.1` from `0.1.0`.
- `latest` only when `PUBLISH_LATEST=true`.
- `sha-<commit>` for traceable commit images.

Pull a released version:

```bash
docker pull ghcr.io/buildyourownstuff/veloxdb:0.1.0
```

## Workflow

The workflow lives at:

```text
.github/workflows/docker-publish.yml
```

It builds multi-architecture images for:

- `linux/amd64`
- `linux/arm64`

It only runs through manual workflow dispatch, normally via `make package-release`, and publishes to
GHCR using GitHub's built-in `GITHUB_TOKEN`.

## Package Visibility

The first published GHCR package may need to be made public in GitHub:

1. Open the repository on GitHub.
2. Go to **Packages**.
3. Open the `veloxdb` container package.
4. Set package visibility to public if public pulls are desired.

Without public visibility, users need permission to the organization package before they can pull.

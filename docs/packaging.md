# Docker Package Publishing

VeloxDB publishes container images to GitHub Container Registry.

Image:

```text
ghcr.io/buildyourownstuff/veloxdb
```

## Pull and Run

```bash
docker pull ghcr.io/buildyourownstuff/veloxdb:latest
docker run --rm -p 6379:6379 -v veloxdb-data:/data ghcr.io/buildyourownstuff/veloxdb:latest
```

Then connect with:

```bash
redis-cli -p 6379 PING
```

Docker Compose without a local build:

```bash
docker compose -f docker/docker-compose.ghcr.yml up
```

## Published Tags

The GitHub Actions workflow publishes:

- `latest` from the default branch.
- `main` from pushes to `main`.
- `sha-<commit>` for traceable commit images.
- Semantic version tags from Git tags like `v0.1.0`.

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

Pull requests build the image but do not publish it. Pushes to `main`, semantic version tags, and
manual workflow dispatches publish to GHCR using GitHub's built-in `GITHUB_TOKEN`.

## Package Visibility

The first published GHCR package may need to be made public in GitHub:

1. Open the repository on GitHub.
2. Go to **Packages**.
3. Open the `veloxdb` container package.
4. Set package visibility to public if public pulls are desired.

Without public visibility, users need permission to the organization package before they can pull.

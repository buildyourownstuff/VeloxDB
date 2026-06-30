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

Published VeloxDB images also include the first-party CLI:

```bash
docker run --name veloxdb --rm -d -p 7379:7379 -v veloxdb-data:/data ghcr.io/buildyourownstuff/veloxdb:latest
docker exec -it veloxdb velox-cli PING
docker stop veloxdb
```

Docker Compose without a local build:

```bash
docker compose -f docker/docker-compose.ghcr.yml up
```

## Manual Package Release

Container publishing is manual. Normal pushes to `main` do not publish a new package.

Publish a released package from a tag:

```bash
make github-release VERSION=0.1.0
make package-release PACKAGE_TAG=0.1.0 PACKAGE_REF=v0.1.0
```

The tag must exist before the package workflow is dispatched. `make package-release` checks this
locally so a missing tag does not become a failed GitHub Actions run.

Publish a released package and also update `latest`:

```bash
make package-release PACKAGE_TAG=0.1.0 PACKAGE_REF=v0.1.0 PUBLISH_LATEST=true
```

Create the GitHub release tag and publish the Docker package in one command:

```bash
make release VERSION=0.1.0 PUBLISH_LATEST=true
```

Publish `latest` from `main`:

```bash
make package-latest
```

Publish a versioned package from the current `main` branch instead of a tag:

```bash
make package-release PACKAGE_TAG=0.1.0 PACKAGE_REF=main
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

The published package target is `runtime-with-cli`, so the image contains:

- `/usr/local/bin/veloxdb`
- `/usr/local/bin/velox-cli`

Because `velox-cli` lives in the private `buildyourownstuff/velox-cli` repository, the workflow
requires a repository secret:

```text
VELOX_CLI_REPO_TOKEN
```

Use a fine-grained token with read-only Contents access to `buildyourownstuff/velox-cli`.

The workflow input `velox_cli_ref` controls the CLI ref included in the package. It defaults to
`main`. For a fully pinned release package, dispatch the workflow with a tagged CLI ref such as
`v0.1.0`.

Local source builds can produce the same image shape when the sibling CLI checkout is available:

```bash
docker buildx build \
  --target runtime-with-cli \
  --build-context velox_cli=../velox-cli \
  -t veloxdb:dev \
  -f docker/Dockerfile .
```

The plain local `docker build -f docker/Dockerfile .` path remains server-only so VeloxDB can still
be built without access to the private CLI repository.

## Package Visibility

The first published GHCR package may need to be made public in GitHub:

1. Open the repository on GitHub.
2. Go to **Packages**.
3. Open the `veloxdb` container package.
4. Set package visibility to public if public pulls are desired.

Without public visibility, users need permission to the organization package before they can pull.

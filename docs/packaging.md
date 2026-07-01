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
make package-release PACKAGE_TAG=0.1.0 PACKAGE_REF=v0.1.0 VELOX_CLI_REF=v0.1.0
```

The tag must exist before the package workflow is dispatched. `make package-release` checks this
locally so a missing tag does not become a failed GitHub Actions run.

The package source ref must include the CLI-enabled Docker target. Older tags created before
`runtime-with-cli` was added cannot publish the bundled CLI image. For those, either publish from
`main` explicitly or cut a newer release tag first.

The Docker package also includes `velox-cli`, which lives in the private
`buildyourownstuff/velox-cli` repository. Before the first package release, add a read-only deploy
key to `buildyourownstuff/velox-cli`, then store its private key as this repository secret on
`buildyourownstuff/VeloxDB`:

```bash
gh secret set VELOX_CLI_DEPLOY_KEY --repo buildyourownstuff/VeloxDB < /path/to/private/key
```

If deploy keys are disabled for the CLI repository, use a fine-grained token with read-only
Contents access to `buildyourownstuff/velox-cli` instead:

```bash
gh secret set VELOX_CLI_REPO_TOKEN --repo buildyourownstuff/VeloxDB
```

`make package-release` checks for one of these secrets locally before dispatching the workflow.

Publish a released package and also update `latest`:

```bash
make package-release PACKAGE_TAG=0.1.0 PACKAGE_REF=v0.1.0 VELOX_CLI_REF=v0.1.0 PUBLISH_LATEST=true
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
make package-release PACKAGE_TAG=0.1.0 PACKAGE_REF=main VELOX_CLI_REF=v0.1.0
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
VELOX_CLI_DEPLOY_KEY
```

or:

```text
VELOX_CLI_REPO_TOKEN
```

Prefer a read-only deploy key attached to `buildyourownstuff/velox-cli`; store the private key in
the VeloxDB repository secret above. If deploy keys are disabled, use a fine-grained token with
read-only Contents access to the CLI repository.

The workflow input `velox_cli_ref` controls the CLI ref included in the package. For a fully pinned
release package, use a tagged CLI ref such as `v0.1.0`.

VeloxDB and `velox-cli` version tags are intentionally independent. The Makefile defaults
`VELOX_CLI_REF` from `VELOX_CLI_VERSION`; override it when publishing a database release that should
bundle a different CLI tag or branch.

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

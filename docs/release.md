# Release Process

VeloxDB uses semantic version tags and GitHub Releases.

## Continuous Main Release

Every push to `main` updates a visible GitHub prerelease:

```text
VeloxDB Continuous (main)
```

It uses the moving tag:

```text
continuous-main
```

This release includes source archives and `SHA256SUMS` for the latest `main` commit. It is marked as
a prerelease and is not marked as the latest stable release.

## Naming

Release tags must use:

```text
vMAJOR.MINOR.PATCH
```

Examples:

```text
v0.1.0
v0.2.0
v1.0.0
```

GitHub Releases are named:

```text
VeloxDB vMAJOR.MINOR.PATCH
```

## Create a Release

1. Update the CMake project version in [CMakeLists.txt](../CMakeLists.txt).
2. Commit and push the version change.
3. Create and push a matching tag:

```bash
git tag -a v0.1.0 -m "VeloxDB v0.1.0"
git push origin v0.1.0
```

The release workflow validates that the tag version matches the CMake project version. It creates:

- A GitHub Release named `VeloxDB vX.Y.Z`.
- Source archives: `.tar.gz` and `.zip`.
- `SHA256SUMS` for release artifacts.

The Docker package is published separately and manually through the Makefile.

## Container Release

For `v0.1.0`, users can pull:

```bash
docker pull ghcr.io/buildyourownstuff/veloxdb:0.1.0
docker pull ghcr.io/buildyourownstuff/veloxdb:0.1
```

Publish that package when you are ready:

```bash
make package-release PACKAGE_TAG=0.1.0 PACKAGE_REF=v0.1.0
```

Update `latest` only when you explicitly want the release to become the default package:

```bash
make package-release PACKAGE_TAG=0.1.0 PACKAGE_REF=v0.1.0 PUBLISH_LATEST=true
```

SHELL := /usr/bin/env bash

GITHUB_REPO ?= buildyourownstuff/VeloxDB
REMOTE ?= origin
PROJECT_VERSION := $(shell awk '/^project/ { for (i = 1; i <= NF; ++i) if ($$i == "VERSION") { print $$(i + 1); exit } }' CMakeLists.txt)
PACKAGE_TAG ?= $(PROJECT_VERSION)
PACKAGE_REF ?= v$(PACKAGE_TAG)
PUBLISH_LATEST ?= false
PLATFORMS ?= linux/amd64,linux/arm64

.PHONY: help package-release package-latest github-release release check-package-ref

help:
	@echo "VeloxDB release helpers"
	@echo
	@echo "  make github-release VERSION=0.1.0"
	@echo "  make package-release PACKAGE_TAG=0.1.0"
	@echo "  make package-release PACKAGE_TAG=0.1.0 PACKAGE_REF=v0.1.0 PUBLISH_LATEST=true"
	@echo "  make package-release PACKAGE_TAG=0.1.0 PACKAGE_REF=main"
	@echo "  make package-latest"
	@echo "  make release VERSION=0.1.0 PUBLISH_LATEST=true"

check-package-ref:
	@test -n "$(PACKAGE_TAG)" || (echo "PACKAGE_TAG is required" >&2; exit 1)
	@test -n "$(PACKAGE_REF)" || (echo "PACKAGE_REF is required" >&2; exit 1)
	@if git ls-remote --exit-code --heads "$(REMOTE)" "$(PACKAGE_REF)" >/dev/null 2>&1 || \
	    git ls-remote --exit-code --tags "$(REMOTE)" "$(PACKAGE_REF)" >/dev/null 2>&1; then \
		:; \
	else \
		echo "Remote ref '$(PACKAGE_REF)' does not exist on $(REMOTE)." >&2; \
		echo "Create the stable release tag first:" >&2; \
		echo "  make github-release VERSION=$(PACKAGE_TAG)" >&2; \
		echo "Or publish this package from main explicitly:" >&2; \
		echo "  make package-release PACKAGE_TAG=$(PACKAGE_TAG) PACKAGE_REF=main" >&2; \
		exit 1; \
	fi

package-release: check-package-ref
	gh workflow run docker-publish.yml \
		--repo "$(GITHUB_REPO)" \
		--ref main \
		-f image_tag="$(PACKAGE_TAG)" \
		-f ref="$(PACKAGE_REF)" \
		-f publish_latest="$(PUBLISH_LATEST)" \
		-f platforms="$(PLATFORMS)"
	@echo "Package workflow requested for ghcr.io/buildyourownstuff/veloxdb:$(PACKAGE_TAG)"
	@echo "Track it with: gh run list --repo $(GITHUB_REPO) --workflow docker-publish.yml --limit 1"

package-latest:
	$(MAKE) package-release PACKAGE_TAG=latest PACKAGE_REF=main PUBLISH_LATEST=false

github-release:
	@test -n "$(VERSION)" || (echo "VERSION is required, for example VERSION=0.1.0" >&2; exit 1)
	@test "$(VERSION)" = "$(PROJECT_VERSION)" || (echo "VERSION=$(VERSION) does not match CMake project version $(PROJECT_VERSION)" >&2; exit 1)
	@if git ls-remote --exit-code --tags "$(REMOTE)" "v$(VERSION)" >/dev/null 2>&1; then \
		echo "Tag v$(VERSION) already exists on $(REMOTE)." >&2; \
		exit 1; \
	fi
	git tag -a "v$(VERSION)" -m "VeloxDB v$(VERSION)"
	git push origin "v$(VERSION)"

release:
	@test -n "$(VERSION)" || (echo "VERSION is required, for example VERSION=0.1.0" >&2; exit 1)
	$(MAKE) github-release VERSION="$(VERSION)"
	$(MAKE) package-release PACKAGE_TAG="$(VERSION)" PACKAGE_REF="v$(VERSION)" PUBLISH_LATEST="$(PUBLISH_LATEST)"

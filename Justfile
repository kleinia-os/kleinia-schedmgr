# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 The Kleinia Authors
#
# Unified build interface: just build / test / package.
# All build artifacts land in ./build-out/ (gitignored).

default: build

# Package version (tag-driven; short sha until the first tag exists)
version := `git describe --tags --always 2>/dev/null || echo 0.1.0`

# Build the daemon and CLI into ./build-out/
build:
	make -C daemon all
	mkdir -p build-out
	cp daemon/schedmgrd daemon/schedmgr build-out/

# Run g_test unit tests and the sim-backend e2e test
# (tests/e2e-sim.sh prints SKIPPED and exits 0 when no private bus is available)
test: build
	make -C daemon check
	tests/e2e-sim.sh

# Package the daemon + GNOME app + shell extension into an RPM.
# packaging/install-destdir.sh stages the full tree under ./build-out/stage/
# (fresh each run) which dist/nfpm.yaml packs; schemas and .mo are regenerated
# up front so stale local artifacts never leak into the package
# (version is rendered from git into a transient root-level config)
package: build
	rm -rf build-out/stage
	glib-compile-schemas extension/schedmgr@kleinia.io/schemas
	cd app && python3 locale/gen-translations.py
	DESTDIR="$PWD/build-out/stage" PREFIX=/usr SYSCONFDIR=/etc ./packaging/install-destdir.sh
	sed "s/{{"{{PKG_VERSION}}"}}/{{version}}/" dist/nfpm.yaml > .nfpm.rendered.yaml
	nfpm pkg -f .nfpm.rendered.yaml -p rpm -t build-out/
	rm -f .nfpm.rendered.yaml

# Build the OCI image (embeds the RPM via dist/Containerfile) into the local store
image:
	podman build -f dist/Containerfile -t localhost/kleinia/kleinia-schedmgr:{{version}} .

# Build the RPM-carrier OCI image (FROM scratch carrying /rpms/*.rpm) consumed
# by the kleinia assembly repo — same rpm-in-oci contract as kleinia-installer
artifact:
	podman build -f dist/Containerfile --target artifacts -t localhost/kleinia/kleinia-schedmgr:rpm .

# Publish the artifact image ($KLN_REGISTRY contract, default ghcr.io/<owner>)
push:
	bash scripts/push.sh

clean:
	make -C daemon clean
	rm -rf build-out

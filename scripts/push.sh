#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 The Kleinia Authors
# Publish the RPM-carrier artifact image. $KLN_REGISTRY contract (see the
# kleinia assembly repo): default pushes to ghcr.io/<owner> with the
# workflow token — a fork publishes to its own namespace; a repo variable
# points this at the internal registry. Tags: <version> + floating main.
# Without credentials (and outside ghcr/GITHUB_TOKEN) the push is skipped,
# not failed.
set -euo pipefail
REG="${KLN_REGISTRY:-ghcr.io/${GITHUB_REPOSITORY_OWNER:-kleinia-os}}"
IMG="kleinia-schedmgr"
VER="$(git describe --tags --always 2>/dev/null || echo 0.1.0)"

opts=()
case "${REG%%/*}" in ghcr.io) ;; *) opts=(--tls-verify=false) ;; esac

if ! podman image exists localhost/kleinia/kleinia-schedmgr:rpm; then
  echo "FATAL: artifact image not built (run: just artifact)" >&2
  exit 2
fi

user="${KLN_REGISTRY_USER:-}"; pass="${KLN_REGISTRY_PASSWORD:-}"
if [ -z "$user" ] || [ -z "$pass" ]; then
  if [ "${REG%%/*}" = ghcr.io ] && [ -n "${GITHUB_TOKEN:-}" ]; then
    user="${GITHUB_REPOSITORY_OWNER:-kleinia-os}"; pass="$GITHUB_TOKEN"
  else
    echo "push skipped (no registry credentials and no GITHUB_TOKEN)"
    exit 0
  fi
fi

echo "$pass" | podman login "${opts[@]}" "$REG" -u "$user" --password-stdin >/dev/null
podman tag localhost/kleinia/kleinia-schedmgr:rpm "$REG/$IMG:$VER" "$REG/$IMG:main"
podman push "${opts[@]}" "$REG/$IMG:$VER"
podman push "${opts[@]}" "$REG/$IMG:main"
echo "pushed: $REG/$IMG:$VER , $REG/$IMG:main"

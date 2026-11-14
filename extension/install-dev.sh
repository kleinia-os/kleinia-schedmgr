#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 The Kleinia Authors
# install-dev.sh — 编译 schemas 并把扩展拷贝到用户扩展目录（开发安装）
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="$HERE/schedmgr@kleinia.io"
DEST="$HOME/.local/share/gnome-shell/extensions/schedmgr@kleinia.io"

command -v glib-compile-schemas >/dev/null || {
    echo "glib-compile-schemas not found (install libglib2.0-bin)" >&2
    exit 1
}

glib-compile-schemas "$SRC/schemas"

mkdir -p "$DEST"
rm -rf "$DEST"/{*.js,metadata.json,schemas,icons}
cp -a "$SRC"/. "$DEST"/

echo "schedmgr@kleinia.io installed to $DEST"
echo "enable with:  gnome-extensions enable schedmgr@kleinia.io"

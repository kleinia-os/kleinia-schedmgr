#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 The Kleinia Authors
# Stage-install Kleinia Sched Manager into DESTDIR.
#   DESTDIR=/tmp/stage PREFIX=/usr SYSCONFDIR=/etc ./install-destdir.sh
# Missing optional components (app / extension) produce warnings, not errors;
# a missing daemon build is fatal unless SKIP_DAEMON=1.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
DESTDIR="${DESTDIR:-}"
PREFIX="${PREFIX:-/usr}"
SYSCONFDIR="${SYSCONFDIR:-/etc}"
SYSTEMD_UNIT_DIR="${SYSTEMD_UNIT_DIR:-/usr/lib/systemd/system}"

dst() { # dst <relative-path>
    local d="$DESTDIR/$1"
    mkdir -p "$d" && printf '%s\n' "$d"
}

die() { echo "install-destdir: ERROR: $*" >&2; exit 1; }
note() { echo "install-destdir:    $*" >&2; }

[ -n "$DESTDIR" ] || die "DESTDIR is empty; refusing to touch the live system (refusing empty DESTDIR is intentional)."

# --- 1. daemon (schedmgrd + schedmgr) --------------------------------------
if [ "${SKIP_DAEMON:-0}" != "1" ]; then
    if [ ! -x "$ROOT/daemon/schedmgrd" ] || [ ! -x "$ROOT/daemon/schedmgr" ]; then
        make -C "$ROOT/daemon" all || die "daemon build failed"
    fi
    install -m 0755 "$ROOT/daemon/schedmgrd"    "$(dst "$PREFIX/bin")/schedmgrd"
    install -m 0755 "$ROOT/daemon/schedmgr" "$(dst "$PREFIX/bin")/schedmgr"
else
    note "SKIP_DAEMON=1 — skipping daemon"
fi

# --- 2. DBus / systemd / polkit / config ----------------------------------
install -m 0644 "$HERE/io.kleinia.SchedMgr.conf" \
        "$(dst "$PREFIX/share/dbus-1/system.d")/io.kleinia.SchedMgr.conf"
install -m 0644 "$HERE/io.kleinia.SchedMgr.service" \
        "$(dst "$PREFIX/share/dbus-1/system-services")/io.kleinia.SchedMgr.service"
install -m 0644 "$ROOT/dist/systemd/kleinia-schedmgrd.service" \
        "$(dst "$SYSTEMD_UNIT_DIR")/kleinia-schedmgrd.service"
install -m 0644 "$HERE/io.kleinia.schedmgr.policy" \
        "$(dst "$PREFIX/share/polkit-1/actions")/io.kleinia.schedmgr.policy"

conf_dir="$(dst "$SYSCONFDIR/kleinia")"
if [ -e "$conf_dir/schedmgr.conf" ]; then
    note "$SYSCONFDIR/kleinia/schedmgr.conf exists in DESTDIR, keeping existing"
else
    install -m 0644 "$HERE/schedmgr.conf" "$conf_dir/schedmgr.conf"
fi
install -d "$(dst "/var/lib/kleinia/schedmgr")"

# --- 3. GNOME Shell extension ---------------------------------------------
ext_src="$ROOT/extension/schedmgr@kleinia.io"
if [ -d "$ext_src" ]; then
    # make sure schemas are compiled before staging
    if [ -d "$ext_src/schemas" ] && [ ! -f "$ext_src/schemas/gschemas.compiled" ]; then
        glib-compile-schemas "$ext_src/schemas" \
            || note "glib-compile-schemas failed; staging sources as-is"
    fi
    ext_dst="$(dst "$PREFIX/share/gnome-shell/extensions")/schedmgr@kleinia.io"
    mkdir -p "$ext_dst"
    cp -a "$ext_src/." "$ext_dst/"
    find "$ext_dst" -name '*.js' -exec chmod 0644 {} +
else
    note "extension/ missing — staged without GNOME Shell extension"
fi

# --- 4. SchedMgr app (Python/PyGObject) --------------------------------------
app_src="$ROOT/app"
if [ -d "$app_src" ]; then
    app_dst="$(dst "$PREFIX/share/kleinia-schedmgr")"
    mkdir -p "$app_dst"
    for f in main.py window.py schedmgr-client.py; do
        [ -f "$app_src/$f" ] && install -m 0644 "$app_src/$f" "$app_dst/$f"
    done
    # tolerate extra ui modules the app may add; mock daemon and the UI test
    # harness stay dev-only tools
    for f in "$app_src"/*.py; do
        b="$(basename "$f")"
        [ "$b" = "mock-schedmgrd.py" ] && continue
        [ "$b" = "uitest.py" ] && continue
        [ -f "$app_dst/$b" ] || install -m 0644 "$f" "$app_dst/$b"
    done
    if [ -f "$app_src/kleinia-schedmgr" ]; then
        install -m 0755 "$app_src/kleinia-schedmgr" "$(dst "$PREFIX/bin")/kleinia-schedmgr"
    else
        # generate a small launcher if the app repo did not ship one
        cat > "$(dst "$PREFIX/bin")/kleinia-schedmgr" <<'EOF'
#!/bin/sh
exec python3 /usr/share/kleinia-schedmgr/main.py "$@"
EOF
        chmod 0755 "$DESTDIR/$PREFIX/bin/kleinia-schedmgr"
    fi
    # desktop entry (prefer final file, else build from .in)
    apps_dst="$(dst "$PREFIX/share/applications")"
    if [ -f "$app_src/io.kleinia.SchedMgr.desktop" ]; then
        install -m 0644 "$app_src/io.kleinia.SchedMgr.desktop" "$apps_dst/io.kleinia.SchedMgr.desktop"
    elif [ -f "$app_src/io.kleinia.SchedMgr.desktop.in" ]; then
        sed -e "s|@prefix@|$PREFIX|g" "$app_src/io.kleinia.SchedMgr.desktop.in" \
            > "$apps_dst/io.kleinia.SchedMgr.desktop"
        chmod 0644 "$apps_dst/io.kleinia.SchedMgr.desktop"
    else
        note "app has no .desktop(.in) — desktop entry skipped"
    fi
    # icons (symbolic svg, original artwork)
    if [ -f "$app_src/io.kleinia.SchedMgr-symbolic.svg" ]; then
        install -m 0644 "$app_src/io.kleinia.SchedMgr-symbolic.svg" \
            "$(dst "$PREFIX/share/icons/hicolor/scalable/apps")/io.kleinia.SchedMgr-symbolic.svg"
    fi
    if [ -f "$app_src/icons/io.kleinia.SchedMgr.svg" ]; then
        install -m 0644 "$app_src/icons/io.kleinia.SchedMgr.svg" \
            "$(dst "$PREFIX/share/icons/hicolor/scalable/apps")/io.kleinia.SchedMgr.svg"
    fi
    # translations (.mo are build artifacts, not tracked; generate when absent)
    if [ -d "$app_src/locale" ]; then
        if ! ls "$app_src"/locale/*/LC_MESSAGES/*.mo >/dev/null 2>&1; then
            note "no compiled .mo found — running app/locale/gen-translations.py"
            (cd "$app_src" && python3 locale/gen-translations.py) \
                || note "translation generation incomplete; staging what exists"
        fi
        for mo in "$app_src"/locale/*/LC_MESSAGES/*.mo; do
            [ -f "$mo" ] || continue
            lang="$(basename "$(dirname "$(dirname "$mo")")")"
            dom="$(basename "$mo" .mo)"
            install -m 0644 "$mo" "$(dst "$PREFIX/share/locale/$lang/LC_MESSAGES")/$dom.mo"
        done
    fi
else
    note "app/ missing — staged without SchedMgr GUI app"
fi


echo "install-destdir: staged under $DESTDIR (prefix $PREFIX)"

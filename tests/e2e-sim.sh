#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 The Kleinia Authors
#
# e2e-sim.sh — end-to-end test of schedmgrd/schedmgr against the sim
# backend.
#
# The test never touches the system bus, the real /sys or any real
# process: the daemon always runs with --backend=sim.  A private bus is
# preferred:
#   1. dbus-run-session            (fully private session bus)
#   2. dbus-daemon + minimal conf  (private session bus, manual)
#   3. python3 Gio.TestDBus        (private session bus via GLib)
#   4. the user's session bus      (only if one is already running)
#   5. direct --bus-address socket (no bus at all; last resort)
# If even (5) is impossible the script prints SKIPPED and exits 0.
#
# Override the automatic choice with PM_E2E_FORCE_MODE=<mode>.
#
# Usage: tests/e2e-sim.sh

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SELF="$SCRIPT_DIR/$(basename "$0")"
DAEMON_DIR="$(cd "$SCRIPT_DIR/../daemon" && pwd)"
SCHEDMGRD="$DAEMON_DIR/schedmgrd"
SCHEDMGRCTL="$DAEMON_DIR/schedmgr"

TMPDIR_E2E="$(mktemp -d /tmp/schedmgr-e2e-XXXXXX)"
MON_LOG="$TMPDIR_E2E/monitor.log"
OUT_LOG="$TMPDIR_E2E/out.txt"
DAEMON_LOG="$TMPDIR_E2E/schedmgrd.log"
DAEMON_PID=""
BUS_PID=""
SOCK=""
PASS=0
FAIL=0

cleanup()
{
    [ -n "$DAEMON_PID" ] && kill "$DAEMON_PID" 2>/dev/null
    [ -n "$BUS_PID" ] && kill "$BUS_PID" 2>/dev/null
    [ -n "$SOCK" ] && rm -f "$SOCK"
    rm -rf "$TMPDIR_E2E"
}
trap cleanup EXIT

ok()   { echo "ok:   $*"; PASS=$((PASS + 1)); }
fail() { echo "FAIL: $*"; FAIL=$((FAIL + 1)); }

# check <desc> <cmd...>            — command must succeed
check()
{
    local desc="$1"; shift
    if "$@" >"$OUT_LOG" 2>&1; then
        ok "$desc"
    else
        fail "$desc: $(head -n 2 "$OUT_LOG" | tr '\n' ' ')"
    fi
}

# check_grep <desc> <pattern> <cmd...> — command must succeed and output
# must contain the pattern
check_grep()
{
    local desc="$1" pat="$2"; shift 2
    if "$@" >"$OUT_LOG" 2>&1 && grep -q -- "$pat" "$OUT_LOG"; then
        ok "$desc"
    else
        fail "$desc: $(head -n 3 "$OUT_LOG" | tr '\n' ' ')"
    fi
}

# check_fail <desc> <err-pattern> <cmd...> — command must fail with the
# given error text on stderr
check_fail()
{
    local desc="$1" pat="$2"; shift 2
    if "$@" >"$OUT_LOG" 2>&1; then
        fail "$desc: unexpectedly succeeded"
    elif ! grep -q -- "$pat" "$OUT_LOG"; then
        fail "$desc: wrong error: $(head -n 2 "$OUT_LOG" | tr '\n' ' ')"
    else
        ok "$desc"
    fi
}

# ---------------- the actual test suite ----------------

start_daemon()
{
    local conf=""
    [ -f "$TMPDIR_E2E/schedmgr.conf" ] && conf="--config=$TMPDIR_E2E/schedmgr.conf"
    # shellcheck disable=SC2086
    "$SCHEDMGRD" $PM_E2E_DAEMON_ARGS --backend=sim $conf \
        --state-dir="$TMPDIR_E2E/state" >"$DAEMON_LOG" 2>&1 &
    DAEMON_PID=$!
}

stop_daemon()
{
    if [ -n "$DAEMON_PID" ]; then
        kill "$DAEMON_PID" 2>/dev/null
        wait "$DAEMON_PID" 2>/dev/null
        DAEMON_PID=""
    fi
}

wait_for_daemon()
{
    local i
    for i in $(seq 1 60); do
        if "${CTL[@]}" status >/dev/null 2>&1; then
            return 0
        fi
        if ! kill -0 "$DAEMON_PID" 2>/dev/null; then
            return 1
        fi
        sleep 0.1
    done
    return 1
}

run_tests()
{
    # config exercising the device-default machinery and the v2 keys
    cat >"$TMPDIR_E2E/schedmgr.conf" <<'EOF'
[Cpu]
DefaultStrategy=none

[Io.DeviceDefaults]
sda=bfq

[Io]
IoxDirs=/usr/bin:/usr/local/bin

[Cache]
CaxDirs=/usr/bin
WatchDir=/var/tmp
Cgroup=/sys/fs/cgroup/cache_ext_schedmgr
EOF

    start_daemon
    if ! wait_for_daemon; then
        fail "schedmgrd started and is reachable"
        echo "---- daemon log ----"
        cat "$DAEMON_LOG"
        return 1
    fi
    ok "schedmgrd started and is reachable"

    # --- 1. status ------------------------------------------------
    check_grep "status: sim backend"            "Backend:.*sim"                  "${CTL[@]}" status
    check_grep "status: kernel release"         "Kernel:.*[0-9]"                 "${CTL[@]}" status
    check_grep "status: CPU description"        "CPU 调度器"                     "${CTL[@]}" status
    check_grep "status: cache description"      "页缓存策略"                     "${CTL[@]}" status
    check_grep "status: cpu initially none"     "Current:.*(none)"               "${CTL[@]}" status
    check_grep "status: 3 devices"              "Block devices (3)"              "${CTL[@]}" status
    check_grep "status: sda present"            "sda .*kyber .*SATA SSD 512G"    "${CTL[@]}" status

    # --- 2. strategy lists (v2: EEVDF, cax_*, iox_* sets) ---------
    check_grep "cpu list: scx_agent"              "scx_agent"                        "${CTL[@]}" cpu list
    check_grep "cpu list: none is EEVDF"        "EEVDF (kernel default scheduling class)" "${CTL[@]}" cpu list
    check_grep "cache list: cax_mru"            "cax_mru"                        "${CTL[@]}" cache list
    check_grep "io list: global iox_fifo"       "iox_fifo"                       "${CTL[@]}" io list
    check_grep "io policy list: iox_wedf"       "iox_wedf"                       "${CTL[@]}" io policy list
    check_grep "io list sda: native kyber"      "kyber"                          "${CTL[@]}" io list sda
    check_grep "io list sda: iox marked global" "global eBPF policy"            "${CTL[@]}" io list sda
    check_grep "io devices: nvme0n1"            "nvme0n1 .*kyber .*NVMe SSD 1T"  "${CTL[@]}" io devices
    check_grep "io devices: 3 rows"             "Block devices (3)"              "${CTL[@]}" io devices

    # --- 3. cpu apply/stop/restart cycle --------------------------
    check       "cpu apply scx_agent"                                               "${CTL[@]}" cpu apply scx_agent
    check_grep "status: current scx_agent"         "Current:.*scx_agent"              "${CTL[@]}" status
    check       "cpu apply with params"                                           "${CTL[@]}" cpu apply scx_simple --slice-ms 20
    check_grep "status: params recorded"         "scx_simple (params: --slice-ms 20)" "${CTL[@]}" status
    check       "cpu restart"                                                     "${CTL[@]}" cpu restart
    check_grep "status: restart kept strategy"   "Current:.*scx_simple"           "${CTL[@]}" status
    check       "cpu stop"                                                        "${CTL[@]}" cpu stop
    check_grep "status: cpu stopped"             "Current:.*(none)"               "${CTL[@]}" status

    # --- 4. cache full chain (apply / params / stop / restart) ----
    check       "cache apply cax_mru"                                              "${CTL[@]}" cache apply cax_mru
    check_grep "status: cache cax_mru"           "Current:       cax_mru"         "${CTL[@]}" status
    check       "cache apply cax_lhd with -w override"                             "${CTL[@]}" cache apply cax_lhd -w /tmp/schedmgr-watch
    check_grep "status: cache params recorded"   "cax_lhd (params: -w /tmp/schedmgr-watch)" "${CTL[@]}" status
    check       "cache stop"                                                      "${CTL[@]}" cache stop
    check_grep "status: cache stopped"           "Current:       (none)"          "${CTL[@]}" status
    check       "cache restart (last strategy)"                                   "${CTL[@]}" cache restart
    check_grep "status: cache restarted cax_lhd" "Current:       cax_lhd"         "${CTL[@]}" status

    # --- 5. global iox policy: apply / switch / stop --------------
    check       "io policy apply iox_fifo"                                          "${CTL[@]}" io policy apply iox_fifo
    check_grep "status: global io iox_fifo"      "Current:       iox_fifo"        "${CTL[@]}" status
    check       "io policy apply iox_wedf (switch)"                                "${CTL[@]}" io policy apply iox_wedf
    check_grep "status: global io iox_wedf"      "Current:       iox_wedf"        "${CTL[@]}" status
    check       "io policy stop"                                                   "${CTL[@]}" io policy stop
    if "${CTL[@]}" status >"$OUT_LOG" 2>&1 && \
       grep -A6 "Domain: Io" "$OUT_LOG" | grep -q -- "Current:       (none)"; then
        ok "status: global io stopped"
    else
        fail "status: global io stopped: $(grep -A6 'Domain: Io' "$OUT_LOG" | tr '\n' ' ')"
    fi

    # --- 6. device apply(iox): two devices, single-select linkage -
    check       "io apply sda iox_fifo"                                            "${CTL[@]}" io apply sda iox_fifo
    check_grep "io devices: sda effective iox_fifo" "sda .*iox_fifo"              "${CTL[@]}" io devices
    check       "io apply nvme0n1 iox_wedf"                                        "${CTL[@]}" io apply nvme0n1 iox_wedf
    check_grep "io devices: nvme0n1 iox_wedf"    "nvme0n1 .*iox_wedf"             "${CTL[@]}" io devices
    check_grep "io devices: sda follows global"  "sda .*iox_wedf"                 "${CTL[@]}" io devices
    check       "global io policy stop"                                             "${CTL[@]}" io stop
    check_grep "io devices: sda falls back to scx_io" "sda .*scx_io"              "${CTL[@]}" io devices

    # --- 7. signals via schedmgr monitor -------------------------
    ("${CTL[@]}" monitor 6 >"$MON_LOG" 2>&1) &
    local MON_PID=$!
    sleep 0.5
    "${CTL[@]}" cpu apply scx_central >/dev/null 2>&1
    sleep 0.3
    "${CTL[@]}" cache apply cax_mru >/dev/null 2>&1
    sleep 0.3
    "${CTL[@]}" sim-hotplug add sdb >/dev/null 2>&1
    wait "$MON_PID" 2>/dev/null
    grep -q "StrategyChanged strategy='scx_central'" "$MON_LOG" \
        && ok "signal: cpu StrategyChanged"   || fail "signal: cpu StrategyChanged"
    grep -q "StrategyChanged strategy='cax_mru'" "$MON_LOG" \
        && ok "signal: cache StrategyChanged" || fail "signal: cache StrategyChanged"
    grep -q "PropertiesChanged iface=io.kleinia.Sched1 CurrentStrategy" "$MON_LOG" \
        && ok "signal: PropertiesChanged"     || fail "signal: PropertiesChanged"
    grep -q "DevicesChanged" "$MON_LOG" \
        && ok "signal: DevicesChanged"        || fail "signal: DevicesChanged"
    "${CTL[@]}" sim-hotplug remove sdb >/dev/null 2>&1

    # raw gdbus monitor check (only possible on a real bus)
    if [ -z "${PM_E2E_DIRECT:-}" ] && command -v gdbus >/dev/null 2>&1; then
        local GLOG="$TMPDIR_E2E/gdbus-monitor.log"
        timeout 4 gdbus monitor $PM_E2E_CTL_ARGS \
            --dest io.kleinia.SchedMgr >"$GLOG" 2>&1 &
        local GPID=$!
        sleep 0.5
        "${CTL[@]}" cpu apply scx_agent >/dev/null 2>&1
        wait "$GPID" 2>/dev/null
        grep -q "StrategyChanged" "$GLOG" \
            && ok "gdbus monitor sees StrategyChanged" \
            || fail "gdbus monitor sees StrategyChanged"
    fi

    # --- 8. error paths (NoSuchStrategy / NotAvailable / ...) -----
    check_fail "bogus cache strategy"  "NoSuchStrategy"                           "${CTL[@]}" cache apply bogus
    check_fail "bogus iox strategy"    "NoSuchStrategy"                           "${CTL[@]}" io policy apply iox_bogus
    check_fail "bogus io scheduler"    "NoSuchStrategy"                          "${CTL[@]}" io apply sda bogus
    check_fail "bogus device iox"      "NoSuchStrategy"                          "${CTL[@]}" io apply sda iox_bogus
    check_fail "cache has no default"  "NoDefault"                               "${CTL[@]}" cache default
    check_fail "io policy has no default" "NoDefault"                            "${CTL[@]}" io default
    check_fail "hotplug duplicate dev" "already exists"                          "${CTL[@]}" sim-hotplug add sda
    check_fail "hotplug remove missing" "no such device"                         "${CTL[@]}" sim-hotplug remove nosuchdev
    # NotAvailable via the sim SetAvailable knob
    check       "sim-available io off"                                              "${CTL[@]}" sim-available io off
    check_fail "io policy stop while unavailable" "NotAvailable"                  "${CTL[@]}" io policy stop
    check_fail "io device iox while global off" "NotAvailable"                    "${CTL[@]}" io apply nvme0n1 iox_merge
    check       "sim-available io on"                                               "${CTL[@]}" sim-available io on
    check       "sim-available cache off"                                           "${CTL[@]}" sim-available cache off
    check_fail "cache apply while unavailable" "NotAvailable"                      "${CTL[@]}" cache apply cax_mru
    check       "sim-available cache on"                                            "${CTL[@]}" sim-available cache on
    check       "cache apply works again"                                           "${CTL[@]}" cache apply cax_mru

    # --- 9. per-device native io apply + config default -----------
    check       "io apply sda none"                                              "${CTL[@]}" io apply sda none
    check_grep "io devices: sda none"           "sda .*none"                     "${CTL[@]}" io devices
    check       "io stop sda restores default"                                   "${CTL[@]}" io stop sda
    check_grep "io devices: sda back to bfq"    "sda .*bfq"                      "${CTL[@]}" io devices

    # --- 10. hotplug lifecycle -------------------------------------
    check       "hotplug add sdb"                                                "${CTL[@]}" sim-hotplug add sdb
    check_grep "io devices: now 4"              "Block devices (4)"              "${CTL[@]}" io devices
    check_grep "io devices: sdb listed"         "sdb .*none .*simulated block device" "${CTL[@]}" io devices
    check       "io apply on hotplugged device"                                  "${CTL[@]}" io apply sdb kyber
    check_grep "io devices: sdb kyber"          "sdb .*kyber"                    "${CTL[@]}" io devices
    check       "hotplug remove sdb"                                             "${CTL[@]}" sim-hotplug remove sdb
    check_grep "io devices: back to 3"          "Block devices (3)"              "${CTL[@]}" io devices
    check       "hotplug re-add works"                                           "${CTL[@]}" sim-hotplug add sdb
    check       "hotplug remove again"                                           "${CTL[@]}" sim-hotplug remove sdb

    # --- 11. cpu default -------------------------------------------
    check       "cpu default applies configured default"                          "${CTL[@]}" cpu default
    check_grep "status: cpu none after default" "Current:.*(none)"               "${CTL[@]}" status

    # --- 12. daemon restart: fresh state --------------------------
    stop_daemon
    start_daemon
    if ! wait_for_daemon; then
        fail "schedmgrd restarted"
        return 1
    fi
    ok "schedmgrd restarted"
    check_grep "restart: fresh sim state"       "Current:       (none)"          "${CTL[@]}" status
    check_grep "restart: devices back"          "Block devices (3)"              "${CTL[@]}" io devices
    check_fail "restart: cpu restart has nothing" "NotRunning"                  "${CTL[@]}" cpu restart

    stop_daemon
    return 0
}

# ---------------- bus / transport selection ----------------

run_inner()
{
    CTL=("$SCHEDMGRCTL" $PM_E2E_CTL_ARGS)
    run_tests
    echo
    echo "e2e-sim: $PASS passed, $FAIL failed"
    [ "$FAIL" -eq 0 ]
}

run_with_session_bus()
{
    # private or user session bus: daemon + ctl use --session
    PM_E2E_DAEMON_ARGS="--session"
    PM_E2E_CTL_ARGS="--session"
    export PM_E2E_DAEMON_ARGS PM_E2E_CTL_ARGS
    # clear stale daemons from aborted earlier runs (ours only)
    pkill -f "^$SCHEDMGRD --session --backend=sim" 2>/dev/null
    sleep 0.2
    "$SELF" --inner
}

run_direct()
{
    SOCK="$TMPDIR_E2E/schedmgrd.sock"
    PM_E2E_DAEMON_ARGS="--bus-address=unix:path=$SOCK"
    PM_E2E_CTL_ARGS="--bus-address=unix:path=$SOCK"
    export PM_E2E_DAEMON_ARGS PM_E2E_CTL_ARGS PM_E2E_DIRECT=1
    "$SELF" --inner
}

select_mode()
{
    if [ -n "${PM_E2E_FORCE_MODE:-}" ]; then
        echo "$PM_E2E_FORCE_MODE"
        return
    fi
    if command -v dbus-run-session >/dev/null 2>&1; then
        echo run-session
        return
    fi
    if command -v dbus-daemon >/dev/null 2>&1 ||
        [ -x /usr/libexec/dbus-daemon ]; then
        echo dbus-daemon
        return
    fi
    if python3 - >/dev/null 2>&1 <<'EOF'
import gi
gi.require_version('Gio', '2.0')
from gi.repository import Gio
db = Gio.TestDBus()
db.up()
db.down()
EOF
    then
        echo testdbus
        return
    fi
    case "${DBUS_SESSION_BUS_ADDRESS:-}" in
        unix:path=*)
            [ -S "${DBUS_SESSION_BUS_ADDRESS#unix:path=}" ] && echo user-session && return
            ;;
    esac
    echo direct
}

main()
{
    local mode

    # inner invocation?
    if [ "${1:-}" = "--inner" ]; then
        run_inner
        return
    fi

    if [ ! -x "$SCHEDMGRD" ] || [ ! -x "$SCHEDMGRCTL" ]; then
        echo "e2e-sim: building daemon first..."
        make -C "$DAEMON_DIR" || { echo "e2e-sim: build failed"; exit 1; }
    fi

    mode="$(select_mode)"
    echo "e2e-sim: transport = $mode"

    case "$mode" in
    run-session)
        # the inner run reads these from the environment; without --session
        # both daemon and ctl would fall back to the system bus (forbidden)
        PM_E2E_DAEMON_ARGS="--session"
        PM_E2E_CTL_ARGS="--session"
        export PM_E2E_DAEMON_ARGS PM_E2E_CTL_ARGS
        dbus-run-session -- "$SELF" --inner
        ;;
    dbus-daemon)
        {
            local DAEMON_BIN
            DAEMON_BIN="$(command -v dbus-daemon || echo /usr/libexec/dbus-daemon)"
            cat >"$TMPDIR_E2E/session.conf" <<EOF
<!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN"
 "http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd">
<busconfig>
  <type>session</type>
  <listen>unix:tmpdir=$TMPDIR_E2E</listen>
  <auth>EXTERNAL</auth>
  <policy context="default">
    <allow send_destination="*" eavesdrop="true"/>
    <allow eavesdrop="true"/>
    <allow own="*"/>
  </policy>
</busconfig>
EOF
            "$DAEMON_BIN" --config-file="$TMPDIR_E2E/session.conf" \
                --print-address >"$TMPDIR_E2E/bus-address" 2>"$TMPDIR_E2E/bus.err" &
            BUS_PID=$!
            for i in $(seq 1 50); do
                [ -s "$TMPDIR_E2E/bus-address" ] && break
                sleep 0.1
            done
            export DBUS_SESSION_BUS_ADDRESS="$(head -n 1 "$TMPDIR_E2E/bus-address")"
            if [ -z "$DBUS_SESSION_BUS_ADDRESS" ]; then
                echo "e2e-sim: dbus-daemon did not come up:"
                cat "$TMPDIR_E2E/bus.err" 2>/dev/null
                exit 1
            fi
            run_with_session_bus
        }
        ;;
    testdbus)
        python3 - "$SELF" <<'EOF'
import os, subprocess, sys
import gi
gi.require_version('Gio', '2.0')
from gi.repository import Gio
db = Gio.TestDBus()
db.up()
env = dict(os.environ)
env['DBUS_SESSION_BUS_ADDRESS'] = db.get_bus_address()
# same contract as run_with_session_bus: the inner script expects these
env['PM_E2E_DAEMON_ARGS'] = '--session'
env['PM_E2E_CTL_ARGS'] = '--session'
rc = subprocess.call([sys.argv[1], '--inner'], env=env)
try:
    db.down()
except Exception:
    pass
sys.exit(rc)
EOF
        ;;
    user-session)
        echo "e2e-sim: NOTE: no private bus tooling found; using the user's session bus"
        run_with_session_bus
        ;;
    direct)
        echo "e2e-sim: NOTE: no D-Bus available at all; using direct socket transport"
        run_direct
        ;;
    *)
        echo "e2e-sim: SKIPPED — no usable bus transport (this should not happen)"
        exit 0
        ;;
    esac
}

main "$@"

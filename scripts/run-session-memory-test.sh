#!/usr/bin/env bash
# Session memory budget test (R6, docs/25-performance.md §4).
#
# The compositor's performance meter reports the memory of every client it has
# (the Taskbar, the Desktop, the applets, apps) — proportional set size, which is
# the honest per-client number, because RSS counts a shared library's pages once
# per process (ten clients each "use" the whole Lagom stack).
#
# This runs the session's *real* startup list headless and asserts the budget:
# the client count, the session's PSS, each individual client's PSS, and that an
# idle session does not grow. It prints the per-client breakdown, so it doubles as
# the "who is using memory" report.
#
# Cleanup is scoped to this run: the compositor is its own process-group leader
# (setsid) and only that group is ever signalled, so a live session is never
# touched.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
SERENITY="${SERENITY:-$LIBGUI_WAYLAND_ROOT/serenity}"
BUILD="${BUILD:-$LIBGUI_WAYLAND_ROOT/build/lagom}"
COMP="$SDE_ROOT/build/WindowServer/WindowServer"

"$SDE_ROOT/scripts/build-window-server.sh" >/dev/null

# Budgets (measured on the development machine: see docs/25-performance.md §4).
SESSION_PSS_LIMIT_KB="${SDE_SESSION_PSS_LIMIT_KB:-98304}"   # 96 MB
COMPOSITOR_PSS_LIMIT_KB="${SDE_COMPOSITOR_PSS_LIMIT_KB:-32768}" # 32 MB
CLIENT_PSS_LIMIT_KB="${SDE_CLIENT_PSS_LIMIT_KB:-16384}"     # 16 MB per client
SESSION_GROWTH_KB="${SDE_SESSION_PSS_GROWTH_KB:-2048}"      # over an idle period
MIN_CLIENTS="${SDE_SESSION_MIN_CLIENTS:-8}"

# The session's real startup list (the shell, the desktop, the applets).
# shellcheck source=scripts/sde-applets.sh
. "$SDE_ROOT/scripts/sde-applets.sh"
APPLET_CMDS="$(sde_applet_startup_commands "$SDE_ROOT")"
STARTUP="$BUILD/bin/Taskbar & $BUILD/bin/FileManager -d"
[[ -n "$APPLET_CMDS" ]] && STARTUP="$STARTUP & $APPLET_CMDS"
[[ -x "$SDE_ROOT/build/BatteryApplet/BatteryApplet" ]] && STARTUP="$STARTUP & $SDE_ROOT/build/BatteryApplet/BatteryApplet"

# The desktop is FileManager -d, an *application*. A build without --with-apps leaves
# it out, and then the wait below just times out with "the desktop never came up" --
# which reads like a compositor fault rather than a missing binary. Say so instead.
if [[ ! -x "$BUILD/bin/FileManager" ]]; then
    echo "==> session memory test FAILED: $BUILD/bin/FileManager is missing (build the applications: scripts/build-full-sde.sh --with-apps)"
    exit 1
fi

RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/sde-sessmem.XXXXXX")"
chmod 700 "$RUNTIME"

# The runner's XDG_RUNTIME_DIR may not exist or may not be writable -- a container is
# not a login session -- and then the compositor cannot create its Wayland socket at
# all ("failed to create a Wayland socket"), so the desktop never comes up.
# run-perf-test.sh isolates itself for exactly this reason; the budget job runs both
# scripts directly rather than through the test driver, so this one needs it too.
export XDG_RUNTIME_DIR="$RUNTIME/run"
mkdir -p "$XDG_RUNTIME_DIR"
chmod 700 "$XDG_RUNTIME_DIR"

SOCK="sde-sessmem-$$"
PERF="$RUNTIME/perf"
COMP_PID=""
cleanup() {
    # Only this run's process group: the compositor leads it (setsid), and the
    # shell/applets it started share it.
    [[ -n "$COMP_PID" ]] && kill -TERM -- "-$COMP_PID" 2>/dev/null || true
    [[ -n "$COMP_PID" ]] && { sleep 0.3; kill -KILL -- "-$COMP_PID" 2>/dev/null || true; }
    for _ in $(seq 1 60); do kill -0 "${COMP_PID:-${CP:-}}" 2>/dev/null || break; sleep 0.05; done; rm -rf "$RUNTIME"
    rm -f "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/$SOCK"{,.lock}
}
trap cleanup EXIT

setsid env -u WAYLAND_DISPLAY -u DISPLAY \
    WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 SDE_OUTPUT_SCALE=1 \
    SDE_STATE_DIR="$RUNTIME" \
    SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}" \
    LD_LIBRARY_PATH="$BUILD/lib" \
    SDE_APP_BIN_DIR="$BUILD/bin" SDE_SESSION_BIN_DIR="$RUNTIME/sbin" \
    SDE_PERF_STATS="$PERF" \
    "$COMP" -n "$SOCK" -s "$STARTUP" > "$RUNTIME/comp.log" 2>&1 &
COMP_PID=$!

# Wait for the desktop (the slowest client) and then a complete report.
for _ in $(seq 1 600); do
    grep -q "layer surface namespace='serenity-desktop'" "$RUNTIME/comp.log" 2>/dev/null && break
    sleep 0.1
done
if ! grep -q "layer surface namespace='serenity-desktop'" "$RUNTIME/comp.log" 2>/dev/null; then
    echo "==> session memory test FAILED (the desktop never came up)"
    tail -20 "$RUNTIME/comp.log"
    exit 1
fi
sleep 4

stat_value() { sed -n "s/^$1=//p" "$PERF" | head -1; }

fail=0
check() {
    local description="$1" actual="$2" op="$3" expected="$4"
    if awk -v a="$actual" -v b="$expected" "BEGIN { exit !(a $op b) }"; then
        echo "  ok: $description ($actual $op $expected)"
    else
        echo "  MISSING: $description ($actual, expected $op $expected)"
        fail=1
    fi
}

echo "=== session memory (proportional set size) ==="
echo "  compositor: $(stat_value pss_kb) kB PSS"
echo "  clients:    $(stat_value client_pss_kb) kB PSS over $(stat_value clients) processes"
printf "    %-22s %8s %8s\n" "client" "PSS" "RSS"
for entry in $(echo "$(stat_value client_memory)" | tr ',' ' '); do
    printf "    %-22s %8s\n" "${entry%%:*}" "${entry##*:}"
done
echo "  session:    $(stat_value session_pss_kb) kB PSS ($(stat_value session_rss_kb) kB RSS — RSS counts shared libraries per process)"

CLIENTS="$(stat_value clients)"
SESSION_PSS="$(stat_value session_pss_kb)"
COMPOSITOR_PSS="$(stat_value pss_kb)"
check "the shell's clients are running" "$CLIENTS" ">=" "$MIN_CLIENTS"
check "the compositor is inside its budget" "$COMPOSITOR_PSS" "<" "$COMPOSITOR_PSS_LIMIT_KB"
check "the session is inside its budget" "$SESSION_PSS" "<" "$SESSION_PSS_LIMIT_KB"

# No single client may dominate: parse the breakdown the meter prints.
while read -r entry; do
    [[ -n "$entry" ]] || continue
    name="${entry%%:*}"
    kb="${entry##*:}"
    check "client '$name' is inside its budget" "$kb" "<" "$CLIENT_PSS_LIMIT_KB"
done <<<"$(echo "$(stat_value client_memory)" | tr ',' '\n')"

# An idle session must not grow: the shell polls its applets, so a leak here is
# exactly what this catches.
sleep 4
SESSION_PSS_AFTER="$(stat_value session_pss_kb)"
GROWTH=$((SESSION_PSS_AFTER - SESSION_PSS))
echo "  session PSS after 4 s idle: ${SESSION_PSS_AFTER} kB (change ${GROWTH} kB)"
check "the idle session does not grow" "$GROWTH" "<" "$SESSION_GROWTH_KB"

if [[ "$fail" -ne 0 ]]; then
    echo "--- perf stats ---"; cat "$PERF"
    echo "==> session memory test FAILED"
    exit 1
fi
echo "==> session memory test PASSED"

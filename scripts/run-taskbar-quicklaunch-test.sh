#!/usr/bin/env bash
# Taskbar quick-launch robustness test (patch 0009).
#
# The quick launch (the pinned buttons next to the system menu) resolves its
# entries from config, and a pinned entry can outlive the app it points at: the
# app was uninstalled, or the resource tree changed under it. An entry's name is
# its widget label and the config key it is saved under, and AppFile::name()
# VERIFYs on an empty one — so a pinned .af that is missing or invalid used to
# take the whole shell down at startup (the system menu path already filters on
# AppFile::is_valid(); the quick-launch path did not).
#
# This runs the Taskbar headless against a resource root whose apps/ directory is
# empty, so every pinned entry is missing, and asserts the shell still comes up.
# Runs entirely headless (no host compositor, no DRM), so it is CI-friendly.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
SERENITY="${SERENITY:-$LIBGUI_WAYLAND_ROOT/serenity}"
BUILD="${BUILD:-$LIBGUI_WAYLAND_ROOT/build/lagom}"
COMP="$SDE_ROOT/build/WindowServer/WindowServer"
XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
export XDG_RUNTIME_DIR

"$SDE_ROOT/scripts/build-window-server.sh" >/dev/null
"$SDE_ROOT/scripts/build-taskbar.sh" >/dev/null

RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/sde-quicklaunch.XXXXXX")"
chmod 700 "$RUNTIME"
SOCK="sde-quicklaunch-$$"
COMP_PID=""
cleanup() {
    # Only this run's process group: the compositor leads it (setsid), and the
    # shell it started shares it.
    [[ -n "$COMP_PID" ]] && kill -TERM -- "-$COMP_PID" 2>/dev/null || true
    [[ -n "$COMP_PID" ]] && { sleep 0.3; kill -KILL -- "-$COMP_PID" 2>/dev/null || true; }
    for _ in $(seq 1 60); do kill -0 "${COMP_PID:-${CP:-}}" 2>/dev/null || break; sleep 0.05; done; rm -rf "$RUNTIME"
    rm -f "$XDG_RUNTIME_DIR/$SOCK"{,.lock}
}
trap cleanup EXIT

# A resource root that is a faithful copy of the real one except that apps/ is
# empty: every pinned entry's .af is therefore missing.
RES="$RUNTIME/res"
mkdir -p "$RES/apps"
for entry in "${SERENITY_RES_ROOT:-$SERENITY/Base/res}"/*; do
    name="$(basename "$entry")"
    [[ "$name" == "apps" ]] && continue
    ln -s "$entry" "$RES/$name"
done

setsid env -u WAYLAND_DISPLAY -u DISPLAY \
    WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 SDE_OUTPUT_SCALE=1 \
    SDE_STATE_DIR="$RUNTIME" SERENITY_RES_ROOT="$RES" \
    LD_LIBRARY_PATH="$BUILD/lib" \
    SDE_APP_BIN_DIR="$BUILD/bin" SDE_SESSION_BIN_DIR="$RUNTIME/sbin" \
    "$COMP" -n "$SOCK" -s "$BUILD/bin/Taskbar" > "$RUNTIME/comp.log" 2>&1 &
COMP_PID=$!

for _ in $(seq 1 300); do
    grep -q "layer surface namespace='serenity-taskbar'" "$RUNTIME/comp.log" 2>/dev/null && break
    sleep 0.1
done
sleep 1

fail=0
if grep -q "VERIFICATION FAILED" "$RUNTIME/comp.log"; then
    echo "  MISSING: the Taskbar aborted on a missing pinned entry"
    grep -m1 -A4 "VERIFICATION FAILED" "$RUNTIME/comp.log" | sed 's/^/    /'
    fail=1
else
    echo "  ok: no verification failure"
fi
if grep -q "layer surface namespace='serenity-taskbar'" "$RUNTIME/comp.log"; then
    echo "  ok: the shell came up with every pinned app missing"
else
    echo "  MISSING: the Taskbar never mapped its layer surface"
    tail -15 "$RUNTIME/comp.log"
    fail=1
fi

if [[ "$fail" -eq 0 ]]; then
    echo "==> quick-launch test PASSED"
else
    echo "==> quick-launch test FAILED"
    exit 1
fi

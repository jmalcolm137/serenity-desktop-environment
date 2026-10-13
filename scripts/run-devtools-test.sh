#!/usr/bin/env bash
# Headless test for the SerenityOS developer tools (the Taskbar's Development
# menu): GMLPlayground, SQLStudio, HackStudio and Profiler, built by
# libgui-wayland (patches 0035/0040). Like the games test, it asserts each tool's .af declares
# Category=Development (which is what the Taskbar's generated menu lists, with
# /bin/X resolving to the binary next to the Taskbar), that the binary exists,
# and that the tool launches into the compositor without faulting.
#
# HackStudio links LibDebug/LibELF/LibCoredump/LibSymbolication, which are
# ported to native Linux (patch 0036) rather than a SerenityOS kernel-ABI shim.
#
# Runs entirely headless (no host compositor, no DRM), so it is CI-friendly.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
SERENITY="${SERENITY:-$LIBGUI_WAYLAND_ROOT/serenity}"
BUILD="${BUILD:-$LIBGUI_WAYLAND_ROOT/build/lagom}"
COMP="$SDE_ROOT/build/WindowServer/WindowServer"

TOOLS=(GMLPlayground SQLStudio HackStudio Profiler)

"$SDE_ROOT/scripts/build-window-server.sh" >/dev/null

RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/sde-devtools.XXXXXX")"
export SDE_STATE_DIR="$RUNTIME"
chmod 700 "$RUNTIME"
trap 'rm -rf "$RUNTIME"; rm -f "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"/sde-*-$$* "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"/sde-*-$$*.lock 2>/dev/null || true' EXIT

fail=0
echo "=== devtools are built and declared in the Development category ==="
for tool in "${TOOLS[@]}"; do
    if [[ -x "$BUILD/bin/$tool" ]]; then
        echo "  ok: binary $tool"
    else
        echo "  MISSING: $BUILD/bin/$tool (build it: ninja -C $BUILD ${TOOLS[*]})"
        fail=1
    fi
    af="$SERENITY/Base/res/apps/$tool.af"
    if [[ -f "$af" ]] && grep -qE '^Category=.*evelopment' "$af"; then
        echo "  ok: $tool.af Category=Development"
    else
        echo "  MISSING: $tool.af with Category=Development"
        fail=1
    fi
done

SOCK="sde-devtools-$$"
env -u WAYLAND_DISPLAY -u DISPLAY \
    WLR_BACKENDS=headless \
    WLR_HEADLESS_OUTPUTS=1 \
    SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}" \
    LD_LIBRARY_PATH="$BUILD/lib" \
    "$COMP" -n "$SOCK" > "$RUNTIME/comp.log" 2>&1 &
COMP_PID=$!

for _ in $(seq 1 50); do
    [[ -e "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/$SOCK" ]] && break
    sleep 0.1
done

echo "=== each devtool launches into the compositor ==="
for tool in "${TOOLS[@]}"; do
    LOG="$RUNTIME/$tool.log"
    before="$(grep -c "toplevel mapped" "$RUNTIME/comp.log" || true)"
    env WAYLAND_DISPLAY="$SOCK" \
        LD_LIBRARY_PATH="$BUILD/lib" \
        SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}" \
        timeout 6 "$BUILD/bin/$tool" > "$LOG" 2>&1 || true
    after="$(grep -c "toplevel mapped" "$RUNTIME/comp.log" || true)"
    if [[ "$after" -gt "$before" ]]; then
        echo "  ok: $tool mapped a window"
    else
        echo "  MISSING: $tool did not map a window"
        fail=1
    fi
    if grep -qiE "SIGSEGV|Segmentation fault|ASSERTION FAILED|VERIFICATION FAILED|Runtime error" "$LOG"; then
        echo "  FAIL: $tool reported a fault"
        fail=1
    fi
done

kill "$COMP_PID" 2>/dev/null || true
wait "$COMP_PID" 2>/dev/null || true

if [[ "$fail" -ne 0 ]]; then
    echo "--- compositor log ---"
    cat "$RUNTIME/comp.log"
    echo "==> devtools test FAILED"
    exit 1
fi
echo "==> devtools test PASSED"

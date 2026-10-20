#!/usr/bin/env bash
# ci-tier: full
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
. "$SDE_ROOT/scripts/lib/testlib.sh"
sde_test_init devtools
sde_test_export_roots

TOOLS=(GMLPlayground SQLStudio HackStudio Profiler)

"$SDE_ROOT/scripts/build-window-server.sh" >/dev/null

export SDE_STATE_DIR="$RUNTIME"
chmod 700 "$RUNTIME"

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

sde_test_compositor
# SQLStudio is the one DevTools app with a service dependency: it talks to the
# ported SQLServer. A session starts that service (sde-session.sh's
# SESSION_SERVICES), and LibWM also spawns it on demand — but spawning resolves the
# binary through SDE_SESSION_BIN_DIR, and this test gave the clients none, so the
# spawn failed and SQLStudio faulted on startup while the other three (which need
# nothing) mapped their windows. Give them a session app directory with it.
SESSION_DIR="$RUNTIME/session"
mkdir -p "$SESSION_DIR/portal"
APP_BIN="$RUNTIME/app-bin"
mkdir -p "$APP_BIN"
for app in "$BUILD/bin"/*; do
    [[ -e "$app" ]] || continue
    ln -sf "$app" "$APP_BIN/$(basename "$app")"
done
# The services these tools spawn on demand: SQLStudio needs SQLServer, and
# HackStudio's WebView asks for WebContent, which in turn wants RequestServer. They
# are SDE services (build-services.sh builds them into the SDE tree), so point the
# session app directory at them the way a session does. Without WebContent, LibWM
# reports "service 'webcontent' not found" and the WebView then trips a
# release_value_but_fixme_should_propagate_errors() -- a SIGILL, not a failed map.
"$SDE_ROOT/scripts/build-services.sh" >/dev/null 2>&1 || true
for service in SQLServer WebContent RequestServer; do
    if [[ -x "$SDE_ROOT/build/$service/$service" ]]; then
        ln -sf "$SDE_ROOT/build/$service/$service" "$APP_BIN/$service"
    else
        echo "  note: $service is not built, so a tool that needs it may fault"
    fi
done

echo "=== each devtool launches into the compositor ==="
for tool in "${TOOLS[@]}"; do
    LOG="$RUNTIME/$tool.log"
    before="$(grep -c "toplevel mapped" "$RUNTIME/comp.log" || true)"
    env WAYLAND_DISPLAY="$SOCK" \
        LD_LIBRARY_PATH="$BUILD/lib" \
        SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}" \
        SDE_SESSION_DIR="$SESSION_DIR" \
        SDE_SESSION_BIN_DIR="$APP_BIN" \
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
        echo "      --- $tool log ---"; tail -6 "$LOG" 2>/dev/null | sed "s/^/      /" || true
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

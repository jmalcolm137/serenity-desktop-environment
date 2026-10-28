#!/usr/bin/env bash
# ci-tier: full
# ci-timeout: 1800
# Run the pinned WLCS compositor conformance suite against SDE's WindowServer.
#
# The integration module starts a private headless WindowServer, so this never
# touches the live session. WLCS itself is fetched and built by
# scripts/build-wlcs.sh; nothing from it is vendored.
#
#   scripts/run-wlcs-test.sh
#   WLCS_FILTER='*' scripts/run-wlcs-test.sh
#   WLCS_FILTER='KeyboardTest.*:WlSubcompositorTest.*' scripts/run-wlcs-test.sh
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
SERENITY="${SERENITY:-$LIBGUI_WAYLAND_ROOT/serenity}"
BUILD="${BUILD:-$LIBGUI_WAYLAND_ROOT/build/lagom}"
WLCS_BUILD_DIR="${WLCS_BUILD_DIR:-$SDE_ROOT/build/wlcs}"
WLCS_BIN="${WLCS_BIN:-$WLCS_BUILD_DIR/wlcs}"
INTEGRATION="${INTEGRATION:-$WLCS_BUILD_DIR/sde_wlcs_integration.so}"
WLCS_FILTER="${WLCS_FILTER:-WlSubcompositorTest.*:SurfaceTest.*:KeyboardTest.*:WlSurfaceTest.*:WlSurfaceEventsTest.*:WlOutputTest.*}"
WLCS_TIMEOUT="${WLCS_TIMEOUT:-600}"

wlcs_build_log="${WLCS_BUILD_LOG:-$WLCS_BUILD_DIR/build.log}"
mkdir -p "$(dirname "$wlcs_build_log")"
if ! "$SDE_ROOT/scripts/build-wlcs.sh" >"$wlcs_build_log" 2>&1; then
    echo "==> WLCS build FAILED; log follows"
    cat "$wlcs_build_log"
    exit 1
fi
"$SDE_ROOT/scripts/build-window-server.sh" >/dev/null
[[ -x "$WLCS_BIN" ]] || { echo "SKIP: WLCS was not built"; exit 0; }
[[ -f "$INTEGRATION" ]] || { echo "SKIP: SDE WLCS integration was not built"; exit 0; }

RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/sde-wlcs.XXXXXX")"
chmod 700 "$RUNTIME"
PID=""
cleanup() {
    if [[ -n "$PID" ]]; then
        kill -TERM -- "-$PID" 2>/dev/null || kill -TERM "$PID" 2>/dev/null || true
        wait "$PID" 2>/dev/null || true
    fi
    rm -rf "$RUNTIME"
}
trap cleanup EXIT

echo "=== WLCS: $WLCS_FILTER ==="
setsid env -u WAYLAND_DISPLAY -u DISPLAY \
    XDG_RUNTIME_DIR="$RUNTIME" \
    SDE_ROOT="$SDE_ROOT" \
    SDE_LOG_LEVEL="${SDE_LOG_LEVEL:-error}" \
    SDE_WINDOW_SERVER="$SDE_ROOT/build/WindowServer/WindowServer" \
    SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}" \
    LD_LIBRARY_PATH="$BUILD/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
    timeout "$WLCS_TIMEOUT" "$WLCS_BIN" "$INTEGRATION" \
        --gtest_filter="$WLCS_FILTER" "$@" &
PID=$!
status=0
wait "$PID" || status=$?
PID=""
if [[ "$status" -eq 124 ]]; then
    echo "==> WLCS timed out after ${WLCS_TIMEOUT}s"
fi
exit "$status"

#!/usr/bin/env bash
# Headless test for the service-layering rule (see docs/06-service-layering.md):
# run the same client twice and check that
#
#   (A) with no external service, libgui-wayland's in-process fallback answers, and
#   (B) with a session service listening on the portal path, LibWM defers to it,
#
# for both the launch and image portals. Needs no compositor: the session portals
# are independent of the window server. Uses a private session directory, so it
# never touches (or gets answered by) a live SDE session.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
SERENITY="${SERENITY:-$LIBGUI_WAYLAND_ROOT/serenity}"
BUILD="${BUILD:-$LIBGUI_WAYLAND_ROOT/build/lagom}"
LAUNCH_SERVER="$SDE_ROOT/build/LaunchServer/LaunchServer"
IMAGE_DECODER="$SDE_ROOT/build/ImageDecoder/ImageDecoder"
TEST="$SDE_ROOT/build/test/libwm-services-test"

echo "== building prerequisites =="
"$SDE_ROOT/scripts/build-launch-server.sh" >/dev/null
"$SDE_ROOT/scripts/build-imagedecoder.sh" >/dev/null
"$SDE_ROOT/scripts/build-tests.sh" libwm-services-test >/dev/null 2>&1 || true

RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/sde-services.XXXXXX")"
chmod 700 "$RUNTIME"
trap 'for _ in $(seq 1 60); do kill -0 "${COMP_PID:-${CP:-}}" 2>/dev/null || break; sleep 0.05; done; rm -rf "$RUNTIME"' EXIT

# A private per-session runtime directory (see the SDE_SESSION_DIR seam), so the
# fixed "/tmp/session/0/portal/..." paths of a live session are left alone.
SESSION_DIR="$RUNTIME/session"
mkdir -p "$SESSION_DIR/portal"
export SDE_SESSION_DIR="$SESSION_DIR"

export SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}"
export SDE_APP_BIN_DIR="${SDE_APP_BIN_DIR:-$BUILD/bin}"
export LD_LIBRARY_PATH="$BUILD/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
# Deliberately no compositor: make LibWM skip the window connection so the test
# is fully self-contained. The launch/image portals do not need one.
export WAYLAND_DISPLAY="sde-services-test-none"

fail=0
check() {
    if grep -qF "$2" "$1"; then
        echo "  ok: $2"
    else
        echo "  MISSING ($1): $2"
        fail=1
    fi
}
check_absent() {
    if grep -qF "$2" "$1"; then
        echo "  UNEXPECTED ($1): $2"
        fail=1
    else
        echo "  ok (absent): $2"
    fi
}

echo
echo "=== (A) no external service: the in-process fallback must answer ==="
"$TEST" > "$RUNTIME/standalone.log" 2>&1 || true
cat "$RUNTIME/standalone.log"
check "$RUNTIME/standalone.log" "LAUNCH-PROBE: png handlers="
check "$RUNTIME/standalone.log" "LAUNCH-PROBE:   png => '/bin/ImageViewer'"
check "$RUNTIME/standalone.log" "LAUNCH-PROBE: txt handlers="
check "$RUNTIME/standalone.log" "APPFILE-PROBE: delegated match=1"
check "$RUNTIME/standalone.log" "IMAGE-PROBE: decoded 4x3 frames=1"
check "$RUNTIME/standalone.log" "SERVICE-LAYERING-TEST: done"
check_absent "$RUNTIME/standalone.log" "deferring to the session service on '$SESSION_DIR/portal/launch'"
check_absent "$RUNTIME/standalone.log" "deferring to the session service on '$SESSION_DIR/portal/image'"

echo
echo "=== (B) external session services: LibWM must defer to them ==="
"$LAUNCH_SERVER" > "$RUNTIME/launchserver.log" 2>&1 &
LAUNCH_PID=$!
"$IMAGE_DECODER" > "$RUNTIME/imagedecoder.log" 2>&1 &
IMAGE_PID=$!
for _ in $(seq 1 50); do
    [[ -S "$SESSION_DIR/portal/launch" && -S "$SESSION_DIR/portal/image" ]] && break
    sleep 0.1
done

"$TEST" > "$RUNTIME/services.log" 2>&1 || true
cat "$RUNTIME/services.log"

kill "$LAUNCH_PID" "$IMAGE_PID" 2>/dev/null || true
wait "$LAUNCH_PID" "$IMAGE_PID" 2>/dev/null || true

check "$RUNTIME/services.log" "SERVICE-LAYERING-TEST: done"
check "$RUNTIME/services.log" "IMAGE-PROBE: decoded 4x3 frames=1"
check "$RUNTIME/services.log" "deferring to the session service on '$SESSION_DIR/portal/launch'"
check "$RUNTIME/services.log" "deferring to the session service on '$SESSION_DIR/portal/image'"
check "$RUNTIME/launchserver.log" "SDE LaunchServer: client connected"
check "$RUNTIME/imagedecoder.log" "SDE ImageDecoder: client connected"

echo
if [[ "$fail" -ne 0 ]]; then
    echo "==> service-layering test FAILED"
    exit 1
fi
echo "==> service-layering test PASSED"

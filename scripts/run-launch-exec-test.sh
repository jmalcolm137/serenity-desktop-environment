#!/usr/bin/env bash
# Headless test (R4.2-adjacent): a Desktop::Launcher::open() of an *executable*
# file URL (the desktop right-click "Display Settings" path) must run the
# executable, mapping Serenity's "/bin/X" to the session app dir — not resolve it
# as a document. Uses the in-process LaunchServer, so no compositor is needed.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
SERENITY="${SERENITY:-$LIBGUI_WAYLAND_ROOT/serenity}"
BUILD="${BUILD:-$LIBGUI_WAYLAND_ROOT/build/lagom}"
TEST="$BUILD/bin/libwm-launch-exec-test"

echo "== building prerequisite =="
ninja -C "$BUILD" libwm-launch-exec-test >/dev/null

RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/sde-launch-exec.XXXXXX")"
chmod 700 "$RUNTIME"
trap 'rm -rf "$RUNTIME"' EXIT

# A fake "DisplaySettings" in the session app dir that records that it ran.
mkdir -p "$RUNTIME/bin" "$RUNTIME/session/portal"
cat > "$RUNTIME/bin/DisplaySettings" <<EOF
#!/bin/sh
touch "$RUNTIME/ran"
EOF
chmod +x "$RUNTIME/bin/DisplaySettings"

env SDE_APP_BIN_DIR="$RUNTIME/bin" \
    SDE_SESSION_DIR="$RUNTIME/session" \
    SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}" \
    WAYLAND_DISPLAY="sde-launch-exec-none" \
    LD_LIBRARY_PATH="$BUILD/lib" \
    "$TEST" > "$RUNTIME/test.log" 2>&1 || true
cat "$RUNTIME/test.log"

fail=0
check() {
    if grep -qF "$1" "$RUNTIME/test.log"; then
        echo "  ok: $1"
    else
        echo "  MISSING: $1"
        fail=1
    fi
    return 0
}

echo "=== launch-exec test ==="
check "LAUNCH-EXEC-TEST: opened /bin/DisplaySettings = true"
if [[ -e "$RUNTIME/ran" ]]; then
    echo "  ok: the executable ran"
else
    echo "  MISSING: the executable did not run"
    fail=1
fi

if [[ "$fail" -ne 0 ]]; then
    echo "==> launch-exec test FAILED"
    exit 1
fi
echo "==> launch-exec test PASSED"

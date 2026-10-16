#!/usr/bin/env bash
# R1 smoke test: run the SDE compositor with the HEADLESS backend and confirm it
# hosts the three surface kinds the milestone calls for:
#
#   1. a LibWindowServer (Serenity) application  -> an xdg toplevel
#   2. a layer-shell panel                        -> a layer surface
#   3. a native (non-Serenity) Wayland application-> an xdg toplevel
#
# It also checks the R6/N4 liveness heartbeat (SDE_HEARTBEAT_FILE) that the
# session supervisor's watchdog watches, R6/N4 protocol-error isolation (a client
# that violates a protocol rule is dropped; the compositor keeps serving), the
# R6/N8 crash-report policy (a fatal signal writes a backtrace), and the R6/N5
# renderer fallback (a broken GPU path falls back to the software renderer
# instead of ending the session).
#
# Runs entirely headless (no host compositor, no DRM), so it is CI-friendly.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
SERENITY="${SERENITY:-$LIBGUI_WAYLAND_ROOT/serenity}"
BUILD="${BUILD:-$LIBGUI_WAYLAND_ROOT/build/lagom}"
WLROOTS_SRC="${WLROOTS_SRC:-$SDE_ROOT/third_party/wlroots}"
COMP="$SDE_ROOT/build/WindowServer/WindowServer"
TOOLS_BUILD="$SDE_ROOT/build/headless-test"

"$SDE_ROOT/scripts/build-window-server.sh" >/dev/null

mkdir -p "$TOOLS_BUILD"
LAYER_XML="$WLROOTS_SRC/protocol/wlr-layer-shell-unstable-v1.xml"
XDG_XML="$(pkg-config --variable=pkgdatadir wayland-protocols)/stable/xdg-shell/xdg-shell.xml"
wayland-scanner client-header "$LAYER_XML" "$TOOLS_BUILD/wlr-layer-shell-unstable-v1-client-protocol.h"
wayland-scanner private-code  "$LAYER_XML" "$TOOLS_BUILD/wlr-layer-shell-unstable-v1-protocol.c"
wayland-scanner client-header "$XDG_XML"   "$TOOLS_BUILD/xdg-shell-client-protocol.h"
wayland-scanner private-code  "$XDG_XML"   "$TOOLS_BUILD/xdg-shell-protocol.c"
SW_XML="$SDE_ROOT/WindowServer/protocol/serenity-window.xml"
wayland-scanner client-header "$SW_XML"    "$TOOLS_BUILD/serenity-window-client-protocol.h"
wayland-scanner private-code  "$SW_XML"    "$TOOLS_BUILD/serenity-window-protocol.c"
PRES_XML="$(pkg-config --variable=pkgdatadir wayland-protocols)/stable/presentation-time/presentation-time.xml"
wayland-scanner client-header "$PRES_XML"  "$TOOLS_BUILD/presentation-time-client-protocol.h"
wayland-scanner private-code  "$PRES_XML"  "$TOOLS_BUILD/presentation-time-protocol.c"
FRAC_XML="$(pkg-config --variable=pkgdatadir wayland-protocols)/staging/fractional-scale/fractional-scale-v1.xml"
wayland-scanner client-header "$FRAC_XML"  "$TOOLS_BUILD/fractional-scale-v1-client-protocol.h"
wayland-scanner private-code  "$FRAC_XML"  "$TOOLS_BUILD/fractional-scale-v1-protocol.c"
DECO_XML="$(pkg-config --variable=pkgdatadir wayland-protocols)/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml"
wayland-scanner client-header "$DECO_XML"  "$TOOLS_BUILD/xdg-decoration-client-protocol.h"
wayland-scanner private-code  "$DECO_XML"  "$TOOLS_BUILD/xdg-decoration-protocol.c"
cc -O2 -I "$TOOLS_BUILD" \
    "$SDE_ROOT/tools/layer-shell-test.c" \
    "$TOOLS_BUILD/wlr-layer-shell-unstable-v1-protocol.c" \
    "$TOOLS_BUILD/xdg-shell-protocol.c" \
    $(pkg-config --cflags --libs wayland-client) \
    -o "$TOOLS_BUILD/layer-shell-test"
cc -O2 -I "$TOOLS_BUILD" \
    "$SDE_ROOT/tools/native-app-test.c" \
    "$TOOLS_BUILD/xdg-shell-protocol.c" \
    "$TOOLS_BUILD/serenity-window-protocol.c" \
    "$TOOLS_BUILD/presentation-time-protocol.c" \
    "$TOOLS_BUILD/fractional-scale-v1-protocol.c" \
    "$TOOLS_BUILD/xdg-decoration-protocol.c" \
    $(pkg-config --cflags --libs wayland-client) \
    -o "$TOOLS_BUILD/native-app-test"
cc -O2 -I "$TOOLS_BUILD" \
    "$SDE_ROOT/tools/protocol-error-test.c" \
    $(pkg-config --cflags --libs wayland-client) \
    -o "$TOOLS_BUILD/protocol-error-test"

RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/sde-headless.XXXXXX")"
export SDE_STATE_DIR="$RUNTIME"
chmod 700 "$RUNTIME"
trap 'for _ in $(seq 1 60); do kill -0 "${COMP_PID:-${CP:-}}" 2>/dev/null || break; sleep 0.05; done; rm -rf "$RUNTIME"; rm -f "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"/sde-*-$$* "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"/sde-*-$$*.lock 2>/dev/null || true' EXIT

SOCK="sde-headless-$$"

# No WAYLAND_DISPLAY/DISPLAY so wlr_backend_autocreate uses the requested backend.
# SDE_WM_PORTAL_PATH keeps the WindowManagerServer socket out of /tmp/portal/wm,
# so the test cannot disturb a live session.
env -u WAYLAND_DISPLAY -u DISPLAY \
    WLR_BACKENDS=headless \
    WLR_HEADLESS_OUTPUTS=1 \
    SDE_WM_PORTAL_PATH="$RUNTIME/wm" \
    SDE_HEARTBEAT_FILE="$RUNTIME/heartbeat" \
    SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}" \
    LD_LIBRARY_PATH="$BUILD/lib" \
    "$COMP" -n "$SOCK" -s "$BUILD/bin/TextEditor" > "$RUNTIME/comp.log" 2>&1 &
COMP_PID=$!

for _ in $(seq 1 50); do
    [[ -e "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/$SOCK" ]] && break
    sleep 0.1
done

env WAYLAND_DISPLAY="$SOCK" timeout 4 "$TOOLS_BUILD/layer-shell-test" > "$RUNTIME/layer.log" 2>&1 || true
env WAYLAND_DISPLAY="$SOCK" timeout 4 "$TOOLS_BUILD/native-app-test" > "$RUNTIME/native.log" 2>&1 || true
# Give the LibWindowServer app a moment to map its toplevel.
sleep 1

# R6/N4: a client protocol error must be isolated to that client; the compositor
# keeps running and can still map a fresh client's window.
PROTOCOL_SCALE_STATUS=0
env WAYLAND_DISPLAY="$SOCK" "$TOOLS_BUILD/protocol-error-test" scale0 > "$RUNTIME/protocol-scale.log" 2>&1 || PROTOCOL_SCALE_STATUS=$?
PROTOCOL_SHM_STATUS=0
env WAYLAND_DISPLAY="$SOCK" "$TOOLS_BUILD/protocol-error-test" shm-size > "$RUNTIME/protocol-shm.log" 2>&1 || PROTOCOL_SHM_STATUS=$?
env WAYLAND_DISPLAY="$SOCK" timeout 4 "$TOOLS_BUILD/native-app-test" > "$RUNTIME/native-after-error.log" 2>&1 || true
sleep 1
NATIVE_MAPPED_COUNT="$(grep -c "toplevel mapped title='SDE Native Test'" "$RUNTIME/comp.log" || true)"

# R6: presentation-time — a client that requests feedback must receive a
# "presented" event (the compositor feeds wlroots' presentation object from
# output_frame).
PRESENTATION_STATUS=0
env WAYLAND_DISPLAY="$SOCK" timeout 4 "$TOOLS_BUILD/native-app-test" --presentation > "$RUNTIME/presentation.log" 2>&1 || PRESENTATION_STATUS=$?

# R6/N4: the compositor writes a liveness heartbeat for the session watchdog.
HEARTBEAT_VALUE="$(cat "$RUNTIME/heartbeat" 2>/dev/null || echo 0)"
HEARTBEAT_NOW="$(date +%s)"

# Stop the shell app the compositor spawned (it is a child of the compositor),
# then the compositor itself.
pkill -P "$COMP_PID" 2>/dev/null || true
kill "$COMP_PID" 2>/dev/null || true
wait "$COMP_PID" 2>/dev/null || true

echo "--- compositor log ---"
cat "$RUNTIME/comp.log"
echo "--- layer-shell client ---"
cat "$RUNTIME/layer.log"
echo "--- native client ---"
cat "$RUNTIME/native.log"
echo "--- protocol-error client (scale0) ---"
cat "$RUNTIME/protocol-scale.log"
echo "--- protocol-error client (shm-size) ---"
cat "$RUNTIME/protocol-shm.log"
echo "--- presentation client ---"
cat "$RUNTIME/presentation.log"

fail=0
check() {
    if grep -qF "$1" "$RUNTIME/comp.log"; then
        echo "  ok: $1"
    else
        echo "  MISSING: $1"
        fail=1
    fi
}

echo "=== R1 headless smoke test ==="
check "output 'HEADLESS-1' 1280x720"
check "layer surface namespace='sde-test-panel'"
check "toplevel mapped title='SDE Native Test'"
check "toplevel mapped title='Untitled[*] - Text Editor'"

# R6/N4: the liveness heartbeat must be present and recent (within 5s).
if [ "${HEARTBEAT_VALUE:-0}" -ge "$((HEARTBEAT_NOW - 5))" ] 2>/dev/null; then
    echo "  ok: compositor heartbeat is live (${HEARTBEAT_VALUE}s)"
else
    echo "  MISSING: compositor heartbeat (got '${HEARTBEAT_VALUE}', now ${HEARTBEAT_NOW})"
    fail=1
fi

# R6/N4: protocol errors are isolated to the offending client.
echo "=== R6/N4 protocol-error isolation ==="
if [ "${PROTOCOL_SCALE_STATUS:-1}" -eq 0 ]; then
    echo "  ok: invalid surface scale delivered to the client"
else
    echo "  MISSING: invalid surface scale was not reported"
    fail=1
fi
if [ "${PROTOCOL_SHM_STATUS:-1}" -eq 0 ]; then
    echo "  ok: invalid shm pool size delivered to the client"
else
    echo "  MISSING: invalid shm pool size was not reported"
    fail=1
fi
if [ "${NATIVE_MAPPED_COUNT:-0}" -ge 2 ]; then
    echo "  ok: compositor survived the protocol errors (a client mapped after)"
else
    echo "  MISSING: no client mapped after the protocol errors"
    fail=1
fi

# R6: presentation-time feedback.
echo "=== R6 presentation-time ==="
if [ "${PRESENTATION_STATUS:-1}" -eq 0 ] && grep -q "presented refresh=" "$RUNTIME/presentation.log"; then
    echo "  ok: presentation feedback delivered ($(grep -o 'presented refresh=.*' "$RUNTIME/presentation.log" | head -1))"
else
    echo "  MISSING: no presentation feedback"
    fail=1
fi

# R6/N5 renderer fallback: a broken GPU path (an unavailable DRM render node)
# must fall back to the software renderer and still bring up a working session,
# rather than ending it.
echo "=== R6/N5 renderer fallback ==="
SOCK_FALLBACK="sde-renderer-$$"
env -u WAYLAND_DISPLAY -u DISPLAY \
    WLR_BACKENDS=headless \
    WLR_HEADLESS_OUTPUTS=1 \
    WLR_RENDERER=gles2 \
    WLR_RENDER_DRM_DEVICE=/nonexistent/render-node \
    SDE_WM_PORTAL_PATH="$RUNTIME/wm-fallback" \
    SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}" \
    LD_LIBRARY_PATH="$BUILD/lib" \
    "$COMP" -n "$SOCK_FALLBACK" > "$RUNTIME/comp-fallback.log" 2>&1 &
FALLBACK_PID=$!
for _ in $(seq 1 50); do
    [[ -e "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/$SOCK_FALLBACK" ]] && break
    sleep 0.1
done
env WAYLAND_DISPLAY="$SOCK_FALLBACK" timeout 4 "$TOOLS_BUILD/native-app-test" > "$RUNTIME/fallback-native.log" 2>&1 || true
sleep 0.5
kill "$FALLBACK_PID" 2>/dev/null || true
wait "$FALLBACK_PID" 2>/dev/null || true
cat "$RUNTIME/comp-fallback.log"
check_log() {
    if grep -qF "$1" "$2"; then
        echo "  ok: $1"
    else
        echo "  MISSING ($2): $1"
        fail=1
    fi
}
check_log "falling back to the software (pixman) renderer" "$RUNTIME/comp-fallback.log"
check_log "output 'HEADLESS-1' 1280x720" "$RUNTIME/comp-fallback.log"
check_log "toplevel mapped title='SDE Native Test'" "$RUNTIME/comp-fallback.log"

# Explicit selection — the documented "software mode" (`[Output] Renderer=pixman`
# or `SDE_RENDERER=pixman`) — must use pixman directly, with no fallback.
SOCK_EXPLICIT="sde-renderer-explicit-$$"
env -u WAYLAND_DISPLAY -u DISPLAY \
    WLR_BACKENDS=headless \
    WLR_HEADLESS_OUTPUTS=1 \
    SDE_RENDERER=pixman \
    SDE_WM_PORTAL_PATH="$RUNTIME/wm-explicit" \
    SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}" \
    LD_LIBRARY_PATH="$BUILD/lib" \
    "$COMP" -n "$SOCK_EXPLICIT" > "$RUNTIME/comp-explicit.log" 2>&1 &
EXPLICIT_PID=$!
for _ in $(seq 1 50); do
    [[ -e "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/$SOCK_EXPLICIT" ]] && break
    sleep 0.1
done
kill "$EXPLICIT_PID" 2>/dev/null || true
wait "$EXPLICIT_PID" 2>/dev/null || true
check_log "renderer 'pixman'" "$RUNTIME/comp-explicit.log"

# R6/N8 crash-dump policy: a fatal signal writes a backtrace to the crash log and
# the process still dies with the signal (the supervisor then restarts).
echo "=== R6/N8 crash report ==="
SOCK_CRASH="sde-crash-$$"
env -u WAYLAND_DISPLAY -u DISPLAY \
    WLR_BACKENDS=headless \
    WLR_HEADLESS_OUTPUTS=1 \
    SDE_WM_PORTAL_PATH="$RUNTIME/wm-crash" \
    SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}" \
    LD_LIBRARY_PATH="$BUILD/lib" \
    "$COMP" -n "$SOCK_CRASH" > "$RUNTIME/comp-crash.log" 2>&1 &
CRASH_PID=$!
for _ in $(seq 1 50); do
    [[ -e "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/$SOCK_CRASH" ]] && break
    sleep 0.1
done
kill -ABRT "$CRASH_PID" 2>/dev/null || true
CRASH_STATUS=0
wait "$CRASH_PID" 2>/dev/null || CRASH_STATUS=$?
if [ "${CRASH_STATUS:-0}" -eq 134 ]; then
    echo "  ok: died with SIGABRT (exit 134)"
else
    echo "  MISSING: exit status ${CRASH_STATUS} (expected 134)"
    fail=1
fi
if grep -q "crashed with SIGABRT" "$RUNTIME/WindowServer-crash.log" 2>/dev/null; then
    echo "  ok: crash report written ($RUNTIME/WindowServer-crash.log)"
else
    echo "  MISSING: crash report"
    fail=1
fi

# R6 fractional scaling: a 1.5x output must assign a 1.5 preferred scale to a
# client (wp_fractional_scale_v1, in 1/120ths -> 180).
echo "=== R6 fractional scaling (1.5x) ==="
SOCK_FRAC="sde-frac-$$"
env -u WAYLAND_DISPLAY -u DISPLAY \
    WLR_BACKENDS=headless \
    WLR_HEADLESS_OUTPUTS=1 \
    SDE_OUTPUT_SCALE=1.5 \
    SDE_WM_PORTAL_PATH="$RUNTIME/wm-frac" \
    SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}" \
    LD_LIBRARY_PATH="$BUILD/lib" \
    "$COMP" -n "$SOCK_FRAC" > "$RUNTIME/comp-frac.log" 2>&1 &
FRAC_PID=$!
for _ in $(seq 1 50); do
    [[ -e "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/$SOCK_FRAC" ]] && break
    sleep 0.1
done
FRAC_STATUS=0
env WAYLAND_DISPLAY="$SOCK_FRAC" timeout 4 "$TOOLS_BUILD/native-app-test" --fractional > "$RUNTIME/fractional.log" 2>&1 || FRAC_STATUS=$?
kill "$FRAC_PID" 2>/dev/null || true
wait "$FRAC_PID" 2>/dev/null || true
check_log "@1.5x" "$RUNTIME/comp-frac.log"
if [ "${FRAC_STATUS:-1}" -eq 0 ] && grep -q "preferred_scale=180" "$RUNTIME/fractional.log"; then
    echo "  ok: client learned the 1.5x preferred scale ($(grep -o 'preferred_scale=[0-9]* (.*)' "$RUNTIME/fractional.log" | head -1))"
else
    echo "  MISSING: fractional preferred scale"
    fail=1
fi

if [[ "$fail" -ne 0 ]]; then
    echo "==> R1 headless smoke test FAILED"
    exit 1
fi
echo "==> R1 headless smoke test PASSED"

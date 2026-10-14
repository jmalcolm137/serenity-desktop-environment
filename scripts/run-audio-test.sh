#!/usr/bin/env bash
# AudioServer service test.
#
# The SerenityOS AudioServer / AudioManager protocols are served by an SDE
# *service* (AudioServer), spawned on demand by LibWM the first time an
# application connects to /tmp/session/<sid>/portal/audio. This runs a headless
# compositor + Piano (an unmodified Serenity audio client) and checks that the
# service is spawned and serves the client; the PipeWire output stream is checked
# when a PipeWire server is reachable.
#
# See docs/06-service-layering.md.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
SERENITY="${SERENITY:-$LIBGUI_WAYLAND_ROOT/serenity}"
BUILD="${BUILD:-$LIBGUI_WAYLAND_ROOT/build/lagom}"
COMP="$SDE_ROOT/build/WindowServer/WindowServer"

"$SDE_ROOT/scripts/build-window-server.sh" >/dev/null
"$SDE_ROOT/scripts/build-services.sh" >/dev/null 2>&1 || true
ninja -C "$BUILD" Piano >/dev/null 2>&1 || true
[[ -x "$BUILD/bin/Piano" ]] || { echo "SKIP: Piano not built"; exit 0; }
[[ -x "$SDE_ROOT/build/AudioServer/AudioServer" ]] || { echo "SKIP: AudioServer not built"; exit 0; }

RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/sde-audio.XXXXXX")"
chmod 700 "$RUNTIME"
trap 'rm -rf "$RUNTIME"' EXIT

# The session app directory LibWM resolves services and applications from.
APP_BIN="$RUNTIME/app-bin"
mkdir -p "$APP_BIN"
for app in "$BUILD/bin"/*; do
    [[ -e "$app" ]] || continue
    ln -sf "$app" "$APP_BIN/$(basename "$app")"
done
ln -sf "$SDE_ROOT/build/AudioServer/AudioServer" "$APP_BIN/AudioServer"

# PipeWire's socket lives in the real runtime dir, not our sandbox.
PW_RUNTIME=""
for candidate in "${PIPEWIRE_RUNTIME_DIR:-}" "/run/user/$(id -u)" "${XDG_RUNTIME_DIR:-}"; do
    if [[ -n "$candidate" && -S "$candidate/pipewire-0" ]]; then
        PW_RUNTIME="$candidate"
        break
    fi
done

SOCK="sde-audio-$$"
setsid env -u WAYLAND_DISPLAY -u DISPLAY \
    WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 SDE_OUTPUT_SCALE=1 \
    SDE_STATE_DIR="$RUNTIME" \
    SDE_APP_BIN_DIR="$APP_BIN" \
    PIPEWIRE_RUNTIME_DIR="${PW_RUNTIME:-$RUNTIME}" \
    SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}" \
    LD_LIBRARY_PATH="$BUILD/lib" \
    "$COMP" -n "$SOCK" -s "$APP_BIN/Piano" > "$RUNTIME/comp.log" 2>&1 &
COMP_PID=$!
for _ in $(seq 1 80); do
    [[ -e "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/$SOCK" ]] && break
    sleep 0.1
done
sleep 5
kill -TERM -- "-$COMP_PID" 2>/dev/null || kill "$COMP_PID" 2>/dev/null || true
wait "$COMP_PID" 2>/dev/null || true

fail=0
check() { if eval "$2"; then echo "  ok: $1"; else echo "  MISSING: $1"; fail=1; fi; }

echo "=== audio service ==="
grep -E "spawned AudioServer|audio client connected|PipeWire" "$RUNTIME/comp.log" | head || true

check "LibWM spawned the AudioServer service" "grep -q 'LibWM: spawned AudioServer' \"$RUNTIME/comp.log\""
check "the AudioServer accepted the client" "grep -q 'AudioServer: audio client connected' \"$RUNTIME/comp.log\""
check "Piano mapped its window" "grep -q \"toplevel mapped title='Piano\" \"$RUNTIME/comp.log\""
if [[ -n "$PW_RUNTIME" ]]; then
    check "the PipeWire output stream started" "grep -q 'PipeWire output stream started' \"$RUNTIME/comp.log\""
else
    echo "  note: no PipeWire server; skipping the output-stream check"
fi

if [[ "$fail" -ne 0 ]]; then
    echo "--- compositor log (tail) ---"; tail -30 "$RUNTIME/comp.log"
    echo "==> audio test FAILED"
    exit 1
fi
echo "==> audio test PASSED"

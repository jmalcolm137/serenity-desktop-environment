#!/usr/bin/env bash
# AudioServer service test.
#
# The SerenityOS AudioServer / AudioManager protocols are served by an SDE
# *service* (AudioServer), spawned on demand by LibWM the first time an
# application connects to /tmp/session/<sid>/portal/audio. This runs a headless
# compositor + three unmodified Serenity clients — Piano (playback; starts a
# PipeWire stream), SoundPlayer (another AudioServer client, with a media/UI
# workload) and the Audio applet (an AudioManager/mixer client) — and checks the
# service is spawned and serves both protocols; the PipeWire output stream is
# checked when a PipeWire server is reachable.
#
# See docs/06-service-layering.md.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
. "$SDE_ROOT/scripts/lib/testlib.sh"
sde_test_init audio
sde_test_export_roots

"$SDE_ROOT/scripts/build-window-server.sh" >/dev/null
"$SDE_ROOT/scripts/build-services.sh" >/dev/null 2>&1 || true
"$SDE_ROOT/scripts/build-applets.sh" >/dev/null 2>&1 || true
ninja -C "$BUILD" Piano SoundPlayer >/dev/null 2>&1 || true
[[ -x "$BUILD/bin/Piano" ]] || { echo "SKIP: Piano not built"; exit 0; }
[[ -x "$BUILD/bin/SoundPlayer" ]] || { echo "SKIP: SoundPlayer not built"; exit 0; }
[[ -x "$SDE_ROOT/build/AudioServer/AudioServer" ]] || { echo "SKIP: AudioServer not built"; exit 0; }
APPLET="$SDE_ROOT/build/Applets/Audio"
[[ -x "$APPLET" ]] || { echo "SKIP: Audio applet not built"; exit 0; }

chmod 700 "$RUNTIME"

# A private per-session runtime directory (see the SDE_SESSION_DIR seam), so the
# fixed "/tmp/session/0/portal/..." paths of a live session are left alone: LibWM
# *defers* to a service that is already listening there, so without this the test
# would be answered by the live session's AudioServer instead of spawning its own.
SESSION_DIR="$RUNTIME/session"
mkdir -p "$SESSION_DIR/portal"
export SDE_SESSION_DIR="$SESSION_DIR"

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

sde_test_compositor --startup "$APP_BIN/Piano & $APP_BIN/SoundPlayer & $APPLET" SDE_STATE_DIR="$RUNTIME" SDE_APP_BIN_DIR="$APP_BIN" SDE_SESSION_DIR="$SESSION_DIR" PIPEWIRE_RUNTIME_DIR="${PW_RUNTIME:-$RUNTIME}"
# The startup clients race the compositor's socket: the test used to signal it as
# soon as the socket existed, so a slower client (Piano) had not mapped yet and
# the run failed intermittently. Wait for the workload instead of a fixed delay.
sde_test_wait_log "$RUNTIME/comp.log" "toplevel mapped title='Piano" 200 || true
sde_test_wait_log "$RUNTIME/comp.log" "toplevel mapped title='Sound Player'" 200 || true
sde_test_wait_log "$RUNTIME/comp.log" "layer surface namespace='serenity-applet:Audio'" 200 || true
kill -TERM -- "-$COMP_PID" 2>/dev/null || kill "$COMP_PID" 2>/dev/null || true
wait "$COMP_PID" 2>/dev/null || true

fail=0
check() { if eval "$2"; then echo "  ok: $1"; else echo "  MISSING: $1"; fail=1; fi; }

echo "=== audio service ==="
grep -E "spawned AudioServer|client connected|serenity-applet:Audio|PipeWire" "$RUNTIME/comp.log" | head || true

check "LibWM spawned the AudioServer service" "grep -q 'LibWM: spawned AudioServer' \"$RUNTIME/comp.log\""
check "Piano mapped its window" "grep -q \"toplevel mapped title='Piano\" \"$RUNTIME/comp.log\""
check "SoundPlayer mapped its window" "grep -q \"toplevel mapped title='Sound Player'\" \"$RUNTIME/comp.log\""
check "the AudioServer served both audio clients" "[[ \$(grep -c 'AudioServer: audio client connected' \"$RUNTIME/comp.log\") -ge 2 ]]"
check "the Audio applet mapped its layer surface" "grep -q \"layer surface namespace='serenity-applet:Audio'\" \"$RUNTIME/comp.log\""
check "the AudioServer served the mixer (manager) client" "grep -q 'AudioServer: manager client connected' \"$RUNTIME/comp.log\""
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

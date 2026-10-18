#!/usr/bin/env bash
# Headless test: the optional JSONL event trace (R6 observability).
#
# SDE_TRACE_FILE makes the compositor write one JSON object per line for window,
# focus, workspace, output and IPC events. It is off by default; the test asserts
# the events a debugging session actually needs, and that the file is valid JSONL
# rather than free-form text.
#
# Runs entirely headless (no host compositor, no DRM), so it is CI-friendly.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
SERENITY="${SERENITY:-$LIBGUI_WAYLAND_ROOT/serenity}"
BUILD="${BUILD:-$LIBGUI_WAYLAND_ROOT/build/lagom}"
COMP="$SDE_ROOT/build/WindowServer/WindowServer"
NATIVE="$SDE_ROOT/build/tools/native-app-test"
WS="$SDE_ROOT/build/tools/ext-workspace-test"
XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
export XDG_RUNTIME_DIR

"$SDE_ROOT/scripts/build-window-server.sh" >/dev/null
"$SDE_ROOT/scripts/build-tools.sh" >/dev/null
[[ -x "$NATIVE" && -x "$WS" ]] || { echo "SKIP: trace test clients not built"; exit 0; }

RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/sde-trace.XXXXXX")"
chmod 700 "$RUNTIME"
TRACE="$RUNTIME/trace.jsonl"
SOCK="sde-trace-$$"
COMP_PID=""
APP_PID=""
cleanup() {
    [[ -n "$APP_PID" ]] && kill "$APP_PID" 2>/dev/null || true
    [[ -n "$COMP_PID" ]] && kill -TERM -- "-$COMP_PID" 2>/dev/null || true
    [[ -n "$COMP_PID" ]] && kill -TERM "$COMP_PID" 2>/dev/null || true
    for _ in $(seq 1 60); do kill -0 "${COMP_PID:-}" 2>/dev/null || break; sleep 0.05; done
    rm -rf "$RUNTIME"
    rm -f "$XDG_RUNTIME_DIR/$SOCK"{,.lock}
}
trap cleanup EXIT

setsid env -u WAYLAND_DISPLAY -u DISPLAY \
    WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 SDE_OUTPUT_SCALE=1 SDE_XWAYLAND=0 \
    SDE_STATE_DIR="$RUNTIME/state" SDE_SESSION_DIR="$RUNTIME/session" \
    SDE_HEARTBEAT_FILE="$RUNTIME/heartbeat" \
    SDE_TRACE_FILE="$TRACE" SDE_TRACE_FRAMES=1 \
    SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}" \
    LD_LIBRARY_PATH="$BUILD/lib" \
    "$COMP" -n "$SOCK" > "$RUNTIME/comp.log" 2>&1 &
COMP_PID=$!
for _ in $(seq 1 80); do
    [[ -e "$XDG_RUNTIME_DIR/$SOCK" ]] && break
    sleep 0.1
done
sleep 0.3

fail=0
wait_for_trace() {
    local pattern="$1" i
    for i in $(seq 1 100); do
        grep -qF "$pattern" "$TRACE" 2>/dev/null && return 0
        sleep 0.1
    done
    return 1
}
check() {
    if eval "$2"; then
        echo "  ok: $1"
    else
        echo "  FAIL: $1"
        fail=1
    fi
}

echo "=== the trace starts and records a window ==="
check "the trace file was created" "[[ -s '$TRACE' ]]"
check "the startup event is recorded" "grep -qF '\"cat\":\"trace\"' '$TRACE'"

env WAYLAND_DISPLAY="$SOCK" LD_LIBRARY_PATH="$BUILD/lib" \
    SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}" \
    "$NATIVE" --title "Trace Test" --linger 8000 > "$RUNTIME/app.log" 2>&1 &
APP_PID=$!
wait_for_trace '"cat":"window"' || true
wait_for_trace '"cat":"focus"' || true
sleep 0.2

check "the window map is recorded" \
    "grep -qF '\"cat\":\"window\"' '$TRACE' && grep -qF \"map title='Trace Test'\" '$TRACE'"
check "the focus change is recorded" \
    "grep -qF '\"cat\":\"focus\"' '$TRACE' && grep -qF \"title='Trace Test'\" '$TRACE'"
check "a sampled frame is recorded" \
    "grep -qF '\"cat\":\"frame\"' '$TRACE'"

echo "=== the trace records a workspace switch ==="
env WAYLAND_DISPLAY="$SOCK" "$WS" 4 > "$RUNTIME/ws.log" 2>&1 || true
wait_for_trace '"cat":"workspace"' || true
check "the switch is recorded" \
    "grep -qF '\"cat\":\"workspace\"' '$TRACE' && grep -qF 'switched 1,1' '$TRACE'"

echo "=== every line is a JSON object ==="
# No jq dependency: check the shape the format guarantees, and that a known
# object parses with python3 when it is available.
bad_lines="$(grep -cvE '^\{"t":[0-9]+,"cat":"[^"]+","msg":".*"\}$' "$TRACE" || true)"
check "all lines are objects with t/cat/msg" "[[ '${bad_lines:-0}' -eq 0 ]]"
if command -v python3 >/dev/null 2>&1; then
    check "the file parses as JSONL" \
        "python3 -c 'import json,sys; [json.loads(line) for line in open(sys.argv[1])]' '$TRACE'"
fi

kill "$APP_PID" 2>/dev/null || true
wait "$APP_PID" 2>/dev/null || true
APP_PID=""
kill -TERM "$COMP_PID" 2>/dev/null || true
wait "$COMP_PID" 2>/dev/null || true
COMP_PID=""

if [[ "$fail" -ne 0 ]]; then
    echo "--- trace ---"
    cat "$TRACE" 2>/dev/null || true
    echo "--- compositor log (tail) ---"
    tail -20 "$RUNTIME/comp.log" 2>/dev/null || true
    echo "==> trace test FAILED"
    exit 1
fi
echo "==> trace test PASSED"

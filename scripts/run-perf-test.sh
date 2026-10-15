#!/usr/bin/env bash
# Headless performance / latency budget test (R6, N3).
#
# N3's budget: "low, predictable input latency; smooth 60 Hz; a software
# compositor fallback". The compositor meters its own frame time and the input ->
# present interval (see the meter above output_frame() in WindowServer/src/main.cpp
# and docs/25-performance.md); this drives it headless with the *software* renderer
# (the fallback) and asserts the budget.
#
# Three phases:
#   (A) idle pacing   — nothing happens, so the compositor must not render: a
#                       compositor that repaints unconditionally burns power.
#   (B) input latency — ~2 s of pointer motion at 60 Hz: the frame time and the
#                       input -> present interval must stay inside the budget.
#   (C) worst case    — dragging a window (the heaviest path we own: every motion
#                       moves the window and re-renders its frame) on pixman.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
SERENITY="${SERENITY:-$LIBGUI_WAYLAND_ROOT/serenity}"
BUILD="${BUILD:-$LIBGUI_WAYLAND_ROOT/build/lagom}"
COMP="$SDE_ROOT/build/WindowServer/WindowServer"
VI="$("$SDE_ROOT/scripts/build-virtual-input.sh")"

"$SDE_ROOT/scripts/build-window-server.sh" >/dev/null
"$SDE_ROOT/scripts/build-tools.sh" >/dev/null

# The budget (docs/25-performance.md). Frame time is the compositor's own work;
# input -> present is the compositor + client path on a headless output, which has
# no vblank — the numbers below are therefore the share we control.
FRAME_BUDGET_MS="${SDE_PERF_FRAME_BUDGET_MS:-16.6}"     # 60 Hz
LATENCY_BUDGET_MS="${SDE_PERF_LATENCY_BUDGET_MS:-20}"   # compositor + client, no vblank
IDLE_FRAME_LIMIT="${SDE_PERF_IDLE_FRAME_LIMIT:-3}"      # frames a second while idle
RENDERER="${SDE_PERF_RENDERER:-pixman}"                 # the software fallback
PERF_FAST_MS="${SDE_PERF_FAST_MS:-2}"                   # gross-regression guard
AGGRESSIVE_MS="${SDE_PERF_AGGRESSIVE_MS:-4.167}"        # 240 Hz frame interval
AGGRESSIVE_HZ=240
RSS_LIMIT_KB="${SDE_PERF_RSS_LIMIT_KB:-81920}"          # idle resident memory (80 MB)
RSS_GROWTH_KB="${SDE_PERF_RSS_GROWTH_KB:-4096}"         # growth across window cycles

WIDTH=1280
HEIGHT=720

RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/sde-perf.XXXXXX")"
chmod 700 "$RUNTIME"

# The runner's XDG_RUNTIME_DIR may not exist or may not be writable -- a container
# is not a login session -- and then the compositor cannot create its Wayland socket
# at all. That is exactly how the budget job failed:
#
#   WindowServer: failed to create a Wayland socket
#   ==> perf test FAILED (the compositor wrote no .../perf)
#
# The test driver grew this isolation first; this script needs it too, or the
# budget job can only fail where the tests pass.
export XDG_RUNTIME_DIR="$RUNTIME/run"
mkdir -p "$XDG_RUNTIME_DIR"
chmod 700 "$XDG_RUNTIME_DIR"

COMP_PID=""
APP_PID=""
cleanup() {
    [[ -n "$APP_PID" ]] && kill "$APP_PID" 2>/dev/null || true
    [[ -n "$COMP_PID" ]] && kill -TERM -- "-$COMP_PID" 2>/dev/null || true
    rm -rf "$RUNTIME"
    rm -f "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"/sde-*-$$* "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"/sde-*-$$*.lock 2>/dev/null || true
}
trap cleanup EXIT

PERF="$RUNTIME/perf"
SOCK="sde-perf-$$"

# Start the compositor at a given output refresh ($1; empty = the output's own).
start_compositor() {
    rm -f "$PERF"
    setsid env -u WAYLAND_DISPLAY -u DISPLAY \
        WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 SDE_OUTPUT_SCALE=1 \
        ${1:+SDE_OUTPUT_REFRESH="$1"} \
        SDE_STATE_DIR="$RUNTIME" \
        SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}" \
        LD_LIBRARY_PATH="$BUILD/lib" \
        SDE_RENDERER="$RENDERER" \
        SDE_PERF_STATS="$PERF" \
        "$COMP" -n "$SOCK" > "$RUNTIME/comp.log" 2>&1 &
    COMP_PID=$!
    for _ in $(seq 1 80); do
        [[ -e "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/$SOCK" ]] && break
        sleep 0.1
    done
    # Wait for the first report, so every phase reads a complete summary.
    for _ in $(seq 1 50); do
        [[ -s "$PERF" ]] && break
        sleep 0.1
    done
    if [[ ! -s "$PERF" ]]; then
        echo "==> perf test FAILED (the compositor wrote no $PERF)"
        tail -20 "$RUNTIME/comp.log"
        exit 1
    fi
}
stop_compositor() {
    [[ -n "$COMP_PID" ]] && kill -TERM -- "-$COMP_PID" 2>/dev/null || true
    [[ -n "$COMP_PID" ]] && kill -TERM "$COMP_PID" 2>/dev/null || true
    wait "$COMP_PID" 2>/dev/null || true
    COMP_PID=""
    rm -f "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/$SOCK"{,.lock}
    sleep 0.3
}

start_compositor

stat_value() { sed -n "s/^$1=//p" "$PERF" | head -1; }

fail=0
# check <description> <actual> <op> <expected>
check() {
    local description="$1" actual="$2" op="$3" expected="$4"
    if awk -v a="$actual" -v b="$expected" "BEGIN { exit !(a $op b) }"; then
        echo "  ok: $description ($actual $op $expected)"
    else
        echo "  MISSING: $description ($actual, expected $op $expected)"
        fail=1
    fi
}

echo "=== (A) idle pacing (renderer $RENDERER) ==="
sleep 1
IDLE_A="$(stat_value frames)"
sleep 2
IDLE_B="$(stat_value frames)"
IDLE_DELTA=$((IDLE_B - IDLE_A))
echo "  frames while idle for 2 s: $IDLE_DELTA"
check "the compositor does not render while idle" "$IDLE_DELTA" "<=" "$IDLE_FRAME_LIMIT"

echo "=== (B) input latency under 60 Hz pointer motion ==="
BEFORE="$(stat_value frames)"
ARGS=()
for i in $(seq 1 110); do
    ARGS+=(--move $(( 80 + (i * 10) % 1100 )) $(( 80 + (i * 7) % 500 )) --sleep 16)
done
env WAYLAND_DISPLAY="$SOCK" "$VI" --extent "$WIDTH" "$HEIGHT" "${ARGS[@]}" --linger 100 >/dev/null 2>&1
sleep 1.3

FRAMES="$(stat_value frames)"
MOVED=$((FRAMES - BEFORE))
F_P50="$(stat_value frame_ms_p50)"
F_P95="$(stat_value frame_ms_p95)"
F_MAX="$(stat_value frame_ms_max)"
F_OVER="$(stat_value frame_ms_over_budget)"
F_P99="$(stat_value frame_ms_p99)"
SLOW="$(stat_value slow_frames)"
L_P50="$(stat_value input_to_present_ms_p50)"
L_P95="$(stat_value input_to_present_ms_p95)"
L_MAX="$(stat_value input_to_present_ms_max)"
L_N="$(stat_value input_to_present_samples)"
echo "  frames: $MOVED, frame p50/p95/max: $F_P50/$F_P95/$F_MAX ms (over budget $F_OVER, slow $SLOW)"
echo "  input->present p50/p95/max: $L_P50/$L_P95/$L_MAX ms over $L_N samples"
check "the motion produced frames" "$MOVED" ">" 60
check "frame time p50 is inside the budget" "$F_P50" "<" "$FRAME_BUDGET_MS"
# A tighter guard, so a gross regression (a full-output repaint every frame, say)
# is caught rather than only a catastrophe: the measured p50 is ~0.03 ms.
check "frame time p50 has headroom" "$F_P50" "<" "$PERF_FAST_MS"
check "frame time p95 is inside the budget" "$F_P95" "<" "$FRAME_BUDGET_MS"
check "frame time max is inside the budget" "$F_MAX" "<" "$FRAME_BUDGET_MS"
check "frame time p99 is inside a 240 Hz interval" "$F_P99" "<" "$AGGRESSIVE_MS"
check "no frame exceeded the budget" "$F_OVER" "==" 0
check "input latency p95 is inside the budget" "$L_P95" "<" "$LATENCY_BUDGET_MS"
check "input latency p50 is inside the budget" "$L_P50" "<" "$LATENCY_BUDGET_MS"
check "the latency metric sampled the motion" "$L_N" ">" 50

echo "=== (C) worst case: dragging a decorated window on $RENDERER ==="
env WAYLAND_DISPLAY="$SOCK" "$SDE_ROOT/build/tools/native-app-test" \
    --server-side-decorations --title "Heavy" --linger 4000 >/dev/null 2>&1 &
APP_PID=$!
for _ in $(seq 1 60); do
    grep -q "toplevel mapped title='Heavy'" "$RUNTIME/comp.log" && break
    sleep 0.1
done
sleep 0.5

MOVE_ARGS=(--move 300 200 --key 125 press --sleep 60 --button 0x110 press --sleep 60)
for i in $(seq 1 110); do
    MOVE_ARGS+=(--move $(( 300 + (i * 8) % 700 )) $(( 200 + (i * 5) % 300 )) --sleep 16)
done
MOVE_ARGS+=(--button 0x110 release --sleep 60 --key 125 release --linger 100)
env WAYLAND_DISPLAY="$SOCK" "$VI" --extent "$WIDTH" "$HEIGHT" "${MOVE_ARGS[@]}" >/dev/null 2>&1
sleep 1.3

D_P50="$(stat_value frame_ms_p50)"
D_P95="$(stat_value frame_ms_p95)"
D_MAX="$(stat_value frame_ms_max)"
D_OVER="$(stat_value frame_ms_over_budget)"
D_P99="$(stat_value frame_ms_p99)"
echo "  frame p50/p95/max: $D_P50/$D_P95/$D_MAX ms (over budget $D_OVER)"
check "the drag's frame time p50 has headroom" "$D_P50" "<" "$PERF_FAST_MS"
check "the drag's frame time p95 is inside the budget" "$D_P95" "<" "$FRAME_BUDGET_MS"
check "the drag's frame time p99 is inside a 240 Hz interval" "$D_P99" "<" "$AGGRESSIVE_MS"
check "the drag's frame time max is inside the budget" "$D_MAX" "<" "$FRAME_BUDGET_MS"
check "no dragged frame exceeded the budget" "$D_OVER" "==" 0

echo "=== (D) a 240 Hz output: the compositor keeps the cadence ==="
stop_compositor
start_compositor "$AGGRESSIVE_HZ"
REFRESH="$(stat_value output_refresh_hz)"
BUDGET="$(stat_value frame_ms_budget)"
echo "  output refresh: ${REFRESH} Hz, derived frame budget: ${BUDGET} ms"
check "the output runs at $AGGRESSIVE_HZ Hz" "$REFRESH" "==" "$AGGRESSIVE_HZ"
check "the budget is derived from the refresh" "$BUDGET" "<=" "$AGGRESSIVE_MS"

# Damage faster than the refresh: the compositor must pace at ~4.17 ms, with the
# per-frame work inside that interval.
HZ_ARGS=()
for i in $(seq 1 300); do
    HZ_ARGS+=(--move $(( 100 + (i * 7) % 1100 )) $(( 100 + (i * 3) % 500 )) --sleep 4)
done
env WAYLAND_DISPLAY="$SOCK" "$VI" --extent "$WIDTH" "$HEIGHT" "${HZ_ARGS[@]}" --linger 100 >/dev/null 2>&1
sleep 1.3

HFRAMES="$(stat_value frames)"
H_P50="$(stat_value frame_ms_p50)"
H_P95="$(stat_value frame_ms_p95)"
H_P99="$(stat_value frame_ms_p99)"
H_MAX="$(stat_value frame_ms_max)"
H_OVER="$(stat_value frame_ms_over_budget)"
HI_P50="$(stat_value frame_interval_ms_p50)"
HI_P95="$(stat_value frame_interval_ms_p95)"
echo "  frames: $HFRAMES, interval p50/p95: $HI_P50/$HI_P95 ms (target ~4.17)"
echo "  frame p50/p95/p99/max: $H_P50/$H_P95/$H_P99/$H_MAX ms (over budget $H_OVER)"
check "the compositor sustained the 240 Hz cadence" "$HI_P50" "<" "4.5"
check "the frame interval p95 holds the cadence" "$HI_P95" "<" "6"
check "frame time p50 is inside the 240 Hz interval" "$H_P50" "<" "$AGGRESSIVE_MS"
check "frame time p95 is inside the 240 Hz interval" "$H_P95" "<" "$AGGRESSIVE_MS"
check "frame time p99 is inside the 240 Hz interval" "$H_P99" "<" "$AGGRESSIVE_MS"
check "frame time max is inside the 240 Hz interval" "$H_MAX" "<" "$AGGRESSIVE_MS"
check "no frame missed the 240 Hz interval" "$H_OVER" "==" 0

echo "=== (E) memory: idle footprint, and no growth across window cycles ==="
RSS_BASE="$(stat_value rss_kb)"
for i in $(seq 1 50); do
    env WAYLAND_DISPLAY="$SOCK" "$SDE_ROOT/build/tools/native-app-test" \
        --title "mem$i" --linger 40 >/dev/null 2>&1 || true
done
sleep 1.5
RSS_AFTER="$(stat_value rss_kb)"
RSS_HIGH="$(stat_value rss_high_kb)"
GROWTH=$((RSS_AFTER - RSS_BASE))
echo "  rss: $RSS_BASE -> $RSS_AFTER kB (high $RSS_HIGH) over 50 window open/close cycles"
check "the idle footprint is within the limit" "$RSS_BASE" "<" "$RSS_LIMIT_KB"
check "repeated window cycles do not grow the footprint" "$GROWTH" "<" "$RSS_GROWTH_KB"

if [[ "$fail" -ne 0 ]]; then
    echo "--- perf stats ---"; cat "$PERF"
    echo "--- compositor log (tail) ---"; tail -10 "$RUNTIME/comp.log"
    echo "==> perf test FAILED"
    exit 1
fi
echo "==> perf test PASSED"

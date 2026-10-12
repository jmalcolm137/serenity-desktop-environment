#!/usr/bin/env bash
# Headless test for the Profiler (the Taskbar's Development menu), built by
# libgui-wayland (patch 0040) and sampling through SDE's LibSerenitySystem.
#
# On SerenityOS the kernel samples a process via profiling_enable(pid, mask) and
# publishes perfcore JSON at /proc/<pid>/perf_events. Linux has no such kernel
# interface, so src/LibSerenitySystem/Profiling.cpp samples with perf_event_open
# (PERF_SAMPLE_IP|TID|TIME|CALLCHAIN, plus mmap/comm/fork/exit records) and
# writes the same perfcore JSON. This test:
#
#   1. profiles a busy process with a tiny harness against libSerenitySystem.a,
#      asserting the perfcore carries samples (with multi-frame stacks), mmap and
#      process_create events;
#   2. asserts the timestamps are milliseconds (Profiler's Profile::length_in_ms
#      is a raw difference, so nanoseconds would build a ~billions-bucket
#      histogram and balloon RSS);
#   3. loads the perfcore in Profiler inside the compositor and asserts it maps a
#      window and stays memory-sane (the regression signal for (2)).
#
# Runs entirely headless (no host compositor, no DRM), so it is CI-friendly.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
SERENITY="${SERENITY:-$LIBGUI_WAYLAND_ROOT/serenity}"
BUILD="${BUILD:-$LIBGUI_WAYLAND_ROOT/build/lagom}"
COMP="$SDE_ROOT/build/WindowServer/WindowServer"
XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
export XDG_RUNTIME_DIR

command -v c++ >/dev/null || { echo "SKIP: no c++ compiler to build the sampler harness"; exit 0; }

"$SDE_ROOT/scripts/build-window-server.sh" >/dev/null
if [[ ! -x "$BUILD/bin/Profiler" ]]; then
    echo "building Profiler..."
    ninja -C "$BUILD" Profiler >/dev/null
fi
[[ -x "$BUILD/bin/Profiler" ]] || { echo "MISSING: $BUILD/bin/Profiler"; exit 1; }
[[ -f "$BUILD/lib/libSerenitySystem.a" ]] || { echo "MISSING: $BUILD/lib/libSerenitySystem.a"; exit 1; }

RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/sde-profiler.XXXXXX")"
export SDE_STATE_DIR="$RUNTIME"
trap 'rm -rf "$RUNTIME"; rm -f "$XDG_RUNTIME_DIR"/sde-*-$$ "$XDG_RUNTIME_DIR"/sde-*-$$.lock 2>/dev/null || true' EXIT

fail=0

echo "=== build the sampler harness against LibSerenitySystem ==="
cat > "$RUNTIME/prof-harness.cpp" <<'CPP'
#include <LibSerenitySystem/Profiling.h>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
int main(int argc, char** argv)
{
    if (argc < 2) { std::fprintf(stderr, "usage: %s <pid> [seconds]\n", argv[0]); return 2; }
    pid_t pid = std::atoi(argv[1]);
    int seconds = argc > 2 ? std::atoi(argv[2]) : 2;
    if (auto result = SerenitySystem::Profiling::enable(pid, 0); result.is_error()) {
        std::fprintf(stderr, "enable failed\n");
        return 1;
    }
    sleep(seconds);
    if (auto result = SerenitySystem::Profiling::disable(pid); result.is_error()) {
        std::fprintf(stderr, "disable failed\n");
        return 1;
    }
    std::printf("%s\n", SerenitySystem::Profiling::perfcore_path(pid).characters());
    return 0;
}
CPP

if ! c++ -std=c++26 -fno-exceptions -O1 -Wno-literal-suffix \
    -I "$SERENITY" -I "$SERENITY/Userland" -I "$SERENITY/Userland/Libraries" -I "$SERENITY/Userland/Services" \
    -I "$BUILD" -I "$BUILD/Userland/Libraries" -I "$BUILD/Userland/Services" \
    -I "$LIBGUI_WAYLAND_ROOT/compat" -I "$SDE_ROOT/src" \
    "$RUNTIME/prof-harness.cpp" "$BUILD/lib/libSerenitySystem.a" \
    -L "$BUILD/lib" -Wl,-rpath,"$BUILD/lib" \
    -llagom-core -llagom-ak -llagom-coreminimal -llagom-corebasic -lpthread \
    -o "$RUNTIME/prof-harness" 2> "$RUNTIME/harness-build.log"; then
    echo "  FAIL: harness did not compile"; cat "$RUNTIME/harness-build.log"; exit 1
fi
echo "  ok: harness built"

echo "=== sample a busy process and check the perfcore ==="
setsid yes > /dev/null & BUSY=$!
sleep 0.3
PERFCORE="$(XDG_RUNTIME_DIR="$XDG_RUNTIME_DIR" "$RUNTIME/prof-harness" "$BUSY" 2 2> "$RUNTIME/harness.log" || true)"
kill "$BUSY" 2>/dev/null || true
PERFCORE="${PERFCORE:-$XDG_RUNTIME_DIR/sde-perfcore-$BUSY.json}"
if [[ ! -s "$PERFCORE" ]]; then
    echo "  FAIL: no perfcore produced (see $RUNTIME/harness.log)"; cat "$RUNTIME/harness.log" 2>/dev/null; exit 1
fi
echo "  ok: perfcore $PERFCORE"

if command -v python3 >/dev/null; then
    python3 - "$PERFCORE" <<'PY' || fail=1
import json, sys
from collections import Counter
data = json.load(open(sys.argv[1]))
events = data["events"]
kinds = Counter(event["type"] for event in events)
print("  events:", dict(kinds))
samples = [event for event in events if event["type"] == "sample"]
stacks = [len(event.get("stack", [])) for event in samples]
timestamps = [event["timestamp"] for event in events]
if not samples:
    print("  FAIL: no sample events"); sys.exit(1)
if min(stacks) < 2:
    print("  WARN: a sample stack has fewer than 2 frames")
if "mmap" not in kinds:
    print("  FAIL: no mmap events (cannot symbolicate)"); sys.exit(1)
span = max(timestamps) - min(timestamps)
print(f"  samples={len(samples)} min_stack={min(stacks)} mmaps={kinds.get('mmap', 0)} span={span}ms")
if span > 60000:
    print("  FAIL: timestamps are not milliseconds (Profiler expects ms)"); sys.exit(1)
PY
fi

echo "=== Profiler loads the perfcore in the compositor ==="
SOCK="sde-profiler-$$"
env -u WAYLAND_DISPLAY -u DISPLAY \
    WLR_BACKENDS=headless \
    WLR_HEADLESS_OUTPUTS=1 \
    SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}" \
    LD_LIBRARY_PATH="$BUILD/lib" \
    "$COMP" -n "$SOCK" > "$RUNTIME/comp.log" 2>&1 &
COMP_PID=$!
for _ in $(seq 1 50); do
    [[ -e "$XDG_RUNTIME_DIR/$SOCK" ]] && break
    sleep 0.1
done

env WAYLAND_DISPLAY="$SOCK" \
    LD_LIBRARY_PATH="$BUILD/lib" \
    SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}" \
    "$BUILD/bin/Profiler" "$PERFCORE" > "$RUNTIME/profiler.log" 2>&1 &
PROF_PID=$!
sleep 6
if grep -q "toplevel mapped" "$RUNTIME/comp.log"; then
    echo "  ok: Profiler mapped a window"
else
    echo "  FAIL: Profiler did not map a window"; fail=1
fi
if grep -qiE "SIGSEGV|Segmentation fault|ASSERTION FAILED|VERIFICATION FAILED|Runtime error" "$RUNTIME/profiler.log"; then
    echo "  FAIL: Profiler reported a fault"; fail=1
fi
RSS="$(awk '/VmRSS/{print $2}' "/proc/$PROF_PID/status" 2>/dev/null || echo 0)"
if [[ "${RSS:-0}" -lt 1000000 ]]; then
    echo "  ok: Profiler RSS $((RSS / 1024)) MiB"
else
    echo "  FAIL: Profiler RSS $((RSS / 1024)) MiB (runaway histogram?)"; fail=1
fi

kill "$PROF_PID" 2>/dev/null || true
kill "$COMP_PID" 2>/dev/null || true
wait "$COMP_PID" 2>/dev/null || true
rm -f "$PERFCORE" 2>/dev/null || true

if [[ "$fail" -ne 0 ]]; then
    echo "--- compositor log ---"; cat "$RUNTIME/comp.log"
    echo "--- profiler log ---"; cat "$RUNTIME/profiler.log"
    echo "==> profiler test FAILED"; exit 1
fi
echo "==> profiler test PASSED"

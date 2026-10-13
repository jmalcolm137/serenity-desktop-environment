#!/usr/bin/env bash
# Headless test for the SerenityOS CrashReporter on the host (R4.9).
#
# CrashReporter is a GUI that reads a core dump and shows the crash backtrace.
# LibCoredump is SerenityOS-specific (it parses the SerenityOS note section), so
# libgui-wayland adds a Linux core backend (patch 0053): it reads the standard
# kernel notes (NT_PRSTATUS/NT_PRPSINFO/NT_FILE/NT_SIGINFO/NT_AUXV) and the
# PT_LOAD segments of a native ELF core dump.
#
# How a crash reaches CrashReporter is a separate, currently-unimplemented piece
# (see docs/04-known-gaps.md, R4.9); this test hands it a core file directly, the
# same way `CrashReporter <core>` would be invoked.
#
# Producing a core file needs no privilege when `coredumpctl` (systemd) or `gdb`
# is present; otherwise the test SKIPs.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
SERENITY="${SERENITY:-$LIBGUI_WAYLAND_ROOT/serenity}"
BUILD="${BUILD:-$LIBGUI_WAYLAND_ROOT/build/lagom}"
COMP="$SDE_ROOT/build/WindowServer/WindowServer"
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"

command -v cc >/dev/null || { echo "SKIP: no C compiler"; exit 0; }

if [[ ! -x "$BUILD/bin/CrashReporter" ]]; then
    echo "building CrashReporter..."
    ninja -C "$BUILD" CrashReporter >/dev/null 2>&1 || true
fi
[[ -x "$BUILD/bin/CrashReporter" ]] || { echo "FAIL: $BUILD/bin/CrashReporter is missing"; exit 1; }

"$SDE_ROOT/scripts/build-window-server.sh" >/dev/null

RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/sde-crashreporter.XXXXXX")"
chmod 700 "$RUNTIME"
export SDE_STATE_DIR="$RUNTIME"
SOCK="sde-crashreporter-$$"
trap 'rm -rf "$RUNTIME"; rm -f "$XDG_RUNTIME_DIR"/$SOCK* 2>/dev/null || true' EXIT

cat > "$RUNTIME/crash.c" <<'C'
#include <stdio.h>
static void crash_here(int x) { volatile int* p = (int*)(long)x; *p = 42; }
int main(void) { printf("about to crash\n"); fflush(stdout); crash_here(0); return 0; }
C
cc -g -O0 "$RUNTIME/crash.c" -o "$RUNTIME/crash"

CORE="$RUNTIME/crash.core"
echo "=== obtain a core dump (needs coredumpctl or gdb) ==="
if command -v coredumpctl >/dev/null 2>&1; then
    ( cd "$RUNTIME" && "./crash" >/dev/null 2>&1 || true )
    sleep 1
    coredumpctl dump --no-pager -o "$CORE" >/dev/null 2>&1 || true
fi
if [[ ! -s "$CORE" ]] && command -v gdb >/dev/null 2>&1; then
    gdb -batch -nx -ex "run" -ex "gcore $CORE" -ex "quit" "$RUNTIME/crash" >/dev/null 2>&1 || true
fi
if [[ ! -s "$CORE" ]]; then
    echo "SKIP: could not produce a core dump (no coredumpctl result, no gdb)"
    exit 0
fi
echo "  ok: core written ($(stat -c%s "$CORE") bytes)"

echo "=== CrashReporter reads the core and maps a window ==="
env -u WAYLAND_DISPLAY -u DISPLAY \
    WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 \
    SDE_OUTPUT_SCALE=1 \
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
    timeout 8 "$BUILD/bin/CrashReporter" "$CORE" > "$RUNTIME/cr.log" 2>&1 || true

mapped="$(grep -c "toplevel mapped" "$RUNTIME/comp.log" || true)"
kill "$COMP_PID" 2>/dev/null || true
wait "$COMP_PID" 2>/dev/null || true

fail=0
if [[ "$mapped" -gt 0 ]]; then
    echo "  ok: CrashReporter mapped a window"
else
    echo "  FAIL: CrashReporter did not map a window"
    fail=1
fi
if grep -qiE "VERIFICATION FAILED|SIGSEGV|Segmentation fault|Illegal instruction" "$RUNTIME/cr.log"; then
    echo "  FAIL: CrashReporter reported a fault"
    fail=1
fi
if grep -qiE "ELF (Header|Program Headers) not valid" "$RUNTIME/cr.log"; then
    echo "  FAIL: LibCoredump rejected the core"
    fail=1
fi

if [[ "$fail" -ne 0 ]]; then
    echo "--- CrashReporter log ---"
    cat "$RUNTIME/cr.log"
    echo "==> crash-reporter test FAILED"
    exit 1
fi
echo "==> crash-reporter test PASSED"

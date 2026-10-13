#!/usr/bin/env bash
# Headless test for the SerenityOS Debugger on the host (R4.9).
#
# The Debugger is a CLI that drives ptrace through LibDebug. libgui-wayland
# ports LibDebug to native Linux (patches 0036 + 0052): PTRACE_GETREGSET for the
# register set (some kernels reject PTRACE_GETREGS with EFAULT), __WALL for
# waitpid, and Linux single-step semantics. It is built by the port and drives a
# real host process here: insert a breakpoint, run to it, print registers, and
# disassemble, with no SerenityOS kernel shim.
#
# Runs entirely headless (a CLI, no compositor), so it is CI-friendly.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
BUILD="${BUILD:-$LIBGUI_WAYLAND_ROOT/build/lagom}"

command -v cc >/dev/null || { echo "SKIP: no C compiler"; exit 0; }

if [[ ! -x "$BUILD/bin/Debugger" ]]; then
    echo "building Debugger..."
    ninja -C "$BUILD" Debugger >/dev/null 2>&1 || true
fi
[[ -x "$BUILD/bin/Debugger" ]] || { echo "FAIL: $BUILD/bin/Debugger is missing"; exit 1; }

RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/sde-debugger.XXXXXX")"
trap 'rm -rf "$RUNTIME"' EXIT

cat > "$RUNTIME/target.c" <<'C'
#include <stdio.h>
int add(int a, int b) { return a + b; }
int main(void) { int x = add(2, 3); printf("sum=%d\n", x); return 0; }
C
cc -g -O0 "$RUNTIME/target.c" -o "$RUNTIME/target"

echo "=== the Debugger runs a scripted session against a host process ==="
set +e
printf 'bp add\ncont\nregs\nbt\ndis 2\ncont\nquit\n' \
    | timeout 30 stdbuf -o0 -e0 env LD_LIBRARY_PATH="$BUILD/lib" \
        "$BUILD/bin/Debugger" "$RUNTIME/target" > "$RUNTIME/session.log" 2>&1
set -e
# Strip LibLine's terminal escapes so the log is plain text.
sed -i 's/\x1b\[[0-9;?]*[a-zA-Z]//g' "$RUNTIME/session.log"

fail=0
check() {
    local what="$1" pattern="$2"
    if grep -qE "$pattern" "$RUNTIME/session.log"; then
        echo "  ok: $what"
    else
        echo "  MISSING: $what (pattern '$pattern')"
        fail=1
    fi
}

check "a breakpoint is inserted"            "Breakpoint inserted"
check "the breakpoint is hit at the symbol" "add \+0x0"
check "the breakpoint resolves to source"   "target\.c:2"
check "registers are read (arg a in rdi)"   "rdi=0x0000000000000002"
check "registers are read (arg b in rsi)"   "rsi=0x0000000000000003"
check "disassembly is produced"             "push\s+rbp"
check "the debuggee runs to completion"     "Program exited|sum=5"

if grep -qE "VERIFICATION FAILED|SIGSEGV|Segmentation fault" "$RUNTIME/session.log"; then
    echo "  FAIL: the debugger reported a fault"
    fail=1
fi

if [[ "$fail" -ne 0 ]]; then
    echo "--- session log ---"
    cat "$RUNTIME/session.log"
    echo "==> debugger test FAILED"
    exit 1
fi
echo "==> debugger test PASSED"

#!/usr/bin/env bash
# Terminal signal-disposition test.
#
# The session starts its applications as background jobs of one non-interactive
# shell (`sh -c "App1 & App2 & ..."`), and POSIX requires such a shell to set
# SIGINT and SIGQUIT to SIG_IGN in each background job. The ignore is inherited by
# everything the application runs and cannot be undone by a shell, so an
# application launched that way had Ctrl-C silently discarded — and so did a shell
# started by our Terminal, and every program it ran. That is the reported "Ctrl-C
# does nothing" symptom, and it was never a terminal bug.
#
# This asserts the two halves of the fix from the terminal's side:
#   * the Terminal resets SIGINT/SIGQUIT for the shell it starts, so a new
#     Terminal works even when the running session still holds the old launcher;
#   * therefore a program in that shell sees the default disposition.
#
# The Terminal is deliberately started as a background job here, exactly as the
# session does it, so the test fails without the reset.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
. "$SDE_ROOT/scripts/lib/test-env.sh"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
SERENITY="${SERENITY:-$LIBGUI_WAYLAND_ROOT/serenity}"
BUILD="${BUILD:-$LIBGUI_WAYLAND_ROOT/build/lagom}"
COMP="$SDE_ROOT/build/WindowServer/WindowServer"
TERMINAL="$BUILD/bin/Terminal"

"$SDE_ROOT/scripts/build-window-server.sh" >/dev/null
"$SDE_ROOT/scripts/build.sh" Terminal >/dev/null

RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/sde-tsignals.XXXXXX")"
chmod 700 "$RUNTIME"
mkdir -p "$RUNTIME/run"
SOCK="sde-tsignals-$$"
COMP_PID=""
TERM_PID=""
cleanup() {
    [[ -n "$TERM_PID" ]] && kill "$TERM_PID" 2>/dev/null || true
    [[ -n "$COMP_PID" ]] && kill "$COMP_PID" 2>/dev/null || true
    for _ in $(seq 1 60); do kill -0 "${COMP_PID:-${CP:-}}" 2>/dev/null || break; sleep 0.05; done; rm -rf "$RUNTIME"
}
trap cleanup EXIT

# A probe reporting its own dispositions. The Terminal only forwards a fixed set
# of environment variables to its child, so bake the path in.
RESULT="$RUNTIME/result.txt"
cat > "$RUNTIME/probe.c" <<EOF
#include <signal.h>
#include <stdio.h>
static char const* describe(int signal)
{
    struct sigaction action;
    if (sigaction(signal, NULL, &action) != 0)
        return "sigaction failed";
    if (action.sa_handler == SIG_IGN)
        return "SIG_IGN";
    if (action.sa_handler == SIG_DFL)
        return "SIG_DFL";
    return "handler";
}
int main(void)
{
    FILE* out = fopen("$RESULT", "a");
    if (!out)
        return 1;
    fprintf(out, "SIGINT=%s SIGQUIT=%s\n", describe(SIGINT), describe(SIGQUIT));
    fclose(out);
    return 0;
}
EOF
cc -O1 "$RUNTIME/probe.c" -o "$RUNTIME/probe"

env -u WAYLAND_DISPLAY -u DISPLAY \
    WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 \
    XDG_RUNTIME_DIR="$RUNTIME/run" \
    SDE_STATE_DIR="$RUNTIME/state" SDE_SESSION_DIR="$RUNTIME/session" \
    SDE_XWAYLAND=0 \
    SERENITY_RES_ROOT="$SERENITY/Base/res" \
    LD_LIBRARY_PATH="$BUILD/lib" \
    "$COMP" -n "$SOCK" > "$RUNTIME/comp.log" 2>&1 &
COMP_PID=$!
for _ in $(seq 1 80); do [[ -e "$RUNTIME/run/$SOCK" ]] && break; sleep 0.1; done
sleep 0.4

# Backgrounded on purpose: this is what gives the Terminal SIG_IGN, and what the
# reset in the child must undo.
env WAYLAND_DISPLAY="$SOCK" XDG_RUNTIME_DIR="$RUNTIME/run" \
    SERENITY_RES_ROOT="$SERENITY/Base/res" LD_LIBRARY_PATH="$BUILD/lib" \
    "$TERMINAL" -e "$RUNTIME/probe" > "$RUNTIME/terminal.log" 2>&1 &
TERM_PID=$!
sleep 3

kill "$TERM_PID" 2>/dev/null || true
wait "$TERM_PID" 2>/dev/null || true
TERM_PID=""

failures=0
check() {
    if eval "$2"; then
        printf '%-52s PASS\n' "$1"
    else
        printf '%-52s FAIL\n' "$1"
        failures=$((failures + 1))
    fi
}

echo "=== dispositions reported by the program in the Terminal ==="
sed 's/^/  /' "$RESULT" 2>/dev/null || echo "  <none>"
check "the shell got the default SIGINT"  "grep -q 'SIGINT=SIG_DFL' '$RESULT' 2>/dev/null"
check "the shell got the default SIGQUIT" "grep -q 'SIGQUIT=SIG_DFL' '$RESULT' 2>/dev/null"

if [[ $failures -ne 0 ]]; then
    echo "==> terminal signals test FAILED ($failures)"
    echo "--- Terminal log ---"; tail -20 "$RUNTIME/terminal.log" 2>/dev/null || true
    exit 1
fi
echo "==> terminal signals test PASSED"

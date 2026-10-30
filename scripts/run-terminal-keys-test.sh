#!/usr/bin/env bash
# Terminal key-encoding test: the bytes the Terminal sends for the keys whose
# convention differs from the host, plus the one case that must *not* arrive as a
# byte. See docs/27-terminal-keys.md.
#
# Runs headless: starts the SDE compositor, runs Terminal with a tiny child as its
# command, injects key events through the wlroots virtual-keyboard protocol, and
# reads back exactly what the child received.
#
#   * raw child   — asserts the byte encoding:
#         a -> 0x61, Ctrl-C -> 0x03, Backspace -> 0x7f,
#         Alt-a -> ESC 'a' (LibVT prefixes on the flag alone, so this also proves
#         the shim's derived Alt/Ctrl flags reach the client), Ctrl-/ -> 0x1f.
#   * cooked child — asserts Ctrl-C produces SIGINT rather than a readable byte,
#         i.e. it still works as an interrupt for a normal shell.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
. "$SDE_ROOT/scripts/lib/test-env.sh"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
SERENITY="${SERENITY:-$LIBGUI_WAYLAND_ROOT/serenity}"
BUILD="${BUILD:-$LIBGUI_WAYLAND_ROOT/build/lagom}"
WLROOTS_SRC="${WLROOTS_SRC:-$SDE_ROOT/third_party/wlroots}"
COMP="$SDE_ROOT/build/WindowServer/WindowServer"
TERMINAL="$BUILD/bin/Terminal"
TOOLS_BUILD="$SDE_ROOT/build/terminal-keys-test"

"$SDE_ROOT/scripts/build-window-server.sh" >/dev/null
"$SDE_ROOT/scripts/build.sh" Terminal >/dev/null

mkdir -p "$TOOLS_BUILD"
VP_XML="$WLROOTS_SRC/protocol/wlr-virtual-pointer-unstable-v1.xml"
VK_XML="$WLROOTS_SRC/protocol/virtual-keyboard-unstable-v1.xml"
wayland-scanner client-header "$VP_XML" "$TOOLS_BUILD/wlr-virtual-pointer-unstable-v1-client-protocol.h"
wayland-scanner private-code  "$VP_XML" "$TOOLS_BUILD/wlr-virtual-pointer-unstable-v1-protocol.c"
wayland-scanner client-header "$VK_XML" "$TOOLS_BUILD/virtual-keyboard-unstable-v1-client-protocol.h"
wayland-scanner private-code  "$VK_XML" "$TOOLS_BUILD/virtual-keyboard-unstable-v1-protocol.c"
cc -O2 -I "$TOOLS_BUILD" \
    "$SDE_ROOT/Tools/virtual-input-test.c" \
    "$TOOLS_BUILD/virtual-keyboard-unstable-v1-protocol.c" \
    "$TOOLS_BUILD/wlr-virtual-pointer-unstable-v1-protocol.c" \
    $(pkg-config --cflags --libs wayland-client xkbcommon) \
    -o "$TOOLS_BUILD/virtual-input-test"
VIT="$TOOLS_BUILD/virtual-input-test"

RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/sde-tkeys.XXXXXX")"
chmod 700 "$RUNTIME"
mkdir -p "$RUNTIME/run"
SOCK="sde-tkeys-$$"

COMP_PID=""
TERM_PID=""
cleanup() {
    [[ -n "$TERM_PID" ]] && kill "$TERM_PID" 2>/dev/null || true
    [[ -n "$COMP_PID" ]] && kill "$COMP_PID" 2>/dev/null || true
    for _ in $(seq 1 60); do kill -0 "${COMP_PID:-${CP:-}}" 2>/dev/null || break; sleep 0.05; done; rm -rf "$RUNTIME"
}
trap cleanup EXIT

# The raw child: no line discipline, so every byte reaches `cat` verbatim.
cat > "$RUNTIME/raw-child.sh" <<EOF
#!/bin/sh
stty raw -echo
exec cat > "$RUNTIME/raw.bin"
EOF
chmod +x "$RUNTIME/raw-child.sh"

# The cooked child: keep the default termios, so ^C (intr) is consumed by the line
# discipline. It must reset SIGINT explicitly: a process started as a background
# job inherits it ignored, and an ignored SIGINT would make this test vacuous.
cat > "$RUNTIME/cooked-child.c" <<'EOF'
#include <signal.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
static volatile sig_atomic_t got = 0;
static void handler(int signal) { got = signal; }
int main(int argc, char** argv)
{
    (void)argc;
    signal(SIGINT, handler);
    int out = open(argv[1], O_WRONLY | O_CREAT | O_APPEND, 0644);
    dprintf(out, "ready\n");
    for (;;) {
        pause();
        if (got) {
            dprintf(out, "GOT signal %d\n", got);
            got = 0;
        }
    }
}
EOF
cc -O1 "$RUNTIME/cooked-child.c" -o "$RUNTIME/cooked-child"
cat > "$RUNTIME/cooked-child.sh" <<EOF
#!/bin/sh
exec "$RUNTIME/cooked-child" "$RUNTIME/sig.txt"
EOF
chmod +x "$RUNTIME/cooked-child.sh"

start_compositor() {
    env -u WAYLAND_DISPLAY -u DISPLAY \
        WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 \
        XDG_RUNTIME_DIR="$RUNTIME/run" \
        SDE_STATE_DIR="$RUNTIME/state" \
        SDE_SESSION_DIR="$RUNTIME/session" \
        SDE_XWAYLAND=0 \
        SERENITY_RES_ROOT="$SERENITY/Base/res" \
        LD_LIBRARY_PATH="$BUILD/lib" \
        "$COMP" -n "$SOCK" > "$RUNTIME/comp.log" 2>&1 &
    COMP_PID=$!
    for _ in $(seq 1 80); do [[ -e "$RUNTIME/run/$SOCK" ]] && break; sleep 0.1; done
    sleep 0.4
}

start_terminal() {
    env WAYLAND_DISPLAY="$SOCK" XDG_RUNTIME_DIR="$RUNTIME/run" \
        SDE_STATE_DIR="$RUNTIME/state" SDE_SESSION_DIR="$RUNTIME/session" \
        SDE_XWAYLAND=0 \
        SERENITY_RES_ROOT="$SERENITY/Base/res" LD_LIBRARY_PATH="$BUILD/lib" \
        SHELL="$1" "$TERMINAL" -e "$1" > "$2" 2>&1 &
    TERM_PID=$!
    sleep 3
    if ! kill -0 "$TERM_PID" 2>/dev/null; then
        echo "FAIL: Terminal exited early: $(head -1 "$2")"
        exit 1
    fi
}

# A click focuses the new window (the compositor only routes keys to the focused
# surface), then the events under test.
inject() {
    env WAYLAND_DISPLAY="$SOCK" XDG_RUNTIME_DIR="$RUNTIME/run" \
        "$VIT" --extent 1280 720 \
        --move 300 300 --sleep 200 --button 0x110 press --sleep 100 --button 0x110 release --sleep 400 \
        "$@"
}

start_compositor

# --- raw: the exact bytes ---------------------------------------------------
start_terminal "$RUNTIME/raw-child.sh" "$RUNTIME/raw-terminal.log"
inject \
    --key 30 press --sleep 80 --key 30 release --sleep 250 \
    --key 29 press --sleep 120 --key 46 press --sleep 100 --key 46 release --sleep 120 --key 29 release --sleep 250 \
    --key 14 press --sleep 80 --key 14 release --sleep 250 \
    --key 56 press --sleep 120 --key 30 press --sleep 100 --key 30 release --sleep 120 --key 56 release --sleep 250 \
    --key 29 press --sleep 120 --key 53 press --sleep 100 --key 53 release --sleep 120 --key 29 release --sleep 250 \
    --linger 1000 >/dev/null 2>&1 || true
sleep 0.5
kill "$TERM_PID" 2>/dev/null || true
wait "$TERM_PID" 2>/dev/null || true
TERM_PID=""

RAW_BYTES="$(od -An -tx1 "$RUNTIME/raw.bin" 2>/dev/null | tr -s ' \n' ' ' | sed 's/^ //; s/ $//')"

# --- cooked: ^C must become SIGINT ------------------------------------------
start_terminal "$RUNTIME/cooked-child.sh" "$RUNTIME/cooked-terminal.log"
inject \
    --key 29 press --sleep 120 --key 46 press --sleep 100 --key 46 release --sleep 120 --key 29 release \
    --linger 1000 >/dev/null 2>&1 || true
sleep 0.5
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

echo "=== bytes received by the raw child (expected: 61 03 7f 1b 61 1f) ==="
echo "  ${RAW_BYTES:-<none>}"
check "a, Ctrl-C, Backspace, Alt-a, Ctrl-/ encoded" \
    "[[ '$RAW_BYTES' == '61 03 7f 1b 61 1f' ]]"
check "Terminal stayed up (no read(ptm) error)" \
    "! grep -q 'Input/output error' '$RUNTIME/raw-terminal.log'"

echo "=== cooked child log (expected: GOT signal 2) ==="
sed 's/^/  /' "$RUNTIME/sig.txt" 2>/dev/null || echo "  <none>"
check "Ctrl-C delivered SIGINT in cooked mode" \
    "grep -q 'GOT signal 2' '$RUNTIME/sig.txt' 2>/dev/null"

if [[ $failures -ne 0 ]]; then
    echo "==> terminal keys test FAILED ($failures)"
    echo "--- raw Terminal log ---"; tail -20 "$RUNTIME/raw-terminal.log" 2>/dev/null || true
    echo "--- compositor log ---"; tail -20 "$RUNTIME/comp.log" 2>/dev/null || true
    exit 1
fi
echo "==> terminal keys test PASSED"

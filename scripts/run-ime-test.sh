#!/usr/bin/env bash
# IME test (R5): zwp_text_input_v3 (clients) + zwp_input_method_v2 (input methods).
# Runs a headless compositor with a text-input client and an input-method client
# and asserts the full relay: activation, surrounding text, preedit + commit back
# into the field, the keyboard grab, and the candidate popup. See docs/22-input-method.md.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
SERENITY="${SERENITY:-$LIBGUI_WAYLAND_ROOT/serenity}"
BUILD="${BUILD:-$LIBGUI_WAYLAND_ROOT/build/lagom}"
COMP="$SDE_ROOT/build/WindowServer/WindowServer"
WLROOTS_SRC="${WLROOTS_SRC:-$SDE_ROOT/third_party/wlroots}"
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"

command -v wayland-scanner >/dev/null || { echo "SKIP: no wayland-scanner"; exit 0; }
pkg-config --exists wayland-client wayland-protocols || { echo "SKIP: no wayland-client/wayland-protocols"; exit 0; }

TI_XML="$(pkg-config --variable=pkgdatadir wayland-protocols)/unstable/text-input/text-input-unstable-v3.xml"
IM_XML="$WLROOTS_SRC/protocol/input-method-unstable-v2.xml"
XDG_XML="$(pkg-config --variable=pkgdatadir wayland-protocols)/stable/xdg-shell/xdg-shell.xml"
[[ -f "$TI_XML" && -f "$IM_XML" && -f "$XDG_XML" ]] || { echo "SKIP: protocol XML missing"; exit 0; }

"$SDE_ROOT/scripts/build-window-server.sh" >/dev/null

TOOLS="$SDE_ROOT/build/ime-test"
mkdir -p "$TOOLS"
wayland-scanner client-header "$XDG_XML" "$TOOLS/xdg-shell-client-protocol.h"
wayland-scanner private-code  "$XDG_XML" "$TOOLS/xdg-shell-protocol.c"
wayland-scanner client-header "$TI_XML" "$TOOLS/text-input-unstable-v3-client-protocol.h"
wayland-scanner private-code  "$TI_XML" "$TOOLS/text-input-unstable-v3-protocol.c"
wayland-scanner client-header "$IM_XML" "$TOOLS/input-method-unstable-v2-client-protocol.h"
wayland-scanner private-code  "$IM_XML" "$TOOLS/input-method-unstable-v2-protocol.c"

cc -O2 -I "$TOOLS" "$SDE_ROOT/Tools/text-input-test.c" \
    "$TOOLS/xdg-shell-protocol.c" "$TOOLS/text-input-unstable-v3-protocol.c" \
    $(pkg-config --cflags --libs wayland-client) -o "$TOOLS/text-input-test"
cc -O2 -I "$TOOLS" "$SDE_ROOT/Tools/input-method-test.c" \
    "$TOOLS/input-method-unstable-v2-protocol.c" \
    $(pkg-config --cflags --libs wayland-client) -o "$TOOLS/input-method-test"
cc -O2 "$SDE_ROOT/Tools/wayland-globals-test.c" \
    $(pkg-config --cflags --libs wayland-client) -o "$TOOLS/globals"

# The virtual keyboard, to prove the input method's keyboard grab gets the keys.
VK="$SDE_ROOT/build/input-test/virtual-input-test"
[[ -x "$VK" ]] || "$SDE_ROOT/scripts/build-virtual-input.sh" >/dev/null 2>&1 || true

R="$(mktemp -d "${TMPDIR:-/tmp}/sde-ime.XXXXXX")"
chmod 700 "$R"
SOCK="sde-ime-$$"
env -u WAYLAND_DISPLAY -u DISPLAY \
    WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 SDE_OUTPUT_SCALE=1 \
    SDE_STATE_DIR="$R" \
    SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}" \
    LD_LIBRARY_PATH="$BUILD/lib" \
    "$COMP" -n "$SOCK" > "$R/comp.log" 2>&1 &
CP=$!
cleanup() {
    kill "$CP" 2>/dev/null || true
    wait "$CP" 2>/dev/null || true
    rm -rf "$R"
    rm -f "$XDG_RUNTIME_DIR"/sde-ime-$$* 2>/dev/null || true
}
trap cleanup EXIT

for _ in $(seq 1 80); do [[ -e "$XDG_RUNTIME_DIR/$SOCK" ]] && break; sleep 0.1; done
sleep 0.5

fail=0
check() { # check <file> <pattern> <description>
    local file="$1" pattern="$2" desc="$3"
    if grep -aqE "$pattern" "$file"; then echo "  ok: $desc"; else echo "  FAIL: $desc"; fail=1; fi
}
check_absent() { # check_absent <file> <pattern> <description>
    local file="$1" pattern="$2" desc="$3"
    if grep -aqE "$pattern" "$file"; then echo "  FAIL: $desc"; fail=1; else echo "  ok: $desc"; fi
}

# Both globals must be advertised.
env WAYLAND_DISPLAY="$SOCK" timeout 10 "$TOOLS/globals" > "$R/globals.log" 2>&1 || true
check "$R/globals.log" "zwp_text_input_manager_v3" "zwp_text_input_manager_v3 is advertised"
check "$R/globals.log" "zwp_input_method_manager_v2" "zwp_input_method_manager_v2 is advertised"

# Run an input method and a text-input client.
env WAYLAND_DISPLAY="$SOCK" timeout 15 "$TOOLS/input-method-test" --duration 8000 > "$R/im.log" 2>&1 &
IM_PID=$!
sleep 1
env WAYLAND_DISPLAY="$SOCK" timeout 15 "$TOOLS/text-input-test" --duration 8000 > "$R/ti.log" 2>&1 &
TI_PID=$!
sleep 1.5

# Inject a key while the input method holds the grab: it must reach the input
# method, not the focused client.
if [[ -x "$VK" ]]; then
    env WAYLAND_DISPLAY="$SOCK" "$VK" --key 30 press --key 30 release >/dev/null 2>&1 || true
else
    echo "  note: virtual-input-test unavailable; skipping the keyboard-grab key check"
fi
sleep 3

# Compositor side.
check "$R/comp.log" "IME: input method connected" "the input method connected"
check "$R/comp.log" "IME: text input enabled" "the text input was enabled (activate sent)"
check "$R/comp.log" "IME: input method grabbed the keyboard" "the input method grabbed the keyboard"
check "$R/comp.log" "IME: input method popup surface created" "the candidate popup surface was created"
check "$R/comp.log" "IME: committed \"日本\"" "the compositor relayed the committed string"

# Client side: the field received enter, preedit and the commit.
check "$R/ti.log" "text-input-test: enter" "the field received text-input enter"
check "$R/ti.log" "text-input-test: preedit \"nihon\"" "the field received the preedit string"
check "$R/ti.log" "text-input-test: commit \"日本\"" "the field received the committed string"

# Input method side: activation, surrounding text, caret popup, and the grab key.
check "$R/im.log" "input-method-test: activate" "the input method was activated"
check "$R/im.log" "input-method-test: surrounding \"ab\" 2 2" "the input method received the surrounding text"
check "$R/im.log" "popup rect -?[0-9]+ -?[0-9]+ [1-9][0-9]* [1-9][0-9]*" "the popup was positioned at the caret"
if [[ -x "$VK" ]]; then
    check "$R/im.log" "input-method-test: key 30 1" "the grabbed keyboard delivered the key to the input method"
fi

# The compositor must survive both clients going away (grab teardown).
kill "$IM_PID" "$TI_PID" 2>/dev/null || true
wait "$IM_PID" "$TI_PID" 2>/dev/null || true
sleep 1
if kill -0 "$CP" 2>/dev/null; then echo "  ok: the compositor survived the IME clients exiting"; else echo "  FAIL: the compositor died"; fail=1; fi
check_absent "$R/comp.log" "Assertion|SIGSEGV|Segmentation fault|VALIDATION FAILED" "the compositor did not fault"

if [[ "$fail" -ne 0 ]]; then echo "==> ime test FAILED"; exit 1; fi
echo "==> ime test PASSED"

#!/usr/bin/env bash
# Selection test: the compositor forwards CLIPBOARD (wl_data_device) and PRIMARY
# (zwp_primary_selection_v1 / gtk) selection requests to the wlr_seat, which is
# what lets wlroots' XWM bridge X11 PRIMARY/CLIPBOARD with the Wayland selection.
# A Wayland client sets each and reads it back (wl-clipboard, if installed).
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
SERENITY="${SERENITY:-$LIBGUI_WAYLAND_ROOT/serenity}"
BUILD="${BUILD:-$LIBGUI_WAYLAND_ROOT/build/lagom}"
COMP="$SDE_ROOT/build/WindowServer/WindowServer"
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"

command -v wl-copy >/dev/null && command -v wl-paste >/dev/null || { echo "SKIP: wl-clipboard not installed"; exit 0; }

"$SDE_ROOT/scripts/build-window-server.sh" >/dev/null

R="$(mktemp -d "${TMPDIR:-/tmp}/sde-selection.XXXXXX")"
chmod 700 "$R"
SOCK="sde-selection-$$"
env -u WAYLAND_DISPLAY -u DISPLAY \
    WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 SDE_OUTPUT_SCALE=1 \
    SDE_STATE_DIR="$R" \
    SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}" \
    LD_LIBRARY_PATH="$BUILD/lib" \
    "$COMP" -n "$SOCK" > "$R/comp.log" 2>&1 &
CP=$!
trap 'kill "$CP" 2>/dev/null || true; rm -rf "$R"; rm -f "$XDG_RUNTIME_DIR/$SOCK" "$XDG_RUNTIME_DIR/$SOCK.lock" 2>/dev/null || true' EXIT
for _ in $(seq 1 80); do [[ -e "$XDG_RUNTIME_DIR/$SOCK" ]] && break; sleep 0.1; done
sleep 0.5

fail=0
check() {
    if [[ "$2" == "$3" ]]; then echo "  ok: $1 ($3)"; else echo "  FAIL: $1 (got '$2', want '$3')"; fail=1; fi
}

echo "=== CLIPBOARD (wl_data_device) ==="
env WAYLAND_DISPLAY="$SOCK" wl-copy "sde-clipboard-test" 2>/dev/null || true
sleep 1
check "wl-paste round-trips the clipboard" "$(env WAYLAND_DISPLAY="$SOCK" timeout 3 wl-paste 2>/dev/null | head -1)" "sde-clipboard-test"

echo "=== PRIMARY (zwp_primary_selection_v1) ==="
env WAYLAND_DISPLAY="$SOCK" wl-copy -p "sde-primary-test" 2>/dev/null || true
sleep 1
check "wl-paste -p round-trips the primary selection" "$(env WAYLAND_DISPLAY="$SOCK" timeout 3 wl-paste -p 2>/dev/null | head -1)" "sde-primary-test"

if grep -qiE "SIGSEGV|Segmentation fault|ASSERTION FAILED|VERIFICATION FAILED" "$R/comp.log"; then echo "  FAIL: the compositor faulted"; fail=1; fi

if [[ "$fail" -ne 0 ]]; then echo "==> selection test FAILED"; exit 1; fi
echo "==> selection test PASSED"

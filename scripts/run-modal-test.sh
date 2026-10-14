#!/usr/bin/env bash
# Modal / transient semantics test (R3, docs/04).
#
# A blocking modal dialog (SerenityOS WindowMode::Blocking, sent over
# serenity-window) preempts its modal chain: input behind it is blocked (a click
# does not focus the window behind) and it cannot be minimized. This drives a
# headless compositor + two test clients and checks the negative (with a modal)
# and positive (without) cases.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
SERENITY="${SERENITY:-$LIBGUI_WAYLAND_ROOT/serenity}"
BUILD="${BUILD:-$LIBGUI_WAYLAND_ROOT/build/lagom}"
COMP="$SDE_ROOT/build/WindowServer/WindowServer"

"$SDE_ROOT/scripts/build-window-server.sh" >/dev/null
VI="$("$SDE_ROOT/scripts/build-virtual-input.sh")"
"$SDE_ROOT/scripts/build-tools.sh" >/dev/null 2>&1 || true
NATIVE="$SDE_ROOT/build/tools/native-app-test"
[[ -x "$NATIVE" ]] || { echo "SKIP: native-app-test not built"; exit 0; }

# $1 name, $2 startup command, $3 [Placement] Windows value, $4 pointer actions.
# Returns the compositor log path.
run_case() {
    local name="$1" startup="$2" placement="$3" actions="$4"
    local runtime sock comp_pid
    runtime="$(mktemp -d "${TMPDIR:-/tmp}/sde-modal.XXXXXX")"
    chmod 700 "$runtime"
    {
        echo "[Placement]"
        echo "Windows=$placement"
    } > "$runtime/WindowServer.ini"

    sock="sde-modal-$$-$name"
    env -u WAYLAND_DISPLAY -u DISPLAY \
        WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 \
        SDE_STATE_DIR="$runtime" SDE_WM_PORTAL_PATH="$runtime/wm" \
        SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}" \
        LD_LIBRARY_PATH="$BUILD/lib" SDE_APP_BIN_DIR="$BUILD/bin" \
        "$COMP" -n "$sock" -s "$startup" > "$runtime/comp.log" 2>&1 &
    comp_pid=$!
    for _ in $(seq 1 80); do
        [[ -e "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/$sock" ]] && break
        sleep 0.1
    done
    sleep 2.5
    # shellcheck disable=SC2086
    env WAYLAND_DISPLAY="$sock" "$VI" --extent 1280 720 $actions --linger 300 >/dev/null 2>&1 || true
    sleep 0.6
    kill -TERM -- "-$comp_pid" 2>/dev/null || kill "$comp_pid" 2>/dev/null || true
    wait "$comp_pid" 2>/dev/null || true
    echo "$runtime/comp.log"
}

CLICK_BEHIND='--move 100 100 --button 0x110 press --sleep 60 --button 0x110 release --sleep 200'
SUPER_DOWN='--key 125 press --key 108 press --key 108 release --key 125 release --sleep 200'

# (A) A blocking modal dialogs the window behind it. The modal starts after the
# other window has settled, so any focus change after its "window mode" line is
# caused by the click.
MODAL_LOG="$(run_case modal \
    "$NATIVE --title 'SDE Behind' --linger 15000 & sleep 1.5; $NATIVE --blocking --title 'SDE Modal' --linger 15000" \
    "SDE Modal:500,100,300,200;SDE Behind:0,0,300,200" \
    "$CLICK_BEHIND $SUPER_DOWN")"

# (B) Control: two normal windows. Clicking the second focuses it, and Super+Down
# minimizes the focused window.
CTRL_LOG="$(run_case ctrl \
    "$NATIVE --title 'SDE A' --linger 15000 & $NATIVE --title 'SDE B' --linger 15000" \
    "SDE A:0,0,300,200;SDE B:500,100,300,200" \
    "--move 600 200 --button 0x110 press --sleep 60 --button 0x110 release --sleep 200 $SUPER_DOWN")"

fail=0
check() { if eval "$2"; then echo "  ok: $1"; else echo "  MISSING: $1"; fail=1; fi; }

echo "=== modal ==="
grep -E "window mode|deactivated|minimize animation" "$MODAL_LOG" | head || true

# Focus changes *after* the modal is established would come from the click.
AFTER_MODAL="$(awk '/window mode 3 \(modal=true/{f=1; next} f' "$MODAL_LOG")"

check "the modal is created as a blocking window" "grep -q 'window mode 3 (modal=true' \"$MODAL_LOG\""
check "clicking behind the modal does not focus that window" "! grep -q \"window 'SDE Modal' deactivated\" <<< \"\$AFTER_MODAL\""
check "the modal cannot be minimized" "! grep -q 'minimize animation' \"$MODAL_LOG\""

echo "=== control (no modal) ==="
check "clicking a window focuses it (deactivates the other)" "grep -q \"window 'SDE A' deactivated\" \"$CTRL_LOG\""
check "a normal window minimizes" "grep -q 'minimize animation' \"$CTRL_LOG\""

if [[ "$fail" -ne 0 ]]; then
    echo "--- modal log (tail) ---"; tail -25 "$MODAL_LOG"
    echo "--- control log (tail) ---"; tail -20 "$CTRL_LOG"
    echo "==> modal test FAILED"
    exit 1
fi
echo "==> modal test PASSED"

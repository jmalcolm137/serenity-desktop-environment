#!/usr/bin/env bash
# Comprehensive Settings test.
#
# Phase A (launch): every dialog surfaced by the Settings launcher (the apps
#   whose .af is Category=Settings), plus the Settings launcher itself and
#   ThemeEditor, starts against the compositor and maps a window without
#   faulting. This catches a whole class of regressions -- the WindowServer IPC
#   `MessageID` is positional, so inserting a method mid-interface breaks every
#   app built against the old enum ("Failed to parse IPC message") until it is
#   rebuilt (new methods must be appended; see libgui-wayland patch 0042).
#
# Phase B (settings work): the settings each dialog manages actually apply and
#   persist -- the compositor-side backends are exercised by the focused tests
#   (input: mouse + keyboard; themes; fonts; output/monitor; workspaces;
#   effects; config) and the Settings portal.
#
# Runs headless (no host compositor, no DRM), so it is CI-friendly.
#
# Usage: run-settings-test.sh [--launch-only]
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
. "$SDE_ROOT/scripts/lib/test-env.sh"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
SERENITY="${SERENITY:-$LIBGUI_WAYLAND_ROOT/serenity}"
BUILD="${BUILD:-$LIBGUI_WAYLAND_ROOT/build/lagom}"
COMP="$SDE_ROOT/build/WindowServer/WindowServer"
SOCK="sde-settings-$$"

LAUNCH_ONLY=0
[[ "${1:-}" == "--launch-only" ]] && LAUNCH_ONLY=1

# The Settings launcher lists every .af with Category=Settings; keep this in sync
# with Base/res/apps/*.af (Serenity's Settings/main.cpp filters on the category).
SETTINGS_APPS=(
    BrowserSettings
    CalendarSettings
    CertificateSettings
    ClockSettings
    DisplaySettings
    GamesSettings
    KeyboardSettings
    MailSettings
    MapsSettings
    MouseSettings
    NetworkSettings
    TerminalSettings
    UsersSettings
)
# The launcher and the theme editor are part of the Settings experience too.
EXTRA_APPS=(Settings ThemeEditor)

"$SDE_ROOT/scripts/build-window-server.sh" >/dev/null

RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/sde-settings.XXXXXX")"
chmod 700 "$RUNTIME"
RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
COMP_PID=""
cleanup() {
    [[ -n "$COMP_PID" ]] && kill -TERM "$COMP_PID" 2>/dev/null || true
    [[ -n "$COMP_PID" ]] && wait "$COMP_PID" 2>/dev/null || true
    for _ in $(seq 1 60); do kill -0 "${COMP_PID:-${CP:-}}" 2>/dev/null || break; sleep 0.05; done; rm -rf "$RUNTIME"
    rm -f "$RUNTIME_DIR"/sde-*-$$* "$RUNTIME_DIR"/sde-*-$$*.lock 2>/dev/null || true
}
trap cleanup EXIT

fail=0
note_fail() {
    echo "  FAIL: $1"
    fail=1
}

start_compositor() {
    env -u WAYLAND_DISPLAY -u DISPLAY \
        WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 \
        XDG_RUNTIME_DIR="$RUNTIME_DIR" \
        XDG_CONFIG_HOME="$RUNTIME/config" \
        SDE_STATE_DIR="$RUNTIME" \
        SDE_WM_PORTAL_PATH="$RUNTIME/wm" \
        SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}" \
        LD_LIBRARY_PATH="$BUILD/lib" \
        "$COMP" -n "$SOCK" > "$RUNTIME/comp.log" 2>&1 &
    COMP_PID=$!
    for _ in $(seq 1 100); do
        [[ -e "$RUNTIME_DIR/$SOCK" ]] && return 0
        sleep 0.1
    done
    echo "  FAIL: compositor did not come up"
    tail -20 "$RUNTIME/comp.log"
    exit 1
}

echo "=== Phase A: every Settings dialog launches ==="
start_compositor

before="$(grep -c 'toplevel mapped' "$RUNTIME/comp.log" || true)"
for app in "${SETTINGS_APPS[@]}" "${EXTRA_APPS[@]}"; do
    if [[ ! -x "$BUILD/bin/$app" ]]; then
        note_fail "$app (not built: $BUILD/bin/$app)"
        continue
    fi
    log="$RUNTIME/$app.log"
    env WAYLAND_DISPLAY="$SOCK" \
        XDG_RUNTIME_DIR="$RUNTIME_DIR" \
        XDG_CONFIG_HOME="$RUNTIME/config" \
        LD_LIBRARY_PATH="$BUILD/lib" \
        SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}" \
        SDE_APP_BIN_DIR="$BUILD/bin" \
        timeout 6 "$BUILD/bin/$app" > "$log" 2>&1 || true
    after="$(grep -c 'toplevel mapped' "$RUNTIME/comp.log" || true)"
    faults="$(grep -ciE 'Failed to parse IPC message|SIGSEGV|Segmentation fault|ASSERTION FAILED|VERIFICATION FAILED|Runtime error' "$log" || true)"
    if [[ "$faults" -ne 0 ]]; then
        note_fail "$app: $(grep -iE 'Failed to parse IPC message|SIGSEGV|ASSERTION FAILED|VERIFICATION FAILED|Runtime error' "$log" | head -1)"
        # Say what the application itself did: "Runtime error" alone does not say
        # which file it could not open.
        echo "      --- $app log ---"; tail -6 "$log" | sed 's/^/      /' || true
    elif [[ "$after" -le "$before" ]]; then
        note_fail "$app did not map a window (log: $log)"
    else
        echo "  ok: $app"
    fi
    before="$after"
done

echo "=== Phase C: settings app icons exist ==="
# Desktop::AppFile::icon() looks for resource://icons/{16,32}/<name>.png, where
# <name> is app-<kebab-executable> (libgui-wayland patch 0010). Most settings
# apps are <Thing>Settings while their icons are named after the thing, so
# 0010 maps them to the real icon. Keep this table in sync with that patch.
declare -A SETTINGS_ICON=(
    [BrowserSettings]=app-browser
    [CalendarSettings]=app-calendar
    [CertificateSettings]=certificate
    [ClockSettings]=app-analog-clock
    [DisplaySettings]=app-display-settings
    [GamesSettings]=games
    [KeyboardSettings]=app-keyboard-settings
    [MailSettings]=app-mail
    [MapsSettings]=app-maps
    [MouseSettings]=app-mouse
    [NetworkSettings]=network
    [TerminalSettings]=app-terminal
    [UsersSettings]=app-user-settings
)
for app in "${SETTINGS_APPS[@]}"; do
    icon="${SETTINGS_ICON[$app]:-}"
    if [[ -z "$icon" ]]; then
        note_fail "$app has no icon mapping in run-settings-test.sh"
        continue
    fi
    for size in 16 32; do
        path="$SERENITY/Base/res/icons/${size}x${size}/${icon}.png"
        if [[ -f "$path" ]]; then
            echo "  ok: $app -> ${icon}.png ($size)"
        else
            note_fail "$app: missing $path"
        fi
    done
done

if [[ "$LAUNCH_ONLY" -eq 0 ]]; then
    echo "=== Phase B: the settings each dialog manages apply/persist ==="
    # Each focused test proves a settings backend end-to-end (config applied at
    # startup and on a live change, plus persistence). They build the compositor
    # and run headless, so they compose cleanly into this suite.
    run_backend() {
        local label="$1" script="$2"
        if "$SDE_ROOT/scripts/$script" > "$RUNTIME/$script.log" 2>&1; then
            echo "  ok: $label ($script)"
        else
            note_fail "$label ($script): see $RUNTIME/$script.log"
        fi
    }
    run_backend "Mouse + Keyboard" run-input-settings-test.sh
    run_backend "Themes"          run-themes-test.sh
    run_backend "Fonts"           run-fonts-test.sh
    run_backend "Monitor"         run-output-test.sh
    run_backend "Workspaces"      run-workspaces-test.sh
    run_backend "Effects"         run-effects-test.sh
    run_backend "Compositor config" run-config-test.sh
    run_backend "Settings portal" run-settings-portal-test.sh
    # The Display Settings > Background tab writes wallpaper state, exercised by
    # the portal backend check (Wallpaper.SetWallpaper -> wallpaper.raw).
    run_backend "Background/wallpaper" run-portal-test.sh
fi

if [[ "$fail" -ne 0 ]]; then
    echo "--- compositor log (tail) ---"
    tail -20 "$RUNTIME/comp.log"
    echo "==> settings test FAILED"
    exit 1
fi
echo "==> settings test PASSED"

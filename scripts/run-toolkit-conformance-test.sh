#!/usr/bin/env bash
# ci-tier: fast
# Toolkit conformance test (R5, docs/08 §3): run real GTK3, GTK4, Qt5, Qt6 and
# Firefox clients against the compositor and assert each one maps an xdg-toplevel
# with the app_id its toolkit sets. This is the reference run that was previously
# manual.
#
# The clients are distro packages, installed by scripts/ci-deps.sh. A missing
# client skips locally, but fails under CI so a changed package list cannot turn
# the coverage off silently. Every client is forced onto Wayland (no DISPLAY), so
# mapping in the compositor proves the toolkit's Wayland backend, not a fallback
# to X11. Runs headless.

set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
SERENITY="${SERENITY:-$LIBGUI_WAYLAND_ROOT/serenity}"
BUILD="${BUILD:-$LIBGUI_WAYLAND_ROOT/build/lagom}"
COMP="$SDE_ROOT/build/WindowServer/WindowServer"
SOCK="sde-toolkit-$$"

"$SDE_ROOT/scripts/build-window-server.sh" >/dev/null

RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/sde-toolkit.XXXXXX")"
chmod 700 "$RUNTIME"
mkdir -p "$RUNTIME/runtime" "$RUNTIME/home" "$RUNTIME/state" "$RUNTIME/session/portal" "$RUNTIME/firefox-profile"
chmod 700 "$RUNTIME/runtime"

CURRENT_PID=""
PREVIOUS_PID=""
COMP_PID=""

stop_pid() {
    local pid="$1"
    [[ -n "$pid" ]] || return 0
    # Each client is started in its own session, so this also takes its helpers
    # (Firefox's content processes) with it.
    kill -TERM -- "-$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
}

cleanup() {
    stop_pid "$CURRENT_PID"
    stop_pid "$PREVIOUS_PID"
    if [[ -n "$COMP_PID" ]]; then
        kill -TERM "$COMP_PID" 2>/dev/null || true
        wait "$COMP_PID" 2>/dev/null || true
    fi
    rm -rf "$RUNTIME"
    rm -f "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"/sde-*-$$* \
          "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"/sde-*-$$*.lock 2>/dev/null || true
}
trap cleanup EXIT

export XDG_RUNTIME_DIR="$RUNTIME/runtime"
export XDG_CONFIG_HOME="$RUNTIME/config"
export XDG_CACHE_HOME="$RUNTIME/cache"
export HOME="$RUNTIME/home"
mkdir -p "$XDG_CONFIG_HOME" "$XDG_CACHE_HOME"

# Minimal QML windows: Qt5 and Qt6 use different import versions, and the titles
# are ours so they can be asserted exactly.
cat > "$RUNTIME/qt5.qml" <<'EOF'
import QtQuick 2.0
import QtQuick.Window 2.0

Window {
    width: 320
    height: 240
    visible: true
    title: "SDE Qt5 conformance"
    color: "steelblue"
}
EOF
cat > "$RUNTIME/qt6.qml" <<'EOF'
import QtQuick
import QtQuick.Window

Window {
    width: 320
    height: 240
    visible: true
    title: "SDE Qt6 conformance"
    color: "steelblue"
}
EOF

env -u WAYLAND_DISPLAY -u DISPLAY \
    WLR_BACKENDS=headless \
    WLR_HEADLESS_OUTPUTS=1 \
    SDE_XWAYLAND=0 \
    SDE_STATE_DIR="$RUNTIME/state" \
    SDE_SESSION_DIR="$RUNTIME/session" \
    SDE_WM_PORTAL_PATH="$RUNTIME/wm" \
    SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}" \
    LD_LIBRARY_PATH="$BUILD/lib" \
    "$COMP" -n "$SOCK" > "$RUNTIME/comp.log" 2>&1 &
COMP_PID=$!

for _ in $(seq 1 80); do
    [[ -e "$XDG_RUNTIME_DIR/$SOCK" ]] && break
    kill -0 "$COMP_PID" 2>/dev/null || break
    sleep 0.1
done
if [[ ! -e "$XDG_RUNTIME_DIR/$SOCK" ]]; then
    echo "FAIL: the compositor did not create its socket"
    tail -20 "$RUNTIME/comp.log" | sed 's/^/  /'
    exit 1
fi

fail=0
checked=0
mapped=0

# check_client <label> <app_id> <expected title or empty> <command...>
check_client() {
    local label="$1"
    local app_id="$2"
    local expected_title="$3"
    shift 3

    if ! command -v "$1" >/dev/null 2>&1; then
        if [[ "${CI:-}" == "true" ]]; then
            echo "  FAIL: $label: '$1' is not installed (scripts/ci-deps.sh must provide it)"
            fail=1
        else
            echo "  SKIP: $label ('$1' is not installed)"
        fi
        return
    fi

    # The previous client is kept alive until this one maps (below); if it has
    # gone away on its own, the toolkit crashed between the two checks.
    if [[ -n "$PREVIOUS_PID" ]] && ! kill -0 "$PREVIOUS_PID" 2>/dev/null; then
        echo "  FAIL: the previous client exited unexpectedly"
        fail=1
        PREVIOUS_PID=""
    fi

    local log="$RUNTIME/$label.log"
    local before_deactivated after_deactivated line title
    before_deactivated="$(grep -c 'deactivated' "$RUNTIME/comp.log" || true)"

    # An empty DISPLAY plus the toolkit's Wayland backend remove the X11
    # fallback: a client that cannot use Wayland exits instead of opening a
    # window on the host.
    setsid env \
        DISPLAY= \
        WAYLAND_DISPLAY="$SOCK" \
        GDK_BACKEND=wayland \
        QT_QPA_PLATFORM=wayland \
        MOZ_ENABLE_WAYLAND=1 \
        MOZ_DISABLE_CONTENT_SANDBOX=1 \
        MOZ_DISABLE_RDD_SANDBOX=1 \
        MOZ_DISABLE_GMP_SANDBOX=1 \
        MOZ_DISABLE_SOCKET_PROCESS_SANDBOX=1 \
        MOZ_CRASHREPORTER_DISABLE=1 \
        SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}" \
        LD_LIBRARY_PATH="$BUILD/lib" \
        "$@" > "$log" 2>&1 &
    CURRENT_PID=$!

    line=""
    for _ in $(seq 1 200); do
        line="$(grep -aF 'toplevel mapped' "$RUNTIME/comp.log" | grep -aF "app_id='$app_id'" | tail -1 || true)"
        [[ -n "$line" ]] && break
        kill -0 "$CURRENT_PID" 2>/dev/null || break
        sleep 0.1
    done

    if [[ -z "$line" ]]; then
        echo "  FAIL: $label did not map an xdg-toplevel (app_id '$app_id')"
        tail -8 "$log" 2>/dev/null | sed 's/^/        /' || true
        fail=1
        stop_pid "$CURRENT_PID"
        CURRENT_PID=""
        return
    fi

    title="${line#*title=\'}"
    title="${title%%\'*}"
    if [[ -z "$title" ]]; then
        echo "  FAIL: $label mapped without a title"
        fail=1
    elif [[ -n "$expected_title" && "$title" != "$expected_title" ]]; then
        echo "  FAIL: $label mapped as '$title', expected '$expected_title'"
        fail=1
    elif ! kill -0 "$CURRENT_PID" 2>/dev/null; then
        echo "  FAIL: $label exited immediately after mapping"
        fail=1
    else
        # A newly mapped window takes focus from the previous one. This is the
        # part a toolkit can get wrong: a client that never receives
        # xdg_toplevel.configure(activated) still maps, so check the transition.
        after_deactivated="$(grep -c 'deactivated' "$RUNTIME/comp.log" || true)"
        if [[ -n "$PREVIOUS_PID" && "$after_deactivated" -le "$before_deactivated" ]]; then
            echo "  FAIL: $label mapped but the previous window was not deactivated"
            fail=1
        else
            echo "  ok: $label ($title, app_id '$app_id')"
            checked=$((checked + 1))
        fi
    fi

    mapped=$((mapped + 1))
    # Keep this client alive for the next one: the focus transition is part of
    # the conformance check above, and a killed window cannot be deactivated.
    stop_pid "$PREVIOUS_PID"
    PREVIOUS_PID="$CURRENT_PID"
    CURRENT_PID=""
}

echo "=== toolkit conformance (GTK, Qt, Firefox) ==="
check_client gtk3 "gtk3-widget-factory" "gtk3-widget-factory" gtk3-widget-factory
check_client gtk4 "org.gtk.WidgetFactory4" "GTK Widget Factory" gtk4-widget-factory
check_client qt5 "org.qt-project.qmlscene" "SDE Qt5 conformance" qmlscene "$RUNTIME/qt5.qml"
check_client qt6 "org.qt-project.qml6" "SDE Qt6 conformance" qml6 "$RUNTIME/qt6.qml"
check_client firefox "firefox" "" firefox \
    --new-instance --no-remote --profile "$RUNTIME/firefox-profile" about:blank

if ! kill -0 "$COMP_PID" 2>/dev/null; then
    echo "  FAIL: the compositor exited during the run"
    tail -20 "$RUNTIME/comp.log" | sed 's/^/        /'
    fail=1
fi
if [[ -n "$PREVIOUS_PID" ]] && ! kill -0 "$PREVIOUS_PID" 2>/dev/null; then
    echo "  FAIL: the last client exited unexpectedly"
    fail=1
fi
if grep -qiE "SIGSEGV|Segmentation fault|VERIFICATION FAILED|ASSERTION FAILED|wl_list_empty" "$RUNTIME/comp.log"; then
    echo "  FAIL: the compositor log shows a crash or assertion"
    grep -aiE "SIGSEGV|Segmentation fault|VERIFICATION FAILED|ASSERTION FAILED|wl_list_empty" "$RUNTIME/comp.log" | head -3 | sed 's/^/        /'
    fail=1
fi

if [[ "$checked" -eq 0 && "$fail" -eq 0 ]]; then
    echo "SKIP: no GTK/Qt/Firefox clients are installed"
    exit 0
fi

if [[ "$fail" -ne 0 ]]; then
    echo "--- compositor log (tail) ---"
    tail -20 "$RUNTIME/comp.log" | sed 's/^/  /'
    echo "==> toolkit conformance test FAILED"
    exit 1
fi

echo "==> toolkit conformance test PASSED ($checked client(s))"

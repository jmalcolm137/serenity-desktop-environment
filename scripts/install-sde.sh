#!/usr/bin/env bash
# Install a built SDE tree into a private prefix (default /usr/lib/sde).
#
# The package layout (docs/13-native-session-and-aur-packaging.md §4) is one
# directory with bin/, lib/, share/res/ and share/sde/scripts/, plus the session
# launcher and wayland-sessions entry. --destdir exists for makepkg-style staged
# installs; the launcher derives its own prefix, so a staged tree can be run
# directly by the test suite.
#
#   sudo scripts/install-sde.sh
#   scripts/install-sde.sh --destdir "$pkgdir"
#   scripts/install-sde.sh --prefix /opt/sde
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
SERENITY="${SERENITY:-$LIBGUI_WAYLAND_ROOT/serenity}"
BUILD="${BUILD:-$LIBGUI_WAYLAND_ROOT/build/lagom}"
PREFIX="/usr/lib/sde"
DESTDIR=""
SESSIONS_DIR="/usr/share/wayland-sessions"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --prefix) PREFIX="$2"; shift ;;
        --destdir) DESTDIR="$2"; shift ;;
        --sessions-dir) SESSIONS_DIR="$2"; shift ;;
        --build) BUILD="$2"; shift ;;
        -h|--help) sed -n '2,16p' "$0"; exit 0 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
    shift
done

DEST="${DESTDIR}${PREFIX}"
BIN="$DEST/bin"
LIB="$DEST/lib"
SHARE="$DEST/share"
SCRIPTS="$SHARE/sde/scripts"
DESKTOP="${DESTDIR}${SESSIONS_DIR}/sde.desktop"

install -d "$BIN" "$LIB" "$SHARE/res" "$SHARE/skeleton" "$SCRIPTS"
install -d "$(dirname "$DESKTOP")"

install_if_executable() {
    local source="$1" name="${2:-$(basename "$1")}"
    if [[ -x "$source" ]]; then
        install -m 0755 "$source" "$BIN/$name"
    fi
}

echo "==> installing to $DEST"
# The SDE's own programs. The test clients are deliberately not installed.
install_if_executable "$SDE_ROOT/build/WindowServer/WindowServer"
install_if_executable "$SDE_ROOT/build/LaunchServer/LaunchServer"
install_if_executable "$SDE_ROOT/build/ImageDecoder/ImageDecoder"
install_if_executable "$SDE_ROOT/build/NotificationServer/NotificationServer"
install_if_executable "$SDE_ROOT/build/AudioServer/AudioServer"
install_if_executable "$SDE_ROOT/build/RequestServer/RequestServer"
install_if_executable "$SDE_ROOT/build/WebContent/WebContent"
install_if_executable "$SDE_ROOT/build/SQLServer/SQLServer"
install_if_executable "$SDE_ROOT/build/BatteryApplet/BatteryApplet"
install_if_executable "$SDE_ROOT/build/WifiApplet/WifiApplet"
for applet in "$SDE_ROOT"/build/Applets/*; do
    [[ -f "$applet" && -x "$applet" ]] && install_if_executable "$applet"
done
for tool in sde-idle sde-dpms sde-settings-portal xdg-desktop-portal-serenity; do
    install_if_executable "$SDE_ROOT/build/tools/$tool"
done
# The portal's LibGUI dialogs are spawned by the backend through the app bin dir.
for dialog in AccessDialog AppChooserDialog FileChooserDialog PrintDialog ColorPicker ScreenCastChooser ScreenshotOverlay; do
    install_if_executable "$SDE_ROOT/build/$dialog/$dialog"
done

# The framework's applications and shared libraries. bin/ and lib/ are siblings,
# matching the Lagom RUNPATH ($ORIGIN:$ORIGIN/../lib); the launcher also exports
# LD_LIBRARY_PATH for the SDE binaries, whose build RUNPATH points at the build
# tree.
if [[ -d "$BUILD/bin" ]]; then
    cp -a "$BUILD/bin/." "$BIN/"
fi
if [[ -d "$BUILD/lib" ]]; then
    cp -a "$BUILD/lib/"*.so* "$LIB/"
fi

# Resources and the skeleton user config. Installed trees flatten Base/res and
# Base/home into the prefix; the launcher points SERENITY_RES_ROOT and
# SDE_SKELETON_DIR at them.
if [[ -d "$SERENITY/Base/res" ]]; then
    cp -a "$SERENITY/Base/res/." "$SHARE/res/"
else
    echo "warning: no $SERENITY/Base/res; the installed tree has no theme or fonts" >&2
fi
if [[ -d "$SERENITY/Base/home/anon/.config" ]]; then
    cp -a "$SERENITY/Base/home/anon/.config/." "$SHARE/skeleton/"
fi

# The runtime scripts (not the test drivers or build wrappers).
for script in \
    start-sde-session.sh sde-session.sh sde-shell.sh sde-service-supervisor.sh \
    sde-applets.sh sde-power.sh sde-seat.sh sde-sleep.sh sde-accessibility.sh \
    seed-serenity-home.sh; do
    install -m 0755 "$SDE_ROOT/scripts/$script" "$SCRIPTS/$script"
done

cat > "$BIN/sde-session" <<'LAUNCHER'
#!/bin/sh
# SDE session launcher (installed). It derives the prefix from its own path, so a
# staged install (a package's $pkgdir) can be run directly.
self="$(readlink -f "$0")"
prefix="$(CDPATH= cd -- "$(dirname -- "$self")/.." && pwd)"
export SDE_ROOT="$prefix/share/sde"
export BUILD="$prefix"
export SDE_APP_BIN_DIR="$prefix/bin"
export SDE_APPLET_DIR="$prefix/bin"
export SDE_SERVICE_BIN_DIR="$prefix/bin"
export SDE_COMPOSITOR_BIN="$prefix/bin/WindowServer"
export SDE_LAUNCH_SERVER_BIN="$prefix/bin/LaunchServer"
export SDE_IMAGE_DECODER_BIN="$prefix/bin/ImageDecoder"
export SDE_PORTAL_BACKEND_BIN="$prefix/bin/xdg-desktop-portal-serenity"
export SDE_SETTINGS_PORTAL_BIN="$prefix/bin/sde-settings-portal"
export SDE_BATTERY_APPLET="$prefix/bin/BatteryApplet"
export SDE_WIFI_APPLET="$prefix/bin/WifiApplet"
export SDE_SESSION_IDLE_BIN="$prefix/bin/sde-idle"
export SDE_SESSION_DPMS_BIN="$prefix/bin/sde-dpms"
export SDE_SKELETON_DIR="$prefix/share/skeleton"
export SERENITY_RES_ROOT="$prefix/share/res"
export LD_LIBRARY_PATH="$prefix/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
exec "$prefix/share/sde/scripts/start-sde-session.sh" "$@"
LAUNCHER
chmod 0755 "$BIN/sde-session"

sed "s|@EXEC@|$PREFIX/bin/sde-session|g" "$SDE_ROOT/session/sde.desktop.in" > "$DESKTOP"
chmod 0644 "$DESKTOP"

echo "==> installed"
echo "    binaries : $BIN"
echo "    libs     : $LIB"
echo "    resources: $SHARE/res"
echo "    scripts  : $SCRIPTS"
echo "    launcher : $BIN/sde-session"
echo "    session  : $DESKTOP"

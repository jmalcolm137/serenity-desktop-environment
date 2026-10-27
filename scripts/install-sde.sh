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
WITHOUT_FRAMEWORK=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --prefix) PREFIX="$2"; shift ;;
        --destdir) DESTDIR="$2"; shift ;;
        --sessions-dir) SESSIONS_DIR="$2"; shift ;;
        --without-framework) WITHOUT_FRAMEWORK=1 ;;
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
install_if_executable "$SDE_ROOT/build/AuthDialog/sde-auth-dialog"
for applet in "$SDE_ROOT"/build/Applets/*; do
    [[ -f "$applet" && -x "$applet" ]] && install_if_executable "$applet"
done
for tool in sde-idle sde-dpms sde-settings-portal xdg-desktop-portal-serenity sde-notifications sde-lock sde-polkit-agent; do
    install_if_executable "$SDE_ROOT/build/tools/$tool"
done
# The portal's LibGUI dialogs are spawned by the backend through the app bin dir.
for dialog in AccessDialog AppChooserDialog FileChooserDialog PrintDialog ColorPicker ScreenCastChooser ScreenshotOverlay; do
    install_if_executable "$SDE_ROOT/build/$dialog/$dialog"
done

# Portal registration. The frontend only selects the SDE backend when the
# .portal file is installed and its UseIn list contains the session's
# XDG_CURRENT_DESKTOP (SDE). Keep the backend activation file absolute too:
# the SDE bin directory is not necessarily on the session PATH.
install -d "$DESTDIR/usr/share/xdg-desktop-portal/portals" "$DESTDIR/usr/share/dbus-1/services"
install -m 0644 "$SDE_ROOT/Base/xdg-desktop-portal/serenity.portal" \
    "$DESTDIR/usr/share/xdg-desktop-portal/portals/serenity.portal"
install -m 0644 "$SDE_ROOT/Base/xdg-desktop-portal/portals.conf" \
    "$DESTDIR/usr/share/xdg-desktop-portal/portals.conf"
sed "s|^Exec=.*|Exec=$PREFIX/bin/xdg-desktop-portal-serenity|" \
    "$SDE_ROOT/Base/xdg-desktop-portal/org.freedesktop.impl.portal.desktop.serenity.service" \
    > "$DESTDIR/usr/share/dbus-1/services/org.freedesktop.impl.portal.desktop.serenity.service"
chmod 0644 "$DESTDIR/usr/share/dbus-1/services/org.freedesktop.impl.portal.desktop.serenity.service"

# The freedesktop notification daemon (org.freedesktop.Notifications). Installed
# as a D-Bus activation file with an absolute Exec: the SDE bin directory is not
# necessarily on the session PATH for the bus.
sed "s|^Exec=.*|Exec=$PREFIX/bin/sde-notifications|" \
    "$SDE_ROOT/Base/dbus-1/services/org.freedesktop.Notifications.service" \
    > "$DESTDIR/usr/share/dbus-1/services/org.freedesktop.Notifications.service"
chmod 0644 "$DESTDIR/usr/share/dbus-1/services/org.freedesktop.Notifications.service"

# Help hardcodes /usr/share/man as its manual root. The Serenity source has the
# manual pages, and Base/res does not contain them, so install the manual tree
# alongside the other system integration files. A split install relies on the
# framework package, which installs the same tree.
if [[ "$WITHOUT_FRAMEWORK" -eq 0 ]]; then
    if [[ -d "$SERENITY/Base/usr/share/man" ]]; then
        mkdir -p "$DESTDIR/usr/share/man"
        cp -a "$SERENITY/Base/usr/share/man/." "$DESTDIR/usr/share/man/"
    else
        echo "warning: no $SERENITY/Base/usr/share/man; Help will have no manuals" >&2
    fi
fi

# The framework's applications and shared libraries. bin/ and lib/ are siblings,
# matching the Lagom RUNPATH ($ORIGIN:$ORIGIN/../lib); the launcher also exports
# LD_LIBRARY_PATH for the SDE binaries, whose build RUNPATH points at the build
# tree.
if [[ "$WITHOUT_FRAMEWORK" -eq 0 ]]; then
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
else
    echo "note: --without-framework: the framework package owns bin/lib/share"
fi

# The runtime scripts (not the test drivers or build wrappers).
for script in \
    start-sde-session.sh sde-session.sh sde-shell.sh sde-service-supervisor.sh \
    sde-applets.sh sde-supervise-applet.sh sde-power.sh sde-seat.sh sde-sleep.sh sde-accessibility.sh \
    sde-user-dirs.sh generate-desktop-entries.sh \
    seed-serenity-home.sh; do
    install -m 0755 "$SDE_ROOT/scripts/$script" "$SCRIPTS/$script"
done

# The .desktop entries for the Serenity apps: the Taskbar's XDG menu and host
# application menus read $XDG_DATA_DIRS/applications.
"$SDE_ROOT/scripts/generate-desktop-entries.sh" \
    --source "$SERENITY/Base/res/apps" --output "$SHARE" >/dev/null 2>&1 || true

# The SDE's default freedesktop menu file and its .directory entries. The
# Taskbar prefers a host ${XDG_MENU_PREFIX}applications.menu and falls back here.
mkdir -p "$SHARE/sde/menus" "$SHARE/sde/desktop-directories"
cp -a "$SDE_ROOT/Base/xdg/menus/." "$SHARE/sde/menus/"
cp -a "$SDE_ROOT/Base/xdg/desktop-directories/." "$SHARE/sde/desktop-directories/"

cat > "$BIN/sde-session" <<'LAUNCHER'
#!/bin/sh
# SDE session launcher (installed). It derives the prefix from its own path, so a
# staged install (a package's $pkgdir) can be run directly.
self="$(readlink -f "$0")"
prefix="$(CDPATH= cd -- "$(dirname -- "$self")/.." && pwd)"
export SDE_ROOT="$prefix/share/sde"
export BUILD="$prefix"
# The split package keeps the libgui-wayland source/header tree (including the
# SerenityOS resources seed-serenity-home reads) under framework/.
export LIBGUI_WAYLAND_ROOT="$prefix/framework"
export SERENITY="$prefix/framework/serenity"
export SERENITY_SRC="$SERENITY"
export SDE_APP_BIN_DIR="$prefix/bin"
export SDE_APPLET_DIR="$prefix/bin"
export SDE_SERVICE_BIN_DIR="$prefix/bin"
export SDE_COMPOSITOR_BIN="$prefix/bin/WindowServer"
export SDE_LAUNCH_SERVER_BIN="$prefix/bin/LaunchServer"
export SDE_IMAGE_DECODER_BIN="$prefix/bin/ImageDecoder"
export SDE_PORTAL_BACKEND_BIN="$prefix/bin/xdg-desktop-portal-serenity"
export SDE_SETTINGS_PORTAL_BIN="$prefix/bin/sde-settings-portal"
export SDE_NOTIFICATIONS_BIN="$prefix/bin/sde-notifications"
export SDE_BATTERY_APPLET="$prefix/bin/BatteryApplet"
export SDE_WIFI_APPLET="$prefix/bin/WifiApplet"
export SDE_SESSION_IDLE_BIN="$prefix/bin/sde-idle"
export SDE_SESSION_DPMS_BIN="$prefix/bin/sde-dpms"
export SDE_AUTH_DIALOG="$prefix/bin/sde-auth-dialog"
export SDE_PORTAL_FILE_CHOOSER="$prefix/bin/FileChooserDialog"
export SDE_PORTAL_PRINT_DIALOG="$prefix/bin/PrintDialog"
export SDE_PORTAL_ACCESS_DIALOG="$prefix/bin/AccessDialog"
export SDE_PORTAL_APP_CHOOSER="$prefix/bin/AppChooserDialog"
export SDE_PORTAL_COLOR_PICKER="$prefix/bin/ColorPicker"
export SDE_PORTAL_SCREENSHOT_OVERLAY="$prefix/bin/ScreenshotOverlay"
export SDE_PORTAL_CHOOSER="$prefix/bin/ScreenCastChooser"
# The native ext-session-lock client shipped by this package. Without this the
# fallback is swaylock, which the package does not depend on.
export SDE_LOCK_CMD="${SDE_LOCK_CMD:-$prefix/bin/sde-lock}"
export SDE_SKELETON_DIR="$prefix/share/skeleton"
export SERENITY_RES_ROOT="$prefix/share/res"
export XDG_DATA_DIRS="$prefix/share:${XDG_DATA_DIRS:-/usr/local/share:/usr/share}"
export SDE_MENU_DIR="$prefix/share/sde/menus"
export LD_LIBRARY_PATH="$prefix/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
exec "$prefix/share/sde/scripts/start-sde-session.sh" "$@"
LAUNCHER
chmod 0755 "$BIN/sde-session"

sed "s|@EXEC@|$PREFIX/bin/sde-session|g" "$SDE_ROOT/Base/session/sde.desktop.in" > "$DESKTOP"
chmod 0644 "$DESKTOP"

echo "==> installed"
echo "    binaries : $BIN"
echo "    libs     : $LIB"
echo "    resources: $SHARE/res"
echo "    manuals  : ${DESTDIR}/usr/share/man"
echo "    scripts  : $SCRIPTS"
echo "    launcher : $BIN/sde-session"
echo "    session  : $DESKTOP"

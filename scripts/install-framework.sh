#!/usr/bin/env bash
# Install the built framework for a split package.
#
# The SDE's compositor/services compile against the framework's headers and need
# generated endpoint headers; a split package therefore installs both runtime
# artefacts (libs/apps/resources) and the framework source/header tree. The SDE
# package then builds with SDE_PREBUILT_FRAMEWORK=ON against this prefix.
#
# Layout under --prefix (default /usr/lib/sde):
#   bin/ lib/ share/res/ share/skeleton/   the runtime payload
#   framework/{AK,Userland,...,serenity,src}  what the SDE build includes
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
SERENITY="${SERENITY:-$LIBGUI_WAYLAND_ROOT/serenity}"
BUILD="${BUILD:-$LIBGUI_WAYLAND_ROOT/build/lagom}"
PREFIX="/usr/lib/sde"
DESTDIR=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --prefix) PREFIX="$2"; shift ;;
        --destdir) DESTDIR="$2"; shift ;;
        --build) BUILD="$2"; shift ;;
        -h|--help) sed -n '2,14p' "$0"; exit 0 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
    shift
done

DEST="${DESTDIR}${PREFIX}"
install -d "$DEST/bin" "$DEST/lib" "$DEST/share/res" "$DEST/share/skeleton" "$DEST/framework"

echo "==> installing the framework to $DEST"
if [[ -d "$BUILD/lib" ]]; then
    cp -a "$BUILD/lib/"*.so* "$DEST/lib/"
    [[ -f "$BUILD/lib/liblagom-main.a" ]] && cp -a "$BUILD/lib/liblagom-main.a" "$DEST/lib/"
fi
if [[ -d "$BUILD/bin" ]]; then
    cp -a "$BUILD/bin/." "$DEST/bin/"
fi
if [[ -d "$SERENITY/Base/res" ]]; then
    cp -a "$SERENITY/Base/res/." "$DEST/share/res/"
fi
if [[ -d "$SERENITY/Base/home/anon/.config" ]]; then
    cp -a "$SERENITY/Base/home/anon/.config/." "$DEST/share/skeleton/"
fi

# Help (and the framework's Help app) reads manuals from /usr/share/man, not the
# framework source tree. The manual pages are a framework payload.
if [[ -d "$SERENITY/Base/usr/share/man" ]]; then
    mkdir -p "$DESTDIR/usr/share/man"
    cp -a "$SERENITY/Base/usr/share/man/." "$DESTDIR/usr/share/man/"
fi

# Generated headers (AK/Debug.h, the IPC endpoint headers, ...) and the source
# they belong to. Keep the same relative layout the build includes use. Use tar
# rather than rsync: the CI image carries tar but not rsync.
( cd "$BUILD" && find . -name '*.h' -print0 | tar --null -T - -cf - ) | ( cd "$DEST/framework" && tar -xf - )
install -d "$DEST/framework/serenity"
( cd "$SERENITY" && tar --exclude=.git --exclude=Build --exclude=build -cf - . ) | ( cd "$DEST/framework/serenity" && tar -xf - )
cp -a "$LIBGUI_WAYLAND_ROOT/src/." "$DEST/framework/src/"
# The SDE's NotificationServer runs Lagom's GML compiler from the build tree.
install -d "$DEST/framework/bin"
[[ -x "$BUILD/bin/GMLCompiler" ]] && install -m 0755 "$BUILD/bin/GMLCompiler" "$DEST/framework/bin/GMLCompiler"

echo "==> installed"
echo "    runtime : $DEST/{bin,lib,share/res,share/skeleton}"
echo "    manuals : ${DESTDIR}/usr/share/man"
echo "    headers : $DEST/framework"

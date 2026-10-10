#!/usr/bin/env bash
# Launch a Serenity/LibWindowServer application INTO the running SDE session.
#
# The common mistake is to launch with the host's WAYLAND_DISPLAY (e.g.
# wayland-0); the app then maps its window on the host compositor instead of
# inside the SDE session. Session apps connect to the compositor's own socket
# (WAYLAND_DISPLAY=sde) plus the shared libgui-wayland runtime environment, all
# of which this wraps.
#
# Usage: scripts/run-sde-app.sh <app> [args...]
#   app   a name resolved under $BUILD/bin (e.g. DisplaySettings, FileManager),
#         or an absolute path.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
SERENITY="${SERENITY:-$LIBGUI_WAYLAND_ROOT/serenity}"
BUILD="${BUILD:-$LIBGUI_WAYLAND_ROOT/build/lagom}"
SDE_DISPLAY="${SDE_DISPLAY:-sde}"
XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"

if [[ $# -lt 1 ]]; then
    echo "usage: $0 <app> [args...]" >&2
    exit 2
fi

app="$1"
shift
[[ "$app" == */* ]] || app="$BUILD/bin/$app"

if [[ ! -x "$app" ]]; then
    echo "error: not executable: $app" >&2
    exit 1
fi

if [[ ! -e "$XDG_RUNTIME_DIR/$SDE_DISPLAY" ]]; then
    echo "error: no SDE session on WAYLAND_DISPLAY=$SDE_DISPLAY (compositor not running?)" >&2
    exit 1
fi

# Make sure the app finds the SerenityOS default config (e.g. Browser's content
# filters and autoplay allowlist), which SerenityOS seeds into a new user's home.
SERENITY="$SERENITY" "$SDE_ROOT/scripts/seed-serenity-home.sh"

exec env \
    WAYLAND_DISPLAY="$SDE_DISPLAY" \
    XDG_RUNTIME_DIR="$XDG_RUNTIME_DIR" \
    SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}" \
    LD_LIBRARY_PATH="$BUILD/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
    "$app" "$@"

#!/usr/bin/env bash
# Install (or remove) the SDE session entry, so a display manager lists
# "Serenity Desktop Environment" at the login screen (SDDM, GDM, ...).
#
# A Wayland session is a .desktop file in the wayland-sessions directory whose
# Exec is a launcher. This installs a launcher that runs THIS checkout's
# scripts/start-sde-session.sh (a dev install: no packaging needed), plus the
# .desktop entry.
#
# Usage:
#   install-session.sh [--repo DIR] [--bin-dir DIR] [--sessions-dir DIR]
#                      [--uninstall] [--dry-run]
#
# Defaults: --repo is this checkout; the launcher goes to /usr/local/bin and the
# entry to /usr/share/wayland-sessions. System paths need root; run with sudo (or
# as root) for those. SDDM's default Wayland session dir is
# /usr/share/wayland-sessions.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN_DIR="/usr/local/bin"
SESSIONS_DIR="/usr/share/wayland-sessions"
UNINSTALL=0
DRY_RUN=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --repo) SDE_ROOT="$(cd "$2" && pwd)"; shift ;;
        --bin-dir) BIN_DIR="$2"; shift ;;
        --sessions-dir) SESSIONS_DIR="$2"; shift ;;
        --uninstall) UNINSTALL=1 ;;
        --dry-run) DRY_RUN=1 ;;
        -h|--help) sed -n '2,20p' "$0"; exit 0 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
    shift
done

WRAPPER="$BIN_DIR/sde-session"
ENTRY="$SESSIONS_DIR/sde.desktop"
TEMPLATE_DESKTOP="$SDE_ROOT/Base/session/sde.desktop.in"
TEMPLATE_WRAPPER="$SDE_ROOT/Base/session/sde-session.in"

# Use sudo only when needed (a system dir and not already root).
SUDO=""
if [[ "$(id -u)" -ne 0 ]]; then
    if ! mkdir -p "$BIN_DIR" 2>/dev/null || [[ ! -w "$BIN_DIR" ]]; then
        SUDO="sudo"
    fi
fi
run() { if [[ -n "$SUDO" ]]; then $SUDO "$@"; else "$@"; fi; }

if [[ "$UNINSTALL" -eq 1 ]]; then
    echo "==> removing $ENTRY and $WRAPPER"
    [[ "$DRY_RUN" -eq 1 ]] && exit 0
    run rm -f "$ENTRY" "$WRAPPER"
    echo "==> done (log out for the login screen to drop Serenity Desktop Environment)"
    exit 0
fi

# Sanity: the checkout must be able to start a session.
if [[ ! -x "$SDE_ROOT/scripts/start-sde-session.sh" ]]; then
    echo "error: $SDE_ROOT/scripts/start-sde-session.sh not found/executable" >&2
    exit 1
fi
if [[ ! -x "$SDE_ROOT/build/WindowServer/WindowServer" ]]; then
    echo "warning: $SDE_ROOT/build/WindowServer/WindowServer is not built;" >&2
    echo "         run scripts/build-window-server.sh before logging in" >&2
fi
[[ -f "$TEMPLATE_DESKTOP" && -f "$TEMPLATE_WRAPPER" ]] || { echo "error: session templates missing under $SDE_ROOT/Base/session" >&2; exit 1; }

echo "==> installing the SDE session entry"
echo "    launcher : $WRAPPER -> $SDE_ROOT/scripts/start-sde-session.sh"
echo "    entry    : $ENTRY"
if [[ "$DRY_RUN" -eq 1 ]]; then
    echo "(dry run: nothing written)"
    exit 0
fi

# The daemon can be launched from a display manager; make sure the directory exists
# and write the two files. `install -D` creates parents.
tmp_wrapper="$(mktemp)"
tmp_entry="$(mktemp)"
trap 'rm -f "$tmp_wrapper" "$tmp_entry"' EXIT
sed "s|@SDE_ROOT@|$SDE_ROOT|g" "$TEMPLATE_WRAPPER" > "$tmp_wrapper"
sed "s|@EXEC@|$WRAPPER|g" "$TEMPLATE_DESKTOP" > "$tmp_entry"

run install -D -m 0755 "$tmp_wrapper" "$WRAPPER"
run install -D -m 0644 "$tmp_entry" "$ENTRY"

echo "==> done. Log out and pick \"Serenity Desktop Environment\" at the login screen."
echo "    Keep another session (e.g. Plasma) and a VT (Ctrl+Alt+F2) as an escape hatch."
echo "    Verify the compositor is built: scripts/build-window-server.sh"

if ! command -v swaylock >/dev/null 2>&1; then
    echo
    echo "note: no lock client found (swaylock). Idle-to-lock (default 300s) and"
    echo "      Super+L will do nothing until one is installed, or set 'lock_command'"
    echo "      in \$XDG_CONFIG_HOME/sde/session.conf (or install swaylock)."
fi

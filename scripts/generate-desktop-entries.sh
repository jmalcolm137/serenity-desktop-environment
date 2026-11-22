#!/usr/bin/env bash
# Generate freedesktop .desktop entries for the SerenityOS applications from
# their .af files (Base/res/apps). Both the Taskbar's XDG menu and host Linux
# desktop menus (KDE, GNOME, XFCE, ...) read them from $XDG_DATA_DIRS/applications.
#
#   generate-desktop-entries.sh --output <data-dir> [options]
#
# Options:
#   --output <dir>       data dir; entries are written to <dir>/applications
#   --source <dir>       the SerenityOS apps dir (default: $SERENITY/Base/res/apps)
#   --bin-dir <dir>      host directory containing the installed Serenity binaries
#                        (used for TryExec and Exec when targeting host menus)
#   --launcher <path>    wrapper binary (e.g. /usr/lib/sde/bin/sde-app) used as
#                        `Exec=<launcher> <App> [args]` so apps launch cleanly
#                        outside an SDE session
#   --id-prefix <pfx>    prefix for generated .desktop filenames (e.g. serenity-)
#                        to avoid colliding with host Terminal/Calculator entries
#   --icon-dir <dir>     hicolor icon theme root (e.g. /usr/share/icons/hicolor);
#                        copies 16x16/32x32 PNG icons there and sets Icon= to
#                        the theme icon name so other desktops display them
#   --res-root <dir>     Serenity resource root (default: $SERENITY/Base/res)
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
SERENITY="${SERENITY:-$LIBGUI_WAYLAND_ROOT/serenity}"
SOURCE="${SERENITY}/Base/res/apps"
RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}"
OUTPUT=""
BIN_DIR=""
LAUNCHER=""
ID_PREFIX=""
ICON_DIR=""

usage() {
    sed -n '2,21p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --output) OUTPUT="${2:-}"; shift ;;
        --source) SOURCE="${2:-}"; shift ;;
        --bin-dir) BIN_DIR="${2:-}"; shift ;;
        --launcher) LAUNCHER="${2:-}"; shift ;;
        --id-prefix) ID_PREFIX="${2:-}"; shift ;;
        --icon-dir) ICON_DIR="${2:-}"; shift ;;
        --res-root) RES_ROOT="${2:-}"; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "unknown argument: $1" >&2; usage >&2; exit 2 ;;
    esac
    shift
done

if [[ -z "$OUTPUT" ]]; then
    usage >&2
    exit 2
fi
if [[ ! -d "$SOURCE" ]]; then
    echo "generate-desktop-entries: no apps dir at $SOURCE" >&2
    exit 0
fi

APPS_DIR="$OUTPUT/applications"
mkdir -p "$APPS_DIR"
rm -f "$APPS_DIR"/"${ID_PREFIX}"*.desktop

if [[ -n "$ICON_DIR" ]]; then
    mkdir -p "$ICON_DIR/16x16/apps" "$ICON_DIR/32x32/apps"
fi

trim() {
    local value="$1"
    value="${value#"${value%%[![:space:]]*}"}"
    value="${value%"${value##*[![:space:]]}"}"
    printf '%s' "$value"
}

# Serenity's .af category (with & mnemonics, possibly a Games/Puzzles subpath) to
# the freedesktop main category.
xdg_category() {
    case "$1" in
        *Games*) printf 'Game;' ;;
        *Graphics*|*Demos*) printf 'Graphics;' ;;
        *Development*) printf 'Development;' ;;
        *Internet*) printf 'Network;' ;;
        *Media*) printf 'AudioVideo;' ;;
        *Office*) printf 'Office;' ;;
        Settings) printf 'Settings;' ;;
        *) printf 'Utility;' ;;
    esac
}

# SDE-session-only settings tools that only make sense inside an SDE session.
is_sde_only_app() {
    case "$1" in
        DisplaySettings|MouseSettings|KeyboardSettings|NetworkSettings|UsersSettings|ClockSettings|Settings)
            return 0 ;;
        *)
            return 1 ;;
    esac
}

# Map common Serenity FileTypes (extensions) to MIME types for MimeType=.
extension_to_mime() {
    case "${1,,}" in
        txt|text|md|log|ini|conf) printf 'text/plain' ;;
        html|htm) printf 'text/html' ;;
        pdf) printf 'application/pdf' ;;
        png) printf 'image/png' ;;
        jpg|jpeg) printf 'image/jpeg' ;;
        gif) printf 'image/gif' ;;
        bmp) printf 'image/bmp' ;;
        webp) printf 'image/webp' ;;
        svg) printf 'image/svg+xml' ;;
        wav) printf 'audio/wav' ;;
        mp3) printf 'audio/mpeg' ;;
        ogg) printf 'audio/ogg' ;;
        flac) printf 'audio/flac' ;;
        mp4) printf 'video/mp4' ;;
        webm) printf 'video/webm' ;;
        csv) printf 'text/csv' ;;
        json) printf 'application/json' ;;
        *) return 1 ;;
    esac
}

count=0
for af in "$SOURCE"/*.af; do
    [[ -e "$af" ]] || continue

    section=""
    name=""
    executable=""
    category=""
    arguments=""
    description=""
    icon_path=""
    file_types=""
    protocols=""
    terminal="false"
    exclude="false"

    while IFS= read -r line || [[ -n "$line" ]]; do
        line="${line%$'\r'}"
        case "$line" in
            \[*\]) section="${line:1:${#line}-2}"; continue ;;
        esac
        [[ "$section" == "App" ]] || continue
        key="$(trim "${line%%=*}")"
        value=""
        [[ "$line" == *=* ]] && value="$(trim "${line#*=}")"
        case "$key" in
            Name) name="$value" ;;
            Executable) executable="$value" ;;
            Category) category="$value" ;;
            Arguments) arguments="$value" ;;
            Description) description="$value" ;;
            IconPath) icon_path="$value" ;;
            FileTypes) file_types="$value" ;;
            Protocols) protocols="$value" ;;
            RunInTerminal) terminal="$value" ;;
            ExcludeFromSystemMenu) exclude="$value" ;;
        esac
    done < "$af"

    [[ "$exclude" == "true" ]] && continue
    name="${name//&/}"
    [[ -n "$name" && -n "$executable" ]] || continue

    executable_name="$(basename "$executable")"
    [[ -n "$executable_name" ]] || continue
    desktop_id="$(basename "$af" .af)"

    if [[ -n "$LAUNCHER" ]]; then
        exec_line="$LAUNCHER $executable_name"
        [[ -n "$arguments" ]] && exec_line="$exec_line $arguments"
        try_exec="${BIN_DIR:+$BIN_DIR/$executable_name}"
        [[ -n "$try_exec" ]] || try_exec="$LAUNCHER"
    elif [[ -n "$BIN_DIR" ]]; then
        exec_line="$BIN_DIR/$executable_name"
        [[ -n "$arguments" ]] && exec_line="$exec_line $arguments"
        try_exec="$BIN_DIR/$executable_name"
    else
        # In-session fallback: keep the SerenityOS /bin/<name> form that LibWM
        # resolves against LIBWM_APP_BIN_DIR.
        exec_line="$executable"
        [[ -n "$arguments" ]] && exec_line="$exec_line $arguments"
        try_exec="$executable"
    fi
    [[ "$terminal" == "true" ]] || terminal="false"

    # Resolve icon: if --icon-dir is provided, copy 16x16/32x32 PNGs into
    # hicolor so other desktops (KDE, GNOME, XFCE) display real Serenity icons.
    icon_entry="$icon_path"
    if [[ -n "$ICON_DIR" && -n "$icon_path" ]]; then
        icon_base="$(basename "$icon_path")"
        theme_icon_name="serenity-${desktop_id,,}"
        copied_icon=0
        for sz in 16x16 32x32; do
            src_icon="$RES_ROOT/icons/$sz/$icon_base"
            if [[ -f "$src_icon" ]]; then
                cp -f "$src_icon" "$ICON_DIR/$sz/apps/$theme_icon_name.png"
                copied_icon=1
            fi
        done
        if [[ "$copied_icon" -eq 1 ]]; then
            icon_entry="$theme_icon_name"
        fi
    fi

    # Build MimeType= list from .af FileTypes / Protocols when present.
    mime_types=""
    if [[ -n "$file_types" ]]; then
        IFS=',' read -ra _fts <<< "$file_types"
        for _ft in "${_fts[@]}"; do
            _ft="$(trim "$_ft")"
            [[ -n "$_ft" ]] || continue
            if _m="$(extension_to_mime "$_ft")"; then
                [[ ";$mime_types" == *";$_m;"* ]] || mime_types+="$_m;"
            fi
        done
    fi
    if [[ -n "$protocols" ]]; then
        IFS=',' read -ra _protos <<< "$protocols"
        for _p in "${_protos[@]}"; do
            _p="$(trim "$_p")"
            [[ -n "$_p" ]] || continue
            _m="x-scheme-handler/${_p,,}"
            [[ ";$mime_types" == *";$_m;"* ]] || mime_types+="$_m;"
        done
    fi

    {
        echo "# Generated by sde-generate-desktop-entries; do not edit."
        echo "# Source: ${af##*/}"
        echo "[Desktop Entry]"
        echo "Type=Application"
        echo "Name=$name"
        if [[ -n "$description" ]]; then
            echo "Comment=$description"
        else
            echo "Comment=SerenityOS application"
        fi
        echo "Exec=$exec_line"
        echo "TryExec=$try_exec"
        if [[ -n "$icon_entry" ]]; then
            echo "Icon=$icon_entry"
        fi
        echo "Categories=$(xdg_category "$category")"
        if [[ -n "$mime_types" ]]; then
            echo "MimeType=$mime_types"
        fi
        echo "Terminal=$terminal"
        if is_sde_only_app "$executable_name"; then
            echo "OnlyShowIn=SDE;"
        fi
        echo "X-SerenityOS=true"
    } > "$APPS_DIR/${ID_PREFIX}${desktop_id}.desktop"
    count=$((count + 1))
done

echo "generate-desktop-entries: wrote $count entries to $APPS_DIR"

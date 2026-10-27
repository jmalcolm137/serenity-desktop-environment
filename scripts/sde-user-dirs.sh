#!/usr/bin/env bash
# XDG User Directories helper (the freedesktop user-dirs spec).
#
#   sde-user-dirs.sh --update          create user-dirs.dirs + the directories
#   sde-user-dirs.sh --query DESKTOP   print the configured path (xdg-user-dir)
#   sde-user-dirs.sh DESKTOP           same as --query
#
# The session runs --update once at login. Applications read the file through
# Core::StandardPaths (the framework patch), so FileManager/FilePicker common
# locations follow it; the environment variables remain the highest priority.
set -euo pipefail

home="${HOME:?HOME is not set}"
config="${XDG_CONFIG_HOME:-$home/.config}"
file="$config/user-dirs.dirs"

key_for() {
    case "$1" in
        XDG_*_DIR) printf '%s' "$1" ;;
        DESKTOP) printf 'XDG_DESKTOP_DIR' ;;
        DOWNLOAD|DOWNLOADS) printf 'XDG_DOWNLOAD_DIR' ;;
        TEMPLATES) printf 'XDG_TEMPLATES_DIR' ;;
        PUBLICSHARE) printf 'XDG_PUBLICSHARE_DIR' ;;
        DOCUMENTS) printf 'XDG_DOCUMENTS_DIR' ;;
        MUSIC) printf 'XDG_MUSIC_DIR' ;;
        PICTURES) printf 'XDG_PICTURES_DIR' ;;
        VIDEOS) printf 'XDG_VIDEOS_DIR' ;;
        *) printf '%s' "$1" ;;
    esac
}

default_for() {
    case "$1" in
        XDG_DESKTOP_DIR) printf '%s/Desktop' "$home" ;;
        XDG_DOWNLOAD_DIR) printf '%s/Downloads' "$home" ;;
        XDG_TEMPLATES_DIR) printf '%s/Templates' "$home" ;;
        XDG_PUBLICSHARE_DIR) printf '%s/Public' "$home" ;;
        XDG_DOCUMENTS_DIR) printf '%s/Documents' "$home" ;;
        XDG_MUSIC_DIR) printf '%s/Music' "$home" ;;
        XDG_PICTURES_DIR) printf '%s/Pictures' "$home" ;;
        XDG_VIDEOS_DIR) printf '%s/Videos' "$home" ;;
    esac
}

read_dir() {
    local key="$1" value=""
    if [[ -f "$file" ]]; then
        value="$(sed -n -E "s/^[[:space:]]*${key}=[\"']?([^\"']*)[\"']?[[:space:]]*$/\1/p" "$file" | tail -n 1)"
        value="${value//\$\{HOME\}/$home}"
        value="${value//\$HOME/$home}"
    fi
    [[ -n "$value" ]] || value="$(default_for "$key")"
    printf '%s\n' "$value"
}

update() {
    mkdir -p "$config"
    if [[ ! -f "$file" ]]; then
        {
            printf '%s\n' '# This file is written by sde-user-dirs.sh (XDG User Directories).'
            printf '%s\n' '# Edit a line to move a standard directory; it is kept on the next run.'
            for key in XDG_DESKTOP_DIR XDG_DOWNLOAD_DIR XDG_TEMPLATES_DIR XDG_PUBLICSHARE_DIR \
                XDG_DOCUMENTS_DIR XDG_MUSIC_DIR XDG_PICTURES_DIR XDG_VIDEOS_DIR; do
                printf '%s="$HOME/%s"\n' "$key" "$(basename "$(default_for "$key")")"
            done
        } > "$file"
    fi
    for key in XDG_DESKTOP_DIR XDG_DOWNLOAD_DIR XDG_TEMPLATES_DIR XDG_PUBLICSHARE_DIR \
        XDG_DOCUMENTS_DIR XDG_MUSIC_DIR XDG_PICTURES_DIR XDG_VIDEOS_DIR; do
        local dir
        dir="$(read_dir "$key")"
        [[ "$dir" == "$home" ]] && continue
        mkdir -p "$dir"
    done
}

case "${1:-}" in
    --query|-q)
        [[ -n "${2:-}" ]] || { echo "usage: $0 --query NAME" >&2; exit 2; }
        read_dir "$(key_for "$2")"
        ;;
    --update|-u|"")
        update
        ;;
    -h|--help)
        sed -n '2,11p' "$0"
        ;;
    *)
        read_dir "$(key_for "$1")"
        ;;
esac

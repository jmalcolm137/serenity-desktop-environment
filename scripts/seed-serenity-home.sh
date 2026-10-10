#!/usr/bin/env bash
# Seed the SerenityOS skeleton user config (Base/home/anon/.config) into the
# session's config directory.
#
# On SerenityOS a new user's home is populated from the Base/home skeleton, so
# apps that read their default configuration at startup find it. On the host the
# config directory starts empty, so apps that TRY() a config read abort —
# Browser, for example, needs BrowserContentFilters.txt and
# BrowserAutoplayAllowlist.txt. Copy only the files that are missing, so a user's
# own configuration always wins.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
SERENITY="${SERENITY:-$LIBGUI_WAYLAND_ROOT/serenity}"
SKELETON="$SERENITY/Base/home/anon/.config"
CONFIG_DIR="${XDG_CONFIG_HOME:-$HOME/.config}"

[[ -d "$SKELETON" ]] || exit 0
mkdir -p "$CONFIG_DIR"

for file in "$SKELETON"/*; do
    [[ -f "$file" ]] || continue
    name="$(basename "$file")"
    [[ -e "$CONFIG_DIR/$name" ]] && continue
    cp "$file" "$CONFIG_DIR/$name"
done

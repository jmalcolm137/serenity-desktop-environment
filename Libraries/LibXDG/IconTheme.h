/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Optional.h>
#include <AK/StringView.h>

namespace XDG {

// freedesktop.org Icon Theme Specification (v0.13):
//   - Searches $XDG_DATA_HOME/icons, $HOME/.icons, $XDG_DATA_DIRS/icons, and
//     /usr/share/pixmaps.
//   - Parses index.theme (Inherits=, Directories=, ScaledDirectories=, Size=,
//     Scale=, MinSize=, MaxSize=, Type=Fixed|Scalable|Threshold).
//   - Follows Inherits= recursively and always falls back to "hicolor", then
//     /usr/share/pixmaps.
//   - Returns an absolute file path (PNG preferred, then SVG) or empty.
Optional<ByteString> find_icon(
    StringView icon_name,
    int desired_size,
    int desired_scale = 1,
    StringView theme_name = "hicolor"sv);

// Resolve an application icon from a Wayland app_id or X11 WM_CLASS:
//   1. Looks up `<app_id>.desktop` (and lowercase variant) via DesktopEntry.
//   2. Resolves its `Icon=` field through `find_icon()`.
//   3. Falls back to `find_icon(app_id)` directly.
Optional<ByteString> find_app_icon(
    StringView app_id,
    int desired_size,
    int desired_scale = 1,
    StringView theme_name = "hicolor"sv);

}

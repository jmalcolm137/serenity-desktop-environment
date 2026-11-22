/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Optional.h>
#include <AK/StringView.h>
#include <AK/Vector.h>

namespace XDG {

struct DesktopAction {
    ByteString id;
    ByteString name;
    ByteString icon;
    ByteString exec;
};

struct DesktopEntry {
    ByteString id; // e.g. "firefox.desktop" or "org.kde.dolphin.desktop"
    ByteString file_path;
    ByteString type;
    ByteString name;
    ByteString generic_name;
    ByteString comment;
    ByteString icon;
    ByteString exec;
    ByteString try_exec;
    ByteString path; // working directory
    Vector<ByteString> categories;
    Vector<ByteString> mime_types;
    Vector<ByteString> only_show_in;
    Vector<ByteString> not_show_in;
    Vector<DesktopAction> actions;
    bool hidden { false };
    bool no_display { false };
    bool terminal { false };
    bool startup_notify { false };
    bool dbus_activatable { false };

    // Whether this entry should be considered runnable on the given desktops.
    bool is_available_on(Vector<ByteString> const& desktops) const;

    // Expand `Exec=` with Desktop Entry field codes (%f, %F, %u, %U, %i, %c, %k, %%).
    Vector<ByteString> expand_exec(Vector<ByteString> const& args = {}) const;
};

// Parse a single .desktop file on disk.
Optional<DesktopEntry> parse_desktop_entry(ByteString const& id, ByteString const& file_path);

// Look up a desktop entry by ID (e.g. "firefox.desktop") across
// $XDG_DATA_HOME/applications and $XDG_DATA_DIRS/applications (including
// subdirectory flattening with '-').
Optional<DesktopEntry> find_desktop_entry(StringView desktop_id);

// Enumerate all desktop entries across $XDG_DATA_HOME/applications and
// $XDG_DATA_DIRS/applications in priority order (first occurrence of an ID wins).
Vector<DesktopEntry> all_desktop_entries();

}

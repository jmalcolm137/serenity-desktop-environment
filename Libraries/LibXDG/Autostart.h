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

struct AutostartEntry {
    ByteString id; // Desktop file basename (e.g. "foo.desktop")
    ByteString name;
    ByteString desktop_file_path;
    Vector<ByteString> argv;
    Optional<ByteString> working_directory;
    bool terminal { false };
};

// Parse a Desktop Entry `Exec=` value into an argv vector, handling quoting,
// backslash escapes and stripping field codes (%f, %u, ...; %% -> %).
Vector<ByteString> parse_exec_command(StringView exec_line);

// Check whether a TryExec value resolves to an executable file (absolute path
// or searched on $PATH).
bool try_exec_available(StringView try_exec);

// Discover enabled autostart entries according to the freedesktop.org Desktop
// Application Autostart Specification:
//   - Scans $XDG_CONFIG_HOME/autostart (or `user_autostart_dir_override`) first,
//     then each $XDG_CONFIG_DIRS/autostart directory in priority order.
//   - A higher-priority file with the same basename overrides (or hides, via
//     Hidden=true or X-GNOME-Autostart-enabled=false) lower-priority ones.
//   - Enforces Type=Application, OnlyShowIn, NotShowIn, and TryExec.
Vector<AutostartEntry> discover_autostart_entries(
    Optional<ByteString> user_autostart_dir_override = {},
    Optional<Vector<ByteString>> desktops_override = {});

}

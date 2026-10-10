/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/ByteString.h>
#include <AK/Function.h>
#include <AK/Span.h>
#include <AK/Vector.h>

// The session's persisted configuration and state, rooted at
// $XDG_CONFIG_HOME/sde (SDE_STATE_DIR overrides). The wallpaper/theme state the
// Display Settings app writes and the compositor config (WindowServer.ini) share
// this one directory, so features add keys here rather than inventing a second
// mechanism. See docs/09-r3-implementation-plan.md §2.3.
namespace Config {

// --- state directory (shared with the wallpaper/theme state) ---------------

ByteString state_directory();
ByteString state_file(StringView name);

ByteString read_state(StringView name);
ByteBuffer read_state_bytes(StringView name);
void write_state(StringView name, ReadonlyBytes bytes);

// --- compositor config (WindowServer.ini) -----------------------------------

// [Workspaces] from WindowServer.ini - the window-stack grid (SerenityOS
// default 2x2). WS1 reads this.
struct Workspaces {
    int rows { 2 };
    int columns { 2 };
};

// Load the effective configuration: the SerenityOS defaults
// ($SERENITY_RES_ROOT/../etc/WindowServer.ini) overlaid by the user's file in the
// state directory. Unknown keys are ignored.
void load();

// Watch the config file and re-load on change; on_reload() runs after each load.
// A single edit produces several inotify events, so reloads are coalesced.
void watch(Function<void()> on_reload);

Workspaces const& workspaces();

// [Applet] Order - the configured applet order (comma-separated). WS4 reads this.
Vector<ByteString> const& applet_order();

// The user's config file path, for tooling and error messages.
ByteString file_path();

}

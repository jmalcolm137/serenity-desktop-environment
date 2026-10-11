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
// mechanism. See docs/14-r4.4-config.md for the schema.
//
// WindowServer.ini schema (all keys optional; the SerenityOS defaults under
// $SERENITY_RES_ROOT/../etc are overlaid by the user's file):
//
//   [Workspaces] Rows, Columns          1..16   (window-stack grid)
//   [Applet]     Order                          (comma-separated)
//   [Tiling]     EdgeThreshold          1..512  (px from an edge that arms snap)
//                DragToEdge             bool    (drag a window to an edge to snap)
//   [Focus]      FollowsMouse           bool    (focus on hover, not just click)
//   [Effects]    WindowGeometry        bool     (geometry HUD while moving/resizing)
//   [Output]     DefaultScale          0..4      (0 = auto integer HiDPI scale)
//
// Invalid values are rejected (the previous value is kept) and reported through
// diagnostics(); see `scripts/sde-config validate`.
namespace Config {

// --- state directory (shared with the wallpaper/theme state) ---------------

ByteString state_directory();
ByteString state_file(StringView name);

ByteString read_state(StringView name);
ByteBuffer read_state_bytes(StringView name);
void write_state(StringView name, ReadonlyBytes bytes);

// --- compositor config (WindowServer.ini) -----------------------------------

// The window-stack grid (SerenityOS default 2x2).
struct Workspaces {
    int rows { 2 };
    int columns { 2 };
};

// Snap/tile behaviour (WS2).
struct Tiling {
    int edge_threshold { 48 };
    bool drag_to_edge { true };
};

// Focus policy.
struct Focus {
    bool follows_mouse { false };
};

// System effects (SerenityOS SystemEffects subset).
struct Effects {
    bool window_geometry { true };
};

// Output defaults.
struct Output {
    int default_scale { 0 }; // 0 = auto (integer HiDPI)
};

// Load the effective configuration: the SerenityOS defaults
// ($SERENITY_RES_ROOT/../etc/WindowServer.ini) overlaid by the user's file in the
// state directory. Invalid values are rejected and recorded in diagnostics().
void load();

// Watch the config file and re-load on change; on_reload() runs after each load.
// A single edit produces several inotify events, so reloads are coalesced.
void watch(Function<void()> on_reload);

Workspaces const& workspaces();

// [Applet] Order - the configured applet order (comma-separated). WS4 reads this.
Vector<ByteString> const& applet_order();

Tiling const& tiling();
Focus const& focus();
Effects const& effects();
Output const& output();

// Warnings from the last load() (invalid values, etc.), one per issue. Empty when
// the config is clean. `sde-config validate` prints these.
Vector<ByteString> const& diagnostics();

// The user's config file path, for tooling and error messages.
ByteString file_path();

}

/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGfx/Color.h>
#include <LibGfx/Forward.h>

// The Wi-Fi glyphs, reproduced from KDE Plasma's Breeze icon theme so the
// applet matches plasma-nm. KDE's `ConnectionIcon` picks an icon name from the
// NetworkManager state (libs/connectionicon.cpp); we draw the same shapes with
// LibGfx instead of shipping theme assets, exactly as the battery applet does.
//
// The design is the Breeze 22x22 "network-wireless-*" fan: a ring dot at the
// bottom centre with three concentric arc-bands above it. The signal level
// fills in the bands (network-wireless-0/20/40/60/80/100); the other states
// overlay an X, a slash or a spinner.
namespace WifiIcon {

enum class State {
    // NetworkManager is off/asleep, or there is no usable device.
    Unavailable,
    // Wi-Fi is administratively disabled (soft or hardware).
    Off,
    // Wi-Fi is on but nothing is connected (plasma-nm's "disconnected").
    Disconnected,
    // An activation is in progress (plasma-nm's "acquiring").
    Connecting,
    // Connected; `level` carries the signal strength.
    Connected,
};

// plasma-nm's strength -> icon-level mapping (connectionicon.cpp).
int level_for_strength(int strength);

// Draws a Wi-Fi glyph filling `rect`. `level` (0/20/40/60/80/100) is used for
// State::Connected. The glyph is monochrome in `color` (the panel's symbolic
// icon), with the red accents Breeze uses for the disconnected/off states.
void paint(Gfx::Painter&, Gfx::IntRect const& rect, State, int level, Gfx::Color color);

// Renders the glyph to a fresh transparent bitmap (for GUI::Button icons).
ErrorOr<NonnullRefPtr<Gfx::Bitmap>> make_bitmap(Gfx::IntSize, State, int level, Gfx::Color);

}

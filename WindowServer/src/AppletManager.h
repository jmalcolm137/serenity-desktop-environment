/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "Server.h"

#include <AK/ByteString.h>
#include <LibGfx/Point.h>
#include <LibGfx/Size.h>

// The applet area (SerenityOS AppletManager). Serenity's WindowServer collects
// the applet windows, lays them out right-to-left in the configured order
// ([Applet] Order), sizes the area and tells the Taskbar via
// applet_area_size_changed; the Taskbar replies with set_applet_area_position.
// Here each applet is a wlr-layer-shell surface ("serenity-applet[:<name>]") and
// the compositor positions it (docs/05-r3-plan.md §4 WS4).
namespace AppletManager {

struct HostHooks {
    // Tell the WindowManagers (the Taskbar) the new applet-area size, mirroring
    // WindowManager::tell_wms_applet_area_size_changed.
    void (*applet_area_size_changed)(Server&, Gfx::IntSize) { nullptr };
    // The primary output's usable area; the applet area falls back to its
    // bottom-right corner until the Taskbar reports a position.
    Gfx::IntRect (*primary_usable_area)(Server&) { nullptr };
    // Report an applet's real output position to its client (serenity_toplevel
    // position), so widget/screen-relative coordinates (tooltips) are exact.
    void (*report_position)(LayerSurface&) { nullptr };
};
void set_host_hooks(HostHooks);

// The Taskbar reports the applet area's on-screen origin (its top-left).
void set_position(Server&, Gfx::IntPoint);

// The current applet-area size (0x0 before any applet has mapped). A WM that
// registers after the applets have already mapped asks for this, so it can
// report the area position even though it missed the size-changed events.
Gfx::IntSize area_size();

// Recompute the layout (order, sizes) and, if the total size changed, tell the
// Taskbar. Repositions the applet surfaces. The layer-shell arrange pass resets
// the nodes' positions, so this runs at the end of every arrange.
void relayout(Server&);

// True (and returns the name) for a layer surface in the applet namespace.
bool parse_namespace(char const* layer_namespace, ByteString& name);

}

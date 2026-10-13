/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "Model.h"

#include <AK/NonnullRefPtr.h>
#include <LibGfx/Forward.h>

// The Alt+Tab window switcher (SerenityOS WindowSwitcher). Compositor-side: it
// lists the current workspace's windows (Super+Tab: all of them), shows a
// Serenity-style overlay of live thumbnails, and activates the selection when the
// modifier is released. See docs/05-r3-plan.md §4 WS3.
namespace WindowSwitcher {

enum class Mode {
    CurrentWorkspace,
    AllWorkspaces,
};

// Operations the switcher calls back into the compositor.
struct HostHooks {
    Gfx::Palette const& (*palette)(Server&) { nullptr };
    void (*focus_toplevel)(Server&, Toplevel*) { nullptr };
    // Switch stacks (all-workspaces mode): WindowsManager's switch_workspace.
    void (*switch_workspace)(Server&, u32 row, u32 column, bool carry_focused) { nullptr };
    // The theme's overlay rect shadow (a 9-slice bitmap), or null.
    Gfx::Bitmap const* (*overlay_shadow)() { nullptr };
    // Repaint a toplevel's frame (used to show/hide the highlight state).
    void (*update_frame)(Toplevel&) { nullptr };
    // X11 (Xwayland) windows join the same list: focus, minimize and re-render
    // their frame (the Highlighted state). See docs/20-xwayland-next-steps.md.
    void (*focus_x11)(Server&, X11Toplevel*) { nullptr };
    void (*set_minimized_x11)(Server&, X11Toplevel&, bool minimized) { nullptr };
    void (*update_x11_frame)(X11Toplevel&) { nullptr };
};
void set_host_hooks(HostHooks);

bool is_visible();

// The toplevel currently highlighted in the switcher (drawn in the theme's
// "Highlighted" window state, as in SerenityOS), or null.
Toplevel* highlighted();
// The X11 window currently highlighted, or null (the switcher lists both kinds).
X11Toplevel* highlighted_x11();

// Pointer input while the switcher is open (hover highlights, click selects).
// Return true when consumed (the switcher grabs the pointer while visible).
bool handle_pointer_motion(Server&, double x, double y);
bool handle_pointer_button(Server&, u32 button, u32 state);

// Handle a key. Alt+Tab / Super+Tab advance (Shift reverses); releasing the
// modifier activates the selection; Escape cancels. Returns true when consumed.
bool handle_key(Server&, u32 keysym, u32 modifiers, bool pressed);

// Re-snapshot the thumbnails while the switcher is open (e.g. on damage).
void refresh(Server&);

void hide();

}

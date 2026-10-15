/*
 * Copyright (c) 2026, the Serenity Desktop Environment developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Compositor.h"
#include "Focus.h"
#include "Frame.h"
#include "InputMethod.h"
#include "Keyboard.h"
#include "Pointer.h"
#include "WindowManager.h"
#include "XWaylandFrame.h"

#include "Wlroots.h"

static Toplevel* toplevel_from_handle(wlr_foreign_toplevel_handle_v1* handle)
{
    return static_cast<Toplevel*>(handle->data);
}

// Keep exactly one window active at a time: deactivate every window (xdg or X11)
// except the one taking focus. This scans the window lists rather than trusting the
// seat's previous focused surface, which is unreliable — the seat has no focused
// surface when there is no keyboard (headless, or the first windows of a session),
// and it is cleared when a layer surface (a client menu / panel) takes focus. A
// window left active by a missed transition is therefore fixed up here, and the
// change is reported to the WindowManagerServer so the Taskbar button follows.
void deactivate_other_windows(Server& server, Toplevel* keep_toplevel, X11Toplevel* keep_x11)
{
    for (auto& other : server.toplevels) {
        if (other.ptr() == keep_toplevel || !other->frame_active)
            continue;
        other->frame_active = false;
        wlr_xdg_toplevel_set_activated(other->xdg_toplevel, false);
        if (other->xdg_toplevel->base->surface->mapped)
            toplevel_update_frame(*other);
        WindowManager::emit_window_state(*other);
        dbgln("WindowServer: window '{}' deactivated", other->xdg_toplevel->title ? other->xdg_toplevel->title : "");
    }
    for (auto& other : server.xwayland.toplevels) {
        if (other.ptr() == keep_x11 || !other->frame_active)
            continue;
        other->frame_active = false;
        wlr_xwayland_surface_activate(other->xsurface, false);
        if (other->frame_decorated)
            x11_update_frame(*other);
        if (other->wm_id != 0)
            WindowManager::emit_window_state(*other);
        dbgln("WindowServer: X11 window '{}' deactivated", other->title);
    }
}

// Keep a transient (a dialog) above its parent, so raising the parent never
// covers its children (SerenityOS stacks a transient on top of its parent).
static void raise_transients_above(Server& server, Toplevel* parent)
{
    for (auto& candidate : server.toplevels) {
        if (candidate->mapped && candidate->transient_parent == parent) {
            wlr_scene_node_raise_to_top(&candidate->scene_tree->node);
            raise_transients_above(server, candidate.ptr());
        }
    }
}

void focus_toplevel(Server& server, Toplevel* toplevel)
{
    if (!toplevel)
        return;

    toplevel->focus_order = ++g_focus_order_counter;

    // Exactly one window is active (see deactivate_other_windows). Run this even
    // when the keyboard focus did not change, so a window left active is fixed up.
    deactivate_other_windows(server, toplevel, nullptr);

    // SerenityOS: activating a window closes any fragile (popup/menu) window.
    dismiss_fragile_windows(server, "window activated"sv);

    wlr_surface* previous_surface = server.seat->keyboard_state.focused_surface;
    wlr_surface* surface = toplevel->xdg_toplevel->base->surface;
    if (previous_surface == surface && toplevel->frame_active)
        return;

    // A toplevel taking focus ends any on_demand layer surface's keyboard focus.
    server.keyboard_focus_layer = nullptr;

    // A focus change retargets the keyboard; stop repeating into the old client.
    stop_key_repeat();

    wlr_scene_node_raise_to_top(&toplevel->scene_tree->node);

    // Always-on-top windows stay above normal ones.
    for (auto& candidate : server.toplevels) {
        if (candidate->always_on_top && !candidate->minimized)
            wlr_scene_node_raise_to_top(&candidate->scene_tree->node);
    }

    // A dialog/transient stays above its parent.
    raise_transients_above(server, toplevel);
    wlr_xdg_toplevel_set_activated(toplevel->xdg_toplevel, true);
    toplevel->frame_active = true;
    toplevel_update_frame(*toplevel);
    // Report the new active window so the Taskbar marks its button (and sinks the
    // others).
    WindowManager::emit_window_state(*toplevel);
    if (toplevel->foreign_handle)
        wlr_foreign_toplevel_handle_v1_set_activated(toplevel->foreign_handle, true);

    auto* keyboard = wlr_seat_get_keyboard(server.seat);
    if (keyboard)
        wlr_seat_keyboard_notify_enter(server.seat, surface, keyboard->keycodes, keyboard->num_keycodes, &keyboard->modifiers);
    // R5 IME: hand the new focus to text-input-v3 and the input method.
    InputMethod::set_focus(server, surface);
}

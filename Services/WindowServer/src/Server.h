/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "Model.h"

// The compositor's state is grouped by the subsystem that owns it. Each group is
// defined in that subsystem's state header, and its wlroots objects and listeners
// are created and wired by the subsystem's initialize_*() (Keyboard.cpp,
// Pointer.cpp, XWayland.cpp, ...) and unhooked by its finish_*(). Server holds
// the groups plus the core every subsystem shares; other subsystems reach a
// group's state through the owner's functions, not its fields.
#include "InputState.h"
#include "PointerState.h"
#include "FrameState.h"
#include "DragState.h"
#include "TabletState.h"
#include "ImeState.h"
#include "XWaylandState.h"
#include "InteropState.h"
#include "OutputManagementState.h"
#include "IdleState.h"
#include "LockState.h"

// Unhook a listener a subsystem may have wired. Subsystems only wire the
// listeners for the globals they created, and wl_list_remove() leaves a removed
// link null, so this is safe for a listener that was never added or is already
// gone, and a finish_*() can unhook every listener its state holds.
inline void remove_listener(wl_listener& listener)
{
    if (listener.link.next)
        wl_list_remove(&listener.link);
}

struct Server {
    wl_display* display { nullptr };
    wlr_backend* backend { nullptr };
    wlr_renderer* renderer { nullptr };
    wlr_allocator* allocator { nullptr };
    wlr_compositor* compositor { nullptr };
    wlr_scene* scene { nullptr };
    wlr_scene_output_layout* scene_layout { nullptr };
    wlr_output_layout* output_layout { nullptr };
    // Per-layer scene trees, in bottom-to-top order, so layer surfaces stack
    // correctly relative to ordinary windows (the flat scene root does not).
    wlr_scene_tree* background_tree { nullptr };
    wlr_scene_tree* bottom_tree { nullptr };
    wlr_scene_tree* windows_tree { nullptr };
    wlr_scene_tree* top_tree { nullptr };
    wlr_scene_tree* overlay_tree { nullptr };
    // Drag icons render above every layer (DragAndDrop.cpp).
    wlr_scene_tree* drag_icon_tree { nullptr };
    wlr_cursor* cursor { nullptr };
    wlr_xcursor_manager* cursor_manager { nullptr };
    wlr_seat* seat { nullptr };

    // Outputs (Output.cpp).
    Vector<NonnullOwnPtr<Output>> outputs;
    wlr_output* primary_output { nullptr };
    wl_listener new_output { };

    // The shell: xdg toplevels, popups and decorations (XdgShell.cpp) and layer
    // surfaces (LayerShell.cpp).
    wlr_xdg_shell* xdg_shell { nullptr };
    wlr_layer_shell_v1* layer_shell { nullptr };
    Vector<NonnullOwnPtr<Toplevel>> toplevels;
    Vector<NonnullOwnPtr<Popup>> popups;
    Vector<NonnullOwnPtr<Decoration>> decorations;
    Vector<NonnullOwnPtr<LayerSurface>> layer_surfaces;
    // X11 windows (XWayland.cpp manages them). A window list like toplevels: the
    // window manager, focus, the switcher and the outputs all walk it.
    Vector<NonnullOwnPtr<X11Toplevel>> x11_toplevels;
    // The on_demand layer surface that currently holds keyboard focus (a client
    // popup such as the Wi-Fi network list or its passphrase field), if any, so
    // focus can be restored when it unmaps.
    LayerSurface* keyboard_focus_layer { nullptr };
    wl_listener new_xdg_toplevel { };
    wl_listener new_xdg_popup { };
    wl_listener new_layer_surface { };
    wl_listener new_toplevel_decoration { };
    // serenity_window_manager resources, so the compositor can ask a client to
    // dismiss its menus (see maybe_dismiss_layer_menus).
    Vector<wl_resource*> serenity_window_managers;
    bool show_desktop { false };
    // The (single) window menu, if any is open.
    WindowMenu window_menu;

    InputState input;
    PointerState pointer;
    GrabState grab;
    FramePressState frame_press;
    DragState drag;
    TabletState tablet;
    ImeState ime;
    XWaylandState xwayland;
    InteropState interop;
    OutputManagementState output_management;
    IdleState idle;
    LockState lock;
};

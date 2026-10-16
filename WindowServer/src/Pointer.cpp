/*
 * Copyright (c) 2026, the Serenity Desktop Environment developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Compositor.h"
#include "CursorHighlight.h"
#include "DragAndDrop.h"
#include "Focus.h"
#include "Frame.h"
#include "Idle.h"
#include "InputCapture.h"
#include "Pointer.h"
#include "SessionLock.h"
#include "WindowManager.h"
#include "WindowMenu.h"
#include "WindowSwitcher.h"
#include "XWayland.h"
#include "XWaylandFrame.h"
#include "XdgShell.h"

#include <fcntl.h>
#include <linux/input-event-codes.h>

#include "Wlroots.h"

extern "C" {
#include "serenity-window-server-protocol.h"
}

// --- cursor / pointer -----------------------------------------------------

Toplevel* toplevel_at(Server& server, double lx, double ly, wlr_surface** surface, double* sx, double* sy)
{
    wlr_scene_node* node = wlr_scene_node_at(&server.scene->tree.node, lx, ly, sx, sy);
    if (!node || node->type != WLR_SCENE_NODE_BUFFER)
        return nullptr;

    auto* scene_buffer = wlr_scene_buffer_from_node(node);
    auto* scene_surface = wlr_scene_surface_try_from_buffer(scene_buffer);
    if (!scene_surface)
        return nullptr;

    *surface = scene_surface->surface;
    // Walk up to the scene tree that belongs to an xdg toplevel. We can't just
    // stop at the first tree with node.data set: layer surfaces (the Taskbar)
    // store their own object there, so match against the known toplevel trees.
    wlr_scene_tree* tree = node->parent;
    while (tree) {
        for (auto& toplevel : server.toplevels) {
            if (toplevel->scene_tree == tree) {
                // Input behind a blocking modal is blocked.
                if (WindowManager::is_toplevel_blocked(server, *toplevel))
                    return nullptr;
                return toplevel.ptr();
            }
        }
        tree = tree->node.parent;
    }
    return nullptr;
}

void reset_cursor_mode(Server& server)
{
    server.grab.mode = CursorMode::Passthrough;
    server.grab.toplevel = nullptr;
    server.grab.x11 = nullptr;
    // The interactive grab ended: hide the geometry HUD (WS2).
    WindowManager::hide_geometry_hud();
}

void begin_grab(Server& server, Toplevel* toplevel, X11Toplevel* x11, CursorMode mode, double offset_x, double offset_y, wlr_box const& box, uint32_t edges)
{
    server.grab.toplevel = toplevel;
    server.grab.x11 = x11;
    server.grab.mode = mode;
    server.grab.x = offset_x;
    server.grab.y = offset_y;
    if (mode != CursorMode::Move) {
        server.grab.box = box;
        server.grab.edges = edges;
    }
    wlr_cursor_set_xcursor(server.cursor, server.cursor_manager, mode == CursorMode::Move ? "grabbing" : resize_cursor_name(edges));
}

Toplevel* moving_toplevel(Server const& server)
{
    return server.grab.mode == CursorMode::Move ? server.grab.toplevel : nullptr;
}

void cancel_frame_grab(Server& server)
{
    server.grab.from_frame = false;
}

void forget_grab_window(Server& server, Toplevel& toplevel)
{
    if (server.grab.toplevel == &toplevel)
        reset_cursor_mode(server);
}

void forget_grab_window(Server& server, X11Toplevel& toplevel)
{
    if (server.grab.x11 == &toplevel)
        server.grab.x11 = nullptr;
}

static void process_cursor_move(Server& server)
{
    if (server.grab.x11) {
        auto* toplevel = server.grab.x11;
        int x = static_cast<int>(server.cursor->x - server.grab.x);
        int y = static_cast<int>(server.cursor->y - server.grab.y);
        wlr_scene_node_set_position(&toplevel->scene_tree->node, x, y);
        wlr_xwayland_surface_configure(toplevel->xsurface, x, y, toplevel->xsurface->width, toplevel->xsurface->height);
        return;
    }
    auto* toplevel = server.grab.toplevel;
    if (!toplevel)
        return;
    wlr_scene_node_set_position(&toplevel->scene_tree->node, server.cursor->x - server.grab.x, server.cursor->y - server.grab.y);
    // Show the tile preview when the pointer reaches a screen edge (WS2).
    WindowManager::update_move_snap(server, *toplevel);
}

static void process_cursor_resize(Server& server)
{
    if (server.grab.x11) {
        auto* toplevel = server.grab.x11;
        double border_x = server.cursor->x - server.grab.x;
        double border_y = server.cursor->y - server.grab.y;
        int new_left = server.grab.box.x;
        int new_right = server.grab.box.x + server.grab.box.width;
        int new_top = server.grab.box.y;
        int new_bottom = server.grab.box.y + server.grab.box.height;
        if (server.grab.edges & WLR_EDGE_TOP) {
            new_top = static_cast<int>(border_y);
            if (new_top >= new_bottom) new_top = new_bottom - 1;
        } else if (server.grab.edges & WLR_EDGE_BOTTOM) {
            new_bottom = static_cast<int>(border_y);
            if (new_bottom <= new_top) new_bottom = new_top + 1;
        }
        if (server.grab.edges & WLR_EDGE_LEFT) {
            new_left = static_cast<int>(border_x);
            if (new_left >= new_right) new_left = new_right - 1;
        } else if (server.grab.edges & WLR_EDGE_RIGHT) {
            new_right = static_cast<int>(border_x);
            if (new_right <= new_left) new_right = new_left + 1;
        }
        // new_left/top/right/bottom are frame edges; the client's size is the
        // content (frame minus the decorations).
        int const frame_left = toplevel->frame_decorated ? toplevel->frame_left : 0;
        int const frame_top = toplevel->frame_decorated ? toplevel->frame_top : 0;
        int const frame_right = toplevel->frame_decorated ? toplevel->frame_right : 0;
        int const frame_bottom = toplevel->frame_decorated ? toplevel->frame_bottom : 0;
        int content_w = new_right - new_left - frame_left - frame_right;
        int content_h = new_bottom - new_top - frame_top - frame_bottom;
        if (content_w < 1) content_w = 1;
        if (content_h < 1) content_h = 1;
        wlr_scene_node_set_position(&toplevel->scene_tree->node, new_left, new_top);
        wlr_xwayland_surface_configure(toplevel->xsurface, new_left, new_top, content_w, content_h);
        return;
    }
    auto* toplevel = server.grab.toplevel;
    if (!toplevel)
        return;

    double border_x = server.cursor->x - server.grab.x;
    double border_y = server.cursor->y - server.grab.y;
    int new_left = server.grab.box.x;
    int new_right = server.grab.box.x + server.grab.box.width;
    int new_top = server.grab.box.y;
    int new_bottom = server.grab.box.y + server.grab.box.height;

    if (server.grab.edges & WLR_EDGE_TOP) {
        new_top = border_y;
        if (new_top >= new_bottom)
            new_top = new_bottom - 1;
    } else if (server.grab.edges & WLR_EDGE_BOTTOM) {
        new_bottom = border_y;
        if (new_bottom <= new_top)
            new_bottom = new_top + 1;
    }
    if (server.grab.edges & WLR_EDGE_LEFT) {
        new_left = border_x;
        if (new_left >= new_right)
            new_left = new_right - 1;
    } else if (server.grab.edges & WLR_EDGE_RIGHT) {
        new_right = border_x;
        if (new_right <= new_left)
            new_right = new_left + 1;
    }

    auto* geometry = &toplevel->xdg_toplevel->base->geometry;
    wlr_scene_node_set_position(&toplevel->scene_tree->node, new_left - geometry->x, new_top - geometry->y);
    wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, new_right - new_left, new_bottom - new_top);
    // Show the geometry HUD with the intended size while resizing (WS2).
    WindowManager::update_resize_hud(server, *toplevel, { new_left, new_top, new_right - new_left, new_bottom - new_top });
}

// zwp-pointer-constraints-v1 (R5). wlroots advertises the protocol; the
// compositor owns enforcement. It activates the constraint of the surface under
// the pointer and, while active, keeps the cursor locked to the client's hint or
// confined to its region.
struct PointerConstraintListener {
    wl_listener destroy;
    Server* server { nullptr };
};

static void pointer_constraint_destroy(wl_listener* listener, void* data)
{
    auto* constraint = static_cast<wlr_pointer_constraint_v1*>(data);
    PointerConstraintListener* entry = nullptr;
    entry = wl_container_of(listener, entry, destroy);
    if (entry->server->pointer.active_constraint == constraint)
        entry->server->pointer.active_constraint = nullptr;
    wl_list_remove(&listener->link);
    delete entry;
}

static void new_pointer_constraint(wl_listener* listener, void* data)
{
    Server* server = nullptr;
    server = wl_container_of(listener, server, pointer.new_constraint);
    auto* constraint = static_cast<wlr_pointer_constraint_v1*>(data);
    auto* entry = new PointerConstraintListener { {}, server };
    entry->destroy.notify = pointer_constraint_destroy;
    wl_signal_add(&constraint->events.destroy, &entry->destroy);
}

static void process_cursor_motion(Server& server, uint32_t time)
{
    // While the session is locked, only the lock surface receives input.
    if (session_locked(server)) {
        if (auto* lock = lock_surface_at_cursor(server)) {
            wlr_box box { };
            wlr_output_layout_get_box(server.output_layout, lock->lock_surface->output, &box);
            double x = server.cursor->x - box.x;
            double y = server.cursor->y - box.y;
            wlr_seat_pointer_notify_enter(server.seat, lock->lock_surface->surface, x, y);
            wlr_seat_pointer_notify_motion(server.seat, time, x, y);
        }
        return;
    }

    // Explicit drag-and-drop: route motion to the surface under the cursor (the
    // drag grab turns it into wl_data_device enter/motion) and follow the cursor
    // with the drag icon.
    if (drag_motion(server, time))
        return;

    if (server.window_menu.open) {
        update_window_menu_hover(server, server.cursor->x, server.cursor->y);
        wlr_seat_pointer_clear_focus(server.seat);
        wlr_cursor_set_xcursor(server.cursor, server.cursor_manager, "default");
        return;
    }

    if (server.grab.mode == CursorMode::Move) {
        process_cursor_move(server);
        wlr_cursor_set_xcursor(server.cursor, server.cursor_manager, "grabbing");
        return;
    }
    if (server.grab.mode == CursorMode::Resize) {
        process_cursor_resize(server);
        return;
    }

    // Implicit grab: a client button is down, so keep delivering motion to the
    // surface that received the press until it is released.
    if (server.pointer.grab_surface) {
        if (server.seat->pointer_state.focused_surface != server.pointer.grab_surface) {
            // The grabbed surface went away (e.g. the client disconnected).
            server.pointer.grab_surface = nullptr;
            server.pointer.buttons_down = 0;
        } else {
            wlr_seat_pointer_notify_motion(server.seat, time,
                server.cursor->x - server.pointer.grab_origin_x,
                server.cursor->y - server.pointer.grab_origin_y);
            return;
        }
    }

    auto hit = hit_test_frame(server, server.cursor->x, server.cursor->y);
    update_button_hover(server, hit);
    update_button_pressed(server, hit);
    update_x11_button_hover(server, server.cursor->x, server.cursor->y);

    double sx = 0;
    double sy = 0;
    wlr_surface* surface = nullptr;
    if (hit.region == FrameRegion::None)
        surface = window_surface_at(server, server.cursor->x, server.cursor->y, &sx, &sy);

    // Pointer constraints: activate the constraint of the surface under the
    // cursor and, while one is active, keep the cursor locked or confined.
    if (server.pointer.constraints) {
        wlr_pointer_constraint_v1* constraint = surface
            ? wlr_pointer_constraints_v1_constraint_for_surface(server.pointer.constraints, surface, server.seat)
            : nullptr;
        if (constraint != server.pointer.active_constraint) {
            if (server.pointer.active_constraint)
                wlr_pointer_constraint_v1_send_deactivated(server.pointer.active_constraint);
            server.pointer.active_constraint = constraint;
            if (constraint)
                wlr_pointer_constraint_v1_send_activated(constraint);
        }
        if (auto* active = server.pointer.active_constraint) {
            double origin_x = 0;
            double origin_y = 0;
            if (surface_global_origin(server, active->surface, &origin_x, &origin_y)) {
                if (active->type == WLR_POINTER_CONSTRAINT_V1_LOCKED) {
                    if (active->current.cursor_hint.enabled)
                        wlr_cursor_warp_closest(server.cursor, nullptr, origin_x + active->current.cursor_hint.x, origin_y + active->current.cursor_hint.y);
                } else if (active->type == WLR_POINTER_CONSTRAINT_V1_CONFINED && (active->current.committed & WLR_POINTER_CONSTRAINT_V1_STATE_REGION)) {
                    double const local_x = server.cursor->x - origin_x;
                    double const local_y = server.cursor->y - origin_y;
                    if (!pixman_region32_contains_point(&active->current.region, local_x, local_y, nullptr)) {
                        auto const* box = pixman_region32_extents(&active->current.region);
                        double const clamped_x = min(max(local_x, static_cast<double>(box->x1)), static_cast<double>(box->x2));
                        double const clamped_y = min(max(local_y, static_cast<double>(box->y1)), static_cast<double>(box->y2));
                        wlr_cursor_warp_closest(server.cursor, nullptr, origin_x + clamped_x, origin_y + clamped_y);
                    }
                }
                // Recompute surface-local coordinates for the (possibly warped) cursor.
                sx = server.cursor->x - origin_x;
                sy = server.cursor->y - origin_y;
            }
        }
    }

    if (surface) {
        if (server.seat->pointer_state.focused_surface != surface)
            wlr_cursor_set_xcursor(server.cursor, server.cursor_manager, "default");
        wlr_seat_pointer_notify_enter(server.seat, surface, sx, sy);
        wlr_seat_pointer_notify_motion(server.seat, time, sx, sy);
    } else {
        wlr_seat_pointer_clear_focus(server.seat);
    }

    // [Focus] FollowsMouse (R4.4): focus the window (xdg or X11) under the cursor.
    if (Config::focus().follows_mouse && server.grab.mode == CursorMode::Passthrough && !server.pointer.grab_surface)
        focus_window_at(server, server.cursor->x, server.cursor->y);

    // Set the cursor for our chrome last: clearing client focus above resets it
    // to the default arrow, so the resize cursor must come after.
    if (hit.region != FrameRegion::None) {
        wlr_cursor_set_xcursor(server.cursor, server.cursor_manager, hit.region == FrameRegion::Border ? resize_cursor_name(hit.edges) : "default");
    } else {
        auto x11_hit = x11_hit_test_frame(server, server.cursor->x, server.cursor->y);
        if (x11_hit.region == FrameRegion::Border)
            wlr_cursor_set_xcursor(server.cursor, server.cursor_manager, resize_cursor_name(x11_hit.edges));
    }
}

static void cursor_motion(wl_listener* listener, void* data)
{
    Server* server = nullptr;
    server = wl_container_of(listener, server, pointer.motion);
    auto* event = static_cast<wlr_pointer_motion_event*>(data);
    wlr_cursor_move(server->cursor, &event->pointer->base, event->delta_x, event->delta_y);
    // R5: relative-pointer clients (games) get the raw relative motion. The time
    // is in microseconds (wl_pointer uses milliseconds).
    if (server->pointer.relative_pointer_manager)
        wlr_relative_pointer_manager_v1_send_relative_motion(server->pointer.relative_pointer_manager, server->seat,
            static_cast<uint64_t>(event->time_msec) * 1000, event->delta_x, event->delta_y, event->unaccel_dx, event->unaccel_dy);
    move_cursor_highlight(*server);
    // The switcher grabs the pointer while it is open.
    if (WindowSwitcher::handle_pointer_motion(*server, server->cursor->x, server->cursor->y)) {
        notify_activity(*server);
        return;
    }
    if (!InputCapture::handle_pointer_motion(*server, event->time_msec))
        process_cursor_motion(*server, event->time_msec);
    notify_activity(*server);
}

static void cursor_motion_absolute(wl_listener* listener, void* data)
{
    Server* server = nullptr;
    server = wl_container_of(listener, server, pointer.motion_absolute);
    auto* event = static_cast<wlr_pointer_motion_absolute_event*>(data);
    wlr_cursor_warp_absolute(server->cursor, &event->pointer->base, event->x, event->y);
    move_cursor_highlight(*server);
    // The switcher grabs the pointer while it is open.
    if (WindowSwitcher::handle_pointer_motion(*server, server->cursor->x, server->cursor->y)) {
        notify_activity(*server);
        return;
    }
    if (!InputCapture::handle_pointer_motion(*server, event->time_msec))
        process_cursor_motion(*server, event->time_msec);
    notify_activity(*server);
}

// Touch routing, shared by real touch devices (via the cursor) and the harness's
// virtual touch (serenity-virtual-input). Coordinates are in the output layout.
void handle_touch_down(Server& server, uint32_t time_msec, int32_t touch_id, double lx, double ly)
{
    double sx = 0;
    double sy = 0;
    wlr_surface* surface = nullptr;
    if (auto* toplevel = toplevel_at(server, lx, ly, &surface, &sx, &sy))
        focus_toplevel(server, toplevel);
    if (surface)
        wlr_seat_touch_notify_down(server.seat, surface, time_msec, touch_id, sx, sy);
    notify_activity(server);
}

void handle_touch_motion(Server& server, uint32_t time_msec, int32_t touch_id, double lx, double ly)
{
    // A touch drag's icon follows the finger (the pointer cursor does not move).
    move_drag_icon(server, lx, ly);
    double sx = 0;
    double sy = 0;
    wlr_surface* surface = nullptr;
    toplevel_at(server, lx, ly, &surface, &sx, &sy);
    if (surface)
        wlr_seat_touch_notify_motion(server.seat, time_msec, touch_id, sx, sy);
    notify_activity(server);
}

void handle_touch_up(Server& server, uint32_t time_msec, int32_t touch_id)
{
    wlr_seat_touch_notify_up(server.seat, time_msec, touch_id);
    notify_activity(server);
}

void handle_touch_cancel(Server& server)
{
    // A device-level cancel: send wl_touch.cancel to every client with touch points.
    wlr_seat_client* seat_client = nullptr;
    wl_list_for_each(seat_client, &server.seat->clients, link) {
        if (!wl_list_empty(&seat_client->touches))
            wlr_seat_touch_notify_cancel(server.seat, seat_client);
    }
}

static void cursor_touch_down(wl_listener* listener, void* data)
{
    Server* server = nullptr;
    server = wl_container_of(listener, server, pointer.touch_down);
    auto* event = static_cast<wlr_touch_down_event*>(data);
    double lx = 0;
    double ly = 0;
    wlr_cursor_absolute_to_layout_coords(server->cursor, &event->touch->base, event->x, event->y, &lx, &ly);
    handle_touch_down(*server, event->time_msec, event->touch_id, lx, ly);
}

static void cursor_touch_motion(wl_listener* listener, void* data)
{
    Server* server = nullptr;
    server = wl_container_of(listener, server, pointer.touch_motion);
    auto* event = static_cast<wlr_touch_motion_event*>(data);
    double lx = 0;
    double ly = 0;
    wlr_cursor_absolute_to_layout_coords(server->cursor, &event->touch->base, event->x, event->y, &lx, &ly);
    handle_touch_motion(*server, event->time_msec, event->touch_id, lx, ly);
}

static void cursor_touch_up(wl_listener* listener, void* data)
{
    Server* server = nullptr;
    server = wl_container_of(listener, server, pointer.touch_up);
    auto* event = static_cast<wlr_touch_up_event*>(data);
    handle_touch_up(*server, event->time_msec, event->touch_id);
}

static void cursor_touch_cancel(wl_listener* listener, void* data)
{
    Server* server = nullptr;
    server = wl_container_of(listener, server, pointer.touch_cancel);
    (void)data;
    handle_touch_cancel(*server);
}

// Client menus opened from layer surfaces (the Taskbar/Desktop) are overlay layer
// surfaces with no input grab, so wlroots will not route an outside press to
// them and the client cannot dismiss them. On a press that lands on another
// client's surface, ask the menu owner to close its menus.
// SerenityOS closes "fragile" windows (popups, menus, tooltips) as soon as
// focus moves elsewhere — a new active window, the window switcher, or a
// workspace switch. SDE's client popups are client menus (mapped `serenity-menu`
// layer surfaces), so we ask their owning clients to dismiss them.
void dismiss_fragile_windows(Server& server, StringView reason)
{
    Vector<wl_client*> owners;
    for (auto& layer : server.layer_surfaces) {
        if (!layer->is_menu || !layer->layer_surface || !layer->layer_surface->surface)
            continue;
        if (!layer->layer_surface->surface->mapped)
            continue;
        auto* client = wl_resource_get_client(layer->layer_surface->resource);
        if (!owners.contains_slow(client))
            owners.append(client);
    }
    if (owners.is_empty())
        return;

    bool dismissed = false;
    for (auto* manager : server.serenity_window_managers) {
        if (!owners.contains_slow(wl_resource_get_client(manager)))
            continue;
        serenity_window_manager_send_dismiss_menus(manager);
        dismissed = true;
    }
    if (dismissed)
        dbgln("WindowServer: closed fragile windows ({})", reason);
}

static void maybe_dismiss_layer_menus(Server& server)
{
    LayerSurface* menu_layer = nullptr;
    for (auto& layer : server.layer_surfaces) {
        if (!layer->is_menu || !layer->layer_surface || !layer->layer_surface->surface)
            continue;
        if (!layer->layer_surface->surface->mapped)
            continue;
        menu_layer = layer.ptr();
        break;
    }
    if (!menu_layer)
        return;

    auto* owner_client = wl_resource_get_client(menu_layer->layer_surface->resource);
    auto* focused = server.seat->pointer_state.focused_surface;
    // A press on one of the owner's own surfaces (its button, or the menu itself)
    // is handled by the client; only presses elsewhere need dismissing.
    if (focused && wl_resource_get_client(focused->resource) == owner_client)
        return;

    bool dismissed = false;
    for (auto* manager : server.serenity_window_managers) {
        if (wl_resource_get_client(manager) == owner_client) {
            serenity_window_manager_send_dismiss_menus(manager);
            dismissed = true;
        }
    }
    if (dismissed)
        dbgln("WindowServer: dismissing client menus on an outside press");
}

// SerenityOS lets you grab a window anywhere to move it (Super+left-drag) or
// resize it (Super+right-drag), without reaching for the titlebar or an edge;
// the resize direction is the window's 3x3 hot area under the cursor.
static bool handle_super_drag_press(Server& server, uint32_t button)
{
    if (button != BTN_LEFT && button != BTN_RIGHT)
        return false;
    auto* keyboard = wlr_seat_get_keyboard(server.seat);
    if (!keyboard || !(wlr_keyboard_get_modifiers(keyboard) & WLR_MODIFIER_LOGO))
        return false;

    wlr_surface* surface = nullptr;
    double sx = 0;
    double sy = 0;
    auto* toplevel = toplevel_at(server, server.cursor->x, server.cursor->y, &surface, &sx, &sy);
    if (!toplevel) {
        // X11 windows get the same Super+<button> move/resize (KWin's Alt+drag).
        if (!x11_surface_at(server, server.cursor->x, server.cursor->y, &surface, &sx, &sy))
            return false;
        auto* x11 = x11_toplevel_for_wlr_surface(server, surface);
        if (!x11 || x11->override_redirect)
            return false;
        focus_x11_toplevel(server, x11);
        if (button == BTN_LEFT) {
            x11_begin_interactive(*x11, CursorMode::Move, 0);
            return true;
        }
        int width = x11->xsurface->width > 0 ? x11->xsurface->width : 1;
        int height = x11->xsurface->height > 0 ? x11->xsurface->height : 1;
        double relative_x = (server.cursor->x - x11->scene_tree->node.x) / width;
        double relative_y = (server.cursor->y - x11->scene_tree->node.y) / height;
        uint32_t edges = 0;
        if (relative_x < 1.0 / 3.0)
            edges |= WLR_EDGE_LEFT;
        else if (relative_x > 2.0 / 3.0)
            edges |= WLR_EDGE_RIGHT;
        if (relative_y < 1.0 / 3.0)
            edges |= WLR_EDGE_TOP;
        else if (relative_y > 2.0 / 3.0)
            edges |= WLR_EDGE_BOTTOM;
        if (edges == 0)
            return false;
        x11_begin_interactive(*x11, CursorMode::Resize, edges);
        return true;
    }
    if (toplevel->fullscreen)
        return false;

    if (button == BTN_LEFT) {
        focus_toplevel(server, toplevel);
        toplevel_begin_interactive(*toplevel, CursorMode::Move, 0);
        return true;
    }

    int width = toplevel->frame_size.width() > 0 ? toplevel->frame_size.width() : toplevel->xdg_toplevel->base->geometry.width;
    int height = toplevel->frame_size.height() > 0 ? toplevel->frame_size.height() : toplevel->xdg_toplevel->base->geometry.height;
    if (width <= 0 || height <= 0)
        return false;
    auto* node = &toplevel->scene_tree->node;
    double relative_x = (server.cursor->x - node->x) / width;
    double relative_y = (server.cursor->y - node->y) / height;
    uint32_t edges = 0;
    if (relative_x < 1.0 / 3.0)
        edges |= WLR_EDGE_LEFT;
    else if (relative_x > 2.0 / 3.0)
        edges |= WLR_EDGE_RIGHT;
    if (relative_y < 1.0 / 3.0)
        edges |= WLR_EDGE_TOP;
    else if (relative_y > 2.0 / 3.0)
        edges |= WLR_EDGE_BOTTOM;
    if (edges == 0)
        return false;

    focus_toplevel(server, toplevel);
    toplevel_begin_interactive(*toplevel, CursorMode::Resize, edges);
    return true;
}

static void cursor_button(wl_listener* listener, void* data)
{
    Server* server = nullptr;
    server = wl_container_of(listener, server, pointer.button);
    auto* event = static_cast<wlr_pointer_button_event*>(data);

    // R4.6: left-handed mode swaps the primary and secondary buttons.
    if (Config::input().mouse.buttons_switched) {
        if (event->button == BTN_LEFT)
            event->button = BTN_RIGHT;
        else if (event->button == BTN_RIGHT)
            event->button = BTN_LEFT;
    }

    notify_activity(*server);

    // The Alt+Tab switcher grabs the pointer while it is open.
    if (WindowSwitcher::handle_pointer_button(*server, event->button, event->state))
        return;

    // R5: InputCapture — while a capture is active, buttons go to the EIS client.
    if (InputCapture::handle_pointer_button(*server, event->button, event->state, event->time_msec))
        return;

    if (session_locked(*server)) {
        if (auto* lock = lock_surface_at_cursor(*server)) {
            wlr_box box { };
            wlr_output_layout_get_box(server->output_layout, lock->lock_surface->output, &box);
            double x = server->cursor->x - box.x;
            double y = server->cursor->y - box.y;
            wlr_seat_pointer_notify_enter(server->seat, lock->lock_surface->surface, x, y);
        }
        wlr_seat_pointer_notify_button(server->seat, event->time_msec, event->button, event->state);
        return;
    }

    if (server->window_menu.open) {
        if (event->state == WL_POINTER_BUTTON_STATE_PRESSED) {
            window_menu_handle_press(*server, event->button);
        } else {
            window_menu_handle_release(*server, event->button);
            // The client saw the press that opened the menu, so let it see the
            // release too and keep its button state (and our implicit grab)
            // balanced. The menu may have swallowed any number of presses; a
            // stray release is harmless to the client.
            wlr_seat_pointer_notify_button(server->seat, event->time_msec, event->button, event->state);
            server->pointer.buttons_down = 0;
            server->pointer.grab_surface = nullptr;
        }
        return;
    }

    if (event->state == WL_POINTER_BUTTON_STATE_PRESSED) {
        // Using Super with the mouse (move/resize, or a click) is not a Super
        // tap, so do not open the Taskbar menu when Super is released.
        if (auto* keyboard = wlr_seat_get_keyboard(server->seat); keyboard && (wlr_keyboard_get_modifiers(keyboard) & WLR_MODIFIER_LOGO))
            WindowManager::clear_super_tap();
        if (handle_frame_press(*server, event->button, event->time_msec)) {
            // Opening the window menu is not an interactive frame grab.
            if (!server->window_menu.open)
                server->grab.from_frame = true;
            return;
        }
        if (handle_x11_frame_press(*server, event->button)) {
            server->grab.from_frame = true;
            return;
        }
        // SerenityOS: Super+LMB moves and Super+RMB resizes, from anywhere.
        if (handle_super_drag_press(*server, event->button)) {
            server->grab.from_frame = true;
            return;
        }
    } else if (server->grab.from_frame) {
        server->grab.from_frame = false;
        // A titlebar drag has ended: tile to the previewed edge target, if any.
        if (server->grab.toplevel && server->grab.mode == CursorMode::Move)
            WindowManager::finish_move_snap(*server, *server->grab.toplevel);
        handle_frame_release(*server, event->button);
        handle_x11_frame_release(*server, event->button);
        if (server->grab.x11)
            reset_cursor_mode(*server);
        return;
    }

    // Dismiss client menus (opened from layer surfaces) on an outside press.
    if (event->state == WL_POINTER_BUTTON_STATE_PRESSED)
        maybe_dismiss_layer_menus(*server);

    auto serial = wlr_seat_pointer_notify_button(server->seat, event->time_msec, event->button, event->state);
    if (event->state == WL_POINTER_BUTTON_STATE_RELEASED) {
        // A drag has ended: tile to the previewed edge target, if any (WS2).
        if (server->grab.toplevel && server->grab.mode == CursorMode::Move)
            WindowManager::finish_move_snap(*server, *server->grab.toplevel);
        // Tell the client where its window ended up.
        if (server->grab.toplevel)
            toplevel_report_position(*server->grab.toplevel);
        if (server->pointer.buttons_down > 0)
            --server->pointer.buttons_down;
        if (server->pointer.buttons_down == 0)
            server->pointer.grab_surface = nullptr;
        reset_cursor_mode(*server);
    } else {
        double sx = 0;
        double sy = 0;
        wlr_surface* surface = window_surface_at(*server, server->cursor->x, server->cursor->y, &sx, &sy);
        focus_window_at(*server, server->cursor->x, server->cursor->y);
        // The seat accepted the press: remember the surface so the release finds
        // its way back to the same client even if the cursor moves away.
        if (serial && surface) {
            server->pointer.grab_surface = surface;
            server->pointer.grab_origin_x = server->cursor->x - sx;
            server->pointer.grab_origin_y = server->cursor->y - sy;
            ++server->pointer.buttons_down;
        }
    }
}

static void cursor_axis(wl_listener* listener, void* data)
{
    Server* server = nullptr;
    server = wl_container_of(listener, server, pointer.axis);
    auto* event = static_cast<wlr_pointer_axis_event*>(data);
    // R4.6: natural-scroll inverts the axis; the scroll step scales it.
    auto const& mouse = Config::input().mouse;
    double delta = event->delta;
    int32_t delta_discrete = event->delta_discrete;
    if (mouse.natural_scroll) {
        delta = -delta;
        delta_discrete = -delta_discrete;
    }
    if (mouse.scroll_step_size > 1) {
        delta *= mouse.scroll_step_size;
        delta_discrete *= static_cast<int32_t>(mouse.scroll_step_size);
    }
    wlr_seat_pointer_notify_axis(server->seat, event->time_msec, event->orientation, delta, delta_discrete, event->source, event->relative_direction);
    notify_activity(*server);
}

static void cursor_frame(wl_listener* listener, void*)
{
    Server* server = nullptr;
    server = wl_container_of(listener, server, pointer.frame);
    wlr_seat_pointer_notify_frame(server->seat);
}

static void seat_request_cursor(wl_listener* listener, void* data)
{
    Server* server = nullptr;
    server = wl_container_of(listener, server, pointer.seat_request_cursor);
    auto* event = static_cast<wlr_seat_pointer_request_set_cursor_event*>(data);
    if (server->seat->pointer_state.focused_client == event->seat_client)
        wlr_cursor_set_surface(server->cursor, event->surface, event->hotspot_x, event->hotspot_y);
}

static void seat_pointer_focus_change(wl_listener* listener, void* data)
{
    Server* server = nullptr;
    server = wl_container_of(listener, server, pointer.seat_pointer_focus_change);
    auto* event = static_cast<wlr_seat_pointer_focus_change_event*>(data);
    if (!event->new_surface)
        wlr_cursor_set_xcursor(server->cursor, server->cursor_manager, "default");
}

static void seat_request_set_selection(wl_listener* listener, void* data)
{
    Server* server = nullptr;
    server = wl_container_of(listener, server, pointer.seat_request_set_selection);
    auto* event = static_cast<wlr_seat_request_set_selection_event*>(data);
    wlr_seat_set_selection(server->seat, event->source, event->serial);
}

// PRIMARY selection (zwp_primary_selection_v1 / gtk): forward it to the seat so
// wlroots' XWM can bridge it to X11 PRIMARY and vice-versa (docs/20 item 5).
static void seat_request_set_primary_selection(wl_listener* listener, void* data)
{
    Server* server = nullptr;
    server = wl_container_of(listener, server, pointer.seat_request_set_primary_selection);
    auto* event = static_cast<wlr_seat_request_set_primary_selection_event*>(data);
    wlr_seat_set_primary_selection(server->seat, event->source, event->serial);
}

// wp-cursor-shape-v1: a client asked for a named cursor shape (e.g. "text").
static void cursor_shape_request(wl_listener* listener, void* data)
{
    Server* server = nullptr;
    server = wl_container_of(listener, server, pointer.cursor_shape_request);
    auto* event = static_cast<wlr_cursor_shape_manager_v1_request_set_shape_event*>(data);
    if (event->device_type != WLR_CURSOR_SHAPE_MANAGER_V1_DEVICE_TYPE_POINTER)
        return;
    if (auto const* name = wlr_cursor_shape_v1_name(event->shape); name && server->cursor)
        wlr_cursor_set_xcursor(server->cursor, server->cursor_manager, name);
}

void initialize_pointer(Server& server)
{
    server.pointer.relative_pointer_manager = wlr_relative_pointer_manager_v1_create(server.display);
    server.pointer.motion.notify = cursor_motion;
    wl_signal_add(&server.cursor->events.motion, &server.pointer.motion);
    server.pointer.motion_absolute.notify = cursor_motion_absolute;
    wl_signal_add(&server.cursor->events.motion_absolute, &server.pointer.motion_absolute);
    server.pointer.button.notify = cursor_button;
    wl_signal_add(&server.cursor->events.button, &server.pointer.button);
    server.pointer.axis.notify = cursor_axis;
    wl_signal_add(&server.cursor->events.axis, &server.pointer.axis);
    server.pointer.frame.notify = cursor_frame;
    wl_signal_add(&server.cursor->events.frame, &server.pointer.frame);
    server.pointer.touch_down.notify = cursor_touch_down;
    wl_signal_add(&server.cursor->events.touch_down, &server.pointer.touch_down);
    server.pointer.touch_motion.notify = cursor_touch_motion;
    wl_signal_add(&server.cursor->events.touch_motion, &server.pointer.touch_motion);
    server.pointer.touch_up.notify = cursor_touch_up;
    wl_signal_add(&server.cursor->events.touch_up, &server.pointer.touch_up);
    server.pointer.touch_cancel.notify = cursor_touch_cancel;
    wl_signal_add(&server.cursor->events.touch_cancel, &server.pointer.touch_cancel);

    server.pointer.seat_request_cursor.notify = seat_request_cursor;
    wl_signal_add(&server.seat->events.request_set_cursor, &server.pointer.seat_request_cursor);
    server.pointer.seat_pointer_focus_change.notify = seat_pointer_focus_change;
    wl_signal_add(&server.seat->pointer_state.events.focus_change, &server.pointer.seat_pointer_focus_change);
    server.pointer.seat_request_set_selection.notify = seat_request_set_selection;
    wl_signal_add(&server.seat->events.request_set_selection, &server.pointer.seat_request_set_selection);
    server.pointer.seat_request_set_primary_selection.notify = seat_request_set_primary_selection;
    wl_signal_add(&server.seat->events.request_set_primary_selection, &server.pointer.seat_request_set_primary_selection);

    // wp-cursor-shape-v1: clients can set named cursor shapes (R4.7/R4.8).
    server.pointer.cursor_shape_manager = wlr_cursor_shape_manager_v1_create(server.display, 1);
    server.pointer.cursor_shape_request.notify = cursor_shape_request;
    wl_signal_add(&server.pointer.cursor_shape_manager->events.request_set_shape, &server.pointer.cursor_shape_request);

    server.pointer.constraints = wlr_pointer_constraints_v1_create(server.display);
    server.pointer.new_constraint.notify = new_pointer_constraint;
    wl_signal_add(&server.pointer.constraints->events.new_constraint, &server.pointer.new_constraint);
}

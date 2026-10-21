/*
 * Copyright (c) 2026, the Serenity Desktop Environment developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Compositor.h"
#include "DragAndDrop.h"
#include "Pointer.h"
#include "XdgToplevelDrag.h"

#include "Wlroots.h"

// The layout-space origin of a surface (toplevel, layer surface or popup), used
// by pointer constraints and wp-pointer-warp-v1.
bool surface_global_origin(Server& server, wlr_surface* surface, double* out_x, double* out_y)
{
    for (auto& toplevel : server.toplevels) {
        if (!toplevel->xdg_toplevel || toplevel->xdg_toplevel->base->surface != surface)
            continue;
        int lx = 0;
        int ly = 0;
        if (!wlr_scene_node_coords(&toplevel->content_tree->node, &lx, &ly))
            return false;
        *out_x = lx;
        *out_y = ly;
        return true;
    }
    for (auto& layer : server.layer_surfaces) {
        if (!layer->layer_surface || layer->layer_surface->surface != surface || !layer->scene)
            continue;
        int lx = 0;
        int ly = 0;
        if (!wlr_scene_node_coords(&layer->scene->tree->node, &lx, &ly))
            return false;
        *out_x = lx;
        *out_y = ly;
        return true;
    }
    for (auto& popup : server.popups) {
        if (!popup->xdg_popup || popup->xdg_popup->base->surface != surface)
            continue;
        auto* tree = static_cast<wlr_scene_tree*>(popup->xdg_popup->base->data);
        if (!tree)
            return false;
        int lx = 0;
        int ly = 0;
        if (!wlr_scene_node_coords(&tree->node, &lx, &ly))
            return false;
        *out_x = lx;
        *out_y = ly;
        return true;
    }
    return false;
}

// Explicit drag-and-drop (data-device DnD between windows). A client asks the
// seat to start a drag; we validate and accept it, wlroots installs the pointer
// grab that turns our normal pointer notifications into wl_data_device
// enter/motion/drop, and we draw the drag icon above everything, following the
// cursor.
static void destroy_drag_icon_node(Server& server)
{
    if (server.drag.icon_node) {
        wlr_scene_node_destroy(server.drag.icon_node);
        server.drag.icon_node = nullptr;
    }
}

static void drag_icon_destroyed(wl_listener* listener, void*)
{
    Server* server = nullptr;
    server = wl_container_of(listener, server, drag.icon_destroy);
    destroy_drag_icon_node(*server);
    wl_list_remove(&listener->link);
}

static void drag_destroyed(wl_listener* listener, void*)
{
    Server* server = nullptr;
    server = wl_container_of(listener, server, drag.destroy);
    xdg_toplevel_drag_end(*server);
    destroy_drag_icon_node(*server);
    server->drag.active = nullptr;
    server->drag.target = nullptr;
    // wlroots asserts that no listeners remain on the signal after it is emitted.
    wl_list_remove(&listener->link);
    dbgln("WindowServer: drag ended");
}

static void seat_start_drag(wl_listener* listener, void* data)
{
    Server* server = nullptr;
    server = wl_container_of(listener, server, drag.start);
    auto* drag = static_cast<wlr_drag*>(data);
    server->drag.active = drag;
    server->drag.target = nullptr;
    server->drag.destroy.notify = drag_destroyed;
    wl_signal_add(&drag->events.destroy, &server->drag.destroy);
    if (drag->icon && drag->icon->surface && server->drag_icon_tree) {
        auto* scene = wlr_scene_surface_create(server->drag_icon_tree, drag->icon->surface);
        server->drag.icon_node = &scene->buffer->node;
        wlr_scene_node_set_position(server->drag.icon_node, static_cast<int>(server->cursor->x), static_cast<int>(server->cursor->y));
        server->drag.icon_destroy.notify = drag_icon_destroyed;
        wl_signal_add(&drag->icon->events.destroy, &server->drag.icon_destroy);
    }
    xdg_toplevel_drag_start(*server, drag);
    dbgln("WindowServer: drag started (icon={})", server->drag.icon_node ? "yes" : "no");
}

static void seat_request_start_drag(wl_listener* listener, void* data)
{
    Server* server = nullptr;
    server = wl_container_of(listener, server, drag.request_start);
    auto* event = static_cast<wlr_seat_request_start_drag_event*>(data);
    // The client uses the same start_drag request for pointer and touch, so the
    // serial (a recent pointer grab or touch point) decides which grab to start;
    // and it proves the client may start a drag at all.
    wlr_touch_point* point = nullptr;
    if (wlr_seat_validate_touch_grab_serial(server->seat, event->origin, event->serial, &point)) {
        wlr_seat_start_touch_drag(server->seat, event->drag, event->serial, point);
    } else if (wlr_seat_validate_pointer_grab_serial(server->seat, event->origin, event->serial)) {
        wlr_seat_start_pointer_drag(server->seat, event->drag, event->serial);
    } else if (event->drag->source) {
        wlr_data_source_destroy(event->drag->source);
    }
}

bool drag_motion(Server& server, uint32_t time)
{
    if (!server.drag.active)
        return false;
    double dx = 0;
    double dy = 0;
    wlr_surface* target = nullptr;
    // An attached xdg-toplevel-drag window follows the cursor and must not be
    // its own drop target. Hide it for the hit test, then put it back.
    auto* dragged = xdg_toplevel_drag_active_toplevel(server);
    bool const dragged_was_enabled = dragged && dragged->scene_tree && dragged->scene_tree->node.enabled;
    if (dragged_was_enabled)
        wlr_scene_node_set_enabled(&dragged->scene_tree->node, false);
    toplevel_at(server, server.cursor->x, server.cursor->y, &target, &dx, &dy);
    if (dragged_was_enabled)
        wlr_scene_node_set_enabled(&dragged->scene_tree->node, true);
    if (target != server.drag.target) {
        server.drag.target = target;
        if (target)
            wlr_seat_pointer_notify_enter(server.seat, target, dx, dy);
        else
            wlr_seat_pointer_clear_focus(server.seat);
    } else if (target) {
        wlr_seat_pointer_notify_motion(server.seat, time, dx, dy);
    }
    if (server.drag.icon_node)
        wlr_scene_node_set_position(server.drag.icon_node, static_cast<int>(server.cursor->x), static_cast<int>(server.cursor->y));
    xdg_toplevel_drag_motion(server);
    return true;
}

void move_drag_icon(Server& server, double lx, double ly)
{
    if (server.drag.active && server.drag.icon_node)
        wlr_scene_node_set_position(server.drag.icon_node, static_cast<int>(lx), static_cast<int>(ly));
}

void initialize_drag_and_drop(Server& server)
{
    // R5: explicit drag-and-drop (data-device DnD between windows).
    server.drag.request_start.notify = seat_request_start_drag;
    wl_signal_add(&server.seat->events.request_start_drag, &server.drag.request_start);
    server.drag.start.notify = seat_start_drag;
    wl_signal_add(&server.seat->events.start_drag, &server.drag.start);
}

void finish_drag_and_drop(Server& server)
{
    remove_listener(server.drag.request_start);
    remove_listener(server.drag.start);
    remove_listener(server.drag.destroy);
    remove_listener(server.drag.icon_destroy);
}

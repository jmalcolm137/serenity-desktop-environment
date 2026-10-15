/*
 * Copyright (c) 2026, the Serenity Desktop Environment developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Compositor.h"
#include "DragAndDrop.h"

#include "Wlroots.h"

// The layout-space origin of a surface, for a constraint on an xdg toplevel (the
// case games use). Returns false for other surfaces.
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
    return false;
}

// Explicit drag-and-drop (data-device DnD between windows). A client asks the
// seat to start a drag; we validate and accept it, wlroots installs the pointer
// grab that turns our normal pointer notifications into wl_data_device
// enter/motion/drop, and we draw the drag icon above everything, following the
// cursor.
static void destroy_drag_icon_node(Server& server)
{
    if (server.drag_icon_node) {
        wlr_scene_node_destroy(server.drag_icon_node);
        server.drag_icon_node = nullptr;
    }
}

static void drag_icon_destroyed(wl_listener* listener, void*)
{
    Server* server = nullptr;
    server = wl_container_of(listener, server, drag_icon_destroy);
    destroy_drag_icon_node(*server);
    wl_list_remove(&listener->link);
}

static void drag_destroyed(wl_listener* listener, void*)
{
    Server* server = nullptr;
    server = wl_container_of(listener, server, drag_destroy);
    destroy_drag_icon_node(*server);
    server->active_drag = nullptr;
    server->drag_target = nullptr;
    // wlroots asserts that no listeners remain on the signal after it is emitted.
    wl_list_remove(&listener->link);
    dbgln("WindowServer: drag ended");
}

void seat_start_drag(wl_listener* listener, void* data)
{
    Server* server = nullptr;
    server = wl_container_of(listener, server, seat_start_drag);
    auto* drag = static_cast<wlr_drag*>(data);
    server->active_drag = drag;
    server->drag_target = nullptr;
    server->drag_destroy.notify = drag_destroyed;
    wl_signal_add(&drag->events.destroy, &server->drag_destroy);
    if (drag->icon && drag->icon->surface && server->drag_icon_tree) {
        auto* scene = wlr_scene_surface_create(server->drag_icon_tree, drag->icon->surface);
        server->drag_icon_node = &scene->buffer->node;
        wlr_scene_node_set_position(server->drag_icon_node, static_cast<int>(server->cursor->x), static_cast<int>(server->cursor->y));
        server->drag_icon_destroy.notify = drag_icon_destroyed;
        wl_signal_add(&drag->icon->events.destroy, &server->drag_icon_destroy);
    }
    dbgln("WindowServer: drag started (icon={})", server->drag_icon_node ? "yes" : "no");
}

void seat_request_start_drag(wl_listener* listener, void* data)
{
    Server* server = nullptr;
    server = wl_container_of(listener, server, seat_request_start_drag);
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

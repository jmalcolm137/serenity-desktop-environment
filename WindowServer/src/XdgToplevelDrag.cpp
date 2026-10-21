/*
 * Copyright (c) 2026, the Serenity Desktop Environment developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Compositor.h"
#include "XdgToplevelDrag.h"

#include "Wlroots.h"

extern "C" {
#include "xdg-toplevel-drag-v1-server-protocol.h"
}

// xdg-toplevel-drag-v1 has no wlroots manager on 0.20, so the compositor serves
// it. A client creates a drag object for a wl_data_source before starting the
// drag, attaches one xdg_toplevel (possibly before mapping it), and the
// compositor moves that toplevel with the cursor until the drag ends.

namespace {

struct XdgToplevelDrag {
    Server* server { nullptr };
    wl_resource* resource { nullptr };
    wl_client* client { nullptr };
    wlr_data_source* source { nullptr };
    Toplevel* toplevel { nullptr };
    int x_offset { 0 };
    int y_offset { 0 };
    bool active { false };
    bool toplevel_listeners_installed { false };
    wl_listener source_destroy { };
    wl_listener toplevel_destroy { };
    wl_listener toplevel_unmap { };
};

Vector<XdgToplevelDrag*> g_drags;
XdgToplevelDrag* g_active_drag = nullptr;

XdgToplevelDrag* drag_for_source(wlr_data_source* source)
{
    if (!source)
        return nullptr;
    for (auto* drag : g_drags) {
        if (drag->source == source)
            return drag;
    }
    return nullptr;
}

void remove_toplevel_listeners(XdgToplevelDrag& drag)
{
    if (!drag.toplevel_listeners_installed)
        return;
    wl_list_remove(&drag.toplevel_destroy.link);
    wl_list_remove(&drag.toplevel_unmap.link);
    drag.toplevel_listeners_installed = false;
}

void detach_toplevel(XdgToplevelDrag& drag)
{
    if (drag.toplevel)
        drag.toplevel->dragged_by_toplevel_drag = false;
    remove_toplevel_listeners(drag);
    drag.toplevel = nullptr;
}

void place_toplevel(XdgToplevelDrag& drag)
{
    auto* toplevel = drag.toplevel;
    if (!toplevel || !toplevel->xdg_toplevel)
        return;
    // The offsets are the grab point relative to the cursor hotspot, in
    // surface-local coordinates and relative to the toplevel's window geometry
    // (the same convention as KWin: cursor - offset).
    auto* geometry = &toplevel->xdg_toplevel->base->geometry;
    int const x = static_cast<int>(drag.server->cursor->x) - drag.x_offset - geometry->x;
    int const y = static_cast<int>(drag.server->cursor->y) - drag.y_offset - geometry->y;
    dbgln("WindowServer: xdg-toplevel-drag moved toplevel to {},{}", x, y);
    wlr_scene_node_set_position(&toplevel->scene_tree->node, x, y);
}

void toplevel_destroyed(wl_listener* listener, void*)
{
    XdgToplevelDrag* drag = nullptr;
    drag = wl_container_of(listener, drag, toplevel_destroy);
    remove_toplevel_listeners(*drag);
    drag->toplevel = nullptr;
}

void toplevel_unmapped(wl_listener* listener, void*)
{
    XdgToplevelDrag* drag = nullptr;
    drag = wl_container_of(listener, drag, toplevel_unmap);
    // The protocol auto-detaches a toplevel that unmaps; the client may attach
    // another one (or re-attach after remapping).
    detach_toplevel(*drag);
}

void source_destroyed(wl_listener* listener, void*)
{
    XdgToplevelDrag* drag = nullptr;
    drag = wl_container_of(listener, drag, source_destroy);
    wl_list_remove(&drag->source_destroy.link);
    drag->source = nullptr;
    drag->active = false;
    if (g_active_drag == drag)
        g_active_drag = nullptr;
    if (drag->toplevel)
        drag->toplevel->dragged_by_toplevel_drag = false;
}

void drag_resource_destroy(wl_resource* resource)
{
    auto* drag = static_cast<XdgToplevelDrag*>(wl_resource_get_user_data(resource));
    if (!drag)
        return;
    if (g_active_drag == drag)
        g_active_drag = nullptr;
    detach_toplevel(*drag);
    if (drag->source)
        wl_list_remove(&drag->source_destroy.link);
    g_drags.remove_first_matching([&](auto* candidate) { return candidate == drag; });
    delete drag;
}

void drag_destroy(wl_client*, wl_resource* resource)
{
    auto* drag = static_cast<XdgToplevelDrag*>(wl_resource_get_user_data(resource));
    if (drag && drag->active) {
        wl_resource_post_error(resource, XDG_TOPLEVEL_DRAG_V1_ERROR_ONGOING_DRAG, "Drag has not ended");
        return;
    }
    wl_resource_destroy(resource);
}

void drag_attach(wl_client*, wl_resource* resource, wl_resource* toplevel_resource, int32_t x_offset, int32_t y_offset)
{
    auto* drag = static_cast<XdgToplevelDrag*>(wl_resource_get_user_data(resource));
    if (!drag || !toplevel_resource)
        return;
    if (wl_resource_get_client(toplevel_resource) != drag->client)
        return;

    auto* xdg_toplevel = wlr_xdg_toplevel_from_resource(toplevel_resource);
    if (!xdg_toplevel)
        return;

    if (drag->toplevel) {
        wl_resource_post_error(resource, XDG_TOPLEVEL_DRAG_V1_ERROR_TOPLEVEL_ATTACHED, "A toplevel is already attached");
        return;
    }

    Toplevel* toplevel = nullptr;
    for (auto& candidate : drag->server->toplevels) {
        if (candidate->xdg_toplevel == xdg_toplevel) {
            toplevel = candidate.ptr();
            break;
        }
    }
    if (!toplevel)
        return;

    drag->toplevel = toplevel;
    drag->x_offset = x_offset;
    drag->y_offset = y_offset;
    toplevel->dragged_by_toplevel_drag = true;
    dbgln("WindowServer: xdg-toplevel-drag attached '{}' at offset {},{}",
        toplevel->xdg_toplevel->title ? toplevel->xdg_toplevel->title : "", x_offset, y_offset);
    drag->toplevel_destroy.notify = toplevel_destroyed;
    wl_signal_add(&toplevel->xdg_toplevel->base->surface->events.destroy, &drag->toplevel_destroy);
    drag->toplevel_unmap.notify = toplevel_unmapped;
    wl_signal_add(&toplevel->xdg_toplevel->base->surface->events.unmap, &drag->toplevel_unmap);
    drag->toplevel_listeners_installed = true;

    if (drag->active)
        place_toplevel(*drag);
}

struct xdg_toplevel_drag_v1_interface const drag_impl = {
    .destroy = drag_destroy,
    .attach = drag_attach,
};

void manager_destroy(wl_client*, wl_resource* resource)
{
    wl_resource_destroy(resource);
}

void manager_get_xdg_toplevel_drag(wl_client*, wl_resource* resource, uint32_t id, wl_resource* source_resource)
{
    auto* server = static_cast<Server*>(wl_resource_get_user_data(resource));
    if (!server)
        return;

    if (!source_resource || wl_resource_get_interface(source_resource) != &wl_data_source_interface) {
        wl_resource_post_error(resource, XDG_TOPLEVEL_DRAG_MANAGER_V1_ERROR_INVALID_SOURCE, "not a wl_data_source");
        return;
    }
    // The wl_data_source resource's user data is a wlr_client_data_source, whose
    // first member is the public wlr_data_source base (types/wlr_data_device.h).
    auto* source = static_cast<wlr_data_source*>(wl_resource_get_user_data(source_resource));
    if (!source || wl_resource_get_client(source_resource) != wl_resource_get_client(resource)) {
        wl_resource_post_error(resource, XDG_TOPLEVEL_DRAG_MANAGER_V1_ERROR_INVALID_SOURCE, "invalid data source");
        return;
    }
    if ((server->seat && server->seat->selection_source == source) || drag_for_source(source)) {
        wl_resource_post_error(resource, XDG_TOPLEVEL_DRAG_MANAGER_V1_ERROR_INVALID_SOURCE, "data source already used");
        return;
    }

    auto* client = wl_resource_get_client(resource);
    auto* drag = new XdgToplevelDrag;
    drag->server = server;
    drag->client = client;
    drag->source = source;
    drag->resource = wl_resource_create(client, &xdg_toplevel_drag_v1_interface, wl_resource_get_version(resource), id);
    if (!drag->resource) {
        delete drag;
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(drag->resource, &drag_impl, drag, drag_resource_destroy);
    drag->source_destroy.notify = source_destroyed;
    wl_signal_add(&source->events.destroy, &drag->source_destroy);
    g_drags.append(drag);
}

struct xdg_toplevel_drag_manager_v1_interface const manager_impl = {
    .destroy = manager_destroy,
    .get_xdg_toplevel_drag = manager_get_xdg_toplevel_drag,
};

void manager_bind(wl_client* client, void* data, uint32_t version, uint32_t id)
{
    auto* resource = wl_resource_create(client, &xdg_toplevel_drag_manager_v1_interface, version, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &manager_impl, data, nullptr);
}

}

void initialize_xdg_toplevel_drag(Server& server)
{
    (void)wl_global_create(server.display, &xdg_toplevel_drag_manager_v1_interface, 1, &server, manager_bind);
}

void xdg_toplevel_drag_start(Server&, wlr_drag* drag)
{
    if (!drag)
        return;
    auto* candidate = drag_for_source(drag->source);
    if (!candidate)
        return;
    g_active_drag = candidate;
    candidate->active = true;
    dbgln("WindowServer: xdg-toplevel-drag started");
    if (candidate->toplevel)
        place_toplevel(*candidate);
}

void xdg_toplevel_drag_motion(Server&)
{
    if (g_active_drag && g_active_drag->toplevel)
        place_toplevel(*g_active_drag);
}

void xdg_toplevel_drag_end(Server&)
{
    if (!g_active_drag)
        return;
    g_active_drag->active = false;
    if (g_active_drag->toplevel)
        g_active_drag->toplevel->dragged_by_toplevel_drag = false;
    g_active_drag = nullptr;
}

void xdg_toplevel_drag_place(Server&, Toplevel& toplevel)
{
    for (auto* drag : g_drags) {
        if (drag->toplevel == &toplevel) {
            place_toplevel(*drag);
            return;
        }
    }
}

Toplevel* xdg_toplevel_drag_active_toplevel(Server&)
{
    return g_active_drag ? g_active_drag->toplevel : nullptr;
}

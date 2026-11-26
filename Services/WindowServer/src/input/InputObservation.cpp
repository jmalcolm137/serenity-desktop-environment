/*
 * Copyright (c) 2026, the Serenity Desktop Environment developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// ext-input-observation-v1: the pointer position relative to a surface, inside
// or outside it and regardless of pointer focus.
//
// The protocol is a draft (wayland-protocols MR !335) and not in
// wayland-protocols, so the XML is vendored. LibWM's set_global_mouse_tracking
// (Serenity's GUI::MouseTracker, used by the Eyes/CatDog demos) is the client:
// a position observer per toplevel, reconstructed into output coordinates with
// the surface origin. wlmaker has a similar implementation.
//
// It is a privileged interface -- it reveals the global pointer -- so a client
// must already have bound the session's serenity-window protocol (the shim
// always has by the time a tracker starts).

#include "Compositor.h"
#include "DragAndDrop.h"
#include "Wlroots.h"

#include <climits>
#include <cmath>

extern "C" {
#include "ext-input-observation-v1-server-protocol.h"
}

namespace {

struct Observer {
    wl_resource* resource { nullptr };
    Server* server { nullptr };
    wlr_surface* surface { nullptr };
    wl_listener surface_destroy { };
    wl_listener cursor_frame { };
};

struct InputObservation {
    wl_global* global { nullptr };
    Vector<Observer*> observers;
};

InputObservation s_observation;

bool client_is_session_client(Server& server, wl_client* client)
{
    // The same clients that speak serenity-window: the applications the desktop
    // itself hosts, not arbitrary third-party compositor clients.
    for (auto* manager : server.serenity_window_managers) {
        if (wl_resource_get_client(manager) == client)
            return true;
    }
    return false;
}

void observer_resource_destroy(wl_resource* resource)
{
    auto* observer = static_cast<Observer*>(wl_resource_get_user_data(resource));
    if (!observer)
        return;
    wl_list_remove(&observer->surface_destroy.link);
    wl_list_remove(&observer->cursor_frame.link);
    s_observation.observers.remove_first_matching([&](auto* candidate) { return candidate == observer; });
    delete observer;
}

void observer_destroy(wl_client*, wl_resource* resource)
{
    wl_resource_destroy(resource);
}

const struct ext_input_position_observer_v1_interface observer_implementation = {
    .destroy = observer_destroy,
};

void observer_handle_surface_destroy(wl_listener* listener, void*)
{
    Observer* observer = nullptr;
    observer = wl_container_of(listener, observer, surface_destroy);
    observer->surface = nullptr;
    wl_resource_destroy(observer->resource);
}

// Report the pointer position relative to the surface, as signed 16:16 fixed
// point. A single instance (0): wlroots' scene graph gives the surface one
// layout position even when it is shown on several outputs.
void observer_handle_cursor_frame(wl_listener* listener, void*)
{
    Observer* observer = nullptr;
    observer = wl_container_of(listener, observer, cursor_frame);
    auto* server = observer->server;
    auto* surface = observer->surface;
    if (!server || !surface || !surface->mapped || !server->cursor)
        return;

    double origin_x = 0;
    double origin_y = 0;
    if (!surface_global_origin(*server, surface, &origin_x, &origin_y))
        return;

    int const scale = max(surface->current.scale, 1);
    double const width = static_cast<double>(surface->current.width) / scale;
    double const height = static_cast<double>(surface->current.height) / scale;
    if (width <= 0 || height <= 0)
        return;

    double const relative_x = (server->cursor->x - origin_x) / width;
    double const relative_y = (server->cursor->y - origin_y) / height;
    auto const fixed_x = static_cast<int32_t>(clamp<int64_t>(
        llround(relative_x * 65536.0), INT64_C(-2147483648), INT64_C(2147483647)));
    auto const fixed_y = static_cast<int32_t>(clamp<int64_t>(
        llround(relative_y * 65536.0), INT64_C(-2147483648), INT64_C(2147483647)));

    ext_input_position_observer_v1_send_position(observer->resource,
        surface->resource, 0, fixed_x, fixed_y);
}

void manager_create_pointer_observer(wl_client* client, wl_resource* resource,
    uint32_t id, wl_resource* pointer_resource, wl_resource* surface_resource)
{
    auto* server = static_cast<Server*>(wl_resource_get_user_data(resource));
    if (!server)
        return;
    if (!client_is_session_client(*server, client)) {
        wl_resource_post_error(resource, WL_DISPLAY_ERROR_INVALID_METHOD,
            "input observation requires the serenity-window protocol");
        return;
    }
    if (wl_resource_get_interface(pointer_resource) != &wl_pointer_interface) {
        wl_resource_post_error(resource, WL_DISPLAY_ERROR_INVALID_METHOD,
            "input observation needs a wl_pointer");
        return;
    }
    if (wl_resource_get_client(pointer_resource) != client ||
        wl_resource_get_client(surface_resource) != client) {
        wl_resource_post_error(resource, WL_DISPLAY_ERROR_INVALID_METHOD,
            "cross-client input observation");
        return;
    }

    auto* surface = wlr_surface_from_resource(surface_resource);
    if (!surface) {
        wl_resource_post_error(resource, WL_DISPLAY_ERROR_INVALID_METHOD,
            "input observation needs a wl_surface");
        return;
    }

    auto* observer = new Observer;
    observer->server = server;
    observer->surface = surface;
    observer->resource = wl_resource_create(client,
        &ext_input_position_observer_v1_interface,
        wl_resource_get_version(resource), id);
    if (!observer->resource) {
        delete observer;
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(observer->resource,
        &observer_implementation, observer, observer_resource_destroy);

    observer->surface_destroy.notify = observer_handle_surface_destroy;
    wl_signal_add(&surface->events.destroy, &observer->surface_destroy);
    observer->cursor_frame.notify = observer_handle_cursor_frame;
    wl_signal_add(&server->cursor->events.frame, &observer->cursor_frame);

    s_observation.observers.append(observer);
    dbgln("WindowServer: input observer for surface {}", static_cast<void*>(surface));
}

const struct ext_input_observation_manager_v1_interface manager_implementation = {
    .destroy = observer_destroy,
    .create_pointer_observer = manager_create_pointer_observer,
};

void bind_manager(wl_client* client, void* data, uint32_t version, uint32_t id)
{
    uint32_t const bound_version = version < 1 ? version : 1;
    auto* resource = wl_resource_create(client,
        &ext_input_observation_manager_v1_interface, bound_version, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &manager_implementation, data, nullptr);
}

}

void initialize_input_observation(Server& server)
{
    s_observation.global = wl_global_create(server.display,
        &ext_input_observation_manager_v1_interface, 1, &server, bind_manager);
    if (!s_observation.global)
        dbgln("WindowServer: could not create the input observation global");
}

void finish_input_observation(Server&)
{
    if (s_observation.global) {
        wl_global_destroy(s_observation.global);
        s_observation.global = nullptr;
    }
    while (!s_observation.observers.is_empty())
        wl_resource_destroy(s_observation.observers.last()->resource);
}

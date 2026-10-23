/*
 * Copyright (c) 2026, the Serenity Desktop Environment developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Compositor.h"
#include "Server.h"
#include "WlcsControl.h"
#include "XdgShell.h"

#include <stdlib.h>
#include <string.h>

#include "Wlroots.h"

extern "C" {
#include "serenity-wlcs-control-server-protocol.h"
}

// --- serenity-wlcs-control (test harness) -----------------------------------
// WLCS owns the compositor-side placement decision, so its integration module
// asks us to move one of its own client surfaces. The global exists only under
// SDE_WLCS_CONTROL=1.

static void move_surface(wl_client*, wl_resource* resource, wl_resource* surface_resource, int32_t x, int32_t y)
{
    auto& server = *static_cast<Server*>(wl_resource_get_user_data(resource));
    auto* surface = wlr_surface_from_resource(surface_resource);
    if (!surface)
        return;

    for (auto& toplevel : server.toplevels) {
        if (!toplevel->xdg_toplevel || toplevel->xdg_toplevel->base->surface != surface)
            continue;

        auto* geometry = &toplevel->xdg_toplevel->base->geometry;
        int left = x - geometry->x;
        int top = y - geometry->y;
        dbgln("WindowServer: WLCS move surface to {},{} (surface {}, wm_id {})", left, top, surface, toplevel->wm_id);
        wlr_scene_node_set_position(&toplevel->scene_tree->node, left, top);
        toplevel->restore_position = { left, top };
        toplevel_report_position(*toplevel);
        return;
    }
    dbgln("WindowServer: WLCS move surface: no toplevel for {}", surface);
}

static struct serenity_wlcs_control_v1_interface const control_implementation = {
    .move_surface = move_surface,
};

static void control_bind(wl_client* client, void* data, uint32_t version, uint32_t id)
{
    auto& server = *static_cast<Server*>(data);
    auto* resource = wl_resource_create(client, &serenity_wlcs_control_v1_interface, min(version, 1u), id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &control_implementation, &server, nullptr);
}

void initialize_wlcs_control(Server& server)
{
    auto const* enabled = getenv("SDE_WLCS_CONTROL");
    if (!enabled || strcmp(enabled, "1") != 0)
        return;
    wl_global_create(server.display, &serenity_wlcs_control_v1_interface, 1, &server, control_bind);
    dbgln("WindowServer: WLCS control channel enabled");
}

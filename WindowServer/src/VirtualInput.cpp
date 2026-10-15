/*
 * Copyright (c) 2026, the Serenity Desktop Environment developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Compositor.h"
#include "Keyboard.h"
#include "Pointer.h"
#include "Tablet.h"
#include "VirtualInput.h"

#include <stdlib.h>
#include <string.h>

#include "Wlroots.h"

extern "C" {
#include "serenity-virtual-input-server-protocol.h"
}

// --- serenity-virtual-input (test harness) ---------------------------------
// The headless harness has no libinput and wlroots offers no virtual touch or
// tablet, so the compositor exposes a private injection protocol; requests feed
// the same routing as real input (see handle_touch_* / handle_tablet_*).
static void virtual_touch_down(wl_client*, wl_resource* resource, uint32_t time, int32_t touch_id, wl_fixed_t x, wl_fixed_t y)
{
    auto& server = *static_cast<Server*>(wl_resource_get_user_data(resource));
    handle_touch_down(server, time, touch_id, wl_fixed_to_double(x), wl_fixed_to_double(y));
}

static void virtual_touch_motion(wl_client*, wl_resource* resource, uint32_t time, int32_t touch_id, wl_fixed_t x, wl_fixed_t y)
{
    auto& server = *static_cast<Server*>(wl_resource_get_user_data(resource));
    handle_touch_motion(server, time, touch_id, wl_fixed_to_double(x), wl_fixed_to_double(y));
}

static void virtual_touch_up(wl_client*, wl_resource* resource, uint32_t time, int32_t touch_id)
{
    auto& server = *static_cast<Server*>(wl_resource_get_user_data(resource));
    handle_touch_up(server, time, touch_id);
}

static void virtual_touch_cancel(wl_client*, wl_resource* resource, uint32_t)
{
    auto& server = *static_cast<Server*>(wl_resource_get_user_data(resource));
    handle_touch_cancel(server);
}

static void virtual_touch_destroy_request(wl_client*, wl_resource* resource)
{
    wl_resource_destroy(resource);
}

static struct serenity_virtual_touch_v1_interface const virtual_touch_implementation = {
    .down = virtual_touch_down,
    .motion = virtual_touch_motion,
    .up = virtual_touch_up,
    .cancel = virtual_touch_cancel,
    .destroy = virtual_touch_destroy_request,
};

static void virtual_touch_resource_destroy(wl_resource* resource)
{
    auto& server = *static_cast<Server*>(wl_resource_get_user_data(resource));
    if (server.input.virtual_touch_count > 0)
        --server.input.virtual_touch_count;
    update_seat_capabilities(server);
}

static void virtual_input_manager_create_touch(wl_client* client, wl_resource* resource, uint32_t id)
{
    auto& server = *static_cast<Server*>(wl_resource_get_user_data(resource));
    auto* touch = wl_resource_create(client, &serenity_virtual_touch_v1_interface, 1, id);
    if (!touch) {
        wl_client_post_no_memory(client);
        return;
    }
    // Advertise the touch capability while a virtual touch device exists, so
    // clients can bind wl_touch and receive the injected events.
    ++server.input.virtual_touch_count;
    update_seat_capabilities(server);
    wl_resource_set_implementation(touch, &virtual_touch_implementation, &server, virtual_touch_resource_destroy);
}

// A hand-built tablet device + pen tool. wlr_tablet_from_input_device() is a
// cast, so the v2 wrappers accept these; kept alive with the resource.
struct VirtualTabletDevice {
    Server* server { nullptr };
    wlr_tablet tablet { };
    wlr_tablet_tool tool { };
};

static void virtual_tablet_proximity_in(wl_client*, wl_resource* resource, uint32_t time, wl_fixed_t x, wl_fixed_t y)
{
    auto* device = static_cast<VirtualTabletDevice*>(wl_resource_get_user_data(resource));
    handle_tablet_proximity(*device->server, &device->tablet, &device->tool, true, wl_fixed_to_double(x), wl_fixed_to_double(y));
}

static void virtual_tablet_proximity_out(wl_client*, wl_resource* resource, uint32_t)
{
    auto* device = static_cast<VirtualTabletDevice*>(wl_resource_get_user_data(resource));
    handle_tablet_proximity(*device->server, &device->tablet, &device->tool, false, 0, 0);
}

static void virtual_tablet_tip_down(wl_client*, wl_resource* resource, uint32_t)
{
    auto* device = static_cast<VirtualTabletDevice*>(wl_resource_get_user_data(resource));
    handle_tablet_tip(*device->server, &device->tool, true);
}

static void virtual_tablet_tip_up(wl_client*, wl_resource* resource, uint32_t)
{
    auto* device = static_cast<VirtualTabletDevice*>(wl_resource_get_user_data(resource));
    handle_tablet_tip(*device->server, &device->tool, false);
}

static void virtual_tablet_motion(wl_client*, wl_resource* resource, uint32_t, wl_fixed_t x, wl_fixed_t y)
{
    auto* device = static_cast<VirtualTabletDevice*>(wl_resource_get_user_data(resource));
    handle_tablet_motion(*device->server, &device->tool, wl_fixed_to_double(x), wl_fixed_to_double(y));
}

static void virtual_tablet_axis(wl_client*, wl_resource* resource, uint32_t, wl_fixed_t pressure, wl_fixed_t distance, wl_fixed_t tilt_x, wl_fixed_t tilt_y, wl_fixed_t rotation, wl_fixed_t slider, wl_fixed_t wheel)
{
    auto* device = static_cast<VirtualTabletDevice*>(wl_resource_get_user_data(resource));
    handle_tablet_axis(*device->server, &device->tool, wl_fixed_to_double(pressure), wl_fixed_to_double(distance),
        wl_fixed_to_double(tilt_x), wl_fixed_to_double(tilt_y), wl_fixed_to_double(rotation), wl_fixed_to_double(slider), wl_fixed_to_double(wheel));
}

static void virtual_tablet_button(wl_client*, wl_resource* resource, uint32_t, uint32_t button, uint32_t pressed)
{
    auto* device = static_cast<VirtualTabletDevice*>(wl_resource_get_user_data(resource));
    handle_tablet_button(*device->server, &device->tool, button, pressed != 0);
}

static void virtual_tablet_destroy_request(wl_client*, wl_resource* resource)
{
    wl_resource_destroy(resource);
}

static struct serenity_virtual_tablet_v1_interface const virtual_tablet_implementation = {
    .proximity_in = virtual_tablet_proximity_in,
    .proximity_out = virtual_tablet_proximity_out,
    .tip_down = virtual_tablet_tip_down,
    .tip_up = virtual_tablet_tip_up,
    .motion = virtual_tablet_motion,
    .axis = virtual_tablet_axis,
    .button = virtual_tablet_button,
    .destroy = virtual_tablet_destroy_request,
};

static void virtual_tablet_resource_destroy(wl_resource* resource)
{
    auto* device = static_cast<VirtualTabletDevice*>(wl_resource_get_user_data(resource));
    if (!device)
        return;
    auto& server = *device->server;
    // Tear down the v2 wrappers (they listen on these synthetic signals), then
    // drop the map entries and free.
    if (server.tablet.tools.contains(reinterpret_cast<u64>(&device->tool))) {
        wl_signal_emit_mutable(&device->tool.events.destroy, &device->tool);
        server.tablet.tools.remove(reinterpret_cast<u64>(&device->tool));
    }
    if (server.tablet.tablets.contains(reinterpret_cast<u64>(&device->tablet))) {
        wl_signal_emit_mutable(&device->tablet.base.events.destroy, &device->tablet.base);
        server.tablet.tablets.remove(reinterpret_cast<u64>(&device->tablet));
    }
    free(device->tablet.base.name);
    delete device;
}

static void virtual_input_manager_create_tablet(wl_client* client, wl_resource* resource, uint32_t id)
{
    auto& server = *static_cast<Server*>(wl_resource_get_user_data(resource));
    auto* device = new VirtualTabletDevice();
    device->server = &server;
    device->tablet.base.type = WLR_INPUT_DEVICE_TABLET;
    device->tablet.base.name = strdup("sde-virtual-tablet");
    wl_signal_init(&device->tablet.base.events.destroy);
    device->tablet.width_mm = 100;
    device->tablet.height_mm = 100;
    device->tool.type = WLR_TABLET_TOOL_TYPE_PEN;
    wl_signal_init(&device->tool.events.destroy);

    auto* tablet = wl_resource_create(client, &serenity_virtual_tablet_v1_interface, 1, id);
    if (!tablet) {
        delete device;
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(tablet, &virtual_tablet_implementation, device, virtual_tablet_resource_destroy);
}

static struct serenity_virtual_input_manager_v1_interface const virtual_input_manager_implementation = {
    .create_touch = virtual_input_manager_create_touch,
    .create_tablet = virtual_input_manager_create_tablet,
};

static void virtual_input_manager_bind(wl_client* client, void* data, uint32_t version, uint32_t id)
{
    auto& server = *static_cast<Server*>(data);
    auto* resource = wl_resource_create(client, &serenity_virtual_input_manager_v1_interface, min(version, 1u), id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &virtual_input_manager_implementation, &server, nullptr);
}

void initialize_virtual_input(Server& server)
{
    wl_global_create(server.display, &serenity_virtual_input_manager_v1_interface, 1, &server, virtual_input_manager_bind);
}

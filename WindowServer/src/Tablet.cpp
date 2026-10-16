/*
 * Copyright (c) 2026, the Serenity Desktop Environment developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Compositor.h"
#include "Focus.h"
#include "Idle.h"
#include "Pointer.h"
#include "Tablet.h"

#include "Wlroots.h"

// zwp-tablet-v2 (pen/tablet). wlroots reports tool events through the cursor; the
// compositor wraps each tablet/tool in its v2 counterpart and forwards surface
// coordinates. The tool's position is normalised [0,1] over the output layout.
static wlr_tablet_v2_tablet* v2_tablet_for(Server& server, wlr_tablet* tablet)
{
    if (!tablet || !server.tablet.manager)
        return nullptr;
    auto key = reinterpret_cast<u64>(tablet);
    if (auto it = server.tablet.tablets.find(key); it != server.tablet.tablets.end())
        return it->value;
    auto* v2 = wlr_tablet_create(server.tablet.manager, server.seat, &tablet->base);
    server.tablet.tablets.set(key, v2);
    return v2;
}

static wlr_tablet_v2_tablet_tool* v2_tablet_tool_for(Server& server, wlr_tablet_tool* tool)
{
    if (!tool || !server.tablet.manager)
        return nullptr;
    auto key = reinterpret_cast<u64>(tool);
    if (auto it = server.tablet.tools.find(key); it != server.tablet.tools.end())
        return it->value;
    auto* v2 = wlr_tablet_tool_create(server.tablet.manager, server.seat, tool);
    server.tablet.tools.set(key, v2);
    return v2;
}

static void tablet_tool_layout_position(Server& server, double x, double y, double* lx, double* ly)
{
    wlr_box box { };
    wlr_output_layout_get_box(server.output_layout, nullptr, &box);
    *lx = box.x + x * box.width;
    *ly = box.y + y * box.height;
}

// Tablet routing, shared by real pen/tablet devices (via the cursor) and the
// harness's virtual tablet. Coordinates are in the output layout.
void handle_tablet_proximity(Server& server, wlr_tablet* tablet, wlr_tablet_tool* tool, bool in, double lx, double ly)
{
    auto* v2_tool = v2_tablet_tool_for(server, tool);
    if (!v2_tool)
        return;
    // wlroots' proximity_in dereferences the surface, so only send it when the
    // pen is over a client surface; otherwise the tool stays out of proximity.
    wlr_surface* surface = nullptr;
    if (in) {
        double sx = 0;
        double sy = 0;
        if (auto* toplevel = toplevel_at(server, lx, ly, &surface, &sx, &sy))
            focus_toplevel(server, toplevel);
    }

    if (in && surface) {
        wlr_tablet_v2_tablet_tool_notify_proximity_in(v2_tool, v2_tablet_for(server, tablet), surface);
        dbgln("WindowServer: tablet tool in proximity");
    } else {
        wlr_tablet_v2_tablet_tool_notify_proximity_out(v2_tool);
        dbgln("WindowServer: tablet tool out of proximity");
    }
    notify_activity(server);
}

void handle_tablet_motion(Server& server, wlr_tablet_tool* tool, double lx, double ly)
{
    auto* v2_tool = v2_tablet_tool_for(server, tool);
    if (!v2_tool)
        return;
    double sx = 0;
    double sy = 0;
    wlr_surface* surface = nullptr;
    toplevel_at(server, lx, ly, &surface, &sx, &sy);
    wlr_tablet_v2_tablet_tool_notify_motion(v2_tool, sx, sy);
    notify_activity(server);
}

void handle_tablet_axis(Server& server, wlr_tablet_tool* tool, double pressure, double distance, double tilt_x, double tilt_y, double rotation, double slider, double wheel)
{
    auto* v2_tool = v2_tablet_tool_for(server, tool);
    if (!v2_tool)
        return;
    wlr_tablet_v2_tablet_tool_notify_pressure(v2_tool, pressure);
    wlr_tablet_v2_tablet_tool_notify_distance(v2_tool, distance);
    wlr_tablet_v2_tablet_tool_notify_tilt(v2_tool, tilt_x, tilt_y);
    wlr_tablet_v2_tablet_tool_notify_rotation(v2_tool, rotation);
    wlr_tablet_v2_tablet_tool_notify_slider(v2_tool, slider);
    if (wheel != 0)
        wlr_tablet_v2_tablet_tool_notify_wheel(v2_tool, wheel, 0);
    notify_activity(server);
}

void handle_tablet_tip(Server& server, wlr_tablet_tool* tool, bool down)
{
    auto* v2_tool = v2_tablet_tool_for(server, tool);
    if (!v2_tool)
        return;
    if (down)
        wlr_tablet_v2_tablet_tool_notify_down(v2_tool);
    else
        wlr_tablet_v2_tablet_tool_notify_up(v2_tool);
    notify_activity(server);
}

void handle_tablet_button(Server& server, wlr_tablet_tool* tool, uint32_t button, bool pressed)
{
    auto* v2_tool = v2_tablet_tool_for(server, tool);
    if (!v2_tool)
        return;
    wlr_tablet_v2_tablet_tool_notify_button(v2_tool, button,
        pressed ? ZWP_TABLET_PAD_V2_BUTTON_STATE_PRESSED : ZWP_TABLET_PAD_V2_BUTTON_STATE_RELEASED);
    notify_activity(server);
}

static void tablet_tool_axis(wl_listener* listener, void* data)
{
    Server* server = nullptr;
    server = wl_container_of(listener, server, tablet.tool_axis);
    auto* event = static_cast<wlr_tablet_tool_axis_event*>(data);
    double lx = 0;
    double ly = 0;
    tablet_tool_layout_position(*server, event->x, event->y, &lx, &ly);
    if (event->updated_axes & (WLR_TABLET_TOOL_AXIS_X | WLR_TABLET_TOOL_AXIS_Y))
        handle_tablet_motion(*server, event->tool, lx, ly);
    if (event->updated_axes & (WLR_TABLET_TOOL_AXIS_PRESSURE | WLR_TABLET_TOOL_AXIS_DISTANCE
            | WLR_TABLET_TOOL_AXIS_TILT_X | WLR_TABLET_TOOL_AXIS_TILT_Y
            | WLR_TABLET_TOOL_AXIS_ROTATION | WLR_TABLET_TOOL_AXIS_SLIDER | WLR_TABLET_TOOL_AXIS_WHEEL))
        handle_tablet_axis(*server, event->tool, event->pressure, event->distance, event->tilt_x, event->tilt_y, event->rotation, event->slider, event->wheel_delta);
}

static void tablet_tool_proximity(wl_listener* listener, void* data)
{
    Server* server = nullptr;
    server = wl_container_of(listener, server, tablet.tool_proximity);
    auto* event = static_cast<wlr_tablet_tool_proximity_event*>(data);
    double lx = 0;
    double ly = 0;
    tablet_tool_layout_position(*server, event->x, event->y, &lx, &ly);
    handle_tablet_proximity(*server, event->tablet, event->tool, event->state == WLR_TABLET_TOOL_PROXIMITY_IN, lx, ly);
}

static void tablet_tool_tip(wl_listener* listener, void* data)
{
    Server* server = nullptr;
    server = wl_container_of(listener, server, tablet.tool_tip);
    auto* event = static_cast<wlr_tablet_tool_tip_event*>(data);
    handle_tablet_tip(*server, event->tool, event->state == WLR_TABLET_TOOL_TIP_DOWN);
}

static void tablet_tool_button(wl_listener* listener, void* data)
{
    Server* server = nullptr;
    server = wl_container_of(listener, server, tablet.tool_button);
    auto* event = static_cast<wlr_tablet_tool_button_event*>(data);
    handle_tablet_button(*server, event->tool, event->button, event->state == WLR_BUTTON_PRESSED);
}

// zwp-tablet-pad-v2: express keys and ring/strip controls.
static void tablet_pad_button(wl_listener* listener, void* data)
{
    TabletPad* pad = nullptr;
    pad = wl_container_of(listener, pad, button);
    auto* event = static_cast<wlr_tablet_pad_button_event*>(data);
    if (pad->v2_pad)
        wlr_tablet_v2_tablet_pad_notify_button(pad->v2_pad, event->button, event->time_msec,
            event->state == WLR_BUTTON_PRESSED ? ZWP_TABLET_PAD_V2_BUTTON_STATE_PRESSED
                                               : ZWP_TABLET_PAD_V2_BUTTON_STATE_RELEASED);
}

static void tablet_pad_ring(wl_listener* listener, void* data)
{
    TabletPad* pad = nullptr;
    pad = wl_container_of(listener, pad, ring);
    auto* event = static_cast<wlr_tablet_pad_ring_event*>(data);
    if (pad->v2_pad)
        wlr_tablet_v2_tablet_pad_notify_ring(pad->v2_pad, event->ring, event->position,
            event->source == WLR_TABLET_PAD_RING_SOURCE_FINGER, event->time_msec);
}

static void tablet_pad_strip(wl_listener* listener, void* data)
{
    TabletPad* pad = nullptr;
    pad = wl_container_of(listener, pad, strip);
    auto* event = static_cast<wlr_tablet_pad_strip_event*>(data);
    if (pad->v2_pad)
        wlr_tablet_v2_tablet_pad_notify_strip(pad->v2_pad, event->strip, event->position,
            event->source == WLR_TABLET_PAD_STRIP_SOURCE_FINGER, event->time_msec);
}

static void tablet_pad_destroy(wl_listener* listener, void*)
{
    TabletPad* pad = nullptr;
    pad = wl_container_of(listener, pad, destroy);
    wl_list_remove(&pad->button.link);
    wl_list_remove(&pad->ring.link);
    wl_list_remove(&pad->strip.link);
    wl_list_remove(&pad->destroy.link);
    remove_owned(pad->server->tablet.pads, pad);
}

void add_tablet_pad(Server& server, wlr_input_device* device)
{
    auto pad = make<TabletPad>();
    pad->server = &server;
    pad->pad = wlr_tablet_pad_from_input_device(device);
    pad->v2_pad = server.tablet.manager ? wlr_tablet_pad_create(server.tablet.manager, server.seat, device) : nullptr;
    pad->button.notify = tablet_pad_button;
    wl_signal_add(&pad->pad->events.button, &pad->button);
    pad->ring.notify = tablet_pad_ring;
    wl_signal_add(&pad->pad->events.ring, &pad->ring);
    pad->strip.notify = tablet_pad_strip;
    wl_signal_add(&pad->pad->events.strip, &pad->strip);
    pad->destroy.notify = tablet_pad_destroy;
    wl_signal_add(&device->events.destroy, &pad->destroy);
    server.tablet.pads.append(move(pad));
}

void forget_tablet_device(Server& server, wlr_tablet& tablet, wlr_tablet_tool& tool)
{
    if (server.tablet.tools.contains(reinterpret_cast<u64>(&tool))) {
        wl_signal_emit_mutable(&tool.events.destroy, &tool);
        server.tablet.tools.remove(reinterpret_cast<u64>(&tool));
    }
    if (server.tablet.tablets.contains(reinterpret_cast<u64>(&tablet))) {
        wl_signal_emit_mutable(&tablet.base.events.destroy, &tablet.base);
        server.tablet.tablets.remove(reinterpret_cast<u64>(&tablet));
    }
}

void initialize_tablet(Server& server)
{
    server.tablet.manager = wlr_tablet_v2_create(server.display);
    server.tablet.tool_axis.notify = tablet_tool_axis;
    wl_signal_add(&server.cursor->events.tablet_tool_axis, &server.tablet.tool_axis);
    server.tablet.tool_proximity.notify = tablet_tool_proximity;
    wl_signal_add(&server.cursor->events.tablet_tool_proximity, &server.tablet.tool_proximity);
    server.tablet.tool_tip.notify = tablet_tool_tip;
    wl_signal_add(&server.cursor->events.tablet_tool_tip, &server.tablet.tool_tip);
    server.tablet.tool_button.notify = tablet_tool_button;
    wl_signal_add(&server.cursor->events.tablet_tool_button, &server.tablet.tool_button);
}

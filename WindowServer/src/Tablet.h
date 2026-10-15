/*
 * Copyright (c) 2026, the Serenity Desktop Environment developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "Compositor.h"

void handle_tablet_proximity(Server& server, wlr_tablet* tablet, wlr_tablet_tool* tool, bool in, double lx, double ly);
void handle_tablet_motion(Server& server, wlr_tablet_tool* tool, double lx, double ly);
void handle_tablet_axis(Server& server, wlr_tablet_tool* tool, double pressure, double distance, double tilt_x, double tilt_y, double rotation, double slider, double wheel);
void handle_tablet_tip(Server& server, wlr_tablet_tool* tool, bool down);
void handle_tablet_button(Server& server, wlr_tablet_tool* tool, uint32_t button, bool pressed);
void tablet_tool_axis(wl_listener* listener, void* data);
void tablet_tool_proximity(wl_listener* listener, void* data);
void tablet_tool_tip(wl_listener* listener, void* data);
void tablet_tool_button(wl_listener* listener, void* data);
void tablet_pad_button(wl_listener* listener, void* data);
void tablet_pad_ring(wl_listener* listener, void* data);
void tablet_pad_strip(wl_listener* listener, void* data);
void tablet_pad_destroy(wl_listener* listener, void*);

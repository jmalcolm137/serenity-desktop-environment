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
// A tablet pad was plugged in: wrap it for zwp-tablet-v2 and route its events.
void add_tablet_pad(Server& server, wlr_input_device* device);
// A synthetic tablet device is going away: tear down its v2 wrappers (they listen
// on the device's destroy signals) and forget them.
void forget_tablet_device(Server& server, wlr_tablet& tablet, wlr_tablet_tool& tool);

void initialize_tablet(Server&);

// Unhook the listeners initialize wired, before the objects they listen on go.
void finish_tablet(Server&);

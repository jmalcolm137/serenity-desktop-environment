/*
 * Copyright (c) 2026, the Serenity Desktop Environment developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "Compositor.h"

bool surface_global_origin(Server& server, wlr_surface* surface, double* out_x, double* out_y);

// Explicit drag-and-drop: route cursor motion to the surface under the cursor
// (the drag grab turns it into wl_data_device enter/motion) and move the icon.
// Returns false when no drag is active, so the motion is the pointer's.
bool drag_motion(Server& server, uint32_t time);
// Keep the drag icon under a cursor that moved some other way (touch, tablet).
void move_drag_icon(Server& server, double lx, double ly);
void initialize_drag_and_drop(Server&);
// Unhook the listeners initialize wired, before the objects they listen on go.
void finish_drag_and_drop(Server&);

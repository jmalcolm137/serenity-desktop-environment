/*
 * Copyright (c) 2026, the Serenity Desktop Environment developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "Compositor.h"

X11Toplevel* x11_toplevel_for_wlr_surface(Server& server, wlr_surface* surface);
bool x11_surface_at(Server& server, double lx, double ly, wlr_surface** surface, double* sx, double* sy);
void focus_x11_toplevel(Server& server, X11Toplevel* toplevel);
wlr_surface* window_surface_at(Server& server, double lx, double ly, double* sx, double* sy);
void focus_window_at(Server& server, double lx, double ly);

void initialize_xwayland(Server&);

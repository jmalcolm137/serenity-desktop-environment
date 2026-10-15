/*
 * Copyright (c) 2026, the Serenity Desktop Environment developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "Compositor.h"

void toplevel_report_position(Toplevel& toplevel);
void layer_report_position(LayerSurface& layer);
void toplevel_begin_interactive(Toplevel& toplevel, CursorMode mode, uint32_t edges);
void toplevel_close(Toplevel& toplevel);
void set_toplevel_minimized(Server& server, Toplevel& toplevel, bool minimized);
void toplevel_maximize_on_primary(Toplevel& toplevel);
void toplevel_toggle_maximized(Toplevel& toplevel);
void toplevel_toggle_minimized(Toplevel& toplevel);
void server_new_xdg_toplevel(wl_listener* listener, void* data);
void server_new_xdg_popup(wl_listener* listener, void* data);
void server_new_toplevel_decoration(wl_listener* listener, void* data);

/*
 * Copyright (c) 2026, the Serenity Desktop Environment developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "Compositor.h"

struct wlr_drag;

void initialize_xdg_toplevel_drag(Server&);
void xdg_toplevel_drag_start(Server&, wlr_drag*);
void xdg_toplevel_drag_motion(Server&);
void xdg_toplevel_drag_end(Server&);
void xdg_toplevel_drag_place(Server&, Toplevel&);
Toplevel* xdg_toplevel_drag_active_toplevel(Server&);

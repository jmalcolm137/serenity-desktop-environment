/*
 * Copyright (c) 2026, the Serenity Desktop Environment developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "Compositor.h"
#include "Frame.h"

void x11_update_frame(X11Toplevel& toplevel);
FrameHit x11_hit_test_frame(Server& server, double x, double y);
void x11_begin_interactive(X11Toplevel& toplevel, CursorMode mode, uint32_t edges);
bool handle_x11_frame_press(Server& server, uint32_t button);
void handle_x11_frame_release(Server& server, uint32_t button);
void update_x11_button_hover(Server& server, double x, double y);
void hook_focus_x11(Server& server, X11Toplevel* toplevel);
void hook_set_minimized_x11(Server&, X11Toplevel& toplevel, bool minimized);

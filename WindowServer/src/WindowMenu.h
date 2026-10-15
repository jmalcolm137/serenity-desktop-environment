/*
 * Copyright (c) 2026, the Serenity Desktop Environment developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "Compositor.h"

void close_window_menu(Server& server);
void open_window_menu(Server& server, Toplevel& toplevel, Gfx::IntPoint position);
void update_window_menu_hover(Server& server, double x, double y);
void window_menu_handle_press(Server& server, uint32_t button);
void window_menu_handle_release(Server& server, uint32_t button);
bool window_menu_handle_key(Server& server, xkb_keysym_t sym);

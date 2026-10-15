/*
 * Copyright (c) 2026, the Serenity Desktop Environment developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "Compositor.h"

extern int g_cursor_highlight_radius;
extern Gfx::Color g_cursor_highlight_color;

void render_cursor_highlight(Server& server);
void move_cursor_highlight(Server& server);
void toggle_cursor_highlight(Server& server);

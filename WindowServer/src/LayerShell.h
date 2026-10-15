/*
 * Copyright (c) 2026, the Serenity Desktop Environment developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "Compositor.h"

#include <LibGfx/Palette.h>

Output* output_for(Server& server, wlr_output* output);
Gfx::IntRect primary_usable_area(Server& server);
Gfx::Palette const& current_palette(Server&);
void update_xwayland_workareas(Server& server);
void arrange_layers(Server& server, wlr_output* output);
void server_new_layer_surface(wl_listener* listener, void* data);

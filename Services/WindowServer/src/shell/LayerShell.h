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
void arrange_layers(Server& server, wlr_output* output);

void initialize_layer_shell(Server&);

// Unhook the listeners initialize wired, before the objects they listen on go.
void finish_layer_shell(Server&);

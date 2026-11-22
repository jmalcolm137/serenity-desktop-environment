/*
 * Copyright (c) 2026, the Serenity Desktop Environment developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "Compositor.h"

Gfx::IntSize output_logical_size(wlr_output* output);
void use_nearest_filter(struct wlr_scene_node* node);
int resolve_output_refresh(Config::OutputSettings const* saved);
float resolve_output_scale(struct wlr_output* output, struct wlr_output_mode* mode, Config::OutputSettings const* saved);

void initialize_outputs(Server&);

// Unhook the listeners initialize wired, before the objects they listen on go.
void finish_outputs(Server&);

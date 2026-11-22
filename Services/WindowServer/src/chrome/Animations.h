/*
 * Copyright (c) 2026, the Serenity Desktop Environment developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "Compositor.h"

void start_minimize_animation(Server& server, Toplevel& toplevel, Gfx::IntRect const& window_rect, bool minimizing);
void start_launch_animation(Server& server, Toplevel& toplevel);

// Drop the outline ChromeNodes while the scene graph still exists, before the
// wlroots scene is destroyed. Their destructors would otherwise run from static
// destruction after shutdown and touch a freed scene node.
void finish_animations();

/*
 * Copyright (c) 2026, the Serenity Desktop Environment developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "Compositor.h"

void start_minimize_animation(Server& server, Toplevel& toplevel, Gfx::IntRect const& window_rect, bool minimizing);
void start_launch_animation(Server& server, Toplevel& toplevel);
